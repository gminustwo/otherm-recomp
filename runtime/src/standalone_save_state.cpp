// Save states for the windowed game: F5 saves, F9 loads (settings_overlay.cpp), one quick slot in
// <UserData>/States/quick.state.
//
// The machinery is save_state.h's: at a capture every
// started guest thread is parked in a recorded HLE wait (VIWaitForRetrace, OSReceiveMessage,
// OSSleepThread), and its guest call chain is re-entered on a load through the functions the translator
// emits resumable (project/resumable_functions.inc); guest memory and the HLE modules' host state
// are copied as they are. A request is taken at the scheduler's idle loop after a VI retrace: no guest
// thread is runnable there and the one that was running idles inside its wait.
//
// A save serializes on the guest thread and writes the file on a worker. A load reads and checks the
// whole file first, then replaces guest memory and HLE state, and rebuilds every guest fiber from a
// helper context (the idling fiber cannot destroy itself); the restored running thread continues in the
// idle loop and the others when they are next switched to. States resume translated code at its call
// sites, so they load only into the engine build that wrote them: the file carries a hash of the engine
// module.
//
// OTHERM_STATE_PROBE=<retraces> (bring-up) describes every thread's chain at that interval and logs the
// functions a capture would still need to be resumable, without saving anything. OTHERM_STATE_AT=
// "save:4000,load:4600" (tests) requests a save or a load at those retrace counts, as F5 / F9 would.
#include "standalone_save_state.h"

#include "abi_bridge.h"
#include "aurora_events.h"
#include "fiber_manager.h"
#include "gx_guest_write.h"
#include "hle_stubs.h"
#include "host_context.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "keybinds.h"
#include "save_state.h"
#include "virtual_remote.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

uint64_t PPC_ReadGuestTimeBase();         // ppc_helpers.cpp
void PPC_SetGuestTimeBase(uint64_t value);
extern "C" void aurora_invalidate_textures();  // aurora gx.cpp

namespace StandaloneState {

std::atomic<bool> g_armed{false};
uint32_t g_idlePasses = 0;

namespace {
constexpr uint32_t kMagic = SaveState::Tag("OTMS"), kVersion = 1;
constexpr uint32_t kMem1Base = 0x80000000u, kMem2Base = 0x90000000u;
constexpr uint32_t kLockedCacheBase = 0xE0000000u, kLockedCacheSize = 16 * 1024;
constexpr uint32_t kBootRoot = 0x80006310u;  // __start: the root of the boot thread's chain

enum Request : int { kNone = 0, kSave = 1, kLoad = 2 };
std::atomic<int> g_request{kNone};
std::atomic<bool> g_writing{false};

std::mutex g_noticeMutex;
std::string g_notice;
std::chrono::steady_clock::time_point g_noticeUntil;

void SetNotice(const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    RT_LOGF(RT_TAG_RUNTIME, "[state] %s\n", text);
    std::lock_guard lock(g_noticeMutex);
    g_notice = text;
    g_noticeUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

// OTHERM_STATE_FILE=<name> (tests) uses <UserData>/States/<name> instead of the quick slot; a scripted
// "psave:<frame>:<name>" names the file of that one save.
std::string g_nextSaveName;
std::filesystem::path StatePath() {
    const char* name = std::getenv("OTHERM_STATE_FILE");
    return RuntimeConfigFile::ApplicationDataDirectory() / "States" / (name && *name ? name : "quick.state");
}

// The whole file in a few large reads. An istreambuf_iterator copy goes a byte at a time (~0.5 s for a 159 MB state,
// most of a state load's pause), and even ifstream::read refills libc++'s small buffer over and over (~0.2 s); stall
// watch, NOTES.md 2026-11-18.
template <typename Bytes>
bool ReadWholeFile(const std::filesystem::path& path, Bytes& out) {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        auto* at = reinterpret_cast<char*>(out.data());
        for (uint64_t left = static_cast<uint64_t>(size.QuadPart); ok && left > 0;) {
            const DWORD chunk = static_cast<DWORD>((std::min<uint64_t>)(left, 64u << 20));
            DWORD got = 0;
            ok = ReadFile(file, at, chunk, &got, nullptr) && got == chunk;
            at += got;
            left -= got;
        }
    }
    CloseHandle(file);
    return ok;
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    return size == 0 || static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
#endif
}

// The engine build: a hash of the code sections of the module this code is in, read from the file on disk
// (unrelocated, so where Windows loads it does not matter). Only code: the build relinks the engine for its
// resources every time, which changes the rest of the file but not what a state resumes into. Elsewhere,
// the compile time of this file.
uint64_t BuildId() {
    static const uint64_t id = [] {
        uint64_t hash = 0xCBF29CE484222325ull;
        auto mixWord = [&](uint64_t w) {
            hash = (hash ^ w) * 0x100000001B3ull;
            hash ^= hash >> 29;
        };
#if defined(_WIN32)
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&RequestSave), &module) &&
            GetModuleFileNameW(module, path, MAX_PATH)) {
            std::vector<char> image;
            ReadWholeFile(std::filesystem::path(path), image);
            auto read32 = [&](size_t at) -> uint32_t {
                uint32_t v = 0;
                if (at + 4 <= image.size()) std::memcpy(&v, image.data() + at, 4);
                return v;
            };
            auto read16 = [&](size_t at) -> uint16_t {
                uint16_t v = 0;
                if (at + 2 <= image.size()) std::memcpy(&v, image.data() + at, 2);
                return v;
            };
            const size_t nt = read32(0x3C);
            if (read32(nt) == 0x00004550u) {  // "PE\0\0"
                const uint16_t sections = read16(nt + 6);
                const uint16_t optionalSize = read16(nt + 20);
                size_t table = nt + 24 + optionalSize;
                bool hashed = false;
                for (uint16_t i = 0; i < sections; ++i, table += 40) {
                    const uint32_t characteristics = read32(table + 36);
                    if (!(characteristics & 0x00000020u)) continue;  // IMAGE_SCN_CNT_CODE
                    const size_t raw = read32(table + 20), rawSize = read32(table + 16);
                    if (raw + rawSize > image.size()) continue;
                    for (size_t at = raw; at + 8 <= raw + rawSize; at += 8) {
                        uint64_t w;
                        std::memcpy(&w, image.data() + at, 8);
                        mixWord(w);
                    }
                    mixWord(rawSize);
                    hashed = true;
                }
                if (hashed) return hash;
            }
        }
#endif
        const char stamp[] = __DATE__ " " __TIME__;
        for (const char c : stamp) mixWord(static_cast<uint8_t>(c));
        return hash;
    }();
    return id;
}

