#include <vector>
#include "frame_input.h"
#include "det_hash.h"
#include "timebase_contract.h"
#include "det_clock.h"
#include "guest_clock.h"
#include "hle_stubs.h"
#include "runtime_config.h"
#include "memory.h"
#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "settings_overlay.h"
#include "discord_presence.h"
#include "gpu_load.h"
#include "fiber_manager.h"
#include "platform/host_platform.h"
#include "runtime_log.h"
#include "guest_profiler.h"
#include <unordered_map>
#include "recomp_mod_loader.h"
#include "mod_runtime.h"
#include "otherm_cheats.h"
#include "save_state.h"

void OthermMovieFrame();  // hle/otherm_movie.cpp: a movie no longer updated gives the ultrawide bars back
void OthermCutsceneFrame();  // hle/otherm_cutscene.cpp: in-engine cutscenes (the game's own skip, logging)
void OthermAreaFrame();      // hle/otherm_area.cpp: where the player is ([area] lines, Discord's status)
#include "standalone_save_state.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <aurora/gfx.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <dxgi.h>
#endif

#include <aurora/aurora.h>
#include <SDL3/SDL_video.h>

// Forward declaration for OSWakeupThread - used to wake threads on VI retrace queue
extern "C" void OSWakeupThread_HLE_801139d0(CpuContext* ctx);
uint64_t PPC_ReadGuestTimeBase();          // ppc_helpers.cpp
void PPC_SetGuestTimeBase(uint64_t value);  // ppc_helpers.cpp

// Forward declaration for OSSleepThread - used by VIWaitForRetrace HLE
extern "C" void OSSleepThread_HLE_801138e0(CpuContext* cpu);
extern "C" int g_gxFrameCount;
extern "C" int32_t OS__DisableInterrupts_8010f160();
extern "C" int32_t OS__RestoreInterrupts_8010f1a0(int32_t level);

// Aurora frame cycle tracking - needs external linkage for the GX HLE
// (declared in gx_internal.h, consumed by gx_frame.cpp). We need to call
// aurora_begin_frame() before GX commands and aurora_end_frame() after.
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGuestGxRenderModeCopyBytes = 0x39;

bool ReadGuestRenderModeObj(uint32_t guestPtr, GXRenderModeObj& out) {
    if (guestPtr == 0 || !Memory::Contains(guestPtr, kGuestGxRenderModeCopyBytes)) {
        return false;
    }

    try {
        out.viTVmode = static_cast<VITVMode>(Memory::Read32(guestPtr + 0x00));
        out.fbWidth = Memory::Read16(guestPtr + 0x04);
        out.efbHeight = Memory::Read16(guestPtr + 0x06);
        out.xfbHeight = Memory::Read16(guestPtr + 0x08);
        out.viXOrigin = Memory::Read16(guestPtr + 0x0a);
        out.viYOrigin = Memory::Read16(guestPtr + 0x0c);
        out.viWidth = Memory::Read16(guestPtr + 0x0e);
        out.viHeight = Memory::Read16(guestPtr + 0x10);
        out.xFBmode = static_cast<VIXFBMode>(Memory::Read32(guestPtr + 0x14));
        out.field_rendering = Memory::Read8(guestPtr + 0x18);
        out.aa = Memory::Read8(guestPtr + 0x19);

        for (size_t i = 0; i < 12; ++i) {
            out.sample_pattern[i][0] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2));
            out.sample_pattern[i][1] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2) + 1);
        }
        for (size_t i = 0; i < 7; ++i) {
            out.vfilter[i] = Memory::Read8(guestPtr + 0x32 + static_cast<uint32_t>(i));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    return true;
}

struct ViState {
    bool initialized = false;
    uint32_t tvFormat = 0; // VIGetTvFormat returns VI_NTSC before VIInit
    uint32_t nextFrameBuffer = 0;
    uint32_t currentFrameBuffer = 0;
    uint32_t retraceCount = 0;
    bool black = false;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t renderWidth = 640;
    uint32_t renderHeight = 480;
    uint32_t viXOrigin = 0;
    uint32_t viYOrigin = 0;
    uint32_t xfbWidth = 640;
    uint32_t xfbHeight = 480;
    bool fieldOdd = false;
    Clock::time_point lastRetrace = Clock::now();
    std::chrono::microseconds retraceInterval{16666us}; // ~60 Hz
    // Deterministic mode: the virtual tick of the last retrace (see det_clock.h); lastRetrace above
    // keeps real time for presentation only.
    uint64_t detLastRetraceTicks = 0;
    bool hasValidXfb = false; // True once we've received at least one GXCopyDisp
    uint32_t readyXfb = 0;    // XFB address from the most recent GXCopyDisp

    // VIConfigure/VISetNextFrameBuffer/VISetBlack only write pending values below; VIFlush arms them but
    // the commit happens at the next retrace, matching real VI hardware. A VIFlush called from a
    // pre-retrace callback therefore misses the imminent field and lands one field late.

    // Pending values (written by VISetNextFrameBuffer, VISetBlack, VIConfigure)
    uint32_t pendingNextFrameBuffer = 0;
    bool pendingBlack = false;
    // Matches the active-state default (NTSC before VIInit) and the pending
    // 16666us interval below; a flush before any VIConfigure must not commit
    // PAL timing onto an NTSC-interval state.
    uint32_t pendingTvFormat = 0;
    uint32_t pendingRenderWidth = 640;
    uint32_t pendingRenderHeight = 480;
    uint32_t pendingViXOrigin = 0;
    uint32_t pendingViYOrigin = 0;
    uint32_t pendingXfbWidth = 640;
    uint32_t pendingXfbHeight = 480;
    std::chrono::microseconds pendingRetraceInterval{16666us};
    // V-Sync: the interval the display lock wants for the next retrace (0 = the game's own interval).
    std::chrono::nanoseconds lockedInterval{0};
    
    // Set by VIFlush(); cleared after commit in AdvanceRetrace
    bool flushArmed = false;
};

std::mutex g_viMutex;
ViState g_vi;


// Guest-side state addresses used by the SDK's VI globals.
constexpr uint32_t kViInitializedFlagAddr   = 0x80332838;  // IsInitialized
constexpr uint32_t kViTvFormatAddr          = 0x803328a8;  // CurrTvMode
constexpr uint32_t kViRenderWidthAddr       = 0x802ec73c;  // HorVer.DispSizeX
constexpr uint32_t kViRenderHeightAddr      = 0x802ec73e;  // HorVer.DispSizeY
constexpr uint32_t kViXfbWidthAddr          = 0x802ec74a;  // HorVer.FBSizeX
constexpr uint32_t kViXfbHeightAddr         = 0x802ec754;  // HorVer+0x1C (PanSizeY)
constexpr uint32_t kViRetraceCountAddr      = 0x803328e4;  // retraceCount; // matches VIWaitForRetrace/handler
constexpr uint32_t kViTimingGuardAddr       = 0x80332844;  // IsInitialized+0xC = __VIDimming_All_Clear (name unverified for Other M)
constexpr uint32_t kViPreRetraceCallback    = 0x803328b8;  // PreCB
constexpr uint32_t kViPostRetraceCallback   = 0x803328b4;  // PostCB
constexpr uint32_t kViNextFrameBufferAddr   = 0x803328a0;  // CurrBufAddr in the SDK layout
constexpr uint32_t kViNextFrameBufferHwAddr = 0x802ec768;  // HorVer.bufAddr
constexpr uint32_t kViRetraceQueueAddr      = 0x803328c0;  // retraceQueue; // Thread queue for VIWaitForRetrace


// ============================================================================
// V-Sync display lock (Windows)
// ============================================================================
// With V-Sync on, presents wait for vblank. A game clock of exactly 60.000 Hz against a display that
// really runs at 59.94 (or 179.96 / 3) drops or repeats a frame every few seconds, and a full flip
// queue makes Present block. So the VI clock runs at exactly k display periods per retrace (k = 1 on a
// 60 Hz display, 3 at 180 Hz), with its phase pulled gently toward the middle of a vblank interval so
// each frame lands on the same vblank every time. The compositor reports the display's exact period
// and the time of its last vblank. Displays with no whole fraction near 60 (75, 144 Hz) keep the
// game's own clock.

struct DisplayLock {
    bool valid = false;
    int k = 0;
    std::chrono::nanoseconds period{0};
    Clock::time_point vblank{};
    bool phaseKnown = false;  // vblank is a real vblank time (DWM's period agreed with the display mode)
    Clock::time_point lastQuery{};
    bool logged = false;
};
DisplayLock g_displayLock;  // under g_viMutex

#if defined(_WIN32)
// The game window's own monitor, timed by DXGI WaitForVBlank on a helper thread: its real vblank times and period.
// DWM's timing is one clock for every display (wrong for a 240 Hz screen next to a 60 Hz one), and without a
// phase the frames land wherever the clock started, sometimes right on a vblank edge, where they slip back and forth.
struct VBlankClock {
    std::atomic<int64_t> lastNs{0};    // steady-clock time of the latest vblank on the grid
    std::atomic<int64_t> periodNs{0};  // the display mode's period, set by RefreshDisplayLockLocked
    std::atomic<HMONITOR> monitor{nullptr};
    std::once_flag started;
};
VBlankClock g_vblankClock;

void VBlankThread() {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
    const auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory1")))
                             : nullptr;
    if (!create) return;
    static const GUID kIidFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    IDXGIFactory1* factory = nullptr;
    if (FAILED(create(kIidFactory1, reinterpret_cast<void**>(&factory))) || !factory) return;
    IDXGIOutput* output = nullptr;
    HMONITOR outputMonitor = nullptr;
    int64_t previous = 0;
    for (;;) {
        const HMONITOR want = g_vblankClock.monitor.load();
        if (want != outputMonitor) {  // the window moved to another monitor: find its output
            if (output) output->Release();
            output = nullptr;
            outputMonitor = want;
            previous = 0;
            g_vblankClock.lastNs = 0;
            IDXGIAdapter1* adapter = nullptr;
            for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
                IDXGIOutput* candidate = nullptr;
                for (UINT o = 0; !output && adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o) {
                    DXGI_OUTPUT_DESC desc{};
                    if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == want) output = candidate;
                    else candidate->Release();
                }
                adapter->Release();
            }
        }
        if (!output || FAILED(output->WaitForVBlank())) {
            std::this_thread::sleep_for(250ms);
            continue;
        }
        // The rate comes from the display mode (exact); these wakeups only give the phase. They jitter and now and
        // then return early (an average of their spacing read 244 Hz on a 240.3 Hz screen), so a stamp counts only
        // when it lies a whole number of periods, within 5%, after the last one that counted.
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        const int64_t period = g_vblankClock.periodNs.load();
        if (period <= 0) continue;
        const int64_t delta = now - previous;
        const int64_t periods = (delta + period / 2) / period;
        const bool onGrid = previous != 0 && periods >= 1 && std::llabs(delta - periods * period) < period / 20;
        static int misses = 0;
        if (onGrid || ++misses >= 8 || previous == 0) {  // 8 off-grid in a row: the grid itself moved
            misses = 0;
            previous = now;
            if (onGrid) g_vblankClock.lastNs = now;
        }
    }
}
#endif

