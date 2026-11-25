#pragma once

// The GPU's load as the system reports it, for the FPS counter's GPU LIMITED note and dynamic resolution
// (dynamic_resolution.h). Linux only, from amdgpu's sysfs files (AMD GPUs, the Steam Deck's among them):
// gpu_busy_percent, and pp_dpm_sclk for the clock it ran at. Elsewhere Available() stays false.
namespace GpuLoad {

struct Reading {
    bool valid = false;
    int busy1s = -1;          // percent busy, mean of the last second
    int busy3s = -1;          // percent busy, mean of the last three seconds
    // How much of the GPU's top speed that busy time was: the last second's mean clock / the highest clock the GPU
    // lists (1 when the clock is not reported). busy1s * clockFraction is the load the GPU would have at its top clock.
    double clockFraction = 1.0;
};

// Starts the sampling thread the first time (any thread); cheap afterwards.
void Start();
// True once a reading exists (Start() found a GPU that reports its load).
bool Available();
Reading Latest();

// Running on a Steam Deck (DMI: Valve Jupiter / Galileo).
bool IsSteamDeck();

}  // namespace GpuLoad