struct Continuations {
    std::vector<SaveState::ThreadContinuation> threads;
    uint32_t running = 0, boot = 0;
    CpuContext live{};
    std::vector<Fiber::FiberRecord> fibers;
    uint32_t currentThread = 0;
};

struct StartedThread { uint32_t thread, root; };

bool ListThreads(std::vector<StartedThread>& started, uint32_t& running, uint32_t& boot, std::string& error) {
    struct Visit { std::vector<StartedThread>* started; void* current; uint32_t running = 0, boot = 0; };
    Visit visit{&started, HostContext::Current()};
    const bool listed = Fiber::GuestFiberManager::ForEachFiber(
        [](uint32_t thread, const Fiber::GuestFiber& fiber, bool, void* user) {
            auto& v = *static_cast<Visit*>(user);
            if (fiber.terminated || !fiber.fiber) return;
            if (fiber.fiber == v.current) v.running = thread;
            if (fiber.isSchedulerFiber) v.boot = thread;
            if (fiber.started) v.started->push_back({thread, fiber.isSchedulerFiber ? kBootRoot : fiber.entryPoint});
        },
        &visit);
    if (!listed) { error = "fiber table busy"; return false; }
    running = visit.running;
    boot = visit.boot;
    if (!running || !boot) {
        char text[96];
        std::snprintf(text, sizeof(text), "running thread %08X or boot thread %08X unknown", running, boot);
        error = text;
        return false;
    }
    return true;
}

bool CaptureContinuations(Continuations& out, std::string& error) {
    std::vector<StartedThread> started;
    if (!ListThreads(started, out.running, out.boot, error)) return false;
    for (const auto& [thread, root] : started) {
        SaveState::ThreadContinuation continuation;
        std::string reason;
        if (!SaveState::CaptureThread(thread, root, thread == out.running, continuation, reason)) {
            char text[160];
            std::snprintf(text, sizeof(text), "thread %08X: %s", thread, reason.c_str());
            error = text;
            return false;
        }
        out.threads.push_back(std::move(continuation));
    }
    out.live = GetPersistentCpuContext();
    Fiber::GuestFiberManager::ExportTable(out.fibers, out.currentThread);
    return true;
}