void RefreshDisplayLockLocked(Clock::time_point now) {
    DisplayLock& d = g_displayLock;
    if (!settings_overlay::VSync()) {
        d.valid = false;
        d.logged = false;
        return;
    }
    if (d.lastQuery != Clock::time_point{} && now - d.lastQuery < 250ms) {
        return;
    }
    d.lastQuery = now;
#if defined(_WIN32)
    using GetTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
    static const GetTimingFn getTiming = [] {
        HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        return dwm != nullptr ? reinterpret_cast<GetTimingFn>(
                                    reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmGetCompositionTimingInfo")))
                              : nullptr;
    }();
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    LARGE_INTEGER frequency{}, qpcNow{};
    const HRESULT result = getTiming != nullptr ? getTiming(nullptr, &info) : E_NOTIMPL;
    const bool dwmOk = SUCCEEDED(result) && info.qpcRefreshPeriod != 0 && ::QueryPerformanceFrequency(&frequency) &&
                       ::QueryPerformanceCounter(&qpcNow);
    // The game window's display mode: DWM's timing is one clock for all displays and was 288-468 Hz on a
    // 240 Hz screen next to a 60 Hz one, which left the lock off ("120 FPS needs to truly work").
    double modeHz = 0.0;
    int windowCount = 0;
    if (SDL_Window** windows = SDL_GetWindows(&windowCount)) {
        if (windowCount > 0) {
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(windows[0])))
                modeHz = mode->refresh_rate;
            if (auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]),
                                                                      SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
                g_vblankClock.monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                std::call_once(g_vblankClock.started, [] { std::thread(VBlankThread).detach(); });
            }
        }
        SDL_free(windows);
    }
    const auto steadyNow = Clock::now();
    const double nsPerTick = dwmOk ? 1e9 / static_cast<double>(frequency.QuadPart) : 0.0;
    const double dwmHz = dwmOk ? 1e9 / (info.qpcRefreshPeriod * nsPerTick) : 0.0;
    // The rate: the window's display mode (exact; DWM read 240.33 and 258.7 Hz for a 240.30 Hz screen). DWM's
    // vblank phase is used only while its rate agrees closely.
    d.phaseKnown = dwmOk && (modeHz <= 1.0 || std::fabs(dwmHz - modeHz) < modeHz * 0.005);
    const double hz = modeHz > 1.0 ? modeHz : dwmHz;
    if (hz > 1.0) g_vblankClock.periodNs = static_cast<int64_t>(1e9 / hz);
    // The phase: the window's own monitor's vblanks (VBlankThread), when it has seen one lately.
    const int64_t vblankLast = g_vblankClock.lastNs.load();
    const int64_t steadyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(steadyNow.time_since_epoch()).count();
    const bool measured = vblankLast != 0 && steadyNs - vblankLast < 200'000'000;
    if (hz <= 1.0) {
        if (!d.logged) {
            d.logged = true;
            RT_LOGF(RT_TAG_VI, "V-Sync: no display timing (0x%08lX); the game keeps its own clock\n",
                    static_cast<unsigned long>(result));
        }
        d.valid = false;
        return;
    }
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / hz));
    const int k = static_cast<int>(std::lround(hz / 60.0));
    d.valid = k >= 1 && k <= 8 && std::fabs(hz / k - 60.0) < 60.0 * 0.02;
    d.k = k;
    d.period = period;
    if (measured) {
        d.phaseKnown = true;
        d.vblank = Clock::time_point(std::chrono::nanoseconds(vblankLast));
    } else if (d.phaseKnown) {
        d.vblank = steadyNow - std::chrono::nanoseconds(static_cast<int64_t>(
                                   static_cast<double>(qpcNow.QuadPart - static_cast<LONGLONG>(info.qpcVBlank)) * nsPerTick));
    }
    static bool loggedMeasured = false;
    if (measured && !loggedMeasured) {
        loggedMeasured = true;
        RT_LOGF(RT_TAG_VI, "V-Sync: frames are timed to the vblanks of the window's monitor (%.3f Hz, %d per frame)\n",
                hz, k);
    }
    if (!d.logged) {
        d.logged = true;
        RT_LOGF(RT_TAG_VI, "V-Sync: display %.3f Hz (DWM %.3f Hz, mode %.3f Hz%s), %s\n", hz, dwmHz, modeHz,
                d.phaseKnown ? "" : ", no vblank phase",
                d.valid ? (std::string("game clock locked at ") + std::to_string(k) + " refresh(es) per frame").c_str()
                        : "no whole fraction near 60 Hz; the game keeps its own clock");
    }
#else
    d.valid = false;
#endif
}

// The interval to the retrace after `retrace`: k display periods, nudged toward the target phase.
std::chrono::nanoseconds LockedIntervalAfter(Clock::time_point retrace) {
    const DisplayLock& d = g_displayLock;
    if (!d.valid || d.period.count() <= 0) {
        return std::chrono::nanoseconds(0);
    }
    const int64_t period = d.period.count();
    if (!d.phaseKnown) return std::chrono::nanoseconds(period * d.k);  // the rate alone, no phase pull
    // Presents go out about 6.5 ms after their retrace (aurora's kNativePresentOffsetNanos): put that
    // moment halfway between two vblanks, the most room either way. Above 60 FPS the inserted frames go out
    // on the retrace itself and every display period after it, so the retrace goes halfway instead: with the
    // 6.5 ms offset they landed 0.25 ms before a 240 Hz vblank and slipped to the next one now and then.
    const int64_t presentOffset = aurora_get_frame_interpolation_fps() > 60 ? 0 : 6'500'000;
    int64_t target = (period / 2 - presentOffset) % period;
    if (target < 0) target += period;
    int64_t phase = (std::chrono::duration_cast<std::chrono::nanoseconds>(retrace - d.vblank).count()) % period;
    if (phase < 0) phase += period;
    int64_t error = phase - target;
    if (error > period / 2) error -= period;
    if (error < -period / 2) error += period;
    const int64_t correction = std::clamp<int64_t>(-error / 20, -100'000, 100'000);
    return std::chrono::nanoseconds(period * d.k + correction);
}

std::chrono::microseconds IntervalForFormat(uint32_t tvFormat) {
    // NTSC-ish defaults to 60 Hz; PAL uses 50 Hz.
    return tvFormat == 1 ? 20000us : 16666us;
}

// Shared busy-wait budget for deadline-precise sleeps (matches Aurora's
// presenter spin window). Larger windows burn a core for no visible gain.
constexpr std::chrono::microseconds kFinalSpinWindow{500};

void SleepPreciselyUntil(Clock::time_point deadline, bool finishWithSpin = false,
                         std::chrono::microseconds spinWindow = 750us) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return;
    }
    const auto timerDeadline =
        finishWithSpin && deadline - now > spinWindow ? deadline - spinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static thread_local HighResolutionTimer timer;
    if (timer.handle != nullptr) {
        const auto remaining100ns =
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
            if (!finishWithSpin) {
                return;
            }
        }
    }
#endif
    if (!finishWithSpin) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    if (Clock::now() < timerDeadline) {
        std::this_thread::sleep_until(timerDeadline);
    }
    // This runs only for the final fraction of a VI interval. Do not yield the
    // host thread here: a scheduler quantum is larger than the remaining
    // budget and would recreate the 17-18 ms sawtooth this path removes.
    // The pause hint keeps a spin on one SMT thread from starving its sibling core thread (a Steam Deck held the
    // game at 30 FPS until a CPU thread was toggled); Linux had a bare spin.
    while (Clock::now() < deadline) {
#if defined(_WIN32)
        YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
}

void ViSetR3(CpuContext* ctx, uint32_t value)
{
    if (ctx) {
        ctx->gpr[3] = value;
    }
}

void WriteGuestStateLocked() {
    try {
        Memory::Write8(kViInitializedFlagAddr, 1);
        Memory::Write8(kViTimingGuardAddr, 1);
        Memory::Write32(kViTvFormatAddr, g_vi.tvFormat);
        Memory::Write16(kViRenderWidthAddr, static_cast<uint16_t>(g_vi.renderWidth));
        Memory::Write16(kViRenderHeightAddr, static_cast<uint16_t>(g_vi.renderHeight));
        Memory::Write16(kViXfbWidthAddr, static_cast<uint16_t>(g_vi.xfbWidth));
        Memory::Write16(kViXfbHeightAddr, static_cast<uint16_t>(g_vi.xfbHeight));
        Memory::Write32(kViRetraceCountAddr, g_vi.retraceCount);
        Memory::Write32(kViPreRetraceCallback, g_vi.preRetraceCallback);
        Memory::Write32(kViPostRetraceCallback, g_vi.postRetraceCallback);
        // Write PENDING frame buffer to guest memory so SDK code sees the queued value
        Memory::Write32(kViNextFrameBufferAddr, g_vi.pendingNextFrameBuffer);
        Memory::Write32(kViNextFrameBufferHwAddr, g_vi.pendingNextFrameBuffer);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "WriteGuestStateLocked", e);
    }
}

// Fast-forward: the game's clock runs this many times faster (1 = normal).
std::atomic<uint32_t> g_viSpeed{1};
// g_vi.retraceCount for readers that must not take the VI lock (VI_HLE_RetraceCount).
std::atomic<uint32_t> g_retraceCountMirror{0};
// Set while the guest waits for the next retrace (the scheduler's idle loop, a paced present): during fast-forward
// only these deliver a retrace at the fast pace (guest_clock.h).
thread_local bool t_idleRetracePump = false;
struct IdleRetracePump {
    bool previous = t_idleRetracePump;
    IdleRetracePump() { t_idleRetracePump = true; }
    ~IdleRetracePump() { t_idleRetracePump = previous; }
};

