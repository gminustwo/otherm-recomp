// Dynamic resolution: keeps 60 FPS first, then the sharpest picture the GPU can draw in a frame's time.
//
// Measured on a Steam Deck in the player's quick.state (NOTES.md 2026-11-24): the GPU was 21% busy at 1x, 29% at Auto
// (1280x792), 39% at 2x, 68% at 3x, and 98% at its top clock at 4.5x, where the game fell to 50-58 FPS. A fixed
// resolution is either too low for light scenes or too high for heavy ones; this follows the scene instead.
//
// Once a second (OnPresentedFrame), from the GPU's load (gpu_load.h: percent busy and the clock it ran at, so the
// load at the GPU's top clock is busy * clock / top clock):
// - Down at once when frames are at risk: the GPU 95% busy at 85% or more of its top clock (no clock left to raise),
//   or the frame rate under 57 with the GPU 85% busy at any clock (a power or clock limit counts too). As far down as
//   the shortfall says (from 50 FPS: 50/60 of the pixels, less 15%), at least one step. At low clocks the Deck's
//   one-second busy readings jump (3% to 94% within seconds at 2x, 60 FPS throughout), so busy alone is not enough.
// - Up one step when the next one would still leave the GPU 20% spare at its top clock, judged by the heaviest
//   second at this step in the last minute (a light pause screen or corridor does not count on its own; seconds at
//   lower steps overstate the load per pixel, whose fixed share is larger there), three seconds running, at 60 FPS,
//   and no sooner than 4 s after the last step.
// - A step that had to be left within 15 s of climbing to it is not tried again for 30 s, then 60, 120... up to
//   10 min: no see-saw between two steps in a scene that sits between them.
// - 2 s after a step nothing is judged (the busy reading still covers the old size).
// Steps are fixed render scales (480 lines each); the render target changes size at each (aurora resize_swapchain
// keeps the swapchain when only the render size changes); a step costs one frame (~20 ms, one presentation slot
// dropped on the Deck).
#include "dynamic_resolution.h"

#include "display_settings.h"
#include "gpu_load.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>