// OTHERM_STATE_PROBE: each thread's chain (once per distinct chain), and the functions on it that are not
// resumable yet (each once).
uint32_t ProbeInterval() {
    static const uint32_t interval = [] {
        const char* v = std::getenv("OTHERM_STATE_PROBE");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return interval;
}
uint32_t g_retraces = 0;
bool g_probeDue = false;
// A request is taken at the next idle pass; one still waiting a second later is logged once (with how many idle
// passes there have been), so a scene that never idles shows up in the log instead of a silent F5.
uint32_t g_requestSince = 0;
bool g_requestReported = false;

// OTHERM_STATE_AT: (retrace, request) pairs in the order given; "psave:<presented frame>:<file>" saves at a presented
// frame into its own file (several states of one run, e.g. to diff guest memory frame by frame).
struct ScriptedRequest { uint32_t retrace; int request; bool byFrame = false; std::string name; };
const std::vector<ScriptedRequest>& ScriptedRequests() {
    static const std::vector<ScriptedRequest> list = [] {
        std::vector<ScriptedRequest> out;
        const char* v = std::getenv("OTHERM_STATE_AT");
        std::string text = v ? v : "";
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find(',', at);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(at, end - at);
            const size_t colon = item.find(':');
            if (colon != std::string::npos) {
                const std::string kind = item.substr(0, colon);
                const uint32_t retrace = static_cast<uint32_t>(std::strtoul(item.c_str() + colon + 1, nullptr, 10));
                if (kind == "save") out.push_back({retrace, 1});
                else if (kind == "load") out.push_back({retrace, 2});
                else if (kind == "psave") {
                    const size_t second = item.find(':', colon + 1);
                    out.push_back({retrace, 1, true, second == std::string::npos ? std::string() : item.substr(second + 1)});
                }
            }
            at = end + 1;
        }
        return out;
    }();
    return list;
}
size_t g_nextScripted = 0;

void Probe() {
    static std::set<uint32_t> reportedFunctions;
    static std::set<std::string> reportedChains;
    std::vector<StartedThread> started;
    uint32_t running = 0, boot = 0;
    std::string error;
    if (!ListThreads(started, running, boot, error)) {
        RT_LOGF(RT_TAG_RUNTIME, "[state] probe: %s\n", error.c_str());
        return;
    }
    for (const auto& [thread, root] : started) {
        SaveState::ThreadContinuation chain;
        std::string reason;
        const bool ok = SaveState::DescribeThread(thread, root, thread == running, chain, reason);
        char part[64];
        std::snprintf(part, sizeof(part), "thread %08X%s:", thread, thread == running ? " (running)" : "");
        std::string text = part;
        for (const auto& frame : chain.frames) {
            std::snprintf(part, sizeof(part), " %08X@%08X%s", frame.function, frame.returnAddress,
                          SaveState::IsResumable(frame.function) ? "" : "*");
            text += part;
        }
        if (ok) {
            static const char* kKinds[] = {"none", "VIWaitForRetrace", "OSReceiveMessage", "OSSleepThread", "OSLockMutex",
                                           "OSSuspendThread"};
            const uint32_t kind = static_cast<uint32_t>(chain.yield.kind);
            text += std::string(" -> ") + (kind < 6 ? kKinds[kind] : "?");
        } else {
            text += " | " + reason;
        }
        if (reportedChains.insert(text).second) RT_LOGF(RT_TAG_RUNTIME, "[state] probe %s\n", text.c_str());
        for (const auto& frame : chain.frames) {
            if (!SaveState::IsResumable(frame.function) && reportedFunctions.insert(frame.function).second) {
                RT_LOGF(RT_TAG_RUNTIME, "[state] probe needs resumable 0x%08X (thread %08X)\n", frame.function, thread);
            }
        }
    }
}

void WriteContinuations(SaveState::Writer& out, const Continuations& state) {
    out.Pod(state.running);
    out.Pod(state.boot);
    out.Pod(state.live);
    out.Pod(state.currentThread);
    out.Pod(uint32_t(state.fibers.size()));
    for (const auto& fiber : state.fibers) out.Pod(fiber);
    out.Pod(uint32_t(state.threads.size()));
    for (const auto& thread : state.threads) {
        out.Pod(thread.thread);
        out.Pod(thread.running);
        out.Pod(thread.yield);
        out.Pod(uint32_t(thread.frames.size()));
        for (const auto& frame : thread.frames) out.Pod(frame);
    }
}