// The interval to the next retrace: the display lock's while V-Sync holds one (NTSC only).
std::chrono::nanoseconds ActiveIntervalLocked() {
    if (const uint32_t speed = g_viSpeed.load(std::memory_order_relaxed); speed > 1) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval) / speed;
    }
    if (g_vi.lockedInterval.count() > 0 && g_vi.tvFormat != 1) {
        return g_vi.lockedInterval;
    }
    return g_vi.retraceInterval;
}

// Fast-forward pacing (guest_clock.h) runs on the whole timeline, as Dolphin's throttle does, not retrace by retrace:
// retrace n is due `speed` times sooner than at 1x counted from where fast-forward (or its last restart) began, so the
// time a busy frame ran over is made up by the waits after it instead of every wait being a full fast interval. More
// than 100 ms behind (a load, a slow scene) and the pace starts over from now rather than racing to catch up.
Clock::time_point g_ffPaceStart{};
uint32_t g_ffPaceRetrace = 0;
constexpr auto kFastForwardMaxLag = std::chrono::milliseconds(100);

void RestartFastForwardPaceLocked(Clock::time_point now) {
    g_ffPaceStart = now;
    g_ffPaceRetrace = g_vi.retraceCount;
}

// When the guest, waiting for it, gets the next retrace: at the normal pace, one interval after the last; during
// fast-forward, the timeline pace above.
Clock::time_point NextIdleRetraceLocked(Clock::time_point now) {
    if (g_viSpeed.load(std::memory_order_relaxed) <= 1 || !GuestClock::FastForwarding()) {
        return g_vi.lastRetrace + ActiveIntervalLocked();
    }
    const uint32_t ahead = g_vi.retraceCount + 1u - g_ffPaceRetrace;  // wraps when a state load set the count back
    const auto due = g_ffPaceStart + ActiveIntervalLocked() * static_cast<int64_t>(ahead);
    if (now - due > kFastForwardMaxLag) {
        RestartFastForwardPaceLocked(now);
        return now;
    }
    // More than a 1x interval away means the count jumped (a state load): one fast interval from now.
    if (ahead > 1'000'000u || due - now > std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval)) {
        RestartFastForwardPaceLocked(now);
        return now + ActiveIntervalLocked();
    }
    return due;
}

uint64_t IntervalTicks(std::chrono::microseconds interval) {
    return TimeBaseContract::NanosecondsToTicks(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count()));
}

void EnsureInitializedLocked() {
    if (g_vi.initialized) {
        return;
    }
    g_vi.initialized = true;
    g_vi.retraceInterval = IntervalForFormat(g_vi.tvFormat);
    g_vi.lastRetrace = Clock::now();
    g_vi.detLastRetraceTicks = DetClock::Now();
    WriteGuestStateLocked();
}

// GXRenderModeObj::viTVmode encodes the output family (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60) in bits [4:2].
uint32_t ExtractTvFormat(uint32_t tvMode) {
    return (tvMode >> 2) & 0x7;
}

// Re-entry guard to prevent AdvanceRetrace calling itself via OSWakeupThread -> SelectThread
static std::atomic<bool> s_inAdvanceRetrace{false};

// Set while VI_HLE_PresentFrame runs its seal/pace/pre-warm sequence. Guest callbacks
// serviced during that window (pace-loop alarms, GX timing polls) still see the stale
// hasValidXfb/g_auroraFrameActive flags; a retrace-context present fired from them would
// end the freshly pre-warmed empty frame and show it as a black frame group.
static std::atomic<bool> s_presentSequenceActive{false};

// Bring-up aid: frames the guest actually handed to the presenter (an XFB was ready), and a one-shot
// watchdog that dumps every guest thread's state if none arrived after 12 s and again after 30 s, so
// a boot that stalls in a wait loop shows where each thread is parked.
static std::atomic<uint32_t> s_presentedXfbFrames{0};
extern "C" { extern int g_othermDrawDump; extern int g_othermCullOff; extern unsigned g_othermDrawIdx; extern unsigned g_othermDrawLimit; }  // aurora command_processor (bring-up diagnostics)
static uint32_t OthermPresentedFramesLocal() { return s_presentedXfbFrames.load(std::memory_order_relaxed); }
static uint32_t (*const g_othermFramesFn)() = &OthermPresentedFramesLocal;

void StartStallWatchdogOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Unattended bring-up: autoexit.txt beside the executable holds a run length in seconds.
        if (const auto dir = RuntimeConfigFile::ExecutableDirectory()) {
            std::ifstream in(*dir / "autoexit.txt");
            int seconds = 0;
            if (in >> seconds && seconds > 0) {
                std::thread([seconds] {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    std::cerr << "[runtime] autoexit after " << seconds << " s" << std::endl;
                    std::_Exit(0);
                }).detach();
            }
        }
        std::thread([] {
            const auto start = std::chrono::steady_clock::now();
            // OTHERM_WATCHDOG_AT=<s>: one more thread dump then (e.g. to see where a hang sits)
            const char* extra = std::getenv("OTHERM_WATCHDOG_AT");
            const int extraSeconds = extra ? std::atoi(extra) : 0;
            for (const int seconds : {12, 30, extraSeconds}) {
                if (seconds == 0) break;
                std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
                const uint32_t frames = s_presentedXfbFrames.load(std::memory_order_relaxed);
                const bool black = g_vi.black;  // racy read; diagnostics only
                RT_LOGF(RT_TAG_VI, "[watchdog] %d s: %u guest frame(s) presented, VI black=%d\n", seconds, frames,
                        black ? 1 : 0);
                // The dump holds the game ~110 ms, so only when nothing shows or OTHERM_WATCHDOG_AT asked for it.
                // Dumping at 30 s every time was a hitch in every session (NOTES.md 2026-11-18).
                if (frames != 0 && !black && seconds != extraSeconds) {
                    continue;
                }
                RT_LOGF(RT_TAG_VI, "[watchdog] nothing visible yet; guest threads:\n");
                Fiber::GuestFiberManager::DumpGuestThreads(std::cerr);
            }
        }).detach();
    });
}

