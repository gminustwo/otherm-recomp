// Save states: guest-thread continuations; see save_state.h.
#include "save_state.h"
#include "save_state_resume.h"

#include "abi_bridge.h"
#include "fiber_manager.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdlib>
#include <map>
#include <unordered_set>

namespace SaveState {
namespace {
constexpr uint32_t kOSCurrentContextAddr = 0x800000d4u;

// The resumable translated functions (project/resumable_functions.inc, which the translator reads too).
const std::unordered_set<uint32_t>& Resumable() {
    static const std::unordered_set<uint32_t> functions = {
#include "../../project/resumable_functions.inc"
    };
    return functions;
}

// The guest address each HLE wait overrides: the innermost call of a recorded chain must target it.
uint32_t WaitFunction(YieldKind kind) {
    switch (kind) {
    case YieldKind::ViWaitForRetrace: return 0x8011C310u;  // VIWaitForRetrace
    case YieldKind::MessageQueue: return 0x801104D0u;      // OSReceiveMessage
    case YieldKind::Sleep: return 0x801138E0u;             // OSSleepThread
    case YieldKind::Mutex: return 0x80110E60u;             // OSLockMutex
    case YieldKind::Suspend: return 0x80113750u;           // OSSuspendThread
    default: return 0;
    }
}

struct Live { YieldRecord record; int depth = 0; };
std::map<uint32_t, Live> g_live;                     // guest thread -> its current outermost wait
std::map<uint32_t, ThreadContinuation> g_pending;   // restored threads not yet run again
std::map<uint32_t, CpuContext> g_resumeEntry;        // YieldEntry of each restored thread

struct Active { bool on = false; uint32_t thread = 0; size_t next = 0; ThreadContinuation continuation; } g_active;

// The guest thread whose fiber is running. The fiber manager's view, since the guest's current context
// is the idle context while a thread idles in SelectThread; the guest's view before the first switch.
uint32_t CurrentThread() {
    if (const uint32_t thread = Fiber::GuestFiberManager::GetCurrentGuestThread()) return thread;
    uint32_t thread = 0;
    return Memory::TryRead32(kOSCurrentContextAddr, thread) ? thread : 0;
}

bool Word(uint32_t address, uint32_t& value) { return (address & 3) == 0 && Memory::TryRead32(address, value); }

// The direct call before a return address: its target, or 0 for anything else.
uint32_t DirectCallBefore(uint32_t returnAddress) {
    uint32_t op = 0;
    if (!Word(returnAddress - 4, op) || (op >> 26) != 18 || !(op & 1)) return 0;
    int32_t offset = int32_t(op & 0x03FFFFFC);
    if (offset & 0x02000000) offset -= 0x04000000;
    return (op & 2) ? uint32_t(offset) : returnAddress - 4 + uint32_t(offset);
}

// bctrl / blrl before a return address: a call through CTR or LR (virtual calls, function pointers, the
// DOL calling into the pre-linked game.rel).
bool IndirectCallBefore(uint32_t returnAddress) {
    uint32_t op = 0;
    return Word(returnAddress - 4, op) && (op == 0x4E800421u || op == 0x4E800021u);
}

// A frameless jump stub: loads and moves only, then bctr or b (main's call into the pre-linked game.rel goes
// through one). The callee it jumps to runs on the caller's call, so re-running the stub during a restore
// only repeats those loads.
bool IsTailJumpStub(uint32_t function) {
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t op = 0;
        if (!Word(function + 4 * i, op)) return false;
        const uint32_t primary = op >> 26;
        if (op == 0x4E800420u) return true;                    // bctr
        if (primary == 18) return (op & 1) == 0;               // b (not bl)
        const bool load = primary == 32 || primary == 34 || primary == 40 || primary == 42;  // lwz lbz lhz lha
        const bool arith = primary == 14 || primary == 15 || primary == 24 || primary == 25;  // addi addis ori oris
        const bool moveToCtr = op == 0x7D8903A6u || (primary == 31 && ((op >> 1) & 0x3FF) == 467 && ((op >> 11) & 0x3FF) == 0x120);  // mtctr
        const bool moveRegister = primary == 31 && ((op >> 1) & 0x3FF) == 444;  // or (mr)
        if (!load && !arith && !moveToCtr && !moveRegister) return false;
    }
    return false;
}

// The translated function an address lies in (the nearest entry at or below it), 0 if none.
uint32_t FunctionContaining(uint32_t address) {
    const auto info = TranslatedFunctionRegistry::FindNearestByAddress(address);
    return info ? info->address : 0;
}
}  // namespace