void ReadContinuations(SaveState::Reader& in, Continuations& state) {
    in.Pod(state.running);
    in.Pod(state.boot);
    in.Pod(state.live);
    in.Pod(state.currentThread);
    state.fibers.resize(std::min<uint32_t>(in.Get<uint32_t>(), 4096));
    for (auto& fiber : state.fibers) in.Pod(fiber);
    state.threads.resize(std::min<uint32_t>(in.Get<uint32_t>(), 4096));
    for (auto& thread : state.threads) {
        in.Pod(thread.thread);
        in.Pod(thread.running);
        in.Pod(thread.yield);
        thread.frames.resize(std::min<uint32_t>(in.Get<uint32_t>(), 64));
        for (auto& frame : thread.frames) in.Pod(frame);
    }
}

// The step a save or load is in, for the watchdog: a capture that does not finish within two seconds is logged
// with the step it stopped at (a lock it waits for, most likely).
std::atomic<const char*> g_step{nullptr};
std::atomic<int64_t> g_stepSince{0};

void Step(const char* name) {
    g_stepSince.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    g_step.store(name, std::memory_order_relaxed);
}

void StartWatchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            const char* reported = nullptr;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const char* step = g_step.load(std::memory_order_relaxed);
                if (!step) { reported = nullptr; continue; }
                const auto since = std::chrono::steady_clock::duration(g_stepSince.load(std::memory_order_relaxed));
                const auto age = std::chrono::steady_clock::now().time_since_epoch() - since;
                if (age > std::chrono::seconds(2) && step != reported) {
                    RT_LOGF(RT_TAG_RUNTIME, "[state] stuck for %lld ms at: %s\n",
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(age).count()), step);
                    reported = step;
                }
            }
        }).detach();
    });
}

bool Serialize(std::vector<uint8_t>& out, const Continuations& continuations, std::string& error) {
    out.clear();
    out.reserve(Memory::kMem1Size + Memory::kMem2Size + (1 << 20));
    SaveState::Writer writer(out);
    writer.Pod(kMagic);
    writer.Pod(kVersion);
    writer.Pod(BuildId());
    Step("save: threads");
    size_t at = writer.Begin(SaveState::Tag("THRD"));
    WriteContinuations(writer, continuations);
    writer.End(at);
    Step("save: clock");
    at = writer.Begin(SaveState::Tag("CLCK"));
    writer.Pod(PPC_ReadGuestTimeBase());
    writer.End(at);
    Step("save: VI state (g_viMutex)");
    at = writer.Begin(SaveState::Tag("VI  "));
    VI_HLE_SaveState(writer);
    writer.End(at);
    Step("save: OS state (sleep timers)");
    at = writer.Begin(SaveState::Tag("OS  "));
    OS_HLE_SaveState(writer);
    writer.End(at);
    Step("save: AI state (g_ai.mutex)");
    at = writer.Begin(SaveState::Tag("AI  "));
    AI_HLE_SaveState(writer);
    writer.End(at);
    Step("save: input state");
    at = writer.Begin(SaveState::Tag("INPT"));
    KPAD_HLE_SaveState(writer);
    VirtualRemote::SaveState(writer);
    writer.End(at);
    Step("save: NAND state");
    at = writer.Begin(SaveState::Tag("NAND"));
    if (!NAND_HLE_SaveState(writer, error)) return false;
    NAND_HLE_SaveAsyncState(writer);
    writer.End(at);
    Step("save: DVD queue");
    at = writer.Begin(SaveState::Tag("DVD "));
    DVD_HLE_SaveState(writer);
    writer.End(at);
    Step("save: GX object ids");
    at = writer.Begin(SaveState::Tag("GXID"));
    GX_HLE_SaveObjectIds(writer);
    writer.End(at);
    Step("save: MEM1");
    at = writer.Begin(SaveState::Tag("MEM1"));
    writer.Bytes(Memory::GetPointer(kMem1Base, Memory::kMem1Size), Memory::kMem1Size);
    writer.End(at);
    Step("save: MEM2");
    at = writer.Begin(SaveState::Tag("MEM2"));
    writer.Bytes(Memory::GetPointer(kMem2Base, Memory::kMem2Size), Memory::kMem2Size);
    writer.End(at);
    Step("save: locked cache");
    at = writer.Begin(SaveState::Tag("LKCH"));
    writer.Bytes(Memory::GetPointer(kLockedCacheBase, kLockedCacheSize), kLockedCacheSize);
    writer.End(at);
    return true;
}