// Other M bring-up: OTHERM_SAMPLE=<start frame>:<frames> samples the active translated function every 100 us on a
// host thread between those presented-frame counts and prints the 48 most frequent addresses (resolve them with
// project/MAP.txt): what the game's logic actually runs while nothing is drawn.
void StartGuestSamplerOnce() {
    static bool started = false;
    if (started) return;
    started = true;
    const char* spec = std::getenv("OTHERM_SAMPLE");
    unsigned start = 0, frames = 0;
    if (spec == nullptr || std::sscanf(spec, "%u:%u", &start, &frames) != 2 || frames == 0) return;
    std::thread([start, frames] {
        while (s_presentedXfbFrames.load(std::memory_order_relaxed) < start) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::unordered_map<uint32_t, uint32_t> hist;
        uint64_t samples = 0;
        while (s_presentedXfbFrames.load(std::memory_order_relaxed) < start + frames) {
            ++hist[RecompMod::CurrentTranslatedExecutionAddress()];
            ++samples;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        std::vector<std::pair<uint32_t, uint32_t>> sorted(hist.begin(), hist.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        RT_LOGF(RT_TAG_VI, "[sample] frames %u..%u: %llu samples, %zu distinct functions\n", start, start + frames,
                static_cast<unsigned long long>(samples), sorted.size());
        for (size_t i = 0; i < sorted.size() && i < 48; ++i)
            RT_LOGF(RT_TAG_VI, "[sample] %6.2f%% 0x%08X\n", 100.0 * sorted[i].second / static_cast<double>(samples), sorted[i].first);
    }).detach();
}

// Other M bring-up: OTHERM_FIND=<frame>:<hex word>[,<frame>:<hex word>...] scans MEM1 and MEM2 once at that presented
// frame for aligned words equal to the value (a vtable pointer finds the live objects of a class) and logs up to
// 32 addresses each.
void LogGuestWordSearch() {
    struct Find { uint32_t frame; uint32_t value; bool done; };
    static std::vector<Find> s_finds = [] {
        std::vector<Find> finds;
        if (const char* list = std::getenv("OTHERM_FIND")) {
            std::string text(list);
            size_t start = 0;
            while (start < text.size()) {
                const size_t comma = text.find(',', start);
                const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    finds.push_back({static_cast<uint32_t>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 10)),
                                     static_cast<uint32_t>(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 16)), false});
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        return finds;
    }();
    const uint32_t frame = s_presentedXfbFrames.load(std::memory_order_relaxed);
    for (Find& f : s_finds) {
        if (f.done || frame < f.frame) continue;
        f.done = true;
        std::string hits;
        uint32_t count = 0;
        for (const auto& [lo, hi] : {std::pair{0x80000000u, 0x81800000u}, std::pair{0x90000000u, 0x94000000u}}) {
            for (uint32_t address = lo; address < hi; address += 4) {
                uint32_t word = 0;
                if (!Memory::TryRead32(address, word) || word != f.value) continue;
                if (++count <= 32) {
                    char text[16];
                    std::snprintf(text, sizeof(text), " %08X", address);
                    hits += text;
                }
            }
        }
        RT_LOGF(RT_TAG_VI, "[find] frame %u: 0x%08X at %u place(s):%s\n", frame, f.value, count, hits.c_str());
    }
}

// Other M bring-up: OTHERM_THREADS=<frames> logs every guest thread each <frames> presented frames: its entry
// point, fiber state, guest priority, the wait it is parked in (OSReceiveMessage / OSSleepThread /
// VIWaitForRetrace, with the queue and the caller), and the guest back chain from the wait's stack pointer, so a
// stalled game shows who waits on what. The host profiler cannot: it names HLE code after the nearest translated
// function.
void LogGuestThreads() {
    static const uint32_t interval = [] {
        const char* v = std::getenv("OTHERM_THREADS");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    const uint32_t frame = s_presentedXfbFrames.load(std::memory_order_relaxed);
    static uint32_t s_lastFrame = ~0u;
    if (interval == 0 || frame % interval != 0 || frame == s_lastFrame) return;
    s_lastFrame = frame;
    struct Ctx { uint32_t frame; };
    Ctx c{frame};
    const bool ok = Fiber::GuestFiberManager::ForEachFiber(
        [](uint32_t thread, const Fiber::GuestFiber& fiber, bool current, void* user) {
            const uint32_t f = static_cast<Ctx*>(user)->frame;
            uint32_t prioWord = 0, stateWord = 0, queue = 0;
            Memory::TryRead32(thread + 0x2C8u, stateWord);  // OSThread.state (u16) + attr (u16)
            Memory::TryRead32(thread + 0x2D0u, prioWord);   // OSThread.priority
            Memory::TryRead32(thread + 0x2DCu, queue);      // OSThread.queue (the thread queue it sleeps on)
            std::string line;
            char text[200];
            std::snprintf(text, sizeof(text), "[threads] frame %u: thread %08X entry %08X fiber %u%s%s guest state %u prio %d sleepq %08X",
                          f, thread, fiber.entryPoint, static_cast<unsigned>(fiber.state), current ? " current" : "",
                          fiber.terminated ? " terminated" : "", stateWord >> 16, static_cast<int32_t>(prioWord), queue);
            line = text;
            if (const SaveState::YieldRecord* wait = SaveState::LiveWait(thread)) {
                static const char* kKinds[] = {"none", "VIWaitForRetrace", "OSReceiveMessage", "OSSleepThread", "OSLockMutex",
                                               "OSSuspendThread"};
                const uint32_t kind = static_cast<uint32_t>(wait->kind);
                std::snprintf(text, sizeof(text), " | in %s arg %08X caller %08X", kind < 6 ? kKinds[kind] : "?",
                              wait->extra[0], wait->entry.lr);
                line += text;
                if (wait->kind == SaveState::YieldKind::MessageQueue) {
                    uint32_t count = 0, used = 0;  // OSMessageQueue: msgCount +0x14, firstIndex +0x18, usedCount +0x1C
                    Memory::TryRead32(wait->extra[0] + 0x14u, count);
                    Memory::TryRead32(wait->extra[0] + 0x1Cu, used);
                    std::snprintf(text, sizeof(text), " (queue %u/%u)", used, count);
                    line += text;
                }
                line += " chain";
                uint32_t sp = wait->entry.gpr[1];
                for (int depth = 0; depth < 12 && sp != 0 && sp != 0xFFFFFFFFu; ++depth) {
                    uint32_t next = 0, lr = 0;
                    if (!Memory::TryRead32(sp, next) || next <= sp || !Memory::TryRead32(next + 4u, lr)) break;
                    std::snprintf(text, sizeof(text), " %08X", lr);
                    line += text;
                    sp = next;
                }
            }
            RT_LOGF(RT_TAG_VI, "%s\n", line.c_str());
        },
        &c);
    if (!ok) RT_LOGF(RT_TAG_VI, "[threads] frame %u: fiber table busy\n", frame);
}

// Other M bring-up: OTHERM_WATCH=80332AC0,803324F0,... logs each 32-bit guest word when it changes, with the
// retrace count, so a stuck state machine shows which global it waits on without a debugger.
void LogWatchedWords() {
    struct Watch { uint32_t addr; uint32_t last; bool seen; };
    static std::vector<Watch> s_watches;
    static bool s_parsed = false;
    if (!s_parsed) {
        s_parsed = true;
        if (const char* list = std::getenv("OTHERM_WATCH")) {
            std::string text(list);
            size_t start = 0;
            while (start < text.size()) {
                const size_t comma = text.find(',', start);
                const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty()) s_watches.push_back({static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)), 0, false});
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }
    static const bool logGx = std::getenv("OTHERM_LOG_GX") != nullptr;
    if (logGx && s_presentedXfbFrames.load(std::memory_order_relaxed) % 300 == 0) {
        uint32_t c[7];
        for (int i = 0; i < 7; ++i) c[i] = g_gxBringupCounters[i].exchange(0);
        RT_LOGF(RT_TAG_VI, "[gxstat] frame %u: last 300 frames: GXBegin %u, GXCopyDisp %u, GXCopyTex %u, GXCallDisplayList %u, GXLoadTexObj %u, KPADReadEx %u, WPADProbe %u\n",
                s_presentedXfbFrames.load(std::memory_order_relaxed), c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
    }
    for (Watch& w : s_watches) {
        uint32_t value = 0;
        if (!Memory::TryRead32(w.addr, value)) continue;
        if (!w.seen || value != w.last) {
            RT_LOGF(RT_TAG_VI, "[watch] retrace %u: 0x%08X = 0x%08X%s\n", s_presentedXfbFrames.load(std::memory_order_relaxed),
                    w.addr, value, w.seen ? "" : " (first)");
            w.seen = true;
            w.last = value;
        }
    }
}

// Other M: report where the game's loader placed game.rel. The translated build was pre-linked for one address
// (project/tools/prelink_otherm_rel.py, pinned by hle/otherm_rel_pin.cpp); a mismatch means translated
// REL code would run against wrong data. Walks __OSModuleInfoList (0x800030C8) once per retrace until reported.
void LogGuestModulesOnce() {
    struct ExpectedModule { uint32_t id; const char* name; uint32_t image; uint32_t bss; };
    static constexpr ExpectedModule kExpected[] = {
        {1, "game.rel", 0x8052B280u, 0x80C75340u},  // game/otherm_rel_layout.json
    };
    static uint32_t s_reported = 0;  // one bit per module id
    if (s_reported == 0x2u) {
        return;
    }
    uint32_t module = 0;
    if (!Memory::TryRead32(0x800030C8u, module)) {
        return;
    }
    for (int guard = 0; module != 0 && guard < 16; ++guard) {
        uint32_t id = 0, next = 0, sectionInfo = 0, sectionBytes = 0, bss = 0;
        if (!Memory::TryRead32(module + 0x00u, id) || !Memory::TryRead32(module + 0x04u, next) ||
            !Memory::TryRead32(module + 0x10u, sectionInfo) || !Memory::TryRead32(module + 0x30u, sectionBytes)) {
            return;
        }
        const uint32_t bssSection = sectionBytes & 0xFFu;  // OSModuleHeader.bssSection (+0x33)
        if (bssSection != 0) {
            Memory::TryRead32(sectionInfo + bssSection * 8u, bss);  // absolute after OSLink
        }
        if (id < 32 && (s_reported & (1u << id)) == 0) {
            s_reported |= 1u << id;
            const ExpectedModule* expected = nullptr;
            for (const auto& candidate : kExpected) {
                if (candidate.id == id) {
                    expected = &candidate;
                }
            }
            const bool match = expected != nullptr && expected->image == module &&
                               (bss == 0 || (bss & ~1u) == expected->bss);
            RT_LOGF(RT_TAG_OS, "REL module %u (%s) linked at image 0x%08X bss 0x%08X: %s\n", id,
                    expected ? expected->name : "unexpected", module, bss,
                    match ? "MATCH" : "MISMATCH vs the pre-linked layout");
        }
        module = next;
    }
}

void AdvanceRetrace(CpuContext* ctx, Clock::time_point retraceStamp, bool serviceAurora) {
    // Prevent re-entry - this can happen if OSWakeupThread triggers SelectThread
    // which goes idle and calls ProcessTimerEvents again
    if (s_inAdvanceRetrace.exchange(true)) {
        return;
    }
    StartStallWatchdogOnce();
    StartGuestSamplerOnce();
    LogGuestModulesOnce();
    LogWatchedWords();
    LogGuestThreads();
    LogGuestWordSearch();
    // Options > Cheats: the built-in cheats, then the Gecko codes (last, so a code can override them).
    if (ctx) {
        OthermCheats::Frame(ctx);
        ModRuntime::Frame(ctx);
    }
    OthermMovieFrame();
    OthermCutsceneFrame();
    OthermAreaFrame();

    uint32_t preCb = 0;
    uint32_t postCb = 0;
    uint32_t retraceValue = 0;
    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    bool isBlack = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        
        // Commit pending state if VIFlush armed it (see ViState).
        if (g_vi.flushArmed) {
            // Commit pending -> active
            g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
            g_vi.black = g_vi.pendingBlack;
            g_vi.tvFormat = g_vi.pendingTvFormat;
            g_vi.renderWidth = g_vi.pendingRenderWidth;
            g_vi.renderHeight = g_vi.pendingRenderHeight;
            g_vi.viXOrigin = g_vi.pendingViXOrigin;
            g_vi.viYOrigin = g_vi.pendingViYOrigin;
            g_vi.xfbWidth = g_vi.pendingXfbWidth;
            g_vi.xfbHeight = g_vi.pendingXfbHeight;
            g_vi.retraceInterval = g_vi.pendingRetraceInterval;
            
            // Clear flush armed flag
            g_vi.flushArmed = false;
            
        }
        
        g_vi.retraceCount++;
        g_retraceCountMirror.store(g_vi.retraceCount, std::memory_order_relaxed);
        g_vi.fieldOdd = !g_vi.fieldOdd;
        // Fast-forward: the virtual time base reaches this retrace's instant now (guest_clock.h).
        GuestClock::FastForwardRetrace(
            IntervalTicks(std::chrono::duration_cast<std::chrono::microseconds>(g_vi.retraceInterval)));
        g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
        currentFb = g_vi.currentFrameBuffer;
        g_vi.lastRetrace = retraceStamp;
        RefreshDisplayLockLocked(Clock::now());
        g_vi.lockedInterval = LockedIntervalAfter(retraceStamp);
        aurora_set_display_periods_per_frame(g_displayLock.valid ? static_cast<uint32_t>(g_displayLock.k) : 0u);
        retraceValue = g_vi.retraceCount;
        preCb = g_vi.preRetraceCallback;
        postCb = g_vi.postRetraceCallback;
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        isBlack = g_vi.black;
        WriteGuestStateLocked();
    }

    // Deterministic mode: this retrace starts a frame. Fingerprint guest memory as the previous
    // frame left it, then latch every channel's input for the new one before any guest code reads it.
    if (DetClock::Enabled()) {
        DetHash::OnRetrace(retraceValue);
        FrameInput::Latch(retraceValue);
    }

    // Wake up threads sleeping on the VI retrace queue (VIWaitForRetrace).
    // The retrace count has been incremented and written to guest memory.
    if (ctx) {
        // Bring-up trace: the first wakeups show whether a VIWaitForRetrace sleeper was queued and
        // whether waking it made the OS see a runnable thread.
        static int s_wakeLogs = 0;
        const bool logWake = s_wakeLogs < 6;
        uint32_t queueHead = 0;
        if (logWake) {
            Memory::TryRead32(kViRetraceQueueAddr, queueHead);
        }
        ctx->gpr[3] = kViRetraceQueueAddr;
        OSWakeupThread_HLE_801139d0(ctx);
        if (logWake) {
            ++s_wakeLogs;
            uint32_t runQueueBits = 0;
            Memory::TryRead32(0x80332770u, runQueueBits);  // RunQueueBits
            RT_LOGF(RT_TAG_VI, "[retrace %u] woke retrace queue (head 0x%08X) -> RunQueueBits 0x%08X\n",
                    retraceValue, queueHead, runQueueBits);
        }
    }

    if (serviceAurora) {
        // Process window events (but don't present - that happens in GXCopyDisp)
        UpdateAuroraAndProcessEvents();

        // Start a new Aurora frame if one isn't already active
        if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
            if (BeginAuroraFrame()) {
                g_auroraFrameActive.store(true, std::memory_order_release);
            }
        }
    }

    if (ctx) {
        ctx->gpr[3] = retraceValue;
        if (preCb) {
            InvokeIndirectCpu(preCb, ctx);
        }
        if (postCb) {
            // Follow the SDK: call whatever callback is set.
            InvokeIndirectCpu(postCb, ctx);
        }
        KPAD_HLE_FireSamplingCallbacks(ctx);
    }

    // VISetBlack(TRUE) keeps frame submission running but shows only the clear color: GX render work is
    // skipped and Aurora's end_frame() clears to black, matching the hardware manual's "signal continues,
    // pixels go black" behavior. When not black, submission waits for hasXfbReady (GXCopyDisp done).
    if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire)) {
        const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
        const bool xfbMatches = (readyXfb != 0 && readyXfb == currentFb);
        const bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
        const bool shouldPresentBlack = isBlack && frameActive;
        const bool shouldSubmit = frameActive && (shouldPresentXfb || shouldPresentBlack);

        if (shouldSubmit) {
            if (!isBlack || settings_overlay::StartupScreenVisible()) {
                // Normal presentation: draw overlay on top of GX content
                settings_overlay::Draw();
            }
            // Outside startup, VI black remains a pure black presentation.
            // Unpaced: this present already runs in retrace context.
            VI_HLE_PresentFrame(shouldPresentXfb, false);
        } else if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !shouldPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            // Don't call aurora_end_frame() as that would present incomplete work
            UpdateAuroraAndProcessEvents();
        }
    }

    // Clear re-entry guard
    s_inAdvanceRetrace.store(false);
    StandaloneState::OnRetraceEnd();  // the windowed game's save states take the next idle pass
}

