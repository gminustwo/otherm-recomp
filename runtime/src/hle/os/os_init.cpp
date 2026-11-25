// Boot/init hooks, OSFatal, and assorted hardware-init stubs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "abi_bridge.h"
#include "memory.h"
#include "mod_runtime.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"
#include "system_bridge.h"

extern "C" void func_80112580(CpuContext* ctx);

extern "C" void OSInitAlarm_RecompModLateInit_80112580(CpuContext* ctx) {
    func_80112580(ctx);
    ModRuntime::ApplyEarlyPatches();  // mods' Riivolution memory patches inside the main program
}

REGISTER_NATIVE_FUNCTION_AS(0x80112580, OSInitAlarm_RecompModLateInit_80112580, "OSInitAlarm_RecompModLateInit_80112580");

namespace {
std::string ReadGuestCStringLimited(uint32_t address, size_t limit = 4096) {
    if (address == 0) {
        return "<null>";
    }

    std::string text;
    text.reserve(128);
    for (size_t i = 0; i < limit; ++i) {
        const uint8_t ch = Memory::Read8(address + static_cast<uint32_t>(i));
        if (ch == 0) {
            return text;
        }
        text.push_back(static_cast<char>(ch));
    }
    text += "<unterminated>";
    return text;
}
}

extern "C" void OSFatal_HLE_8010d5f0(CpuContext* ctx) {
    const uint32_t fg = ctx ? ctx->gpr[3] : 0;
    const uint32_t bg = ctx ? ctx->gpr[4] : 0;
    const uint32_t messagePtr = ctx ? ctx->gpr[5] : 0;
    RT_LOG(RT_TAG_OS) << "OS::Fatal called fg=0x" << std::hex << fg
              << " bg=0x" << bg
              << " message=0x" << messagePtr
              << std::dec << " '" << ReadGuestCStringLimited(messagePtr) << "'" << std::endl;
    if (ctx) {
        SystemBridge::DumpCpuState(ctx);
    }
    const std::string guestMessage = ReadGuestCStringLimited(messagePtr);
    const std::string details =
        guestMessage.empty() ? std::string("OS::Fatal was called without a message.") : guestMessage;
    // MarkFatalErrorReported below suppresses the atexit reporter, so this path
    // has to write its own artifacts or the run folder gets nothing.
    RuntimeCrash::WriteCrashArtifacts("osfatal", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a fatal error", details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

REGISTER_NATIVE_FUNCTION_AS(0x8010D5F0, OSFatal_HLE_8010d5f0, "OSFatal_HLE_8010d5f0");

extern "C" void GKI_delay_HLE_800b3a00(CpuContext* ctx)
{
    const uint32_t delayMs = ctx ? static_cast<uint32_t>(ctx->gpr[3]) : 0;
    const uint32_t sleepMs = delayMs == 0 ? 1u : std::min(delayMs, 10u);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
}

extern "C" uint32_t BTM_IsDeviceUp_HLE_800bdcb4(CpuContext* ctx)
{
    // Force Bluetooth stack to "up" to avoid endless polling loops while we lack
    // real hardware bring-up.
    constexpr uint32_t kBtmCbBase = 0x802CA6B8u;  // btm_cb; BTM_IsDeviceUp reads +0x64E == 5
    constexpr uint32_t kDevStateOffset = 0x64Eu;
    try {
        ::Memory::Write8(kBtmCbBase + kDevStateOffset, 5u);
    } catch (const ::Memory::AccessViolation&) {
        // Ignore; best-effort write
    }

    if (ctx) {
        ctx->gpr[3] = 1;
    }
    return 1;
}

PPC_NATIVE_OVERRIDE_VOID(800B3A00, GKI_delay_HLE_800b3a00, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE(800bdcb4, BTM_IsDeviceUp_HLE_800bdcb4, uint32_t, (CpuContext* ctx), (ctx));

// Serial Interface (SI) - GameCube controller ports; stubbed since we don't emulate the MMIO.

// SIInit (0x801baef0): skips MMIO setup at 0xCD006434 and controller detection.
extern "C" void SIInit_801190c0()
{
    RT_LOG(RT_TAG_OS) << "SIInit_801190c0 called: skipping MMIO register setup and controller detection" << std::endl;
}

// SISetSamplingRate (0x801bb840): ignored, we don't emulate SI polling timing.
extern "C" void HLE_SISetSamplingRate_80119a10(uint32_t msec)
{
    RT_LOG(RT_TAG_OS) << "HLE_SISetSamplingRate_80119a10 called: msec=" << msec << ": Stubbed success." << std::endl;
}

// Video Interface (VI) - TV output.

REGISTER_NATIVE_FUNCTION(0x801190C0, SIInit_801190c0);
PPC_NATIVE_OVERRIDE_VOID(80119A10, HLE_SISetSamplingRate_80119a10, (uint32_t msec), (msec));

// OS____InitMemoryProtection (0x801B2D50): real version touches MMU/MMIO we don't emulate;
// no-op and return success so boot doesn't stall.
extern "C" uint32_t OS____InitMemoryProtection_80110d50(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8)
{
    RT_LOG(RT_TAG_OS) << "OS____InitMemoryProtection_80110d50 called (stubbed): r3=0x" << std::hex << r3 << std::dec << std::endl;

    return 0;
}

PPC_NATIVE_OVERRIDE(80110D50, OS____InitMemoryProtection_80110d50, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8), (r3, r4, r5, r6, r7, r8));

// OSGetConsoleType: standard Wii = 0x12, NDEV (expanded MEM2) = 0x10000012;
// a game can use the NDEV result to enable an extra-memory heap path.
extern "C" uint32_t OS__GetConsoleType_80107ea0(uint32_t /*r4*/, uint32_t /*r5*/, uint32_t /*r6*/,
                                                uint32_t /*r7*/, uint32_t /*r8*/, uint32_t /*r31*/)
{
    constexpr uint32_t kRetailMem2Size = 64u * 1024u * 1024u;
    const uint32_t physicalMem2Size = Memory::Read32(0x80003118u);
    const uint32_t consoleType =
        physicalMem2Size == kRetailMem2Size ? 0x00000012u : 0x10000012u;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        RT_LOG(RT_TAG_OS) << "OSGetConsoleType: MEM2="
                  << (physicalMem2Size / (1024u * 1024u))
                  << " MB, returning 0x" << std::hex << consoleType << std::dec << std::endl;
    }
    return consoleType;
}

// Register the function
PPC_NATIVE_OVERRIDE(80107EA0, OS__GetConsoleType_80107ea0, uint32_t, (uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r31), (r4, r5, r6, r7, r8, r31));

// OSGetResetCode (0x801b3c90): real version reads MMIO 0xCC003024; we always report
// Cold Boot (0).
extern "C" uint32_t OSGetResetCode_801119f0()
{
    // Log occasionally just to track boot flow
    static bool logged = false;
    if (!logged) {
        RT_LOG(RT_TAG_OS) << "OSGetResetCode_801119f0 called: returning 0 (Cold Boot)" << std::endl;
        logged = true;
    }
    return 0; 
}

// Register the function
PPC_NATIVE_OVERRIDE(801119F0, OSGetResetCode_801119f0, uint32_t, (), ());

// __OSInitSTM (0x801B6820): real version opens /dev/stm/* handles. We stub it by writing
// fake handles and the success flag into the SDA (r13) block so OSResetSystem's checks pass.
extern "C" uint32_t __OSInitSTM_HLE_801145a0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) return 0;

    RT_LOG(RT_TAG_OS) << "__OSInitSTM_HLE_801145a0 called: initializing STM state" << std::endl;

    // R13 (SDA2) holds the base for small data variables
    const uint32_t r13 = cpu->gpr[13];
    if (r13 == 0) {
         RT_LOG(RT_TAG_OS) << "__OSInitSTM: Warning - R13 is 0, cannot write state." << std::endl;
         return 0;
    }

    // r13-0x725C=StmReady, r13-0x7258=StmImDesc (/dev/stm/immediate),
    // r13-0x7254=StmEhDesc (/dev/stm/eventhook), traced in Other M's STM code.
    try {
        // Mark STM as initialized
        ::Memory::Write32(r13 - 0x725cu, 1);  // StmReady

        // Fake non-zero handles so callers' zero-checks pass.
        ::Memory::Write32(r13 - 0x7258u, 0x00535401);  // StmImDesc // "ST\x01"
        ::Memory::Write32(r13 - 0x7254u, 0x00535402);  // StmEhDesc // "ST\x02"

        // Default Power/Reset callback pointers are left unset; safe since we never fire
        // the STM hardware interrupt that would invoke them.
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "__OSInitSTM: Failed to write STM state to SDA @ 0x"
                  << std::hex << e.address() << std::dec << " (" << e.reason() << ")" << std::endl;
        return 0; // Return failure
    }

    // Return 1 (success)
    return 1;
}