void Save() {
    Continuations continuations;
    std::string error;
    const auto began = std::chrono::steady_clock::now();
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    StartWatchdog();
    Step("save: thread chains (fiber table)");
    const bool captured = CaptureContinuations(continuations, error) && Serialize(*bytes, continuations, error);
    g_step.store(nullptr, std::memory_order_relaxed);
    if (!captured) {
        SetNotice("Couldn't save the state: %s", error.c_str());
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    RT_LOGF(RT_TAG_RUNTIME, "[state] captured %zu threads, %zu bytes in %.1f ms\n", continuations.threads.size(),
            bytes->size(), ms);
    g_writing.store(true);
    std::filesystem::path path = StatePath();
    if (!g_nextSaveName.empty()) {
        path = path.parent_path() / g_nextSaveName;
        g_nextSaveName.clear();
    }
    std::thread([bytes, path] {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        static std::atomic<uint32_t> s_writeSerial{0};
        auto temp = path;
        temp += ".tmp" + std::to_string(s_writeSerial.fetch_add(1));  // overlapping writes never share a temp file
        bool ok = false;
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            ok = file && file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        }
        if (ok) {
            std::filesystem::rename(temp, path, ec);
            ok = !ec;
        }
        if (ok) {
            // The load keys and buttons Options > Controls has: "State saved (F9 or View + LB loads it)".
            std::string loads = Keybinds::HotkeyLabel(Keybinds::kLoadState);
            if (loads == "None") loads.clear();
            const std::vector<std::string> pad = VirtualRemote::HotkeyInputs(VirtualRemote::PadHotkey::LoadState);
            if (!pad.empty()) loads += (loads.empty() ? "" : " or ") + VirtualRemote::PadInputsLabel(pad);
            const std::string notice = loads.empty() ? std::string("State saved") : "State saved (" + loads + " loads it)";
            SetNotice("%s", notice.c_str());
        }
        else SetNotice("Couldn't write %s", RuntimeConfigFile::PathToUtf8(path).c_str());
        g_writing.store(false);
    }).detach();
}

// A load in progress: the parsed state, kept off the fiber that is about to be destroyed.
struct PendingLoad {
    std::vector<uint8_t> bytes;
    Continuations threads;
};
std::unique_ptr<PendingLoad> g_load;
void* g_originalScheduler = nullptr;  // the process thread's own context: never destroyed
void* g_helper = nullptr;
uint32_t g_bootThread = 0;

void BootResume(void*) {
    auto& cpu = GetPersistentCpuContext();
    CpuContextScope scope(&cpu);
    const uint32_t root = SaveState::BeginResume(g_bootThread);
    try {
        InvokeIndirectCpu(root, &cpu);
    } catch (const std::exception& error) {
        RT_LOGF(RT_TAG_RUNTIME, "[state] guest failed after a load: %s\n", error.what());
        std::abort();
    }
    RT_LOGF(RT_TAG_RUNTIME, "[state] the guest's main returned after a load\n");
    ExitForAuroraWindowClose();
}

// The helper context: rebuilds every fiber (the one that asked is among them), then runs the restored
// running thread. Re-entered for each later load.
void HelperMain(void*) {
    for (;;) {
        void* newBoot = nullptr;
        void* newRunning = nullptr;
        const Continuations& state = g_load->threads;
        if (!Fiber::GuestFiberManager::RebuildForRestore(state.fibers, g_originalScheduler, BootResume, state.running,
                                                         &newBoot, &newRunning)) {
            RT_LOGF(RT_TAG_RUNTIME, "[state] fiber rebuild failed after the guest state was replaced\n");
            std::abort();
        }
        GetPersistentCpuContext() = state.live;
        g_load.reset();
        HostContext::Switch(newRunning);
    }
}

