#include <algorithm>
#include "gx_internal.h"
#include "runtime_log.h"

extern "C" void __GXSetSUTexRegs();

// ============================================================================
// FIFO Write Helpers
// ============================================================================

extern "C" void GX_HLE_FIFO_WriteFloat(float val) {
    u32 raw; std::memcpy(&raw, &val, 4);
    try { HleFifoWrite(raw, 4); } catch (...) { RT_LOGF(RT_TAG_GX, "FIFO write float failed\n"); }
}

extern "C" void GX_HLE_FIFO_Write32(uint32_t val) { HleFifoWrite(val, 4); }
extern "C" void GX_HLE_FIFO_Write16(uint16_t val) { HleFifoWrite(static_cast<u32>(val), 2); }
extern "C" void GX_HLE_FIFO_Write8(uint8_t val) { HleFifoWrite(static_cast<u32>(val), 1); }

// GXSetDrawSync (0x800ef7a0). A game's draw-sync wait can poll the PE token register through __peReg for up to
// 100 ms; no PE is emulated and __peReg was 0, so every wait timed out (3.5 s frames). Draws are consumed in
// order here, so the token is reached at once: point __peReg at a RAM block and store the token in its token
// register (+0xE).
extern "C" void OTHERM_GXSetDrawSync(uint32_t token) {
    constexpr uint32_t kOthermPeRegPtrAddr = 0x80332588u;  // __peReg: r13-0x7458, where Other M's GXPoke* functions load it (lwz at 0x800EF960)
    constexpr uint32_t kOthermPeRegBlock = 0x80002F00u;    // low-memory debugger area; no exception vectors are installed
    try {
        Memory::Write32(kOthermPeRegPtrAddr, kOthermPeRegBlock);
        Memory::Write16(kOthermPeRegBlock + 0xEu, static_cast<uint16_t>(token));
    } catch (const ::Memory::AccessViolation&) {}
}
PPC_NATIVE_OVERRIDE_VOID(800ef7a0, OTHERM_GXSetDrawSync, (uint32_t token), (token));

extern "C" void GX__FinishInterruptHandler_800efbc0() {
    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(Memory::Read16(gd + 0x0Au) | 0x0008u));
        Memory::Write8(kGxDrawDoneFlagAddr, 1);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(800efbc0, GX__FinishInterruptHandler_800efbc0, (), ());

extern "C" void GX__DrawDone_800ef860() {
    try { Memory::Write8(kGxDrawDoneFlagAddr, 0); } catch (...) {}
    GXDrawDone(); GX__FinishInterruptHandler_800efbc0();
}
PPC_NATIVE_OVERRIDE_VOID(800ef860, GX__DrawDone_800ef860, (), ());

extern "C" void GX__PixModeSync_800ef930() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
    GXPixModeSync();
}
PPC_NATIVE_OVERRIDE_VOID(800ef930, GX__PixModeSync_800ef930, (), ());

// ============================================================================
// Hardware Revision / Thread Query - No-ops
// ============================================================================

extern "C" void __GX__InitRevisionBits_800ecf50() {}
PPC_NATIVE_OVERRIDE_VOID(800ecf50, __GX__InitRevisionBits_800ecf50, (), ());

// ============================================================================
// Texture State Management - Aurora handles internally
// ============================================================================

extern "C" void __GX__SetSUTexRegs_800f23a0() {
    __GXSetSUTexRegs();
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(800f23a0, __GX__SetSUTexRegs_800f23a0, (), ());

extern "C" void __GX__SetTmemConfig_800F2510(uint32_t mode) {
    // TMEM layout configuration - Aurora manages internally
    (void)mode;
}
PPC_NATIVE_OVERRIDE_VOID(800F2510, __GX__SetTmemConfig_800F2510, (uint32_t mode), (mode));

extern "C" void __GX__FlushTextureState_800f2cf0() {
    // BP texture state flush - Aurora handles via API
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(800f2cf0, __GX__FlushTextureState_800f2cf0, (), ());

// ============================================================================
// Copy Configuration - No-ops for features Aurora doesn't use
// ============================================================================

extern "C" void GX__SetDispCopyFrame2Field_800f05e0(uint32_t f) {
    GXSetDispCopyFrame2Field(f);
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFCFFFu) | ((f & 3u) << 12));
            Memory::Write32(gd + 0x24Cu, Memory::Read32(gd + 0x24Cu) & 0xFFFFCFFFu);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(800f05e0, GX__SetDispCopyFrame2Field_800f05e0, (uint32_t f), (f));

extern "C" void GX__SetCopyClamp_800f0600(uint32_t c) {
    GXSetCopyClamp(static_cast<GXFBClamp>(c));
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            const uint32_t clamp = c & 3u;
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFFFFCu) | clamp);
            Memory::Write32(gd + 0x24Cu, (Memory::Read32(gd + 0x24Cu) & 0xFFFFFFFCu) | clamp);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(800f0600, GX__SetCopyClamp_800f0600, (uint32_t c), (c));

extern "C" void GX__ClearBoundingBox_800f0e60() {
    OthermBBoxClear();
    GXClearBoundingBox();
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 2, 0);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(800f0e60, GX__ClearBoundingBox_800f0e60, (), ());

// ============================================================================
// FIFO/State Management - No-ops
// ============================================================================

extern "C" void GX__SetDirtyState_800efcb0() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write32(gd + 0x5FCu, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(800efcb0, GX__SetDirtyState_800efcb0, (), ());