YieldScope::YieldScope(YieldKind kind, const CpuContext& entry, std::initializer_list<uint32_t> extra)
    : thread_(CurrentThread()) {
    Live& live = g_live[thread_];
    if (live.depth++ != 0) return;
    owner_ = true;
    live.record.kind = kind;
    live.record.entry = entry;
    size_t i = 0;
    for (const uint32_t value : extra) if (i < 6) live.record.extra[i++] = value;
    for (; i < 6; ++i) live.record.extra[i] = 0;
}

YieldScope::YieldScope(const YieldRecord& resumed) : thread_(CurrentThread()) {
    Live& live = g_live[thread_];
    if (live.depth++ != 0) return;
    owner_ = true;
    live.record = resumed;
}

YieldScope::~YieldScope() {
    const auto it = g_live.find(thread_);
    if (it != g_live.end() && --it->second.depth <= 0) g_live.erase(it);
}

const YieldRecord* TakeYield(YieldKind kind, bool& running) {
    if (!g_active.on || g_active.thread != CurrentThread()) return nullptr;
    if (g_active.next != g_active.continuation.frames.size() || g_active.continuation.yield.kind != kind) {
        SaveStateResume::Mismatch(WaitFunction(kind));
    }
    g_active.on = false;
    running = g_active.continuation.running;
    return &g_active.continuation.yield;
}

const YieldRecord* LiveWait(uint32_t thread) {
    const auto it = g_live.find(thread);
    return it != g_live.end() && it->second.depth > 0 ? &it->second.record : nullptr;
}

bool IsResumable(uint32_t function) noexcept { return Resumable().count(function) != 0; }

bool DescribeThread(uint32_t thread, uint32_t rootFunction, bool running, ThreadContinuation& out, std::string& error) {
    // A restored thread that has not run since: the continuation it was restored with still holds.
    if (const auto pending = g_pending.find(thread); pending != g_pending.end()) {
        out = pending->second;
        out.running = running;
        return true;
    }
    const auto live = g_live.find(thread);
    if (live == g_live.end() || live->second.record.kind == YieldKind::None) {
        error = "thread not in a recorded wait";
        return false;
    }
    out = {};
    out.thread = thread;
    out.running = running;
    out.yield = live->second.record;
    // Innermost first: the wait's caller, then saved LRs along the EABI back chain to the root.
    std::vector<uint32_t> returns;
    uint32_t returnAddress = out.yield.entry.lr, sp = out.yield.entry.gpr[1];
    while (returnAddress != 0) {
        returns.push_back(returnAddress);
        if (returns.size() > 64) { error = "chain too deep"; return false; }
        uint32_t back = 0;
        if (!Word(sp, back)) { error = "unreadable stack"; return false; }
        if (back == 0 || back == 0xFFFFFFFFu) break;  // the end of the chain (__start stores -1)
        if (back <= sp || !Word(back + 4, returnAddress)) { error = "broken back chain"; return false; }
        sp = back;
        if (returnAddress == 0x80113090u) break;  // OSExitThread: a thread entry's root LR
    }
    if (!returns.empty() && returns.back() == 0x80113090u) returns.pop_back();
    // The boot thread's back chain ends in __start's own frame, whose saved LR slot is not a return address.
    while (!returns.empty() && FunctionContaining(returns.back()) != rootFunction &&
           returns.size() > 1 && FunctionContaining(returns[returns.size() - 2]) == rootFunction) {
        returns.pop_back();
    }
    if (returns.empty()) { error = "empty chain"; return false; }
    // Frames outermost first: the root is the thread entry; each next frame is the call's target, read
    // from a direct bl, or for a call through CTR/LR the function the next return address lies in.
    uint32_t function = rootFunction;
    for (size_t i = returns.size(); i-- > 0;) {
        if (FunctionContaining(returns[i]) != function && IsTailJumpStub(function)) function = FunctionContaining(returns[i]);
        if (FunctionContaining(returns[i]) != function) {
            char text[96];
            std::snprintf(text, sizeof(text), "return 0x%08X is in 0x%08X, not 0x%08X", returns[i],
                          FunctionContaining(returns[i]), function);
            error = text;
            out.frames.push_back({function, returns[i]});
            return false;
        }
        out.frames.push_back({function, returns[i]});
        function = DirectCallBefore(returns[i]);
        if (function == 0 && IndirectCallBefore(returns[i]))
            function = i > 0 ? FunctionContaining(returns[i - 1]) : WaitFunction(out.yield.kind);
        if (function == 0) {
            char text[80];
            std::snprintf(text, sizeof(text), "no call before 0x%08X", returns[i]);
            error = text;
            return false;
        }
    }
    if (function != WaitFunction(out.yield.kind)) { error = "innermost call is not the recorded wait"; return false; }
    return true;
}

