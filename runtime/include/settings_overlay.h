#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>
#include "wii_remote_input.h"

namespace settings_overlay {
// Apply persistent controller settings once Aurora has discovered host devices.
void InitializeRuntimeSettings() noexcept;
// Options > Display > Shader compilation into aurora. Called before aurora_initialize too: startup compiles the
// ubershader pipelines only when ubershaders are on.
void ApplyShaderCompilation() noexcept;
// Draw the Options menu (F1) before each Aurora present.
void HandleEvents(const AuroraEvent* events) noexcept;
void Draw() noexcept;
// The first remote's buttons, before the game reads them: HOME opens Options, which then takes the input.
void FilterMenuInput(uint32_t chan, WiiRemoteInput::KpadSample& sample) noexcept;
bool StartupScreenVisible() noexcept;
// Opens Options at the next presented frame unless it is up already (any thread: the tray icon's menu). Not on the
// start screen, which has the Options pages itself.
void RequestOptionsOpen() noexcept;
// Options is up (past the startup screen): the game waits in VIWaitForRetrace until it closes (vi.cpp).
bool GamePauseRequested() noexcept;
// The start screen before the game boots: Options in a launcher mode (START: Play, the game's files and setting them
// up from the player's disc image, Quit; then the Options pages). vi.cpp's VI_HLE_RunLauncher draws it until Play.
void BeginLauncher() noexcept;
bool LauncherActive() noexcept;
// The start screen is up only to show dropped mod zips being unpacked (mod_install.h): it plays by itself after.
void PlayWhenModsInstalled() noexcept;
void NotifyStrapInputAccepted() noexcept;
void AdvancePresentedFrame() noexcept;
// Put host controllers back to a neutral state before the process ends.
void ReleaseControllers() noexcept;
// FPS counter on or off (the Android menu sets it through JNI); saved to Config.toml.
void SetShowFps(bool show) noexcept;
bool ShowFps() noexcept;
// A frame-time graph under the FPS counter, off by default. Saved to Config.toml.
void SetFrameGraph(bool show) noexcept;
bool FrameGraph() noexcept;
// V-Sync: presents wait for the display, and the game's clock follows its refresh (hle/vi.cpp).
// Off by default. Saved to Config.toml.
void SetVSync(bool enabled) noexcept;
bool VSync() noexcept;
// Animated tiles (spinning coins, ? blocks...), on by default. Saved to Config.toml.
// Render scale: 1 = 480 lines, 0 = the window's (screen's) own resolution. Saved to Config.toml.
void SetRenderScale(float scale) noexcept;
float RenderScale() noexcept;
// DisplaySettings' aspect (4:3, 16:9, Fill), including a change not yet applied. Saved to Config.toml.
void SetAspect(int32_t mode) noexcept;
int32_t Aspect() noexcept;
} // namespace settings_overlay
