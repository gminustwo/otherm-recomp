#pragma once
// Save states for the windowed game (standalone_save_state.cpp): requested from the keyboard, taken at
// the scheduler's idle loop after a VI retrace.
#include "ppc_runtime.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace StandaloneState {

enum class Slot : int { Quick = 0 };

void RequestSave();
void RequestLoad();
// A save or load waits for the scheduler's idle loop. In 3D scenes the game rarely idles there: it waits
// for the next retrace in the present (vi.cpp, paced GXCopyDisp) instead, so while a request is pending
// the present skips that wait and the game waits in VIWaitForRetrace, which idles.
bool RequestPending() noexcept;

// vi.cpp, at the end of each retrace: arms the next idle-loop pass when a request (or a probe) is due.
void OnRetraceEnd() noexcept;

// os_scheduler.cpp, at the top of each idle-loop pass (no guest thread is runnable there).
extern std::atomic<bool> g_armed;
extern uint32_t g_idlePasses;  // every idle-loop pass (diagnostics: a request that waits logs it)
inline bool Armed() noexcept {
    ++g_idlePasses;
    return g_armed.load(std::memory_order_relaxed);
}
void AtIdle(CpuContext* cpu);

// The quick slot's file (Options > Game shows when it was written); it may not exist yet.
std::filesystem::path QuickSlotPath();

// settings_overlay.cpp: the last outcome ("State saved", "Couldn't load the state: ..."), while it is
// still shown; empty otherwise.
std::string Notice();

}  // namespace StandaloneState
