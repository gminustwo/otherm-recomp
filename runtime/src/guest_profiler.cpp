// OTHERM_PROFILE=<start s>:<length s> (Windows): a sampling profiler for the game thread. From
// <start> seconds after the first presented frame, for <length> seconds, the thread that presents
// frames is suspended about a thousand times a second and its instruction pointer recorded. At the
// end, samples are mapped back to the translated guest function whose code they fall in (by each
// translated function's host entry point) and the top entries are logged with their guest address,
// so a slow scene can be traced to the game code (or the runtime and aurora code) it spends time in.
//
// Stall watch (always on; OTHERM_STALL_WATCH=<ms> sets the length, 0 turns it off): a frame that takes longer than
// 250 ms is sampled the same way while it lasts, and where the thread spent it is logged when it ends ("stall ...
// ms"), so a freeze in someone's session comes with its cause. Between stalls it only reads a timestamp every 25 ms.
#include "guest_profiler.h"

#include "abi_bridge.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

namespace GuestProfiler {
namespace {

#ifdef _WIN32
struct Entry {
    uintptr_t host;
    uint32_t guest;
};

const char* GuestName(uint32_t address) {
    const uint32_t* begin = kGuestMapSymbolAddresses;
    const uint32_t* end = kGuestMapSymbolAddresses + kGuestMapSymbolCount;
    const uint32_t* it = std::lower_bound(begin, end, address);
    return it != end && *it == address ? kGuestMapSymbolNames[it - begin] : "";
}

// Every translated function's host entry point, sorted, to find which one a sample falls in. Built once.
const std::vector<Entry>& Entries() {
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> out;
        for (const auto& [lo, hi] : {std::pair{0x80004000u, 0x8020B3C0u}, std::pair{0x8052B364u, 0x80AFABE0u}}) {  // Other M main.dol text1, game.rel text (otherm_rel_layout.json)
            for (uint32_t address = lo; address < hi; address += 4) {
                if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                    info != nullptr && info->entryPoint != nullptr && info->address == address) {
                    out.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
                }
            }
        }
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
        return out;
    }();
    return entries;
}

// The engine module (engine_usa.dll / engine_pal.dll since the loader split), not the small loader exe.
struct ModuleRange {
    uintptr_t base = 0, end = 0;
    bool Contains(uintptr_t address) const { return address >= base && address < end; }
};
ModuleRange Engine() {
    static const ModuleRange range = [] {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&OnFramePresented), &self);
        const auto base = reinterpret_cast<uintptr_t>(self ? self : GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return ModuleRange{base, base + nt->OptionalHeader.SizeOfImage};
    }();
    return range;
}

std::string ModuleName(uintptr_t rip) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rip), &module)) {
        GetModuleFileNameA(module, path, MAX_PATH);
    }
    const char* slash = std::strrchr(path, '\\');
    return std::string("[") + (slash ? slash + 1 : path) + "]";
}

// A host address as a translated guest function, or exe+RVA (resolve with llvm-nm), or [dll].
std::string Describe(uintptr_t rip) {
    const ModuleRange engine = Engine();
    if (engine.Contains(rip)) {
        const std::vector<Entry>& entries = Entries();
        auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                   [](uintptr_t value, const Entry& e) { return value < e.host; });
        // Translated code sits in its own range: past the last entry point it is runtime code.
        if (it != entries.begin() && it != entries.end()) {
            --it;
            char text[160];
            std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
            return text;
        }
        char text[40];
        std::snprintf(text, sizeof(text), "exe+%llX", static_cast<unsigned long long>(rip - engine.base));
        return text;
    }
    return ModuleName(rip);
}

// The thread's call stack, by the unwind data every x64 function carries. No heap use while the thread is suspended:
// it may hold the heap lock (deadlock).
int CaptureStack(HANDLE thread, uintptr_t* frames, int maxFrames) {
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        return 0;
    }
    int depth = 0;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        for (; depth < maxFrames && context.Rip != 0; ++depth) {
            frames[depth] = static_cast<uintptr_t>(context.Rip);
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function == nullptr) {
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // a leaf: return address on top
                context.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                             &establisher, nullptr);
        }
    }
    ResumeThread(thread);
    return depth;
}

