#include <xxhash.h>
#include <algorithm>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <mutex>
// gx_init.cpp - GX Initialization and FIFO Management
#include "gx_internal.h"
#include "runtime_log.h"
#include "save_state.h"

#include <cstdio>

// Forward declarations for HLE functions used by GXInit
extern "C" void __GX__FifoInit_800ee610();
extern "C" void __GX__PEInit_800efc40();
extern "C" void __GX__SetTmemConfig_800F2510(uint32_t mode);
extern "C" void GX__InitFifoBase_800edfb0(uint32_t fa, uint32_t ba, uint32_t s);
// GX__SetCPUFifo_800ee140 is already declared in gx_internal.h.
extern "C" void GX__SetGPFifo_800ee320(uint32_t fa);
extern "C" GXFifoObj* GXInit(void* base, u32 size);

// ============================================================================
// GXInit
// ============================================================================


/**
 * GXInit HLE - Initialize the Graphics subsystem
 * This replaces the translated function that writes to MMIO addresses.
 */
extern "C" uint32_t GX__Init_800ed080(uint32_t fifoBase, uint32_t fifoSize)
{
    constexpr uint32_t kFifoObjAddr = 0x802D7BA0u;  // FifoObj
    constexpr uint32_t kGXDataAddr = 0x802D7C20u;   // gxData: the initial value of __GXData (0x803335D8) in Other M's main.dol
    constexpr uint32_t kGXDataSize = 0x600u;

    GXInit(GuestToHostPtr(fifoBase, fifoSize), fifoSize);

    // Initialize GXData structure in guest memory
    try {
        for (uint32_t offset = 0; offset < kGXDataSize; offset += 4) {
            Memory::Write32(kGXDataAddr + offset, 0);
        }
        Memory::Write8(kGXDataAddr + 0x5f8, 0);
        Memory::Write8(kGXDataAddr + 0x5f9, 1);
        Memory::Write8(kGXDataAddr + 0x5fa, 1);
        Memory::Write32(kGXDataAddr + 0x5e4, 0);
        Memory::Write32(kGXDataAddr + 0x5e8, 0);
        Memory::Write32(kGXDataAddr + 0x5fc, 0);
        Memory::Write32(kGXDataPtrAddr, kGXDataAddr);
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOGF(RT_TAG_GX, "Memory access violation during GXData init: %s\n", e.what());
    }

    __GX__FifoInit_800ee610();
    GX__InitFifoBase_800edfb0(kFifoObjAddr, fifoBase, fifoSize);
    GX__SetCPUFifo_800ee140(kFifoObjAddr);
    GX__SetGPFifo_800ee320(kFifoObjAddr);
    __GX__PEInit_800efc40();

    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            Memory::Write32(gd + 0x254, 0);
            Memory::Write32(gd + 0x174, 0x0f0000ff);
            Memory::Write32(gd + 0x7c, 0x22000000);
            Memory::Write32(gd + 0x170, 0x27000000);
            const uint32_t baseRegs[] = {0x30, 0x38};
            for (int i = 0; i < 2; ++i) {
                uint32_t r = baseRegs[i];
                Memory::Write32(gd + 0x108 + i * 0x10, r << 24);
                Memory::Write32(gd + 0x128 + i * 0x10, (r + 1) << 24);
                Memory::Write32(gd + 0x10c + i * 0x10, (r + 2) << 24);
                Memory::Write32(gd + 0x12c + i * 0x10, (r + 3) << 24);
                Memory::Write32(gd + 0x110 + i * 0x10, (r + 4) << 24);
                Memory::Write32(gd + 0x130 + i * 0x10, (r + 5) << 24);
                Memory::Write32(gd + 0x114 + i * 0x10, (r + 6) << 24);
                Memory::Write32(gd + 0x134 + i * 0x10, (r + 7) << 24);
            }
        }
    } catch (...) {}

    __GX__SetTmemConfig_800F2510(2);

    RT_LOGF(RT_TAG_GX, "GX initialized, FIFO at 0x%08X (base=0x%08X size=0x%08X)\n",
            kFifoObjAddr, fifoBase, fifoSize);

    return kFifoObjAddr;
}
PPC_NATIVE_OVERRIDE(800ed080, GX__Init_800ed080, uint32_t, (uint32_t fifoBase, uint32_t fifoSize), (fifoBase, fifoSize));

// ============================================================================
// FIFO Management
// ============================================================================

extern "C" void GX__InitFifoBase_800edfb0(uint32_t fa, uint32_t ba, uint32_t s) { GXInitFifoBase((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj)), GuestToHostPtr(ba, s), s); }

