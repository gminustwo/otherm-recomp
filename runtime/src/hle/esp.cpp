// ESP HLE Stubs
// ESP functions communicate with /dev/es to get title information.
// We stub these to return the game's title ID directly.

#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>

static constexpr uint32_t OTHERM_TITLE_ID_HI = 0x00010000;  // the disc's title 00010000-52334F45 (tmd.bin)
static constexpr uint32_t OTHERM_TITLE_ID_LO = 0x52334F45; // "R3OE" fallback

// The guest asked for its title ID or data directory through a pointer we cannot
// honour. Nothing downstream explains why the save-data path then fails, so say
// so - once per site, because the caller may retry every frame.
static void ReportEspArgError(bool& logged, const char* who, const char* what)
{
    if (logged) {
        return;
    }
    logged = true;
    RT_LOGF(RT_TAG_HLE, "%s: %s\n", who, what);
    std::fflush(stderr);
}

// 0x801D6640 -> ESP_GetDataDir(titleHi, titleLo, outPathPtr)
// Gets the NAND data directory path for a title.
extern "C" int32_t ESP_GetDataDir_stub(uint32_t titleHi, uint32_t titleLo, uint32_t outPathPtr)
{
    if (outPathPtr == 0) {
        static bool loggedNull = false;
        ReportEspArgError(loggedNull, "ESP_GetDataDir", "null output pointer, returning error");
        return -0x3f9;
    }

    // Check alignment (original requires 32-byte alignment)
    if ((outPathPtr & 0x1f) != 0) {
        static bool loggedUnaligned = false;
        ReportEspArgError(loggedUnaligned, "ESP_GetDataDir",
                          "output pointer is not 32-byte aligned, returning error");
        return -0x3f9;
    }


    // Build the path: /title/XXXXXXXX/YYYYYYYY/data
    // This is what the game expects for its save data location
    char path[64];
    std::sprintf(path, "/title/%08x/%08x/data", titleHi, titleLo);
    
    // Copy path to game memory
    const char* src = path;
    uint32_t dst = outPathPtr;
    while (*src) {
        Memory::Write8(dst++, static_cast<uint8_t>(*src++));
    }
    Memory::Write8(dst, 0); // null terminator
    return 0;
}
PPC_NATIVE_OVERRIDE(800E92A0, ESP_GetDataDir_stub, int32_t, (uint32_t titleHi, uint32_t titleLo, uint32_t outPathPtr), (titleHi, titleLo, outPathPtr));
