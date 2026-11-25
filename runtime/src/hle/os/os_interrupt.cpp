// Interrupt masking/dispatch plus the EXI/IPC hardware stubs that hang off it.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "os_internal.h"

extern "C" uint32_t SetInterruptMask_8010f2d0(uint32_t mask, uint32_t enable);

namespace OsHleInternal {
std::atomic<bool> g_interrupts_enabled{true};
std::atomic<uint32_t> g_interrupt_mask{0};
} // namespace OsHleInternal

namespace {
std::once_flag interrupt_init_log_once;
std::once_flag exception_init_log_once;

inline void UpdateCurrentContextInterruptFlag(bool enabled)
{
    // Both OSDisableInterrupts and OSRestoreInterrupts land here. A failed lookup means no
    // context exists yet (normal during early boot), so we just skip the write in that case.
    uint32_t currentContext = 0;
    if (!MemoryInline::TryReadGuestScalar(kOSCurrentContextAddr, currentContext) ||
        currentContext == 0) {
        return;
    }
    const uint32_t modeFlagsAddr = currentContext + 0x1A2u;
    uint16_t modeFlags = 0;
    if (!MemoryInline::TryReadGuestScalar(modeFlagsAddr, modeFlags)) {
        return;
    }
    const uint16_t updated = enabled ? static_cast<uint16_t>(modeFlags | 0x0002u)
                                     : static_cast<uint16_t>(modeFlags & static_cast<uint16_t>(~0x0002u));
    if (updated == modeFlags) {
        return;
    }
    MemoryInline::TryWriteGuestScalar(modeFlagsAddr, updated);
}
} // namespace

// Minimal, host-side replacements for early OS interrupt/exception helpers.
extern "C" int32_t OS__DisableInterrupts_8010f160()
{
    const bool previous = g_interrupts_enabled.exchange(false);
    UpdateCurrentContextInterruptFlag(false);
    return previous ? 1 : 0;
}

extern "C" int32_t OS__RestoreInterrupts_8010f1a0(int32_t level)
{
    const bool prev = g_interrupts_enabled.exchange(level != 0);
    UpdateCurrentContextInterruptFlag(level != 0);
    return prev ? 1 : 0;
}

extern "C" int32_t OS__EnableInterrupts_8010f180()
{
    const bool prev = g_interrupts_enabled.exchange(true);
    UpdateCurrentContextInterruptFlag(true);
    return prev ? 1 : 0;
}

bool OS_HLE_InterruptsEnabled() noexcept
{
    return g_interrupts_enabled.load(std::memory_order_acquire);
}

extern "C" uint32_t OS____InterruptInit_8010f200(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8)
{
    (void)r3;
    (void)r4;
    (void)r5;
    (void)r6;
    (void)r7;
    (void)r8;

    std::call_once(interrupt_init_log_once, [&]() {
        RT_LOG(RT_TAG_OS) << "OS____InterruptInit_8010f200 called: r3=" << r3 << " r4=" << r4 << " r5=" << r5
                  << " r6=" << r6 << " r7=" << r7 << " r8=" << r8 << std::endl;
        RT_LOG(RT_TAG_OS) << "OSInterruptInit: skipped hardware MMIO setup; marking interrupts initialized" << std::endl;
    });

    // Keep interrupts disabled until RestoreInterrupts decides otherwise.
    g_interrupts_enabled.store(false);

    try {
        // Initialize interrupt vector table pointer and clear the table (0x20 entries).
        ::Memory::Write32(kInterruptHandlerTablePtrAddr, kInterruptHandlerTableAddr);
        if (auto* table = ::Memory::GetPointer(kInterruptHandlerTableAddr, kInterruptHandlerTableBytes)) {
            std::memset(table, 0, kInterruptHandlerTableBytes);
        } else {
            for (size_t offset = 0; offset < kInterruptHandlerTableBytes; offset += 4) {
                ::Memory::Write32(kInterruptHandlerTableAddr + static_cast<uint32_t>(offset), 0);
            }
        }

        // Reset interrupt mask state tracked in guest memory and our host mirror.
        ::Memory::Write32(kInterruptMaskLoAddr, 0);
        ::Memory::Write32(kInterruptMaskHiAddr, 0);
        g_interrupt_mask.store(0);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS____InterruptInit_8010f200", e);
    }

    return 0;
}

