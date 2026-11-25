#include "hle_stubs.h"

#include "console_identity.h"
#include "sc_serial_contract.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "runtime_config.h"
#include "aurora_events.h"
#include "runtime_log.h"

namespace {

// Use the SDK's own value tables, including its unknown-region result.
uint32_t LookupProductRegion(uint32_t table, uint32_t stride, uint32_t count,
                             const std::string& value) {
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t entry = table + index * stride;
        if (!Memory::Contains(entry, stride)) {
            break;
        }
        const auto* bytes = static_cast<const uint8_t*>(Memory::GetPointer(entry, stride));
        if (bytes[0] == 0xFF) {
            break;
        }
        if (value.size() < stride - 1 &&
            std::memcmp(bytes + 1, value.c_str(), value.size() + 1) == 0) {
            return bytes[0];
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace

// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC); we have no async IPC callbacks, so return 0 (SUCCESS) immediately.

// 0x801DB8E0 -> SCCheckStatus()
// Returns: 0 = success, 1 = busy, 2 = error
extern "C" uint32_t SCCheckStatus_HLE()
{
    // Return 0 (success) immediately to avoid infinite busy-wait in OSInit
    return 0;
}

PPC_NATIVE_OVERRIDE(801168B0, SCCheckStatus_HLE, uint32_t, (), ());

// 0x801DD1D0 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9. The display settings' aspect at boot: 16:9 and Fill both run the
// game in widescreen (Fill then widens its canvas further).
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return DisplaySettings::GameWidescreen() ? 1u : 0u;
}

PPC_NATIVE_OVERRIDE(801181A0, SCGetAspectRatio_HLE, uint32_t, (), ());

// 0x801182F0 -> SCGetLanguage(): the console's language setting, from Config.toml [game] language (runtime_config.h).
extern "C" uint32_t SCGetLanguage_HLE()
{
    return RuntimeConfigFile::Language();
}

PPC_NATIVE_OVERRIDE(801182F0, SCGetLanguage_HLE, uint32_t, (), ());

// Expose the selected emulated NAND identity through the SDK SC APIs.

extern "C" uint32_t SCGetProductArea_HLE()
{
    return LookupProductRegion(0x8023F588u, 5, 13,  // SC area table {code, "JPN"...}
                               RuntimeConsoleIdentity::Current().area);
}

PPC_NATIVE_OVERRIDE(801188B0, SCGetProductArea_HLE, uint32_t, (), ());

