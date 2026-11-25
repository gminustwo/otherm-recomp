#pragma once

// Dynamic resolution (Options > Display > Resolution: Dynamic): the render scale follows the GPU's load so the game
// keeps 60 FPS at the sharpest picture the GPU can draw in a frame's time, between 1x and the chosen highest scale.
// It needs the GPU's load (gpu_load.h: Linux with an AMD GPU, the Steam Deck among them); elsewhere it stays at Auto.
namespace DynamicResolution {

// Once per presented frame, on the game thread. `hold`: Options is up or fast-forward runs (the load says nothing
// about normal play then), so no step is taken.
void OnPresentedFrame(bool hold);

bool Enabled();
// Turns it on (from the screen's own size) or off (back to the fixed resolution_multiplier); saved in Config.toml.
void SetEnabled(bool enabled, float fixedScale);
float Max();
void SetMax(float scale);
// The scale it renders at now (0: Auto, before its first step or where the load is not reported).
float CurrentScale();
// The GPU's load is reported here, so it can work.
bool Measurable();

}  // namespace DynamicResolution