extern "C" uint32_t OS__ExceptionInit_80108d20(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r20)
{
    (void)r3;
    (void)r4;
    (void)r5;
    (void)r6;
    (void)r7;
    (void)r8;
    (void)r20;

    std::call_once(exception_init_log_once, [&]() {
        RT_LOG(RT_TAG_OS) << "OS__ExceptionInit_80108d20 called: r3=" << r3 << " r4=" << r4 << " r5=" << r5
                  << " r6=" << r6 << " r7=" << r7 << " r8=" << r8 << " r20=" << r20 << std::endl;
        RT_LOG(RT_TAG_OS) << "OSExceptionInit: skipped exception vector setup" << std::endl;
    });
    return 0;
}

extern "C" uint32_t __OSSetInterruptHandler_801a65f8_hle(uint32_t interrupt, uint32_t handler)
{
    constexpr uint32_t kMaxInterrupts = 32;

    uint32_t tableBase = 0;
    try {
        tableBase = ::Memory::Read32(kInterruptHandlerTablePtrAddr);
    } catch (const ::Memory::AccessViolation&) {
        tableBase = 0;
    }

    if (tableBase == 0) {
        tableBase = kInterruptHandlerTableAddr;
        try {
            ::Memory::Write32(kInterruptHandlerTablePtrAddr, tableBase);
        } catch (const ::Memory::AccessViolation&) {
            // If we cannot write the pointer, fail gracefully.
        }
    }

    if (interrupt >= kMaxInterrupts) {
        return 0;
    }

    const uint32_t entryAddr = tableBase + interrupt * 4u;
    uint32_t previous = 0;
    try {
        previous = ::Memory::Read32(entryAddr);
        ::Memory::Write32(entryAddr, handler);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "__OSSetInterruptHandler_8010f1d0", e);
    }

    return previous;
}

extern "C" uint32_t __OSUnmaskInterrupts_801a69bc_hle(uint32_t mask)
{
    const int32_t level = OS__DisableInterrupts_8010f160();

    uint32_t previous = 0;
    try {
        const uint32_t loMask = ::Memory::Read32(kInterruptMaskLoAddr);
        const uint32_t hiMask = ::Memory::Read32(kInterruptMaskHiAddr);
        previous = loMask;

        const uint32_t newLo = loMask & ~mask;
        ::Memory::Write32(kInterruptMaskLoAddr, newLo);
        g_interrupt_mask.store(newLo);

        const uint32_t combinedBefore = loMask | hiMask;
        const uint32_t combinedAfter = newLo | hiMask;
        for (uint32_t pending = mask & combinedBefore; pending != 0; pending = SetInterruptMask_8010f2d0(pending, combinedAfter)) {
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "__OSUnmaskInterrupts_8010f5b0", e);
    }

    OS__RestoreInterrupts_8010f1a0(level);
    return previous;
}

// EXI: early hardware init touches Hollywood registers (0xCD00xxxx). Provide a no-op stub.
extern "C" uint32_t EXIInit_800ea9a0()
{
    RT_LOG(RT_TAG_OS) << "EXIInit_800ea9a0 called: skipping MMIO register setup" << std::endl;
    return 0;
}

// ----------------------------------------------------------------------------
// Interrupt Controller - Mask/Unmask Hardware Interrupts
// ----------------------------------------------------------------------------

// SetInterruptMask (0x801b12b0): stub for Hollywood interrupt controller MMIO we don't emulate.
// Callers loop on this until it returns 0, so always return 0 to break the loop.
extern "C" uint32_t SetInterruptMask_8010f2d0(uint32_t mask, uint32_t enable)
{
    // Keep the state tracking (useful for debugging)
    static std::atomic<int> call_count{0};
    const int prev_count = call_count.fetch_add(1);

    uint32_t old_mask = g_interrupt_mask.load();
    uint32_t new_mask = old_mask;
    if (enable) {
        new_mask = old_mask | mask;
    } else {
        new_mask = old_mask & ~mask;
    }
    g_interrupt_mask.store(new_mask);

    // Log occasionally
    if (prev_count < 8 || (prev_count & 0x3FF) == 0) {
        RT_LOG(RT_TAG_OS) << "SetInterruptMask_8010f2d0 called: mask=0x" << std::hex << mask << std::dec
                  << " enable=" << enable
                  << " (forcing return 0 to break loop)" << std::endl;
    }
    return 0;
}