bool AdvanceDueRetraces(CpuContext* ctx, int maxToProcess, bool serviceAurora)
{
    bool advancedAny = false;

    if (DetClock::Enabled()) {
        for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                if (!g_vi.initialized) {
                    return advancedAny;
                }
                const uint64_t due = g_vi.detLastRetraceTicks + IntervalTicks(g_vi.retraceInterval);
                if (DetClock::Now() < due) {
                    return advancedAny;
                }
                g_vi.detLastRetraceTicks = due;
            }
            AdvanceRetrace(ctx, Clock::now(), serviceAurora);
            advancedAny = true;
        }
        return advancedAny;
    }

    // Fast-forward (guest_clock.h): a retrace comes at the fast pace only where the guest waits for it; anywhere else
    // (GX work, a VI query in the middle of a frame) at the normal moment, as a frame still being worked on would see
    // it on the console. One at a time: the virtual clock has no backlog of its own to catch up.
    const bool fastForward = GuestClock::FastForwarding();
    for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
        Clock::time_point target;
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            if (!g_vi.initialized) {
                return advancedAny;
            }
            if (!fastForward) {
                target = g_vi.lastRetrace + ActiveIntervalLocked();
            } else if (t_idleRetracePump) {
                target = NextIdleRetraceLocked(now);
            } else {
                target = g_vi.lastRetrace + std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval);
            }
            if (now < target) {
                return advancedAny;
            }
        }


        AdvanceRetrace(ctx, fastForward ? now : target, serviceAurora);
        advancedAny = true;
        if (fastForward) break;
    }

    return advancedAny;
}

} // namespace
uint32_t VI_HLE_PresentedFrames() noexcept { return OthermPresentedFramesLocal(); }
std::atomic<uint32_t> g_gxBringupCounters[7]{};

// Force one retrace boundary to pass, whether or not its wall-clock deadline
// has arrived, so the guest's retrace callbacks (AsyncDisplay's counters and
// friends) run. VI_HLE_PollRetrace below is the time-driven counterpart.
void VI_HLE_ForceRetrace(CpuContext* ctx) {
    Clock::time_point target;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        target = g_vi.lastRetrace + ActiveIntervalLocked();
    }
    AdvanceRetrace(ctx, target, true);
}

void VI_HLE_SetSpeed(uint32_t speed) {
    speed = speed < 1 ? 1 : speed;
    const uint32_t previous = g_viSpeed.exchange(speed, std::memory_order_relaxed);
    if (previous == speed || DetClock::Enabled()) return;
    uint64_t intervalTicks = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        intervalTicks = IntervalTicks(std::chrono::duration_cast<std::chrono::microseconds>(g_vi.retraceInterval));
        if (speed == 1) g_vi.lastRetrace = Clock::now();  // the normal pace starts over from here
        else RestartFastForwardPaceLocked(Clock::now());
    }
    // The game's clock follows its retraces while fast-forward runs (guest_clock.h).
    if (previous == 1) {
        GuestClock::BeginFastForward(speed, intervalTicks);
    } else if (speed == 1) {
        GuestClock::EndFastForward();
    } else {
        GuestClock::SetFastForwardSpeed(speed);
    }
    RT_LOGF(RT_TAG_VI, "[ff] fast-forward %s (speed %u) at retrace %u\n", speed > 1 ? "on" : "off", speed,
            VI_HLE_RetraceCount());
}

uint32_t VI_HLE_RetraceCount() noexcept {
    // Without the VI lock: the controllers' sampling reads it from inside a retrace.
    return g_retraceCountMirror.load(std::memory_order_relaxed);
}

uint32_t VI_HLE_Speed() {
    return g_viSpeed.load(std::memory_order_relaxed);
}

bool VI_HLE_IsAdvancingRetrace() {
    return s_inAdvanceRetrace.load(std::memory_order_acquire);
}

// Advance every retrace whose interval has already elapsed. Safe to call from
// busy loops (GX drawing, the scheduler's idle spin) to keep VBlank ticking.
void VI_HLE_PollRetrace(CpuContext* ctx) {
    AdvanceDueRetraces(ctx, 8, true);
}

void VI_HLE_PollRetraceIdle(CpuContext* ctx) {
    const IdleRetracePump idle;
    AdvanceDueRetraces(ctx, 8, true);
}

void VI_HLE_ProcessRetracesDeferred(int maxToProcess) {
    // A wall-clock pump (GX work, host frame waits); deterministic mode delivers retraces only from
    // the scheduler and the guest's own VI calls.
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled() || DetClock::Enabled()) {
        return;
    }

    // This entry point runs from the middle of an arbitrary translated function
    // (GX__Begin's timing service, the host frame loop). Retrace callbacks are
    // an interrupt from that function's point of view, so they get a private
    // copy of its register file exactly as the hardware interrupt would.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    // Host renderer ownership is not a guest critical section. Deliver due VI
    // callbacks at wall-clock cadence while suppressing thread switches and
    // all recursive Aurora/event work until the native GX call has unwound.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        AdvanceDueRetraces(cpu, maxToProcess, false);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

uint64_t VI_HLE_DetNextRetraceTicks() {
    std::lock_guard<std::mutex> lock(g_viMutex);
    if (!g_vi.initialized) {
        return DetClock::kNever;
    }
    return g_vi.detLastRetraceTicks + IntervalTicks(g_vi.retraceInterval);
}

void VI_HLE_WaitForNextRetracePoll() {
    Clock::time_point retraceDeadline;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        retraceDeadline = NextIdleRetraceLocked(Clock::now());
    }

    const auto now = Clock::now();
    if (now >= retraceDeadline) {
        return;
    }
    // Audio DMA and alarm queues still need regular service even when the VI
    // deadline is farther away. The high-resolution wait removes the 1 ms
    // scheduler overshoot when retrace is the next event.
    const bool retraceIsNext = retraceDeadline <= now + 1ms;
    SleepPreciselyUntil(retraceIsNext ? retraceDeadline : now + 1ms, retraceIsNext,
                        kFinalSpinWindow);
}

namespace {

// Retrace count consumed by the most recent paced present. This is a slot memo
// against the VI timeline, not a second clock: it only answers "did a retrace
// already elapse while this frame was being produced?". Producer-thread only.
uint32_t s_lastPacedRetraceCount = ~0u;

// Last presentation anchor handed to Aurora, in nanoseconds on the VI retrace
// grid. Guarantees consecutive sealed frames never share an anchor (see the
// comment at the stamping site). Producer-thread only, like the memo above.
uint64_t s_lastPresentAnchorNanos = 0;

// Sleeps to the same VI retrace boundary VIWaitForRetrace targets, servicing alarms every 1 ms so audio
// DMA and timers keep running, then delivers that retrace so guest logic starts exactly on the grid.
void PaceToRetraceBoundary(Clock::time_point deadline) {
    constexpr auto kServiceSlice = 1ms;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        if (deadline - now > kServiceSlice + kFinalSpinWindow) {
            SleepPreciselyUntil(now + kServiceSlice);
            OS_HLE_ProcessAlarmsDeferred(8);
            // Audio DMA is a 3 ms cadence and this wait is up to a full display
            // period long. Without a pump here the blocks that came due during
            // the wait are all delivered at once when the guest next reaches
            // idle, which the guest observes as AI slack jitter.
            Audio_HLE_PollDeferred();
            continue;
        }
        SleepPreciselyUntil(deadline, true, kFinalSpinWindow);
        break;
    }
    const IdleRetracePump idle;  // the frame is done: the game waits for this retrace
    VI_HLE_ProcessRetracesDeferred(1);
}

} // namespace

// Single owner of the Aurora frame presentation sequence: seals the active frame, optionally paces the
// producer to the VI retrace boundary, and pre-warms the next frame. Paced from GXCopyDisp; unpaced for
// the retrace-context black/boot present path in AdvanceRetrace.
// OTHERM_LOG_FPS (debug.otherm.logfps on Android): every two seconds, the frames presented and where
// each frame's time went: the game (guest code and GX command building), aurora_end_frame (encoding,
// submission, and waiting on the GPU when it falls behind) and pacing to the retrace. A game share near
// the frame budget is CPU-bound; a large end_frame share is GPU-bound.
struct FrameTimes {
    Clock::time_point windowStart{};
    Clock::time_point lastPresentEnd{};
    double gameMs = 0, endFrameMs = 0, paceMs = 0, worstMs = 0, sumSq = 0;
    int slow = 0;
    int frames = 0;
};

