// Metroid: Other M: Options > Display > Bloom ([video] bloom in Config.toml), the game's glow around bright lights:
// On (as the game draws it, at the internal resolution), Native resolution, Blurred, or Off. The last three are
// Dolphin's "Native Resolution Bloom", "Bloom Blurred" and "Bloom Removal" graphics mods (GitHub issue #17; Dolphin's
// Sys/Load/GraphicMods/Metroid Other M names the bloom's 160x120 and 80x60 RGB565 copies).
//
// The bloom pass is rel_80813E4C (the post-process pass rel_80800AD0 runs it as its mode 0; 0x808ACE14 calls it too).
// It bright-passes the scene into the EFB's top-left quarter, blurs that through 160x120 / 80x60 RGB565 copies (made
// through rel_80801CF4, called at 0x8081460C, 0x808146E4 and, for the result at 0x0137EF60, 0x80814BA0; and inside
// its blur helper, reached from 0x80814734), puts the quarter back, and ends by adding the glow over the scene:
// GXSetBlendMode(BLEND, ONE, ONE, CLEAR) at 0x80814E00, then two quads drawn from the result (NOTES.md 2026-11-21,
// 2026-11-26).
//  - Off: the code tap at 0x80814DF4 (li r4, 1: the source factor) gives GX_BL_ZERO, so the two quads leave the
//    picture as it is.
//  - Native resolution: every copy the pass makes is made at its size on the Wii (160x120, 80x60), not scaled with
//    the internal resolution, so the glow is as soft and wide as on the console (Dolphin warns of some shimmer).
//  - Blurred: the result the quads draw from (the copy called at 0x80814BA0) is made at the internal resolution and
//    box-blurred by the resolution scale, horizontally then vertically, as Dolphin's mod blurs it where it is drawn.
// A copy is the bloom's when it is RGB565 and a return address in the guest's call chain lies inside rel_80813E4C:
// the pass also copies the whole scene (640x480 RGBA8, called at 0x80813E90) to put the quarter back from, which stays as
// it is (made smaller, the top-left quarter of the screen came back blurred).
#include "code_patches.h"
#include "abi_bridge.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <aurora/gfx.h>

#include <atomic>
#include <cstdint>
#include <set>
#include <tuple>

namespace {
constexpr uint32_t kGlowSourceFactorTap = 0x80814DF4u;  // after r4
constexpr uint32_t kBlendZero = 0;                      // GX_BL_ZERO
constexpr uint32_t kBloomPassStart = 0x80813E4Cu;       // rel_80813E4C
constexpr uint32_t kBloomPassEnd = 0x80814E80u;         // the next function
constexpr uint32_t kResultCopyReturn = 0x80814BA4u;     // after "bl rel_80801CF4" at 0x80814BA0
constexpr uint32_t kFormatRGB565 = 4;                   // GX_TF_RGB565

std::atomic<int> g_mode{RuntimeConfigFile::kBloomOn};
}  // namespace

// Before the game starts (main.cpp) and when Options changes it (settings_overlay.cpp).
void OthermBloomApply()
{
    const int mode = RuntimeConfigFile::BloomMode();
    g_mode.store(mode, std::memory_order_relaxed);
    const bool off = mode == RuntimeConfigFile::kBloomOff;
    if (!CodePatches::SetPortTap(kGlowSourceFactorTap, true, 4, off, kBlendZero) && off) {
        RT_LOGF(RT_TAG_RUNTIME, "[bloom] off is not possible: the translation has no code tap at 0x80814DF4 "
                                "(project/code_taps.txt; translate again)\n");
    }
}

// GXCopyTex (hle/gx/gx_copy.cpp), on the game's thread: how aurora makes this copy.
extern "C" AuroraCopyStyle OthermBloomCopyStyle(uint32_t dest, uint32_t width, uint32_t height, uint32_t format)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != RuntimeConfigFile::kBloomNative && mode != RuntimeConfigFile::kBloomBlurred) return AURORA_COPY_AS_USUAL;
    if (format != kFormatRGB565) return AURORA_COPY_AS_USUAL;
    const CpuContext* cpu = TryGetCpuContext();
    if (cpu == nullptr) return AURORA_COPY_AS_USUAL;
    // The guest's return addresses: lr, then the saved LR of each frame up the back chain.
    uint32_t site = 0;
    bool result = false;
    uint32_t sp = cpu->gpr[1];
    for (int depth = 0; depth < 4 && site == 0; ++depth) {
        uint32_t back = 0, ret = 0;
        if (!Memory::TryRead32(sp, back) || back == 0 || !Memory::TryRead32(back + 4, ret)) break;
        if (ret >= kBloomPassStart && ret < kBloomPassEnd) {
            site = ret;
            result = ret == kResultCopyReturn;
        }
        sp = back;
    }
    if (site == 0) return AURORA_COPY_AS_USUAL;
    const AuroraCopyStyle style = mode == RuntimeConfigFile::kBloomNative ? AURORA_COPY_NATIVE_SIZE
                                  : result                                 ? AURORA_COPY_BLURRED
                                                                           : AURORA_COPY_AS_USUAL;
    static std::set<std::tuple<int, uint32_t, uint32_t>> logged;  // each copy once per mode
    if (logged.size() < 32 && logged.insert({mode, site, dest}).second) {
        RT_LOGF(RT_TAG_RUNTIME, "[bloom] copy %ux%u to %08X from 0x%08X: %s\n", width, height, dest, site - 4,
                style == AURORA_COPY_NATIVE_SIZE ? "native size"
                : style == AURORA_COPY_BLURRED   ? "blurred"
                                                 : "as usual");
    }
    return style;
}
