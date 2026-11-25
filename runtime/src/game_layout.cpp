#include "game_layout.h"

#include "display_settings.h"

#include <imgui.h>

#include <algorithm>

namespace GameLayout {

View GameView() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    View view;
    float w = display.x, h = display.y;
    const auto settings = DisplaySettings::Current();
    const bool wide = DisplaySettings::GameWidescreen();
    float aspect = 0.0f;  // 0 = fills the window
    if (!wide || settings.aspect == DisplaySettings::kAspect4x3) {
        aspect = 4.0f / 3.0f;
    } else if (settings.aspect == DisplaySettings::kAspectCinematic) {
        aspect = DisplaySettings::kRatioCinematic;
    } else if (settings.aspect == DisplaySettings::kAspect16x9 || w / std::max(h, 1.0f) <= 16.0f / 9.0f) {
        aspect = 16.0f / 9.0f;
    }
    if (aspect > 0.0f) {
        if (w / std::max(h, 1.0f) > aspect) {
            w = h * aspect;
        } else {
            h = w / aspect;
        }
    }
    view.centerX = display.x / 2;
    view.centerY = display.y / 2;
    view.scale = h / 456.0f;
    view.gameScaleX = view.scale;
    return view;
}

} // namespace GameLayout
