#pragma once
#include <string>

#include "abi_bridge.h"

// hle/gx/gx_fatal_stubs.cpp includes nothing but this header and reaches
// std::fprintf / std::snprintf / std::abort through it.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// VI Utils
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
// VI_HLE_PollRetrace from where the guest waits for the next retrace (the scheduler's idle loop): during fast-forward
// the retrace may come at the fast pace here, elsewhere only at the normal moment (guest_clock.h).
void VI_HLE_PollRetraceIdle(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
void VI_HLE_WaitForNextRetracePoll();
// Deterministic mode (det_clock.h): the virtual tick of the next VI retrace, DetClock::kNever
// before VIInit.
uint64_t VI_HLE_DetNextRetraceTicks();
namespace SaveState { class Writer; class Reader; }
void VI_HLE_SaveState(SaveState::Writer& out);
void VI_HLE_LoadState(SaveState::Reader& in);
void OS_HLE_SaveState(SaveState::Writer& out);
void OS_HLE_LoadState(SaveState::Reader& in);
void AI_HLE_SaveState(SaveState::Writer& out);
void AI_HLE_LoadState(SaveState::Reader& in);
void KPAD_HLE_SaveState(SaveState::Writer& out);
void KPAD_HLE_LoadState(SaveState::Reader& in);
bool NAND_HLE_SaveState(SaveState::Writer& out, std::string& error);
void NAND_HLE_LoadState(SaveState::Reader& in);
void GX_HLE_SaveObjectIds(SaveState::Writer& out);
void GX_HLE_LoadObjectIds(SaveState::Reader& in);
void DVD_HLE_SaveState(SaveState::Writer& out);
void DVD_HLE_LoadState(SaveState::Reader& in);
// The host file a disc path is read from (a mod's replacement or the game's own), once the disc index is built;
// empty when the path has no file. UTF-8 path.
std::string DVD_HLE_HostPath(const std::string& dvdPath);
void NAND_HLE_SaveAsyncState(SaveState::Writer& out);
void NAND_HLE_LoadAsyncState(SaveState::Reader& in);
// Single owner of the Aurora frame presentation sequence (seal, optional pace
// to the VI retrace boundary, pre-warm the next frame). paceToRetrace is true
// for the GXCopyDisp producer path and false for retrace-context presents.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace);
// The start screen (settings_overlay.h) before the game boots, until the player picks Play: the window's events,
// the menu's input and frames of it alone (the guest has not started).
void VI_HLE_RunLauncher();
bool VI_HLE_IsAdvancingRetrace();
// Fast-forward: the game's clock runs up to `speed` times faster (1 = normal; guest_clock.h).
void VI_HLE_SetSpeed(uint32_t speed);
uint32_t VI_HLE_Speed();
// Retraces delivered so far (the game's own frame clock: 60 a second at normal speed).
uint32_t VI_HLE_RetraceCount() noexcept;
void VI_HLE_SetXfbReady(uint32_t xfbAddr); // Called by GXCopyDisp to signal EFB→XFB copy
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
// The AI / DSP interface registers (0xCC005000 block and the AI control block at 0xCC006C00) read by guest code
// that polls them directly instead of calling the SDK (Other M's sound engine reads AI DMA bytes-left, 0xCC00503A).
// memory.cpp's slow read paths answer those reads from the AI HLE state instead of faulting.
bool Audio_HLE_IsAiRegister(uint32_t addr) noexcept;
// Bring-up instrumentation (Other M): frames presented so far, and GX call counters the retrace prints every
// 300 frames under OTHERM_LOG_GX=1 (0 GXBegin, 1 GXCopyDisp, 2 GXCopyTex, 3 GXCallDisplayList, 4 GXLoadTexObj).
uint32_t VI_HLE_PresentedFrames() noexcept;
// Other M reads its controllers only inside KPAD's sampling callback (KPADSetSamplingCallback per channel), which
// the WPAD driver's reports would trigger on a console; the retrace fires it for every present channel instead.
void KPAD_HLE_FireSamplingCallbacks(CpuContext* ctx);
extern std::atomic<uint32_t> g_gxBringupCounters[7];  // 5 KPADReadEx, 6 WPADProbe
uint32_t Audio_HLE_ReadAiRegister(uint32_t addr, size_t size) noexcept;
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop, the Aurora frame-worker wait
// callback). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
// Bytes written so far to OTHERM_AUDIO_DUMP (0 without it).
uint64_t Audio_HLE_DumpedBytes();
// Options pauses or resumes the game: holds the output device and leaves the paused time out of the audio clock.
void Audio_HLE_SetMenuPause(bool paused);
// Deterministic mode: the virtual tick at which the AI DMA block in flight completes, or
// DetClock::kNever while audio DMA is stopped.
uint64_t Audio_HLE_DetNextEventTicks();
bool OS_HLE_InterruptsEnabled() noexcept;
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address (not a stub; genuine not-yet-implemented entries live in hle/gx/gx_fatal_stubs.cpp
// and abort). The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.

#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