PPC_NATIVE_OVERRIDE(8010F160, OS__DisableInterrupts_8010f160, int32_t, (), ());
PPC_NATIVE_OVERRIDE(8010F180, OS__EnableInterrupts_8010f180, int32_t, (), ());
PPC_NATIVE_OVERRIDE(8010F1A0, OS__RestoreInterrupts_8010f1a0, int32_t, (int32_t level), (level));
PPC_NATIVE_OVERRIDE(8010F200, OS____InterruptInit_8010f200, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8), (r3, r4, r5, r6, r7, r8));
PPC_NATIVE_OVERRIDE(8010F2D0, SetInterruptMask_8010f2d0, uint32_t, (uint32_t mask, uint32_t enable), (mask, enable));
PPC_NATIVE_OVERRIDE(80108D20, OS__ExceptionInit_80108d20, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r20), (r3, r4, r5, r6, r7, r8, r20));
PPC_NATIVE_OVERRIDE(800EA9A0, EXIInit_800ea9a0, uint32_t, (), ());
REGISTER_NATIVE_FUNCTION(0x8010F1D0, __OSSetInterruptHandler_801a65f8_hle);
REGISTER_NATIVE_FUNCTION(0x8010F5B0, __OSUnmaskInterrupts_801a69bc_hle);

// OS____MaskInterrupts (0x801b1510): stubbed out because its verification loop reads MMIO
// registers we don't emulate, which would spin forever.
extern "C" uint32_t OS____MaskInterrupts_8010f530(uint32_t mask, uint32_t unmask)
{
    static std::atomic<int> call_count{0};
    const int count = call_count.fetch_add(1);
    
    if (count < 5) {
        RT_LOG(RT_TAG_OS) << "OS____MaskInterrupts_8010f530 called: mask=0x" << std::hex << mask
                  << " unmask=0x" << unmask << std::dec 
                  << " (stubbed to prevent MMIO verification loop)" << std::endl;
    }
    
    // Update our internal mask state
    uint32_t old_mask = g_interrupt_mask.load();
    uint32_t new_mask = old_mask;
    
    if (unmask) {
        new_mask = old_mask & ~mask;  // Clear bits
    } else {
        new_mask = old_mask | mask;   // Set bits
    }
    
    g_interrupt_mask.store(new_mask);
    
    // Return the previous mask state
    return old_mask;
}

PPC_NATIVE_OVERRIDE(8010F530, OS____MaskInterrupts_8010f530, uint32_t, (uint32_t mask, uint32_t unmask), (mask, unmask));

// ----------------------------------------------------------------------------
// EXISelect / EXIDeselect - HLE Stubs (0x801b95c0 / 0x801b96f0)
// Real implementation touches MMIO at 0xCD0068xx; stub returns 1 (success).
// ----------------------------------------------------------------------------

extern "C" uint32_t EXISelect_800ea3c0(uint32_t channel, uint32_t device, uint32_t frequency)
{
    // Log occasionally to avoid spam if polled frequently
    static int log_counter = 0;
    if (log_counter++ < 10) {
        RT_LOG(RT_TAG_OS) << "EXISelect_800ea3c0 called: channel=" << channel
                  << " device=" << device << " freq=" << frequency 
                  << " (stubbed success)" << std::endl;
    }
    return 1; // Return 1 (true) to indicate successful selection
}

// Body for the EXI stubs that take only a channel and report success: the real
// ops drive Hollywood MMIO we do not emulate, and the callers loop until they
// see success. Each keeps its own log budget and wording; the registrations
// stay spelled out below because the translator scans them by text.
#define EXI_CHANNEL_STUB(name, logLimit, channelLabel)                    \
    extern "C" uint32_t name(uint32_t channel)                            \
    {                                                                     \
        static int log_count = 0;                                         \
        if (log_count++ < (logLimit)) {                                   \
            RT_LOG(RT_TAG_OS) << #name " called: " channelLabel           \
                      << channel << " (stubbed success)" << std::endl;    \
        }                                                                 \
        return 1;                                                         \
    }

EXI_CHANNEL_STUB(EXIDeselect_800ea4f0, 10, "channel=")

// Register the functions
PPC_NATIVE_OVERRIDE(800EA3C0, EXISelect_800ea3c0, uint32_t, (uint32_t channel, uint32_t device, uint32_t frequency), (channel, device, frequency));
PPC_NATIVE_OVERRIDE(800EA4F0, EXIDeselect_800ea4f0, uint32_t, (uint32_t channel), (channel));

