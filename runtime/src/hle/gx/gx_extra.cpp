// gx_extra.cpp - GX entry points the other gx_*.cpp files don't cover: GXAbortFrame and the GXPoke* modes.
#include "gx_internal.h"
#include "runtime_log.h"

#include <dolphin/gx/GXGet.h>

namespace {
void LogOnce(bool& logged, const char* what)
{
    if (!logged) {
        logged = true;
        RT_LOGF(RT_TAG_GX, "%s\n", what);
    }
}
} // namespace

// GXAbortFrame asks the GPU to drop the frame in flight. Aurora submits whole frames and has nothing
// half-finished to abandon, so there is nothing to do.
extern "C" void GX__AbortFrame_800ef5e0() {
    static bool logged = false;
    LogOnce(logged, "GXAbortFrame: no partial GPU frame to abandon (no-op)");
}
PPC_NATIVE_OVERRIDE_VOID(800ef5e0, GX__AbortFrame_800ef5e0, (), ());

// GXPoke*: modes for CPU writes straight into the embedded frame buffer (GXPokeARGB/GXPokeZ). On
// hardware they program pixel-engine registers; the runtime does not emulate CPU-to-EFB access, so
// the modes have no consumer. Accept them (Aurora declares but does not implement these).
#define OTHERM_GX_POKE_NOOP(addr, name, params, uses)                                        \
    extern "C" void GX__##name##_##addr params {                                            \
        static bool logged = false;                                                          \
        (void)(uses);                                                                        \
        LogOnce(logged, "GX" #name ": CPU-to-EFB poke modes are not emulated (no-op)");     \
    }

OTHERM_GX_POKE_NOOP(800ef960, PokeAlphaMode, (uint32_t func, uint32_t threshold), (func, threshold))
PPC_NATIVE_OVERRIDE_VOID(800ef960, GX__PokeAlphaMode_800ef960, (uint32_t func, uint32_t threshold), (func, threshold));
OTHERM_GX_POKE_NOOP(800ef970, PokeAlphaRead, (uint32_t mode), (mode))
PPC_NATIVE_OVERRIDE_VOID(800ef970, GX__PokeAlphaRead_800ef970, (uint32_t mode), (mode));
OTHERM_GX_POKE_NOOP(800ef990, PokeAlphaUpdate, (uint32_t enable), (enable))
PPC_NATIVE_OVERRIDE_VOID(800ef990, GX__PokeAlphaUpdate_800ef990, (uint32_t enable), (enable));
OTHERM_GX_POKE_NOOP(800ef9b0, PokeBlendMode, (uint32_t type, uint32_t src, uint32_t dst, uint32_t op), (type, src, dst, op))
PPC_NATIVE_OVERRIDE_VOID(800ef9b0, GX__PokeBlendMode_800ef9b0, (uint32_t type, uint32_t src, uint32_t dst, uint32_t op),
                         (type, src, dst, op));
OTHERM_GX_POKE_NOOP(800efa10, PokeColorUpdate, (uint32_t enable), (enable))
PPC_NATIVE_OVERRIDE_VOID(800efa10, GX__PokeColorUpdate_800efa10, (uint32_t enable), (enable));
OTHERM_GX_POKE_NOOP(800efa30, PokeDstAlpha, (uint32_t enable, uint32_t alpha), (enable, alpha))
PPC_NATIVE_OVERRIDE_VOID(800efa30, GX__PokeDstAlpha_800efa30, (uint32_t enable, uint32_t alpha), (enable, alpha));
OTHERM_GX_POKE_NOOP(800efa50, PokeDither, (uint32_t dither), (dither))
PPC_NATIVE_OVERRIDE_VOID(800efa50, GX__PokeDither_800efa50, (uint32_t dither), (dither));
OTHERM_GX_POKE_NOOP(800efa70, PokeZMode, (uint32_t compareEnable, uint32_t func, uint32_t updateEnable),
                   (compareEnable, func, updateEnable))
PPC_NATIVE_OVERRIDE_VOID(800efa70, GX__PokeZMode_800efa70, (uint32_t compareEnable, uint32_t func, uint32_t updateEnable),
                         (compareEnable, func, updateEnable));

#undef OTHERM_GX_POKE_NOOP