static bool LogFpsEnabled() {
    static const bool on = std::getenv("OTHERM_LOG_FPS") != nullptr;
    return on;
}

// dvd.cpp: time in the game's disc reads since start, and their count.
uint64_t DvdReadNanosTotal();
extern std::atomic<uint64_t> g_copyDispWorkerWaitNanos;  // gx_copy.cpp
extern std::atomic<uint64_t> g_schedulerIdleNanos;       // os_scheduler.cpp
uint32_t DvdReadCount();

void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    if (s_presentSequenceActive.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    GuestProfiler::OnFramePresented();
    static FrameTimes times;
    const Clock::time_point presentStart = Clock::now();
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    struct SequenceGuard {
        ~SequenceGuard() { s_presentSequenceActive.store(false, std::memory_order_release); }
    } sequenceGuard;
    if (presentedXfb) {
        {
            const uint32_t pf = s_presentedXfbFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            // drawdump.txt: "<frame>" dumps that presented frame's draws; "<frame> <count>" a run of frames
            // (g_othermDrawDump then holds the frame number, which the [omdraw]/[omb] lines carry).
            static int32_t dumpCount = 1;
            static const int32_t dumpTarget = [] {
                std::ifstream in("drawdump.txt");
                int32_t f = -1, n = 1;
                in >> f;
                if (in >> n && n > 0) dumpCount = n;
                return f;
            }();
            static const bool cullTest = std::ifstream("depth_test.txt").good();
            const bool inDump = dumpTarget >= 0 && pf >= static_cast<uint32_t>(dumpTarget) &&
                                pf < static_cast<uint32_t>(dumpTarget) + static_cast<uint32_t>(dumpCount);
            g_othermDrawDump = inDump ? static_cast<int>(pf) : 0;
            static const int envTest = [] { const char* e = std::getenv("OTHERM_CULL_TEST"); return e ? std::atoi(e) : 0; }();  // 1 = no culling, 2 = no depth test (diagnostic)
            static const unsigned drawLimit = [] { const char* e = std::getenv("OTHERM_DRAW_LIMIT"); return e ? static_cast<unsigned>(std::atoi(e)) : ~0u; }();
            g_othermDrawLimit = drawLimit;
            g_othermDrawIdx = 0;
            g_othermCullOff = envTest ? envTest : ((cullTest && pf >= 6900u) ? 2 : 0);
        }
        // Tell the runtime's startup cover about the game's first visible frame.
        static std::once_flag firstVisibleFrame;
        if (!g_vi.black) {
            std::call_once(firstVisibleFrame, [] {
                // Other M: the cover only covers the boot shaders (settings_overlay StartupScreenVisible): it lifts by
                // itself once they are ready, a second after this frame at the earliest. It used to wait for A or 2,
                // and the game's own wrist-strap screen behind it then asked for A again.
                RT_LOGF(RT_TAG_VI, "first visible guest frame presented; the startup cover lifts once the shaders are ready\n");
                settings_overlay::NotifyStrapInputAccepted();
            });
        }
    }
    Clock::time_point paceDeadline{};
    bool paceThisFrame = false;
    // Deterministic mode paces in the scheduler's idle wait instead, and must not deliver the
    // retrace from here (PaceToRetraceBoundary does), so frames present as soon as they are sealed.
    if (DetClock::Enabled()) {
        paceToRetrace = false;
    }
    if (paceToRetrace) {
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            paceDeadline = NextIdleRetraceLocked(Clock::now());
            retraceCount = g_vi.retraceCount;
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                ActiveIntervalLocked().count());
        }
        // Hold the frame to its boundary only when no retrace elapsed during
        // its production. A frame that missed its boundary presents
        // immediately: hardware would quantize down to the next vblank here,
        // but free-running late frames matches the previous pacer and keeps a
        // heavy scene at e.g. 50 fps instead of hard 30.
        const uint32_t retracesElapsed = retraceCount - s_lastPacedRetraceCount;
        // A pending save state request needs an idle pass (standalone_save_state.h): wait in VIWaitForRetrace.
        paceThisFrame = retracesElapsed == 0 && !StandaloneState::RequestPending();
        s_lastPacedRetraceCount = retraceCount;
        // aurora_report_producer_paced needs a different signal than the pace-wait above: the guest
        // self-paces via VIWaitForRetrace, so one retrace per produced frame is the healthy locked-60
        // cadence, and zero only happens when production outruns VI. "Kept up" means <=1 retrace
        // elapsed; 2+ means a boundary was missed, so Aurora seals that frame without its interpolated
        // slots (a windowed backstop lowers the slot target only under sustained overload).
        aurora_report_producer_paced(retracesElapsed <= 1);
        // Stamp the sealed frame's presentation schedule so Aurora paces interpolated slots against
        // this same VI timeline. Anchor to the NEXT retrace boundary, not the period just produced,
        // since slots anchored to the current period would already be expired by seal time. Encoding
        // overruns are corrected by sliding the whole slot group forward onto a later boundary of this
        // same grid, so this stays the single cadence authority. Anchors must also be strictly
        // monotonic: two frames sealed before lastRetrace advances would collide on one boundary and
        // burst-present, so a colliding anchor steps onto the next grid point instead of repeating it.
        uint64_t anchorNanos = baseNanos + intervalNanos;
        if (s_lastPresentAnchorNanos != 0 && anchorNanos <= s_lastPresentAnchorNanos) {
            anchorNanos = s_lastPresentAnchorNanos + intervalNanos;
        }
        s_lastPresentAnchorNanos = anchorNanos;
        aurora_set_present_schedule(anchorNanos, intervalNanos);
    } else {
        // Retrace-context presents (VI black, boot) have no display period of
        // their own to subdivide; present as soon as the frame is ready. The
        // schedule grid is gone, so the anchor cursor must not constrain the
        // next paced frame.
        s_lastPresentAnchorNanos = 0;
        aurora_set_present_schedule(0, 0);
    }

    const Clock::time_point endFrameStart = Clock::now();
    aurora_end_frame();
    const Clock::time_point paceStart = Clock::now();
    if (paceThisFrame) {
        PaceToRetraceBoundary(paceDeadline);
        std::lock_guard<std::mutex> lock(g_viMutex);
        s_lastPacedRetraceCount = g_vi.retraceCount;
    } else if (paceToRetrace) {
        // A game frame that missed its boundary does not wait, and the pace wait is what services audio between
        // frames. Fast-forward in a busy scene misses every boundary and the guest never idles, so its audio blocks
        // came only from rare idle passes: 48-108 a second instead of ~1,000, then a flood when it idled (boot aq5,
        // 3x at 169 fps). One poll per such frame keeps them flowing.
        Audio_HLE_PollDeferred();
    }
    // Always on: a frame that took over 20 ms (a dropped frame at 60) goes to the log with where the
    // time went, so a report of drops on someone's machine comes with its cause. At most 30 a minute.
    if (presentedXfb) {
        static Clock::time_point s_lastEnd{}, s_windowStart{};
        static int s_logged = 0;
        static uint64_t s_lastGx = 0, s_lastDvd = 0, s_lastWorker = 0;
        static uint32_t s_lastDvdReads = 0;
        const uint64_t gxTotal = AuroraGxProcessNanosTotal();
        const double gxMs = (gxTotal - s_lastGx) / 1e6;
        s_lastGx = gxTotal;
        const uint64_t workerTotal = g_copyDispWorkerWaitNanos.load(std::memory_order_relaxed);
        const double workerMs = (workerTotal - s_lastWorker) / 1e6;
        s_lastWorker = workerTotal;
        const uint64_t dvdTotal = DvdReadNanosTotal();
        const uint32_t dvdReads = DvdReadCount();
        const double dvdMs = (dvdTotal - s_lastDvd) / 1e6;
        const uint32_t frameReads = dvdReads - s_lastDvdReads;
        s_lastDvd = dvdTotal;
        s_lastDvdReads = dvdReads;
        // What in aurora a slow frame spent its time on: game textures converted and uploaded (inside gx), pack
        // replacements loaded (inside gx), the CPU waiting for EFB copies (outside gx: the game's own time).
        // The GPU's load over the last second (Linux AMD, -1 elsewhere): ~100 when the GPU, not the game, is behind (a
        // resolution it cannot draw in a frame's time; the game thread then waits in aurora, inside "game").
        const int gpuBusy = GpuLoad::Latest().busy1s;
        static uint64_t s_lastIdle = 0;
        const uint64_t idleTotal = g_schedulerIdleNanos.load(std::memory_order_relaxed);
        const double idleMs = (idleTotal - s_lastIdle) / 1e6;
        s_lastIdle = idleTotal;
        static uint64_t s_lastCostNs[3] = {}, s_lastCostN[3] = {};
        uint64_t costNs[3] = {}, costN[3] = {};
        AuroraFrameCostTotals(costNs, costN);
        double costMs[3];
        uint32_t costCount[3];
        for (int k = 0; k < 3; ++k) {
            costMs[k] = (costNs[k] - s_lastCostNs[k]) / 1e6;
            costCount[k] = static_cast<uint32_t>(costN[k] - s_lastCostN[k]);
            s_lastCostNs[k] = costNs[k];
            s_lastCostN[k] = costN[k];
        }
        // Draws the ubershader stood in for (their own shader still compiling): this frame, and a line at most once a
        // second while it keeps happening, with how many pipelines were still queued.
        static uint64_t s_lastUberDraws = 0, s_uberWindowDraws = 0;
        static Clock::time_point s_uberWindowStart{}, s_uberLogMinute{};
        static uint32_t s_uberLines = 0;
        const uint64_t uberTotal = aurora_get_ubershader_draw_count();
        const uint32_t frameUberDraws = static_cast<uint32_t>(uberTotal - s_lastUberDraws);
        s_lastUberDraws = uberTotal;
        s_uberWindowDraws += frameUberDraws;
        const Clock::time_point now = Clock::now();
        if (s_uberWindowDraws > 0 && now - s_uberWindowStart >= std::chrono::seconds(1)) {
            if (now - s_uberLogMinute > std::chrono::minutes(1)) {
                s_uberLogMinute = now;
                s_uberLines = 0;
            }
            if (s_uberLines++ < 20) {
                RT_LOGF(RT_TAG_VI, "ubershaders drew %llu draws in %.1f s while shaders compiled (%u pipelines queued)\n",
                        static_cast<unsigned long long>(s_uberWindowDraws),
                        s_uberWindowStart == Clock::time_point{} ? 1.0 : ms(now - s_uberWindowStart) / 1000.0,
                        aurora_get_queued_pipeline_count());
            }
            s_uberWindowDraws = 0;
            s_uberWindowStart = now;
        } else if (s_uberWindowDraws == 0) {
            s_uberWindowStart = now;
        }
        if (s_lastEnd != Clock::time_point{}) {
            const double frame = ms(now - s_lastEnd);
            if (now - s_windowStart > std::chrono::minutes(1)) {
                s_windowStart = now;
                s_logged = 0;
            }
            // OTHERM_SLOW_FRAMES_ALL=1 (tests): no cap, so a run's boot does not use up the minute it is compared in.
            static const bool logAll = std::getenv("OTHERM_SLOW_FRAMES_ALL") != nullptr;
            if (frame > 20.0 && frame < 2000.0 && (logAll || s_logged++ < 30)) {
                RT_LOGF(RT_TAG_VI,
                        "slow frame %.1f ms: game %.2f (gx %.2f, worker wait %.2f, disc %.2f in %u reads), present %.2f, "
                        "end_frame %.2f, pace %.2f; idle %.2f; textures %u in %.2f, pack %u in %.2f, efb sync %u in %.2f; "
                        "ubershader draws %u, gpu busy %d%%\n",
                        frame, ms(presentStart - s_lastEnd), gxMs, workerMs, dvdMs, frameReads,
                        ms(endFrameStart - presentStart),
                        ms(paceStart - endFrameStart), ms(now - paceStart), idleMs, costCount[0], costMs[0], costCount[1],
                        costMs[1], costCount[2], costMs[2], frameUberDraws, gpuBusy);
            }
        }
        s_lastEnd = now;
    }
    if (LogFpsEnabled() && presentedXfb) {
        const Clock::time_point now = Clock::now();
        if (times.lastPresentEnd != Clock::time_point{}) {
            const double game = ms(presentStart - times.lastPresentEnd) + ms(endFrameStart - presentStart);
            const double frame = ms(now - times.lastPresentEnd);
            times.gameMs += game;
            times.endFrameMs += ms(paceStart - endFrameStart);
            times.paceMs += ms(now - paceStart);
            times.worstMs = std::max(times.worstMs, frame);
            times.sumSq += frame * frame;
            times.slow += frame > 20.0 ? 1 : 0;
            ++times.frames;
        } else {
            times.windowStart = now;
        }
        times.lastPresentEnd = now;
        const double window = ms(now - times.windowStart);
        if (window >= 2000.0 && times.frames > 0) {
            const double n = times.frames;
            uint64_t gxNanos = 0, lockNanos = 0, draws = 0;
            AuroraTakeGxStats(&gxNanos, &lockNanos, &draws);
            uint64_t copies = 0, creates = 0;
            AuroraTakeCopyStats(&copies, &creates);
            static uint64_t s_lastWorkerWait = 0;
            const uint64_t workerTotal = g_copyDispWorkerWaitNanos.load(std::memory_order_relaxed);
            const uint64_t workerWait = workerTotal - s_lastWorkerWait;
            s_lastWorkerWait = workerTotal;
            RT_LOGF(RT_TAG_VI,
                    "fps %.1f: game %.2f ms (gx %.2f ms, worker wait %.2f ms, lock wait %.2f ms, %.0f draws, %.1f copies, %.1f new copy "
                    "textures), end_frame %.2f ms, pace %.2f ms, worst frame %.1f ms, jitter %.2f ms, %d over 20 ms, "
                    "gpu busy %d%%\n",
                    n * 1000.0 / window, times.gameMs / n, gxNanos / 1e6 / n, workerWait / 1e6 / n, lockNanos / 1e6 / n, draws / n,
                    copies / n, creates / n, times.endFrameMs / n, times.paceMs / n, times.worstMs,
                    std::sqrt(std::max(0.0, times.sumSq / n - (window / n) * (window / n))), times.slow,
                    GpuLoad::Latest().busy1s);
            if (aurora_get_frame_interpolation_fps() != 0) {  // what the screen shows vs the 60 Hz game
                AuroraPresentTiming present{};
                aurora_get_present_timing(&present);
                AuroraFrameInterpolationDiagnostics fi{};
                aurora_get_frame_interpolation_diagnostics(&fi);
                RT_LOGF(RT_TAG_VI, "interp: displayed %.1f fps (motion %.1f), target %u, slots %u/%u, matched %u/%u of %u draws, "
                        "eligible %u, low-match %llu/%llu frames, reductions %llu, late drops %llu\n",
                        present.framesPerSecond, present.effectiveFramesPerSecond, fi.targetFps, fi.activeSamples,
                        fi.targetSamples, fi.matches, fi.matchable, fi.candidates, fi.eligible,
                        (unsigned long long)fi.framesLowMatch, (unsigned long long)fi.framesSealed,
                        (unsigned long long)fi.slotReductions, (unsigned long long)fi.lateSealDrops);
            }
            times = FrameTimes{};
            times.windowStart = now;
            times.lastPresentEnd = now;
        }
    }
    settings_overlay::AdvancePresentedFrame();
    g_auroraFrameActive.store(false, std::memory_order_release);
    g_auroraFrameHadWork.store(false, std::memory_order_release);
    if (presentedXfb) {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.hasValidXfb = false;
        g_vi.readyXfb = 0;
    }
    // Pre-warm the next frame so subsequent GX work has a valid frame context.
    {
        UpdateAuroraAndProcessEvents();
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
    }
}