namespace DynamicResolution {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<float, 13> kSteps{1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f, 6.0f, 8.0f};
constexpr double kTargetLoad = 0.80;  // the GPU's load at its top clock a step up may reach
constexpr auto kSettle = std::chrono::seconds(2);
constexpr auto kUpInterval = std::chrono::seconds(4);
constexpr int kUpStreak = 3;
constexpr auto kPeakWindow = std::chrono::seconds(60);

struct State {
    bool enabled = RuntimeConfigFile::DynamicResolution(false);
    float max = RuntimeConfigFile::DynamicResolutionMax(4.5f);
    int step = -1;  // index into kSteps; -1: still at Auto
    Clock::time_point lastEval{}, lastChange{}, settleUntil{};
    std::array<Clock::time_point, kSteps.size()> blockedUntil{};
    std::array<int, kSteps.size()> blockSeconds{};
    int lastUpStep = -1;
    Clock::time_point lastUpTime{};
    int upStreak = 0;
    // The load per unit of scale squared (pixels), each judged second: its largest in the last minute stands for the
    // heaviest recent scene.
    struct Second {
        Clock::time_point at;
        int step;
        double loadPerPixel;
    };
    std::deque<Second> seconds;
} g_state;

int HighestStep(float max) {
    int index = 0;
    for (int i = 0; i < int(kSteps.size()); ++i)
        if (kSteps[i] <= max + 0.001f) index = i;
    return index;
}

// The step nearest the picture's current height (pixels, so in log space): where Auto put it.
int StepNearHeight(uint32_t height) {
    const double scale = std::max(1.0, height / 480.0);
    int best = 0;
    for (int i = 0; i < int(kSteps.size()); ++i)
        if (std::fabs(std::log(kSteps[i] / scale)) < std::fabs(std::log(kSteps[best] / scale))) best = i;
    return best;
}

void Request(float scale) {
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.renderScale = scale;
    DisplaySettings::Request(settings);
}

void Apply(int step, const char* why, int busy, double clock, double fps, Clock::time_point now) {
    State& s = g_state;
    RT_LOGF(RT_TAG_RUNTIME, "[dynres] %gx -> %gx: %s (GPU %d%% busy at %.0f%% of its top clock, %.0f FPS)\n",
            s.step >= 0 ? kSteps[static_cast<size_t>(s.step)] : 0.0f, kSteps[static_cast<size_t>(step)], why, busy,
            clock * 100.0, fps);
    s.step = step;
    s.lastChange = now;
    s.settleUntil = now + kSettle;
    s.upStreak = 0;
    Request(kSteps[static_cast<size_t>(step)]);
}

void Evaluate(Clock::time_point now) {
    State& s = g_state;
    const GpuLoad::Reading reading = GpuLoad::Latest();
    if (!reading.valid) return;  // nothing to go by: stays at Auto
    AuroraPresentTiming timing{};
    aurora_get_present_timing(&timing);
    if (timing.sampleCount == 0) return;
    const double fps = timing.framesPerSecond;
    const int busy = reading.busy1s;
    const double load = busy / 100.0 * reading.clockFraction;
    const int top = HighestStep(s.max);

    if (s.step < 0) {  // the first step: the one nearest the screen's own size (Auto), within the highest allowed
        uint32_t width = 0, height = 0;
        AuroraGetPictureSize(&width, &height);
        int first = StepNearHeight(height);
        // OTHERM_TEST_DYNRES_START=<scale> (tests): start at that scale instead, e.g. 4.5 to watch it come down.
        if (const char* start = std::getenv("OTHERM_TEST_DYNRES_START")) first = StepNearHeight(
            static_cast<uint32_t>(std::atof(start) * 480.0));
        Apply(std::min(first, top), "start near the screen's size", busy, reading.clockFraction, fps, now);
        return;
    }
    if (s.step > top) {
        Apply(top, "highest step lowered", busy, reading.clockFraction, fps, now);
        return;
    }
    if (now < s.settleUntil) return;

    const double scale = kSteps[static_cast<size_t>(s.step)];
    if (busy >= 5) s.seconds.push_back({now, s.step, load / (scale * scale)});
    while (!s.seconds.empty() && now - s.seconds.front().at > kPeakWindow) s.seconds.pop_front();

    // Down: the GPU has no time left, or frames are already missed while it is nearly full.
    const bool short60 = fps < 57.0;
    const bool full = busy >= 95 && reading.clockFraction >= 0.85;
    if ((full || (short60 && busy >= 85)) && s.step > 0) {
        const double keep = short60 ? std::clamp(fps / 60.0 * 0.85, 0.30, 0.85) : 0.80;  // share of the pixels
        const double wanted = scale * std::sqrt(keep);
        int next = s.step - 1;
        while (next > 0 && kSteps[static_cast<size_t>(next)] > wanted) --next;
        if (s.lastUpStep == s.step && now - s.lastUpTime < std::chrono::seconds(15)) {
            int& seconds = s.blockSeconds[static_cast<size_t>(s.step)];
            seconds = seconds == 0 ? 30 : std::min(seconds * 2, 600);
            s.blockedUntil[static_cast<size_t>(s.step)] = now + std::chrono::seconds(seconds);
            RT_LOGF(RT_TAG_RUNTIME, "[dynres] %gx did not hold: not tried again for %d s\n", scale, seconds);
        }
        Apply(next, short60 ? "frames missed" : "GPU full", busy, reading.clockFraction, fps, now);
        return;
    }

    // Up: one step, when the heaviest recent second would still fit there with room to spare.
    const int up = s.step + 1;
    if (up > top || fps < 59.0 || now < s.blockedUntil[static_cast<size_t>(up)]) {
        s.upStreak = 0;
        return;
    }
    double peak = 0.0;
    int samples = 0;
    for (const auto& second : s.seconds) {
        if (second.step != s.step) continue;
        peak = std::max(peak, second.loadPerPixel);
        ++samples;
    }
    if (samples < kUpStreak) return;
    const double upScale = kSteps[static_cast<size_t>(up)];
    if (peak * upScale * upScale > kTargetLoad) {
        s.upStreak = 0;
        return;
    }
    if (++s.upStreak < kUpStreak || now - s.lastChange < kUpInterval) return;
    s.lastUpStep = up;
    s.lastUpTime = now;
    Apply(up, "room to spare", busy, reading.clockFraction, fps, now);
}

}  // namespace

void OnPresentedFrame(bool hold) {
    State& s = g_state;
    if (!s.enabled) return;
    GpuLoad::Start();
    const Clock::time_point now = Clock::now();
    if (now - s.lastEval < std::chrono::seconds(1)) return;
    const bool gap = s.lastEval != Clock::time_point{} && now - s.lastEval > std::chrono::seconds(3);
    s.lastEval = now;
    if (hold || gap) {  // Options, fast-forward, or a long gap (loading): judge again after a settle
        s.upStreak = 0;
        s.settleUntil = std::max(s.settleUntil, now + kSettle);
        return;
    }
    Evaluate(now);
}

bool Enabled() { return g_state.enabled; }

void SetEnabled(bool enabled, float fixedScale) {
    State& s = g_state;
    if (s.enabled == enabled) return;
    s.enabled = enabled;
    RuntimeConfigFile::SetDynamicResolution(enabled);
    s.step = -1;
    s.upStreak = 0;
    s.seconds.clear();
    s.blockedUntil = {};
    s.blockSeconds = {};
    s.lastEval = {};
    RT_LOGF(RT_TAG_RUNTIME, "[dynres] %s\n", enabled ? "on: starting at Auto" : "off");
    Request(enabled ? 0.0f : fixedScale);  // on: Auto until the first step
}

float Max() { return g_state.max; }

void SetMax(float scale) {
    g_state.max = scale;
    RuntimeConfigFile::SetDynamicResolutionMax(scale);
}

float CurrentScale() {
    const State& s = g_state;
    return s.enabled && s.step >= 0 ? kSteps[static_cast<size_t>(s.step)] : 0.0f;
}

bool Measurable() {
    GpuLoad::Start();
    return GpuLoad::Available();
}

}  // namespace DynamicResolution