bool CaptureThread(uint32_t thread, uint32_t rootFunction, bool running, ThreadContinuation& out, std::string& error) {
    if (!DescribeThread(thread, rootFunction, running, out, error)) return false;
    for (const Frame& frame : out.frames) {
        if (!IsResumable(frame.function)) {
            char text[64];
            std::snprintf(text, sizeof(text), "function 0x%08X not resumable", frame.function);
            error = text;
            return false;
        }
    }
    return true;
}

void ResetForRestore() {
    g_live.clear();
    g_pending.clear();
    g_resumeEntry.clear();
    g_active = {};
}

void SetPendingResume(ThreadContinuation continuation) {
    const uint32_t thread = continuation.thread;
    g_resumeEntry[thread] = continuation.yield.entry;
    g_pending[thread] = std::move(continuation);
}

uint32_t BeginResume(uint32_t thread) {
    const auto it = g_pending.find(thread);
    if (it == g_pending.end() || it->second.frames.empty()) {
        RT_LOGF(RT_TAG_RUNTIME, "save state: thread %08X has no continuation\n", thread);
        SaveStateResume::Mismatch(0);
    }
    g_active.continuation = std::move(it->second);
    g_pending.erase(it);
    g_active.thread = thread;
    g_active.next = 0;
    g_active.on = true;
    return g_active.continuation.frames.front().function;
}
}  // namespace SaveState

namespace SaveStateResume {
bool Active() noexcept { return SaveState::g_active.on && SaveState::g_active.thread == SaveState::CurrentThread(); }

uint32_t Enter(uint32_t function) {
    auto& active = SaveState::g_active;
    if (!active.on || active.next >= active.continuation.frames.size() ||
        active.continuation.frames[active.next].function != function) {
        Mismatch(function);
    }
    return active.continuation.frames[active.next++].returnAddress;
}

uint32_t Target(uint32_t original) noexcept {
    const auto& active = SaveState::g_active;
    return Active() && active.next < active.continuation.frames.size() ? active.continuation.frames[active.next].function
                                                                         : original;
}

const CpuContext& YieldEntry() noexcept {
    static const CpuContext none{};
    const auto it = SaveState::g_resumeEntry.find(SaveState::CurrentThread());
    return it != SaveState::g_resumeEntry.end() ? it->second : none;
}

void Mismatch(uint32_t function) {
    RT_LOGF(RT_TAG_RUNTIME, "save state: resume mismatch at function 0x%08X\n", function);
    std::abort();
}
}  // namespace SaveStateResume