// -----------------------------------------------------------------------------
// VI_HLE_SetXfbReady - Called by GXCopyDisp to signal EFB->XFB copy completed.
// This marks that we now have valid framebuffer data to present.
// -----------------------------------------------------------------------------
void VI_HLE_SetXfbReady(uint32_t xfbAddr) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_vi.hasValidXfb = true;
    g_vi.readyXfb = xfbAddr;
    if (g_vi.currentFrameBuffer == 0 && g_vi.nextFrameBuffer == 0) {
        g_vi.currentFrameBuffer = xfbAddr;
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
        WriteGuestStateLocked();
    } else if (g_vi.nextFrameBuffer != xfbAddr) {
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
    }
}

// VIInit (0x801BC7A0) and its lower-level helper __VIInit (0x801BC5A0) both
// program MMIO at 0xCC0020xx on hardware. We skip all hardware access and seed
// the same defaults instead, so the two entry points share one body.
static void SeedViStateForInit(CpuContext* ctx, const char* who)
{
    RT_LOG(RT_TAG_VI) << who << " called: seeding VI state (HLE)" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    ViSetR3(ctx, 0);
}

extern "C" void VIInit_HLE_8011bdc0(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "VIInit_8011bdc0");
}
PPC_NATIVE_OVERRIDE_VOID(8011BDC0, VIInit_HLE_8011bdc0, (CpuContext* ctx), (ctx));