void Report(const std::unordered_map<uintptr_t, uint32_t>& samples, const std::vector<std::vector<uintptr_t>>& stacks,
            uint32_t total) {
    const std::vector<Entry>& entries = Entries();
    const ModuleRange engine = Engine();
    const uintptr_t exeBase = engine.base;
    const uintptr_t exeEnd = engine.end;
    // Raw stacks (exe RVAs, "-" for other modules) for offline symbolization: nm the exe, see AGENTS.md.
    if (FILE* raw = std::fopen("profile_stacks.txt", "w")) {
        for (const auto& stack : stacks) {
            for (const uintptr_t rip : stack) {
                if (rip >= exeBase && rip < exeEnd) {
                    std::fprintf(raw, "%llX ", static_cast<unsigned long long>(rip - exeBase));
                } else {
                    std::fputs("- ", raw);
                }
            }
            std::fputc('\n', raw);
        }
        std::fclose(raw);
    }
    // Where each sample's stack enters system code from ours: the first frames inside the exe.
    std::unordered_map<std::string, uint32_t> chains;
    for (const auto& stack : stacks) {
        std::string chain;
        int exeFrames = 0;
        for (const uintptr_t rip : stack) {
            if (rip >= exeBase && rip < exeEnd) {
                chain += (chain.empty() ? "" : " < ") + Describe(rip);
                if (++exeFrames == 5) break;
            } else if (exeFrames > 0) {
                break;
            }
        }
        if (stack.empty() || (stack[0] >= exeBase && stack[0] < exeEnd)) {
            continue;  // already in our code: the leaf table covers it
        }
        chains[Describe(stack[0]) + " from " + (chain.empty() ? "?" : chain)]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sortedChains(chains.begin(), chains.end());
    std::sort(sortedChains.begin(), sortedChains.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sortedChains.size() && i < 25; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile chain: %5.1f%%  %s\n", 100.0 * sortedChains[i].second / total,
                sortedChains[i].first.c_str());
    }

    std::unordered_map<std::string, uint32_t> byName;
    for (const auto& [rip, count] : samples) {
        std::string key;
        if (rip < exeBase || rip >= exeEnd) {
            key = ModuleName(rip);
        } else {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it == entries.begin()) {
                key = "[exe, before translated code]";
            } else {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                key = text;
            }
        }
        byName[key] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples, %zu translated functions\n", total, entries.size());
    for (size_t i = 0; i < sorted.size() && i < 60; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / total, sorted[i].first.c_str());
    }
}

void Run(HANDLE thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    std::unordered_map<uintptr_t, uint32_t> samples;
    std::vector<std::vector<uintptr_t>> stacks;
    uint32_t total = 0;
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (std::chrono::steady_clock::now() < end) {
        // About 1000 samples a second; Sleep() is too coarse for that, so wait by yielding.
        next += std::chrono::microseconds(1000);
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::yield();
        }
        uintptr_t frames[24];
        const int depth = CaptureStack(thread, frames, 24);
        if (depth > 0) {
            ++samples[frames[0]];
            ++total;
            stacks.emplace_back(frames, frames + depth);
        }
    }
    Report(samples, stacks, total);
}

