#ifndef AURORA_IMGUI_H
#define AURORA_IMGUI_H

#include <imgui.h>

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8);
// The overlay menus' UI font baked at or above `pixelSize` (the largest if none is), or NULL when no system font
// was found (then ImGui's default font serves).
ImFont* aurora_imgui_ui_font(float pixelSize);

#ifdef __cplusplus
}
#endif

#endif
