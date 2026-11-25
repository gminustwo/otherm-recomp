#include "hud_theme.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include "runtime_config.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);

namespace HudTheme {
namespace {

ImFont* FontFor(float px) {
    ImFont* font = aurora_imgui_ui_font(px);
    return font != nullptr ? font : ImGui::GetFont();
}

// The byte length of the UTF-8 sequence starting with `c`.
int Utf8Length(unsigned char c) { return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

void DrawRaw(ImDrawList* list, ImFont* font, float px, ImVec2 at, ImU32 col, const char* text, float tracking) {
    if (tracking == 0.0f) {
        list->AddText(font, px, at, col, text);
        return;
    }
    for (const char* p = text; *p != '\0';) {
        const char* next = p + Utf8Length(static_cast<unsigned char>(*p));
        for (const char* q = p + 1; q < next; ++q)
            if (*q == '\0') next = q;
        list->AddText(font, px, at, col, p, next);
        at.x += font->CalcTextSizeA(px, FLT_MAX, 0.0f, p, next).x + tracking;
        p = next;
    }
}

}  // namespace

ImVec2 Measure(float px, const char* text, float tracking) {
    ImFont* font = FontFor(px);
    ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    if (tracking != 0.0f) {
        int letters = 0;
        for (const char* p = text; *p != '\0'; ++p) letters += (static_cast<unsigned char>(*p) & 0xC0) != 0x80;
        size.x += tracking * static_cast<float>(std::max(0, letters - 1));
    }
    return size;
}

void Text(ImDrawList* list, ImVec2 pos, float px, ImU32 col, const char* text, float ax, float ay, float tracking,
          ImU32 glow) {
    if (text == nullptr || *text == '\0' || px <= 0.5f) return;
    ImFont* font = FontFor(px);
    const ImVec2 size = Measure(px, text, tracking);
    const ImVec2 at(std::floor(pos.x - size.x * ax + 0.5f), std::floor(pos.y - size.y * ay + 0.5f));
    if (glow != 0) {
        const float r = std::max(1.0f, px * 0.06f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.7854f;
            DrawRaw(list, font, px, ImVec2(at.x + std::cos(a) * r, at.y + std::sin(a) * r), glow, text, tracking);
        }
    }
    DrawRaw(list, font, px, at, col, text, tracking);
}

void ChamferPoints(ImVec2 a, ImVec2 b, float cut, ImVec2 out[6]) {
    cut = std::min(cut, std::min(b.x - a.x, b.y - a.y) * 0.5f);
    out[0] = ImVec2(a.x + cut, a.y);
    out[1] = ImVec2(b.x, a.y);
    out[2] = ImVec2(b.x, b.y - cut);
    out[3] = ImVec2(b.x - cut, b.y);
    out[4] = ImVec2(a.x, b.y);
    out[5] = ImVec2(a.x, a.y + cut);
}

void Panel(ImDrawList* list, ImVec2 a, ImVec2 b, float cut, ImU32 top, ImU32 bottom, ImU32 border, float thickness) {
    if (b.x - a.x < 2.0f || b.y - a.y < 2.0f) return;
    ImVec2 pts[6];
    ChamferPoints(a, b, cut, pts);
    const int start = list->VtxBuffer.Size;
    list->AddConvexPolyFilled(pts, 6, top);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(list, start, list->VtxBuffer.Size, a, ImVec2(a.x, b.y), top, bottom);
    if (border != 0 && thickness > 0.0f) {
        list->AddPolyline(pts, 6, Alpha(border, 40), ImDrawFlags_Closed, thickness * 4.0f);  // glow
        list->AddPolyline(pts, 6, border, ImDrawFlags_Closed, thickness);
    }
}

void Scanlines(ImDrawList* list, ImVec2 a, ImVec2 b, float step, ImU32 col) {
    step = std::max(2.0f, step);
    for (float y = std::floor(a.y) + 0.5f; y < b.y; y += step) list->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), col, 1.0f);
}

void CornerBrackets(ImDrawList* list, ImVec2 a, ImVec2 b, float length, ImU32 col, float thickness) {
    const float o = thickness * 2.5f;  // just outside the panel's edge
    const ImVec2 tr[3] = {ImVec2(b.x - length, a.y - o), ImVec2(b.x + o, a.y - o), ImVec2(b.x + o, a.y + length)};
    const ImVec2 bl[3] = {ImVec2(a.x - o, b.y - length), ImVec2(a.x - o, b.y + o), ImVec2(a.x + length, b.y + o)};
    list->AddPolyline(tr, 3, col, ImDrawFlags_None, thickness);
    list->AddPolyline(bl, 3, col, ImDrawFlags_None, thickness);
}