extern "C" void __VIInit_HLE_8011bbc0(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "__VIInit_8011bbc0");
}
PPC_NATIVE_OVERRIDE_VOID(8011BBC0, __VIInit_HLE_8011bbc0, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPreRetraceCallback (0x801BC3E0)
// -----------------------------------------------------------------------------
extern "C" void VISetPreRetraceCallback_HLE_8011ba00(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.preRetraceCallback;
        g_vi.preRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(8011BA00, VISetPreRetraceCallback_HLE_8011ba00, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPostRetraceCallback (0x801BC430)
// -----------------------------------------------------------------------------
extern "C" void VISetPostRetraceCallback_HLE_8011ba50(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.postRetraceCallback;
        g_vi.postRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(8011BA50, VISetPostRetraceCallback_HLE_8011ba50, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIGetDTVStatus (0x801BE070)
// Reads DTV status from VI hardware (MMIO 0xCC00206E). Stub to "not ready".
// -----------------------------------------------------------------------------
extern "C" void VIGetDTVStatus_HLE_8011d6e0(CpuContext* ctx)
{
    ViSetR3(ctx, 0); // return 0 -> not ready / disabled
}
PPC_NATIVE_OVERRIDE_VOID(8011D6E0, VIGetDTVStatus_HLE_8011d6e0, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIConfigure & related helpers: translate GXRenderModeObj into guest globals.
// -----------------------------------------------------------------------------
extern "C" void VIConfigure_HLE_8011c8a0(CpuContext* ctx)
{
    // One read of the guest GXRenderModeObj serves both the VI pending state and
    // aurora, rather than unpacking the same 0x39 bytes twice.
    const uint32_t renderModePtr = ctx ? ctx->gpr[3] : 0;
    GXRenderModeObj renderMode{};
    if (!ReadGuestRenderModeObj(renderModePtr, renderMode)) {
        RT_LOG(RT_TAG_VI) << "VIConfigure: invalid GXRenderModeObj pointer 0x"
                  << std::hex << renderModePtr << std::dec << std::endl;
        ViSetR3(ctx, 0);
        return;
    }

    const uint32_t decodedTvFormat =
        ExtractTvFormat(static_cast<uint32_t>(renderMode.viTVmode));
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingTvFormat = decodedTvFormat;
        g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        g_vi.pendingRenderWidth =
            renderMode.viWidth != 0 ? renderMode.viWidth : renderMode.fbWidth;
        g_vi.pendingRenderHeight =
            renderMode.viHeight != 0 ? renderMode.viHeight : renderMode.xfbHeight;
        g_vi.pendingViXOrigin = renderMode.viXOrigin;
        g_vi.pendingViYOrigin = renderMode.viYOrigin;
        g_vi.pendingXfbWidth = renderMode.fbWidth;
        g_vi.pendingXfbHeight =
            renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight;
    }

    ::VIConfigure(&renderMode);

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8011C8A0, VIConfigure_HLE_8011c8a0, (CpuContext* ctx), (ctx));

extern "C" void VIFlush_HLE_8011d2d0(CpuContext* ctx)
{
    uint32_t guestNextFb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        if (g_vi.pendingNextFrameBuffer == 0) {
            try {
                guestNextFb = Memory::Read32(kViNextFrameBufferAddr);
            } catch (const Memory::AccessViolation&) {
                guestNextFb = 0;
            }
            if (guestNextFb == 0) {
                try {
                    guestNextFb = Memory::Read32(kViNextFrameBufferHwAddr);
                } catch (const Memory::AccessViolation&) {
                    guestNextFb = 0;
                }
            }
            if (guestNextFb != 0) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
            }
        }
        
        // Arm only; the commit happens at the next retrace (see ViState).
        g_vi.flushArmed = true;
    }

    // NOTE: We do NOT set hasValidXfb here. Frame readiness is signaled ONLY by
    // GXCopyDisp (which sets hasValidXfb = true), as that's when the EFB->XFB
    // copy is complete and we have a valid frame to present.

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8011D2D0, VIFlush_HLE_8011d2d0, (CpuContext* ctx), (ctx));

extern "C" void VISetNextFrameBuffer_HLE_8011d3f0(CpuContext* ctx)
{
    const uint32_t fbPtr = ctx ? ctx->gpr[3] : 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingNextFrameBuffer = fbPtr;
        // Also update guest memory for SDK code that reads this directly
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8011D3F0, VISetNextFrameBuffer_HLE_8011d3f0, (CpuContext* ctx), (ctx));

extern "C" void VISetBlack_HLE_8011d460(CpuContext* ctx)
{
    const bool makeBlack = ctx ? (ctx->gpr[3] != 0) : false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingBlack = makeBlack;
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8011D460, VISetBlack_HLE_8011d460, (CpuContext* ctx), (ctx));

extern "C" void VIGetRetraceCount_HLE_8011d4e0(CpuContext* ctx)
{
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        count = g_vi.retraceCount;
    }
    ViSetR3(ctx, count);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(8011D4E0, VIGetRetraceCount_HLE_8011d4e0, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextField_HLE_8011d4f0(CpuContext* ctx)
{
    bool fieldOdd = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        fieldOdd = g_vi.fieldOdd;
    }
    ViSetR3(ctx, fieldOdd ? 1 : 0);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(8011D4F0, VIGetNextField_HLE_8011d4f0, (CpuContext* ctx), (ctx));

extern "C" void VIGetCurrentLine_HLE_8011d590(CpuContext* ctx)
{
    uint32_t height = 480;
    std::chrono::nanoseconds interval{16666us};
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        height = g_vi.xfbHeight;
        interval = ActiveIntervalLocked();
        last = g_vi.lastRetrace;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - last);
    if (DetClock::Enabled()) {
        uint64_t lastTicks = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            lastTicks = g_vi.detLastRetraceTicks;
        }
        const uint64_t now = DetClock::Now();
        elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            TimeBaseContract::TicksToDuration(now > lastTicks ? now - lastTicks : 0));
    }
    uint32_t line = 0;
    if (interval.count() > 0 && height > 0) {
        const uint64_t scaled = static_cast<uint64_t>(elapsed.count()) * height;
        const uint64_t intervalUs = static_cast<uint64_t>(std::max<int64_t>(1, interval.count() / 1000));
        line = static_cast<uint32_t>(std::min<uint64_t>(height - 1, scaled / intervalUs));
    }
    ViSetR3(ctx, line);
}
PPC_NATIVE_OVERRIDE_VOID(8011D590, VIGetCurrentLine_HLE_8011d590, (CpuContext* ctx), (ctx));

// Options (settings_overlay) pauses the game: its main loop is held here, in the wait every frame ends with, while
// frames keep presenting the game's last picture (aurora_present_last_display_copy) with the menu over it; the menu
// reads the first remote itself (FilterMenuInput) because the game no longer polls KPAD. Nothing guest runs meanwhile
// (guest threads are fibers on this thread; alarms and audio are serviced from guest waits), so the sound stops too.
// Guest time stands still: the time base continues from where it stopped and the retrace clock restarts from now, so
// no alarm, timeout or retrace catches up afterwards. g_gamePausedForMenu also stops main.cpp's servicing of retraces,
// alarms and audio while aurora waits for its frame worker, which would otherwise run guest callbacks from here.
std::atomic_bool g_gamePausedForMenu{false};

static void RunPausedFrames()
{
    const uint64_t timeBase = PPC_ReadGuestTimeBase();
    g_gamePausedForMenu.store(true, std::memory_order_release);
    Audio_HLE_SetMenuPause(true);
    const auto retraces = [] {
        std::lock_guard<std::mutex> lock(g_viMutex);
        return g_vi.retraceCount;
    };
    RT_LOGF(RT_TAG_VI, "[pause] game paused for Options at retrace %u, time base %llu, audio dump byte %llu\n", retraces(),
            static_cast<unsigned long long>(timeBase), static_cast<unsigned long long>(Audio_HLE_DumpedBytes()));
    uint32_t frames = 0;
    while (settings_overlay::GamePauseRequested()) {
        const Clock::time_point deadline = Clock::now() + 16666us;
        UpdateAuroraAndProcessEvents();
        if (!g_auroraFrameActive.load(std::memory_order_acquire) && BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
        WiiRemoteInput::KpadSample sample{};
        if (WiiRemoteInput::ReadKpadSample(0, sample)) settings_overlay::FilterMenuInput(0, sample);
        if (g_auroraFrameActive.load(std::memory_order_acquire)) {
            aurora_present_last_display_copy();
            settings_overlay::Draw();
            VI_HLE_PresentFrame(true, false);
            ++frames;
        }
        SleepPreciselyUntil(deadline);
    }
    PPC_SetGuestTimeBase(timeBase);
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.lastRetrace = Clock::now();
        RestartFastForwardPaceLocked(g_vi.lastRetrace);
    }
    Audio_HLE_SetMenuPause(false);
    g_gamePausedForMenu.store(false, std::memory_order_release);
    RT_LOGF(RT_TAG_VI, "[pause] resumed after %u frames at retrace %u, time base %llu, audio dump byte %llu\n", frames,
            retraces(), static_cast<unsigned long long>(PPC_ReadGuestTimeBase()),
            static_cast<unsigned long long>(Audio_HLE_DumpedBytes()));
}

void VI_HLE_RunLauncher()
{
    // As RunPausedFrames, before the game exists: the window's events, the menu's input (the stand-in remote reads
    // the controllers and keys itself) and the start screen alone, at 60 frames a second, until Play.
    settings_overlay::BeginLauncher();
    uint32_t frames = 0;
    while (settings_overlay::LauncherActive()) {
        const Clock::time_point deadline = Clock::now() + 16666us;
        UpdateAuroraAndProcessEvents();
        if (!g_auroraFrameActive.load(std::memory_order_acquire) && BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
        WiiRemoteInput::KpadSample sample{};
        if (WiiRemoteInput::ReadKpadSample(0, sample)) settings_overlay::FilterMenuInput(0, sample);
        if (g_auroraFrameActive.load(std::memory_order_acquire)) {
            settings_overlay::Draw();
            VI_HLE_PresentFrame(false, false);
            ++frames;
        }
        if (frames % 30u == 0u) {
            DiscordPresence::Publish(settings_overlay::LauncherSettingUp() ? "Setting up the game" : "On the start screen", "");
        }
        SleepPreciselyUntil(deadline);
    }
    DiscordPresence::Publish("", "");
    RT_LOGF(RT_TAG_VI, "[launcher] the game boots after %u start-screen frames\n", frames);
}

extern "C" void VIWaitForRetrace_HLE_8011c310(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    
    if (Fiber::GuestFiberManager::IsInitialized()) {
        bool resumedRunning = false;
        const SaveState::YieldRecord* resumed = SaveState::TakeYield(SaveState::YieldKind::ViWaitForRetrace, resumedRunning);
        if (!resumed && !DetClock::Enabled() && settings_overlay::GamePauseRequested()) {
            RunPausedFrames();
        }
        const CpuContext entry = *cpu;
        const int32_t irqState = resumed ? static_cast<int32_t>(resumed->extra[1]) : OS__DisableInterrupts_8010f160();
        uint32_t retraceCount = 0;
        if (resumed) {
            retraceCount = resumed->extra[0];
        } else {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            retraceCount = g_vi.retraceCount;
        }
        // Save states: the wait every frame's main loop parks in; a restored thread continues after its sleep.
        std::optional<SaveState::YieldScope> yieldScope;
        if (resumed) yieldScope.emplace(*resumed);
        else yieldScope.emplace(SaveState::YieldKind::ViWaitForRetrace, entry,
                                std::initializer_list<uint32_t>{retraceCount, static_cast<uint32_t>(irqState)});
        bool continuing = resumed != nullptr;

        do {
            cpu->gpr[3] = kViRetraceQueueAddr;
            if (continuing) {
                OS_HLE_SleepResume(cpu, resumedRunning, 0);  // the sleep ran with interrupts already off
                continuing = false;
            } else {
                OSSleepThread_HLE_801138e0(cpu);
            }

            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                EnsureInitializedLocked();
                if (g_vi.retraceCount != retraceCount) {
                    break;
                }
            }
        } while (true);

        OS__RestoreInterrupts_8010f1a0(irqState);
    } else {
        std::chrono::nanoseconds interval{16666us};
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            interval = ActiveIntervalLocked();
            target = g_vi.lastRetrace + interval;
        }

        const auto now = Clock::now();
        if (now < target) {
            SleepPreciselyUntil(target, true);
        }
        AdvanceRetrace(cpu, target, true);
    }
    ViSetR3(cpu, 0);
}
PPC_NATIVE_OVERRIDE_VOID(8011C310, VIWaitForRetrace_HLE_8011c310, (CpuContext* ctx), (ctx));

// VIGetScanMode (0x8011D680): reads the VI clock/DTV registers on hardware. The host has no
// cable, so report interlaced (VI_INTERLACE = 0), the mode every NTSC console supports.
extern "C" uint32_t VIGetScanMode_HLE() { return 0; }
PPC_NATIVE_OVERRIDE(8011D680, VIGetScanMode_HLE, uint32_t, (), ());


uint32_t OthermPresentedFrames() { return g_othermFramesFn(); }

void VI_HLE_SaveState(SaveState::Writer& out) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    out.Pod(g_vi);
}

void VI_HLE_LoadState(SaveState::Reader& in) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    in.Pod(g_vi);
    g_retraceCountMirror.store(g_vi.retraceCount, std::memory_order_relaxed);
    RestartFastForwardPaceLocked(Clock::now());  // the saved count belongs to the timeline of the state's session
    // Real-time pacing restarts from now: the saved time point belongs to the session that wrote the state.
    g_vi.lastRetrace = Clock::now();
}