// Render targets the game draws once per scene and then only reads. Aurora keeps EFB copies on the GPU (RAM never
// receives them), so a state holds whatever the heap had there before the copy, and a load drops the GPU copy: the
// game would sample that garbage until its next scene setup. Ask the game to draw them again on the next frame.
//
// dummy_SBD (NOTES.md 2026-11-11): the 320x240 ripple-distortion map of the force-field glass (an indirect texture).
// Pointer at 0x80CB6B44 (rel_808AAE04 allocates it); the pass rel_808AB7CC draws the wave hits and copies RG8 into
// it when its byte 0x80C5FABC is set or hits are active (count 0x80CB713C), then sets the byte to (count != 0).
// rel_808AAE04 sets that byte every time it runs (its callers look like scene setup), so writing it here is the
// game's own "draw it once more".
void RedrawOneShotRenderTargets() {
    uint32_t sbd = 0;
    if (!Memory::TryRead32(0x80CB6B44u, sbd) || sbd == 0) return;  // no scene with the ripple map set up yet
    Memory::Write8(0x80C5FABCu, 1);
    RT_LOGF(RT_TAG_RUNTIME, "[state] ripple map at %08X is drawn again on the next frame\n", sbd);
}

void Load() {
    auto load = std::make_unique<PendingLoad>();
    const auto path = StatePath();
    if (!ReadWholeFile(path, load->bytes)) {
        SetNotice("No saved state yet");
        return;
    }
    // Check every section before touching the running guest.
    SaveState::Reader in(load->bytes.data(), load->bytes.size());
    const bool header = in.Get<uint32_t>() == kMagic && in.Get<uint32_t>() == kVersion;
    const uint64_t build = in.Get<uint64_t>();
    if (!header || !in.Ok()) {
        SetNotice("Couldn't load the state: not a Metroid: Other M state");
        return;
    }
    // OTHERM_STATE_ANY_BUILD=1 (bring-up only): load a state from another build, e.g. after a diagnostics-only
    // rebuild that left the translated code alone. A real change of the translation makes such a load crash.
    static const bool anyBuild = std::getenv("OTHERM_STATE_ANY_BUILD") != nullptr;
    if (build != BuildId() && !anyBuild) {
        SetNotice("Couldn't load the state: it was saved by a different build of the game");
        return;
    }
    SaveState::Reader threads = in.Section(SaveState::Tag("THRD"));
    SaveState::Reader clock = in.Section(SaveState::Tag("CLCK"));
    SaveState::Reader vi = in.Section(SaveState::Tag("VI  "));
    SaveState::Reader os = in.Section(SaveState::Tag("OS  "));
    SaveState::Reader ai = in.Section(SaveState::Tag("AI  "));
    SaveState::Reader input = in.Section(SaveState::Tag("INPT"));
    SaveState::Reader nand = in.Section(SaveState::Tag("NAND"));
    SaveState::Reader dvd = in.Section(SaveState::Tag("DVD "));
    SaveState::Reader gxid = in.Section(SaveState::Tag("GXID"));
    SaveState::Reader mem1 = in.Section(SaveState::Tag("MEM1"));
    SaveState::Reader mem2 = in.Section(SaveState::Tag("MEM2"));
    SaveState::Reader lockedCache = in.Section(SaveState::Tag("LKCH"));
    ReadContinuations(threads, load->threads);
    const uint64_t timeBase = clock.Get<uint64_t>();
    const uint8_t* mem1Bytes = mem1.Take(Memory::kMem1Size);
    const uint8_t* mem2Bytes = mem2.Take(Memory::kMem2Size);
    const uint8_t* lcBytes = lockedCache.Take(kLockedCacheSize);
    if (!in.AtEnd() || !threads.AtEnd() || !clock.AtEnd() || !mem1.AtEnd() || !mem2.AtEnd() || !lockedCache.AtEnd() ||
        !vi.Ok() || !os.Ok() || !ai.Ok() || !input.Ok() || !nand.Ok() || !dvd.Ok() || !gxid.Ok() ||
        load->threads.threads.empty()) {
        SetNotice("Couldn't load the state: the file is damaged");
        return;
    }

    // From here the running guest is replaced.
    std::memcpy(Memory::GetPointer(kMem1Base, Memory::kMem1Size), mem1Bytes, Memory::kMem1Size);
    std::memcpy(Memory::GetPointer(kMem2Base, Memory::kMem2Size), mem2Bytes, Memory::kMem2Size);
    std::memcpy(Memory::GetPointer(kLockedCacheBase, kLockedCacheSize), lcBytes, kLockedCacheSize);
    PPC_SetGuestTimeBase(timeBase);
    VI_HLE_LoadState(vi);
    OS_HLE_LoadState(os);
    AI_HLE_LoadState(ai);
    KPAD_HLE_LoadState(input);
    VirtualRemote::LoadState(input);
    NAND_HLE_LoadState(nand);
    NAND_HLE_LoadAsyncState(nand);
    DVD_HLE_LoadState(dvd);
    GX_HLE_LoadObjectIds(gxid);
    GxGuestWrite::NotifyWrite(kMem1Base, Memory::kMem1Size);
    GxGuestWrite::NotifyWrite(kMem2Base, Memory::kMem2Size);
    aurora_invalidate_textures();
    RedrawOneShotRenderTargets();

    SaveState::ResetForRestore();
    for (const auto& thread : load->threads.threads) SaveState::SetPendingResume(thread);
    g_bootThread = load->threads.boot;
    load->bytes.clear();
    load->bytes.shrink_to_fit();
    g_load = std::move(load);
    RT_LOGF(RT_TAG_RUNTIME, "[state] loading at idle pass %u\n", g_idlePasses);
    SetNotice("State loaded");
    aurora_mark_frame_capture_origin();  // tests: AURORA_CAPTURE_FRAMES=+<n>:<file> counts from here
    if (!g_helper) g_helper = HostContext::Create(256 * 1024, HelperMain, nullptr);
    if (!g_helper) {
        RT_LOGF(RT_TAG_RUNTIME, "[state] no helper context for the load\n");
        std::abort();
    }
    HostContext::Switch(g_helper);  // never returns: this fiber is destroyed there
    std::abort();
}
}  // namespace