// Register the function
PPC_NATIVE_OVERRIDE(801145A0, __OSInitSTM_HLE_801145a0, uint32_t, (CpuContext* ctx), (ctx));


extern "C" void OS____PSInit_801090b0()
{
    RT_LOG(RT_TAG_OS) << "OS____PSInit_801090b0 called (stubbed)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(801090B0, OS____PSInit_801090b0, (), ());



extern "C" void __init_hardware_80006620()
{
    RT_LOG(RT_TAG_OS) << "__init_hardware_80006620 called (stubbed)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(80006620,__init_hardware_80006620, (), ());


// PPC SPR (Special Purpose Register) access stubs; these registers don't exist on x86 so
// each one just logs and no-ops.

// Each PPC_NATIVE_OVERRIDE_VOID line stays written out per stub (not looped) because the
// translator text-scans them to decide which addresses to skip translating.
#define PPC_SPR_STUB_BODY(name, message) \
    extern "C" void name() { RT_LOG(RT_TAG_OS) << message << std::endl; }

PPC_SPR_STUB_BODY(PPCMfhid0_800b1c70, "PPCMfhid0 called (stubbed) - Move From HID0")
PPC_NATIVE_OVERRIDE_VOID(800b1c70, PPCMfhid0_800b1c70, (), ());

PPC_SPR_STUB_BODY(PPCMthid0_800b1c80, "PPCMthid0 called (stubbed) - Move To HID0")
PPC_NATIVE_OVERRIDE_VOID(800b1c80, PPCMthid0_800b1c80, (), ());

extern "C" void PPCMtdec_800b1cb0()
{
    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMtdec called (stubbed) - Move To Decrementer" << std::endl;
    }
    // We don't simulate decrementer exceptions, so pump due alarms here instead. Capped at
    // 32, not 1: a single alarm let unrelated periodic alarms backlog and delay
    // AsyncDisplay's pacing alarm by multiple retraces.
    OS_HLE_ProcessAlarms(32);
}
PPC_NATIVE_OVERRIDE_VOID(800B1CB0, PPCMtdec_800b1cb0, (), ());

extern "C" void PPCSync_800b1cc0()
{
    // PowerPC `sync`. Translated guest code runs on one host thread at a time
    // and the runtime's own cross-thread state uses C++ atomics, so there is no
    // guest-visible reordering for this barrier to prevent.
}
PPC_NATIVE_OVERRIDE_VOID(800b1cc0, PPCSync_800b1cc0, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr0_800b1cf0, "PPCMtmmcr0 called (stubbed) - Move To MMCR0")
PPC_NATIVE_OVERRIDE_VOID(800b1cf0, PPCMtmmcr0_800b1cf0, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr1_800b1d00, "PPCMtmmcr1 called (stubbed) - Move To MMCR1")
PPC_NATIVE_OVERRIDE_VOID(800b1d00, PPCMtmmcr1_800b1d00, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc1_800b1d10, "PPCMtpmc1 called (stubbed) - Move To PMC1")
PPC_NATIVE_OVERRIDE_VOID(800b1d10, PPCMtpmc1_800b1d10, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc2_800b1d20, "PPCMtpmc2 called (stubbed) - Move To PMC2")
PPC_NATIVE_OVERRIDE_VOID(800b1d20, PPCMtpmc2_800b1d20, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc3_800b1d30, "PPCMtpmc3 called (stubbed) - Move To PMC3")
PPC_NATIVE_OVERRIDE_VOID(800b1d30, PPCMtpmc3_800b1d30, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc4_800b1d40, "PPCMtpmc4 called (stubbed) - Move To PMC4")
PPC_NATIVE_OVERRIDE_VOID(800b1d40, PPCMtpmc4_800b1d40, (), ());

extern "C" uint32_t PPCMfhid2_8012e630_impl()
{
    if (CpuContext* cpu = TryGetCpuContext()) {
        if (cpu->hid2 == 0) {
            cpu->hid2 = 0x10000000u;
        }
        return cpu->hid2;
    }
    return 0x10000000u;
}
// Register the function using a return-value stub
extern "C" void PPCMfhid2_HLE_800b1da0(CpuContext* ctx)
{
    ctx->gpr[3] = PPCMfhid2_8012e630_impl();
}
REGISTER_TRANSLATED_FUNCTION(0x800b1da0, PPCMfhid2_HLE_800b1da0);

extern "C" void PPCMthid2_800b1db0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    cpu->hid2 = cpu->gpr[3];

    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMthid2 set HID2=0x" << std::hex << cpu->hid2 << std::dec << std::endl;
    }
}
PPC_NATIVE_OVERRIDE_VOID(800b1db0, PPCMthid2_800b1db0, (CpuContext* ctx), (ctx));

PPC_SPR_STUB_BODY(PPCMtwpar_800b1dc0,
                  "PPCMtwpar called (stubbed) - Move To Write Pipe Address Register")
PPC_NATIVE_OVERRIDE_VOID(800b1dc0, PPCMtwpar_800b1dc0, (), ());

PPC_SPR_STUB_BODY(PPCDisableSpeculation_800b1dd0, "PPCDisableSpeculation called (stubbed)")
PPC_NATIVE_OVERRIDE_VOID(800b1dd0, PPCDisableSpeculation_800b1dd0, (), ());

PPC_SPR_STUB_BODY(PPCMthid4_800b1e10, "PPCMthid4 called (stubbed) - Move To HID4")
PPC_NATIVE_OVERRIDE_VOID(800b1e10, PPCMthid4_800b1e10, (), ());