bool Emblem(ImDrawList* list, ImVec2 center, float size, ImU32 tint) {
    static ImTextureID texture{};
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (const auto dir = RuntimeConfigFile::ExecutableDirectory()) {
            const std::string path = RuntimeConfigFile::PathToUtf8(*dir / "startup" / "emblem.png");
            uint32_t w = 0, h = 0;
            if (uint8_t* rgba = aurora_load_png_rgba(path.c_str(), &w, &h)) {
                texture = aurora_imgui_add_texture(w, h, rgba);
                std::free(rgba);
            }
        }
    }
    if (!texture) return false;
    const float h = size * 0.5f;
    list->AddImage(texture, ImVec2(center.x - h, center.y - h), ImVec2(center.x + h, center.y + h), ImVec2(0, 0),
                   ImVec2(1, 1), tint);
    return true;
}

void ApplyImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    ImVec4* c = style.Colors;
    const ImVec4 cyan(0.31f, 0.86f, 1.00f, 1.00f);
    const ImVec4 orange(1.00f, 0.59f, 0.16f, 1.00f);
    auto withAlpha = [](ImVec4 v, float a) { v.w = a; return v; };
    c[ImGuiCol_Text] = ImVec4(0.88f, 0.96f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.51f, 0.69f, 0.80f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.08f, 0.15f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.08f, 0.15f, 0.97f);
    c[ImGuiCol_Border] = withAlpha(cyan, 0.45f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.06f, 0.17f, 0.27f, 0.85f);
    c[ImGuiCol_FrameBgHovered] = withAlpha(cyan, 0.25f);
    c[ImGuiCol_FrameBgActive] = withAlpha(cyan, 0.40f);
    c[ImGuiCol_TitleBg] = ImVec4(0.02f, 0.06f, 0.11f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.16f, 0.27f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.02f, 0.06f, 0.11f, 0.75f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.03f, 0.10f, 0.18f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.05f, 0.10f, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = withAlpha(cyan, 0.30f);
    c[ImGuiCol_ScrollbarGrabHovered] = withAlpha(cyan, 0.50f);
    c[ImGuiCol_ScrollbarGrabActive] = withAlpha(cyan, 0.70f);
    c[ImGuiCol_CheckMark] = orange;
    c[ImGuiCol_SliderGrab] = withAlpha(cyan, 0.70f);
    c[ImGuiCol_SliderGrabActive] = orange;
    c[ImGuiCol_Button] = ImVec4(0.07f, 0.22f, 0.35f, 0.90f);
    c[ImGuiCol_ButtonHovered] = withAlpha(cyan, 0.40f);
    c[ImGuiCol_ButtonActive] = withAlpha(orange, 0.70f);
    c[ImGuiCol_Header] = withAlpha(cyan, 0.22f);
    c[ImGuiCol_HeaderHovered] = withAlpha(cyan, 0.38f);
    c[ImGuiCol_HeaderActive] = withAlpha(cyan, 0.55f);
    c[ImGuiCol_Separator] = withAlpha(cyan, 0.35f);
    c[ImGuiCol_SeparatorHovered] = withAlpha(cyan, 0.60f);
    c[ImGuiCol_SeparatorActive] = orange;
    c[ImGuiCol_ResizeGrip] = withAlpha(cyan, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(cyan, 0.50f);
    c[ImGuiCol_ResizeGripActive] = orange;
    c[ImGuiCol_Tab] = ImVec4(0.05f, 0.15f, 0.25f, 0.90f);
    c[ImGuiCol_TabHovered] = withAlpha(cyan, 0.40f);
    c[ImGuiCol_TabSelected] = ImVec4(0.09f, 0.30f, 0.45f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = orange;
    c[ImGuiCol_TabDimmed] = ImVec4(0.04f, 0.10f, 0.17f, 0.90f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.07f, 0.20f, 0.31f, 1.00f);
    c[ImGuiCol_PlotLines] = cyan;
    c[ImGuiCol_PlotHistogram] = orange;
    c[ImGuiCol_TextSelectedBg] = withAlpha(cyan, 0.35f);
    c[ImGuiCol_NavCursor] = orange;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.03f, 0.07f, 0.60f);
}

}  // namespace HudTheme