extern "C" void GX__SetCPUFifo_800ee140(uint32_t fa) { GXSetCPUFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(800ee140, GX__SetCPUFifo_800ee140, (uint32_t fa), (fa));

extern "C" void GX__SetGPFifo_800ee320(uint32_t fa) { GXSetGPFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(800ee320, GX__SetGPFifo_800ee320, (uint32_t fa), (fa));

extern "C" void __GX__FifoInit_800ee610()
{
    constexpr uint32_t kCpInterruptId = 0x11u;
    constexpr uint32_t kCpInterruptMask = 0x4000u;
    constexpr uint32_t kCpInterruptHandlerAddr = 0x800ede50u;
    constexpr uint32_t kGxCurrentThreadPtrAddr = 0x803325bcu;  // __GXCurrentThread
    constexpr uint32_t kGxThreadQueueAddr = 0x803325b8u;       // GXOverflowSuspendInProgress
    constexpr uint32_t kCpuFifoObjAddr = 0x802D8244u;  // CPUFifo
    constexpr uint32_t kGpFifoObjAddr = 0x802D8220u;   // GPFifo
    constexpr size_t kFifoObjSize = 0x24u;
    constexpr uint32_t kFifoWrapFlagAddr = 0x803325a8u;   // CPUFifoReady
    constexpr uint32_t kFifoWrapFlag2Addr = 0x803325a9u;  // GPFifoReady

    __OSSetInterruptHandler_801a65f8_hle(kCpInterruptId, kCpInterruptHandlerAddr);
    __OSUnmaskInterrupts_801a69bc_hle(kCpInterruptMask);

    try { Memory::Write32(kGxCurrentThreadPtrAddr, OS__GetCurrentThread_801a98b0_hle()); } catch (const ::Memory::AccessViolation&) {}
    try { Memory::Write32(kGxThreadQueueAddr, 0); } catch (const ::Memory::AccessViolation&) {}

    auto zeroBlock = [](uint32_t addr, size_t sizeBytes) {
        for (size_t offset = 0; offset < sizeBytes; offset += 4) {
            Memory::Write32(addr + static_cast<uint32_t>(offset), 0);
        }
    };

    try { zeroBlock(kCpuFifoObjAddr, kFifoObjSize); } catch (const ::Memory::AccessViolation&) {}
    try { zeroBlock(kGpFifoObjAddr, kFifoObjSize); } catch (const ::Memory::AccessViolation&) {}

    try { Memory::Write8(kFifoWrapFlagAddr, 0); } catch (const ::Memory::AccessViolation&) {}
    try { Memory::Write8(kFifoWrapFlag2Addr, 0); } catch (const ::Memory::AccessViolation&) {}
}

extern "C" void __GX__PEInit_800efc40()
{
    constexpr uint32_t kPeTokenInterruptId = 0x12u;
    constexpr uint32_t kPeFinishInterruptId = 0x13u;
    constexpr uint32_t kPeTokenInterruptMask = 0x1000u;
    constexpr uint32_t kPeFinishInterruptMask = 0x2000u;
    constexpr uint32_t kPeTokenHandlerAddr = 0x800efae0;
    constexpr uint32_t kPeFinishHandlerAddr = 0x800efbc0;
    constexpr uint32_t kPeThreadQueueAddr = 0x803325c8u;  // FinishQueue

    __OSSetInterruptHandler_801a65f8_hle(kPeTokenInterruptId, kPeTokenHandlerAddr);
    __OSSetInterruptHandler_801a65f8_hle(kPeFinishInterruptId, kPeFinishHandlerAddr);
    __OSUnmaskInterrupts_801a69bc_hle(kPeTokenInterruptMask);
    __OSUnmaskInterrupts_801a69bc_hle(kPeFinishInterruptMask);

    try { Memory::Write32(kPeThreadQueueAddr, 0); } catch (const ::Memory::AccessViolation&) {}
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            const uint16_t cur = Memory::Read16(gd + 0x0Au);
            Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(cur | 0x000Fu));
        }
    } catch (const ::Memory::AccessViolation&) {}
}
PPC_NATIVE_OVERRIDE_VOID(800efc40, __GX__PEInit_800efc40, (), ());

// ============================================================================
// Save states
// ============================================================================

extern "C" void aurora_gx_get_object_ids(uint32_t* tex, uint32_t* tlut);
extern "C" void aurora_gx_set_object_ids(uint32_t tex, uint32_t tlut);

// Aurora's texture/TLUT object id counters.
void GX_HLE_SaveObjectIds(SaveState::Writer& out) {
    uint32_t tex = 0, tlut = 0;
    aurora_gx_get_object_ids(&tex, &tlut);
    out.Pod(tex);
    out.Pod(tlut);
}

void GX_HLE_LoadObjectIds(SaveState::Reader& in) {
    const uint32_t tex = in.Get<uint32_t>(), tlut = in.Get<uint32_t>();
    if (in.Ok()) aurora_gx_set_object_ids(tex, tlut);
}

// ============================================================================
// Flush
// ============================================================================

extern "C" void GX__Flush_800ef410() { GXFlush(); }
PPC_NATIVE_OVERRIDE_VOID(800ef410, GX__Flush_800ef410, (), ());
