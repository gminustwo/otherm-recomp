#pragma once
// The look of the port's own screens for Metroid: Other M (settings_overlay.cpp: the startup screen, Options, the
// save-state notice): dark glass panels with cut corners, cyan lines and orange accents in the style of a visor HUD,
// the port's own emblem (startup/emblem.png beside the executable, drawn by tools/make-app-icon.py) and the UI font
// aurora bakes (aurora_imgui_ui_font: Bahnschrift on Windows). Every helper draws into an ImDrawList, in pixels.

#include <imgui.h>

namespace HudTheme {

constexpr ImU32 kCyan = IM_COL32(80, 220, 255, 255);
constexpr ImU32 kOrange = IM_COL32(255, 150, 40, 255);
constexpr ImU32 kText = IM_COL32(225, 244, 255, 255);
constexpr ImU32 kTextDim = IM_COL32(130, 175, 205, 255);
constexpr ImU32 kPanelTop = IM_COL32(10, 30, 54, 238);
constexpr ImU32 kPanelBottom = IM_COL32(3, 10, 24, 242);

// `col` with its alpha replaced (0-255).
constexpr ImU32 Alpha(ImU32 col, int alpha) {
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha) << IM_COL32_A_SHIFT);
}

// Size of `text` at `px` pixels with `tracking` extra pixels between letters.
ImVec2 Measure(float px, const char* text, float tracking = 0.0f);
// `text` placed so that the point (ax, ay) of its box (0 = left / top, 0.5 = centre, 1 = right / bottom) is at `pos`.
// A non-zero `glow` draws a soft halo of that colour behind it.
void Text(ImDrawList* list, ImVec2 pos, float px, ImU32 col, const char* text, float ax = 0.0f, float ay = 0.5f,
          float tracking = 0.0f, ImU32 glow = 0);
// The six corners of a box from `a` (top left) to `b` with its top-left and bottom-right corners cut by `cut`.
void ChamferPoints(ImVec2 a, ImVec2 b, float cut, ImVec2 out[6]);
// A panel of that shape: a vertical gradient from `top` to `bottom`, a `border` line and a faint outer glow.
void Panel(ImDrawList* list, ImVec2 a, ImVec2 b, float cut, ImU32 top, ImU32 bottom, ImU32 border, float thickness);
// Horizontal lines every `step` pixels: the visor's scan lines.
void Scanlines(ImDrawList* list, ImVec2 a, ImVec2 b, float step, ImU32 col);
// L-shaped brackets on the uncut corners (top right, bottom left) of a ChamferPoints box.
void CornerBrackets(ImDrawList* list, ImVec2 a, ImVec2 b, float length, ImU32 col, float thickness);
// The emblem, `size` pixels square, centred on `center` and tinted `tint`. False when the picture is missing.
bool Emblem(ImDrawList* list, ImVec2 center, float size, ImU32 tint);
// The developer windows' (ImGui) colours, in the same scheme. Once is enough.
void ApplyImGuiStyle();

}  // namespace HudTheme
