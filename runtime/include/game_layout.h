#pragma once

// Where the game picture is on screen, for the port's own menus (settings_overlay.cpp) that are laid out in the
// game's layout units: 456 units across the picture's height, origin at its centre, y up.
namespace GameLayout {

// Layout units are mapped with the origin at `centerX/centerY` (ImGui coordinates) and `scale` pixels per unit.
struct View {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float scale = 1.0f;
    // Units across: the layouts keep square pixels on a widened canvas too (display_settings.cpp), so this is
    // `scale` as well.
    float gameScaleX = 1.0f;
};

// The game picture's rectangle for the current display mode (4:3 or 16:9 pillar/letterbox, the whole window for
// Fill).
View GameView();

} // namespace GameLayout
