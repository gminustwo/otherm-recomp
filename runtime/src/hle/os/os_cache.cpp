// Data/instruction/locked cache maintenance HLE.

#include <cstdint>
#include <cstring>
#include <iostream>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "hle/gx/gx_internal.h"

// ============================================================================
// Data cache maintenance (DCInvalidate/Flush/Store)
// These touch hardware on console; on host we fast-path them to avoid huge
// translated loops while still validating the guest range.
// ============================================================================
namespace {
struct CacheRange {
    uint32_t start = 0;
    uint32_t size = 0;
};

constexpr uint32_t kCacheLineSize = 32;

bool NormalizeCacheRange(uint32_t addr, uint32_t length, CacheRange& out)
{
    if (length == 0) {
        return false;
    }

    const uint32_t alignedStart = addr & ~(kCacheLineSize - 1u);
    const uint64_t end = static_cast<uint64_t>(addr) + static_cast<uint64_t>(length) + (kCacheLineSize - 1u);
    const uint64_t alignedEnd = end & ~(static_cast<uint64_t>(kCacheLineSize) - 1u);
    if (alignedEnd <= alignedStart || alignedEnd >= 0x100000000ull) {
        return false;
    }

    out.start = alignedStart;
    out.size = static_cast<uint32_t>(alignedEnd - alignedStart);
    return out.size != 0;
}

bool ValidateCacheRange(const char* label, const CacheRange& range)
{
    if (range.size == 0) {
        return false;
    }

    if (!::Memory::Contains(range.start, range.size)) {
        RT_LOG(RT_TAG_OS) << label << ": range 0x" << std::hex << range.start
                  << " len=0x" << range.size << std::dec
                  << " outside guest memory; skipping" << std::endl;
        return false;
    }
    return true;
}

void DcRangeOp(const char* label, uint32_t addr, uint32_t length)
{
    CacheRange range{};
    if (!NormalizeCacheRange(addr, length, range) || !ValidateCacheRange(label, range)) {
        return;
    }

    DownloadEfbCopiesForRange(range.start, range.size);
    GxNotifyGuestRamDmaWrite(range.start, range.size);
}
} // namespace

extern "C" void DCInvalidateRange_8010a3f0(uint32_t addr, uint32_t length)
{
    DcRangeOp("DCInvalidateRange_8010a3f0", addr, length);
}

extern "C" void DCFlushRange_8010a420(uint32_t addr, uint32_t length)
{
    DcRangeOp("DCFlushRange_8010a420", addr, length);
}

extern "C" void DCStoreRange_8010a450(uint32_t addr, uint32_t length)
{
    DcRangeOp("DCStoreRange_8010a450", addr, length);
}

extern "C" void DCFlushRangeNoSync_8010a480(uint32_t addr, uint32_t length)
{
    DcRangeOp("DCFlushRangeNoSync_8010a480", addr, length);
}

PPC_NATIVE_OVERRIDE_VOID(8010A3F0, DCInvalidateRange_8010a3f0, (uint32_t addr, uint32_t length), (addr, length));
PPC_NATIVE_OVERRIDE_VOID(8010A420, DCFlushRange_8010a420, (uint32_t addr, uint32_t length), (addr, length));
PPC_NATIVE_OVERRIDE_VOID(8010A450, DCStoreRange_8010a450, (uint32_t addr, uint32_t length), (addr, length));
PPC_NATIVE_OVERRIDE_VOID(8010A480, DCFlushRangeNoSync_8010a480, (uint32_t addr, uint32_t length), (addr, length));

// ----------------------------------------------------------------------------
// CPU Cache Maintenance Stubs (DC/IC/LC)
// These are safe to no-op because the host CPU handles caching.
// ----------------------------------------------------------------------------
extern "C" void Cache_Maintenance_Stub()
{
}

namespace {

constexpr uint32_t kCacheOpLineSize = 32u;
constexpr uint32_t kLcOpPageSize = 4096u;
constexpr uint32_t kLcOpMaxBlocksPerTransfer = 128u;

uint32_t AlignDown32(uint32_t value)
{
    return value & ~(kCacheOpLineSize - 1u);
}

uint32_t AlignUp32(uint32_t value)
{
    return (value + (kCacheOpLineSize - 1u)) & ~(kCacheOpLineSize - 1u);
}

} // namespace

extern "C" void DCZeroRange_HLE_8010a4e0(CpuContext* ctx)
{
    const uint32_t addr = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t len = static_cast<uint32_t>(ctx->gpr[4]);
    if (len == 0) {
        return;
    }

    const uint32_t alignedAddr = AlignDown32(addr);
    const uint32_t alignedLen = AlignUp32((addr - alignedAddr) + len);
    try {
        auto* dst = ::Memory::GetPointer(alignedAddr, alignedLen);
        std::memset(dst, 0, alignedLen);
        GxNotifyGuestRamDmaWrite(alignedAddr, alignedLen);
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "DCZeroRange: memory access failed @0x" << std::hex << e.address()
                  << " len=0x" << alignedLen << std::dec << " (" << e.reason() << ")" << std::endl;
    }
}

PPC_NATIVE_OVERRIDE_VOID(8010a3d0, Cache_Maintenance_Stub, (), ()); // DCEnable
PPC_NATIVE_OVERRIDE_VOID(8010a4e0, DCZeroRange_HLE_8010a4e0, (CpuContext* ctx), (ctx)); // DCZeroRange
PPC_NATIVE_OVERRIDE_VOID(8010a510, Cache_Maintenance_Stub, (), ()); // ICInvalidateRange
PPC_NATIVE_OVERRIDE_VOID(8010a550, Cache_Maintenance_Stub, (), ()); // ICFlashInvalidate
PPC_NATIVE_OVERRIDE_VOID(8010a560, Cache_Maintenance_Stub, (), ()); // ICEnable
PPC_NATIVE_OVERRIDE_VOID(8010a690, Cache_Maintenance_Stub, (), ()); // LCDisable
PPC_NATIVE_OVERRIDE_VOID(8010a940, Cache_Maintenance_Stub, (), ()); // OS____CacheInit