// ----------------------------------------------------------------------------
// SetExiInterruptMask (0x801b8a30): stubbed no-op, our fake EXI devices need no interrupt masking.
// ----------------------------------------------------------------------------
extern "C" void SetExiInterruptMask_800e9830(uint32_t channel, uint32_t exi_struct_ptr)
{
    // channel: r3 (0, 1, 2)
    // exi_struct_ptr: r4
    // This function is void and typically just modifies internal OS masks.
    // We treat it as a successful no-op.
}

// Register the function
PPC_NATIVE_OVERRIDE_VOID(800E9830, SetExiInterruptMask_800e9830, (uint32_t channel, uint32_t exi_struct_ptr), (channel, exi_struct_ptr));

// ----------------------------------------------------------------------------
// EXI Transaction Stubs (Imm, Dma, Sync, Unlock)
// ----------------------------------------------------------------------------

// RVL__EXIImm / EXIImm
// Address: 0x801b8b20
// Behavior: Performs an Immediate transfer (1-4 bytes) over EXI.
//           Stub: Return 1 (success). If it's a read, we clear the buffer.
extern "C" uint32_t EXIImm_800e9920(uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback)
{
    // type: 0=Read, 1=Write, 2=RW
    // If reading, clear the destination buffer to 0 to be safe.
    if (type == 0 || type == 2) {
        try {
            for (uint32_t i = 0; i < length; ++i) {
                ::Memory::Write8(buffer + i, 0);
            }
        } catch (...) {
            RT_LOG(RT_TAG_OS) << "EXIImm: Failed to write to guest buffer 0x" << std::hex << buffer << std::dec << std::endl;
        }
    }
    
    // Log only occasionally
    static int log_count = 0;
    if (log_count++ < 5) {
        RT_LOG(RT_TAG_OS) << "EXIImm_800e9920 called: chan=" << channel << " len=" << length << " type=" << type << " (stubbed success)" << std::endl;
    }
    return 1; // Success
}

// RVL__EXIDma / EXIDma
// Address: 0x801B8E50
// Behavior: Performs a DMA transfer over EXI.
//           Stub: Return 1 (success).
extern "C" uint32_t EXIDma_800E9C50(uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback)
{
    static int log_count = 0;
    if (log_count++ < 5) {
        RT_LOG(RT_TAG_OS) << "EXIDma_800E9C50 called: chan=" << channel << " len=" << length << " (stubbed success)" << std::endl;
    }
    return 1; // Success
}

// RVL__EXISync / EXISync
// Address: 0x801B8F50
// Behavior: Waits for the current EXI transfer to complete.
//           Stub: Return 1 (success) immediately.
EXI_CHANNEL_STUB(EXISync_800E9D50, 5, "chan=")

// RVL__EXIUnlock / EXIUnlock
// Address: 0x801B9E70
// Behavior: Unlocks the EXI channel and triggers any pending callbacks.
//           Stub: Return 1 (success) to bypass internal callback logic that causes the 0x0 crash.
EXI_CHANNEL_STUB(EXIUnlock_800EAC70, 5, "chan=")

// ----------------------------------------------------------------------------
// OSSetPowerCallback (0x801B6730): sets the power-button callback pointer in the SDA (r13);
// the real STM/IOS registration is stubbed.
// ----------------------------------------------------------------------------
extern "C" uint32_t OSSetPowerCallback_801144b0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) return 0;

    const uint32_t newCallback = cpu->gpr[3];
    const uint32_t r13 = cpu->gpr[13];

    // __OSDefaultPowerCallback
    constexpr uint32_t kDefaultCallbackAddr = 0x80114960u;

    // Offsets from R13 (SDA2)
    const uint32_t kCallbackPtrAddr = r13 - 0x7248u;    // PowerCallback
    const uint32_t kHandlerActiveAddr = r13 - 0x7250u;  // StmEhRegistered

    RT_LOG(RT_TAG_OS) << "OSSetPowerCallback_801144b0 called: newCB=0x"
              << std::hex << newCallback << std::dec << std::endl;

    // 1. Disable Interrupts (Preserve atomicity as per SDK)
    int32_t oldLevel = OS__DisableInterrupts_8010f160();

    uint32_t oldCallback = kDefaultCallbackAddr;

    try {
        // 2. Read the current callback
        if (::Memory::Contains(kCallbackPtrAddr, 4)) {
            oldCallback = ::Memory::Read32(kCallbackPtrAddr);
        }

        // 3. Update to the new callback (or reset to default if nullptr passed)
        uint32_t callbackToWrite = (newCallback != 0) ? newCallback : kDefaultCallbackAddr;
        ::Memory::Write32(kCallbackPtrAddr, callbackToWrite);

        // 4. Simulate STM Event Handler Registration
        // Real code calls IOS_IoctlAsync here. We just set the flag to 1 
        // to pretend the event handler was successfully registered.
        uint32_t isActive = ::Memory::Read32(kHandlerActiveAddr);
        if (isActive == 0) {
            ::Memory::Write32(kHandlerActiveAddr, 1);
        }

    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSetPowerCallback", e);
    }

    // 5. Restore Interrupts
    OS__RestoreInterrupts_8010f1a0(oldLevel);

    // 6. Return logic: If old callback was default, return NULL. Otherwise return old address.
    if (oldCallback == kDefaultCallbackAddr) {
        return 0;
    }
    return oldCallback;
}