// Stall watch: the time of the latest present, from the presenting thread.
std::atomic<int64_t> s_lastPresentNs{0};

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One stall's samples, logged as the stacks they share: each as its first frames (consecutive frames in one
// function or DLL once), the most common first. The names come from the nearest translated entry point below each
// address, which is wrong for runtime code linked between translated shards, so each group also logs one sample's
// raw stack ("e:" = RVA in the engine DLL) to resolve exactly with llvm-nm against that build's DLL.
void ReportStall(double stallMs, const uintptr_t (*frames)[32], const uint8_t* depths, size_t count) {
    struct Group {
        uint32_t samples = 0;
        size_t first = 0;
    };
    std::unordered_map<std::string, Group> stacks;
    for (size_t i = 0; i < count; ++i) {
        std::string text, last;
        int shown = 0;
        for (int f = 0; f < depths[i] && shown < 9; ++f) {
            std::string name = Describe(frames[i][f]);
            if (name == last) continue;
            text += (text.empty() ? "" : " < ") + name;
            last = std::move(name);
            ++shown;
        }
        Group& group = stacks[text.empty() ? "?" : text];
        if (group.samples++ == 0) group.first = i;
    }
    std::vector<std::pair<std::string, Group>> sorted(stacks.begin(), stacks.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.samples > b.second.samples; });
    const ModuleRange engine = Engine();
    // The time of day, to line a stall up with what else happened on the PC then (device changes, other programs).
    SYSTEMTIME now{};
    GetLocalTime(&now);
    RT_LOGF(RT_TAG_RUNTIME, "stall %.1f ms (ended %02u:%02u:%02u.%03u): %zu samples of the game thread\n", stallMs,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, count);
    for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "stall   %5.1f%%  %s\n", 100.0 * sorted[i].second.samples / std::max<size_t>(count, 1),
                sorted[i].first.c_str());
        std::string raw, lastModule;
        const size_t s = sorted[i].second.first;
        for (int f = 0; f < depths[s] && f < 20; ++f) {
            if (engine.Contains(frames[s][f])) {
                char text[24];
                std::snprintf(text, sizeof(text), " e:%llX", static_cast<unsigned long long>(frames[s][f] - engine.base));
                raw += text;
                lastModule.clear();
            } else if (std::string module = ModuleName(frames[s][f]); module != lastModule) {
                raw += " " + module;
                lastModule = std::move(module);
            }
        }
        RT_LOGF(RT_TAG_RUNTIME, "stall           at%s\n", raw.c_str());
    }
}

void WatchStalls(HANDLE thread, int64_t stallNs) {
    constexpr size_t kMaxSamples = 2000;
    // Static: no heap while the game thread is suspended, and ~0.5 MB is too much for a thread stack.
    static uintptr_t frames[kMaxSamples][32];
    static uint8_t depths[kMaxSamples];
    int64_t windowStart = 0;
    int reported = 0;
    for (;;) {
        Sleep(25);
        const int64_t last = s_lastPresentNs.load(std::memory_order_acquire);
        if (last == 0 || NowNs() - last < stallNs) {
            continue;
        }
        // Sampled every millisecond or so (the timer resolution decides) until the next present.
        size_t count = 0;
        while (s_lastPresentNs.load(std::memory_order_acquire) == last) {
            if (count < kMaxSamples) {
                depths[count] = static_cast<uint8_t>(CaptureStack(thread, frames[count], 32));
                if (depths[count] > 0) ++count;
            }
            Sleep(1);
        }
        const double stallMs = (s_lastPresentNs.load(std::memory_order_acquire) - last) / 1e6;
        // At most 10 a minute, like the slow-frame line's cap, so a scene that stalls every frame does not flood the log.
        const int64_t now = NowNs();
        if (now - windowStart > 60'000'000'000LL) {
            windowStart = now;
            reported = 0;
        }
        if (reported++ < 10) {
            ReportStall(stallMs, frames, depths, count);
        }
    }
}
#endif

} // namespace

void OnFramePresented() {
#ifdef _WIN32
    s_lastPresentNs.store(NowNs(), std::memory_order_release);
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const auto openThisThread = [] {
        HANDLE thread = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        return thread;
    };
    // OTHERM_STALL_WATCH=<ms>: the stall length to sample (default 250), 0 = off.
    const char* watch = std::getenv("OTHERM_STALL_WATCH");
    if (const long stallMs = watch != nullptr ? std::strtol(watch, nullptr, 10) : 250; stallMs > 0) {
        if (HANDLE thread = openThisThread()) std::thread(WatchStalls, thread, int64_t{stallMs} * 1'000'000).detach();
    }
    const char* value = std::getenv("OTHERM_PROFILE");
    // Also read "<start>:<length>" from profile.txt beside the executable (the test loop's switch).
    static char fileValue[64] = {};
    if (value == nullptr) {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        path = path.substr(0, path.find_last_of("\\/") + 1) + "profile.txt";
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            if (std::fgets(fileValue, sizeof(fileValue), f)) value = fileValue;
            std::fclose(f);
        }
    }
    double start = 0, length = 0;
    if (value == nullptr || std::sscanf(value, "%lf:%lf", &start, &length) != 2 || length <= 0) {
        return;
    }
    if (HANDLE thread = openThisThread()) std::thread(Run, thread, start, length).detach();
#endif
}

} // namespace GuestProfiler