void RequestSave() { g_request.store(kSave); }
bool RequestPending() noexcept { return g_request.load(std::memory_order_relaxed) != kNone; }
void RequestLoad() { g_request.store(kLoad); }

void OnRetraceEnd() noexcept {
    ++g_retraces;
    if (const uint32_t interval = ProbeInterval(); interval && g_retraces % interval == 0) g_probeDue = true;
    const auto& scripted = ScriptedRequests();
    const auto due = [&](const ScriptedRequest& r) {
        return r.byFrame ? VI_HLE_PresentedFrames() >= r.retrace : g_retraces >= r.retrace;
    };
    // One scripted request at a time: issuing the next while a save is still waiting for an idle pass would replace its
    // file name (a busy frame, e.g. with draw logging on, can keep the scheduler from idling for many retraces).
    if (g_nextScripted < scripted.size() && due(scripted[g_nextScripted]) &&
        g_request.load(std::memory_order_relaxed) == kNone) {
        g_nextSaveName = scripted[g_nextScripted].name;
        RT_LOGF(RT_TAG_RUNTIME, "[state] scripted %s at retrace %u\n", scripted[g_nextScripted].request == kSave ? "save" : "load",
                g_retraces);
        g_request.store(scripted[g_nextScripted++].request);
    }
    const int pending = g_request.load(std::memory_order_relaxed);
    if (pending == kNone) {
        g_requestSince = 0;
        g_requestReported = false;
    } else if (g_requestSince == 0) {
        g_requestSince = g_retraces;
    } else if ((!g_requestReported && g_retraces - g_requestSince > 60) || g_retraces - g_requestSince == 300) {
        g_requestReported = true;
        RT_LOGF(RT_TAG_RUNTIME, "[state] %s request still waiting after %u retraces (%u idle passes so far)\n",
                pending == kSave ? "save" : "load", g_retraces - g_requestSince, g_idlePasses);
        Fiber::GuestFiberManager::DumpGuestThreads(std::cerr);
    }
    if (g_probeDue || pending != kNone) g_armed.store(true, std::memory_order_relaxed);
}

void AtIdle(CpuContext*) {
    g_armed.store(false, std::memory_order_relaxed);
    if (!g_originalScheduler) g_originalScheduler = Fiber::GuestFiberManager::SchedulerFiber();
    if (g_probeDue) {
        g_probeDue = false;
        Probe();
    }
    const int request = g_request.exchange(kNone);
    if (request == kSave) Save();
    else if (request == kLoad) Load();
}

std::filesystem::path QuickSlotPath() { return StatePath(); }

std::string Notice() {
    std::lock_guard lock(g_noticeMutex);
    return std::chrono::steady_clock::now() < g_noticeUntil ? g_notice : std::string();
}

}  // namespace StandaloneState