// Register the function
PPC_NATIVE_OVERRIDE(801144B0, OSSetPowerCallback_801144b0, uint32_t, (CpuContext* ctx), (ctx));

PPC_NATIVE_OVERRIDE(800E9920, EXIImm_800e9920, uint32_t, (uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback), (channel, buffer, length, type, callback));
PPC_NATIVE_OVERRIDE(800E9C50, EXIDma_800E9C50, uint32_t, (uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback), (channel, buffer, length, type, callback));
PPC_NATIVE_OVERRIDE(800E9D50, EXISync_800E9D50, uint32_t, (uint32_t channel), (channel));
PPC_NATIVE_OVERRIDE(800EAC70, EXIUnlock_800EAC70, uint32_t, (uint32_t channel), (channel));

// ----------------------------------------------------------------------------
// IPC Register Access Stubs (0x80224350 write / 0x80224340 read): Broadway-IOS MMIO,
// unmapped here, so abort loudly instead of crashing silently.
// ----------------------------------------------------------------------------

extern "C" void IPCWriteReg_800F4C30(uint32_t index, uint32_t value)
{
    // index: Register index (0=Command, 1=Result, 2=Control, etc.)
    // value: 32-bit value to write
    // CRASH: We need to identify callers and stub them at a higher level
    RT_LOG(RT_TAG_OS) << "IPCWriteReg_800F4C30 called: idx=" << index
              << " val=0x" << std::hex << value << std::dec << std::endl;
    RT_LOG(RT_TAG_OS) << "IPC hardware access detected - stub the caller instead!" << std::endl;
    std::fflush(stderr);
    std::abort();
}

extern "C" uint32_t IPCReadReg_800F4C20(uint32_t index)
{
    // index: Register index
    // CRASH: We need to identify callers and stub them at a higher level
    RT_LOG(RT_TAG_OS) << "IPCReadReg_800F4C20 called: idx=" << index << std::endl;
    RT_LOG(RT_TAG_OS) << "IPC hardware access detected - stub the caller instead!" << std::endl;
    std::fflush(stderr);
    std::abort();
}

// Register the functions
PPC_NATIVE_OVERRIDE_VOID(800f4c30, IPCWriteReg_800F4C30, (uint32_t index, uint32_t value), (index, value));
PPC_NATIVE_OVERRIDE(800f4c20, IPCReadReg_800F4C20, uint32_t, (uint32_t index), (index));

// ----------------------------------------------------------------------------
// IPCCltInit: hardware parts (interrupt handler, MMIO) are stubbed, but it still calls the translated
// IPCInit (0x800F4B80) to set up the IPC buffer globals; without it IPCGetBufferLo/Hi return 0 and
// ISFS_OpenLib fails with "APP ERROR: Not enough IPC arena". Other M's own IPCCltInit (0x800F50C0) then
// creates the iosHeap from the first 0x2000 bytes (IPCBufferLo is r13-0x73FC); nothing here uses that heap, so the
// buffer stays where IPCInit put it.
// ----------------------------------------------------------------------------
extern "C" int32_t IPCCltInit_800F50C0(CpuContext* ctx)
{
    RT_LOG(RT_TAG_OS) << "IPCCltInit_800F50C0 called: calling IPCInit for buffer setup" << std::endl;
    
    // Sets IPCBufferLo/Hi + Initialized from __OSGetIPCBufferLo/Hi.
    InvokeIndirectCpu(0x800F4B80u, ctx);

    // Skip the rest (interrupt handler, IPC MMIO access) - those are hardware-specific
    return 0; // Success
}

PPC_NATIVE_OVERRIDE(800F50C0, IPCCltInit_800F50C0, int32_t, (CpuContext* ctx), (ctx));
