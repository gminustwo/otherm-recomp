#include <filesystem>
#include "runtime_log.h"
// gx_pixel.cpp - Pixel Processing, Blending, and Fog
#include "gx_internal.h"

// ============================================================================
// Blend Mode
// ============================================================================

extern "C" void GX__SetBlendMode_800f3890(uint32_t t, uint32_t s, uint32_t d, uint32_t op) {
    if (OthermDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] blend %u %u %u %u\n", t, s, d, op);
    GXSetBlendMode(static_cast<GXBlendMode>(t), static_cast<GXBlendFactor>(s),
                   static_cast<GXBlendFactor>(d), static_cast<GXLogicOp>(op));
}
PPC_NATIVE_OVERRIDE_VOID(800F3890, GX__SetBlendMode_800f3890, (uint32_t t, uint32_t s, uint32_t d, uint32_t op), (t, s, d, op));

extern "C" void GX__SetColorUpdate_800f38e0(uint32_t en) {
    GXSetColorUpdate(static_cast<GXBool>(en));
}
PPC_NATIVE_OVERRIDE_VOID(800F38E0, GX__SetColorUpdate_800f38e0, (uint32_t en), (en));

extern "C" void GX__SetAlphaUpdate_800f3910(uint32_t en) {
    GXSetAlphaUpdate(static_cast<GXBool>(en));
}
PPC_NATIVE_OVERRIDE_VOID(800f3910, GX__SetAlphaUpdate_800f3910, (uint32_t en), (en));

// ============================================================================
// Z Buffer
// ============================================================================

extern "C" void GX__SetZMode_800F3940(uint32_t ce, uint32_t f, uint32_t ue) {
    if (OthermDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] zmode %u %u %u\n", ce, f, ue);
    GXSetZMode(static_cast<GXBool>(ce), static_cast<GXCompare>(f), static_cast<GXBool>(ue));
}
PPC_NATIVE_OVERRIDE_VOID(800F3940, GX__SetZMode_800F3940, (uint32_t ce, uint32_t f, uint32_t ue), (ce, f, ue));

extern "C" void GX__SetZCompLoc_800F3980(uint32_t bt) {
    GXSetZCompLoc(static_cast<GXBool>(bt));
}
PPC_NATIVE_OVERRIDE_VOID(800F3980, GX__SetZCompLoc_800F3980, (uint32_t bt), (bt));

// ============================================================================
// Pixel Format and Dither
// ============================================================================

extern "C" void GX__SetPixelFmt_800F39B0(uint32_t pf, uint32_t zf) {
    GXSetPixelFmt(static_cast<GXPixelFmt>(pf), static_cast<GXZFmt16>(zf));
}
PPC_NATIVE_OVERRIDE_VOID(800F39B0, GX__SetPixelFmt_800F39B0, (uint32_t pf, uint32_t zf), (pf, zf));

extern "C" void GX__SetDither_800F3A60(uint32_t d) {
    GXSetDither(static_cast<GXBool>(d));
}
PPC_NATIVE_OVERRIDE_VOID(800F3A60, GX__SetDither_800F3A60, (uint32_t d), (d));

extern "C" void GX__SetDstAlpha_800f3ab0(uint32_t en, uint32_t a) {
    GXSetDstAlpha(static_cast<GXBool>(en), static_cast<u8>(a));
}
PPC_NATIVE_OVERRIDE_VOID(800f3ab0, GX__SetDstAlpha_800f3ab0, (uint32_t en, uint32_t a), (en, a));

// ============================================================================
// Fog and Alpha Compare
// ============================================================================

extern "C" void GX__SetFog_800f33f0(uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp) {
    GXSetFog((GXFogType)t, sz, ez, nz, fz, DecodeGxColor(Memory::Read32(cp)));
}
PPC_NATIVE_OVERRIDE_VOID(800f33f0, GX__SetFog_800f33f0, (uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp), (t, sz, ez, nz, fz, cp));

extern "C" void GX__SetAlphaCompare_800F3190(uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1) {
    if (OthermDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] alphacmp %u %u %u %u %u\n", c0, r0, op, c1, r1);
    g_alphaCompareValid = true;
    static const bool s_alphaExperiment = std::filesystem::exists("alphatest_off.txt");  // bring-up experiment
    if (s_alphaExperiment && c0 == 6 && r0 == 128) { c0 = 7; op = 1; c1 = 7; }
    GXSetAlphaCompare((GXCompare)c0, (u8)r0, (GXAlphaOp)op, (GXCompare)c1, (u8)r1);
}
PPC_NATIVE_OVERRIDE_VOID(800F3190, GX__SetAlphaCompare_800F3190, (uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1), (c0, r0, op, c1, r1));

extern "C" void GX__SetZTexture_800f31d0(uint32_t op, uint32_t f, uint32_t b) { GXSetZTexture((GXZTexOp)op, (GXTexFmt)f, b); }
PPC_NATIVE_OVERRIDE_VOID(800f31d0, GX__SetZTexture_800f31d0, (uint32_t op, uint32_t f, uint32_t b), (op, f, b));

// ============================================================================
// Culling and Clipping
// ============================================================================

extern "C" void GX__SetCullMode_800f0220(uint32_t m) {
    if (OthermDumpFrame()) RT_LOGF(RT_TAG_GX, "[drawdump] cull %u\n", m);
    GXSetCullMode(static_cast<GXCullMode>(m));
}
PPC_NATIVE_OVERRIDE_VOID(800f0220, GX__SetCullMode_800f0220, (uint32_t m), (m));

extern "C" void GX__SetCoPlanar_800f0250(uint32_t en) { GXSetCoPlanar((GXBool)en); }
PPC_NATIVE_OVERRIDE_VOID(800f0250, GX__SetCoPlanar_800f0250, (uint32_t en), (en));

extern "C" void GX__SetClipMode_800f4290(uint32_t m) { GXSetClipMode((GXClipMode)m); }
PPC_NATIVE_OVERRIDE_VOID(800f4290, GX__SetClipMode_800f4290, (uint32_t m), (m));
