// System (IPL ROM) font support.
//
// A game that loads the console's ROM font asks the SDK for the font encoding first.
// OSGetFontEncode reads the VI DTV status register (0xCC00206E) on hardware: bit 1 set means a
// Japanese console (Shift-JIS font), clear means ANSI. The runtime has no answer for that register
// read, so these natives give one.
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

// Every SDK font entry point repeats the encoding check with its own inline register read
// (OSSetFontEncode, OSInitFont, OSLoadFont, OSGetFontTexel/Texture/Width), so each one that the game
// can reach without a loaded font needs a native.
namespace {
constexpr uint32_t kOsFontEncodeAnsi = 0;  // OS_FONT_ENCODE_ANSI: NTSC-U / PAL consoles
constexpr uint32_t kOsFontEncodeSjis = 1;  // OS_FONT_ENCODE_SJIS: Japanese consoles
uint32_t g_fontEncode = kOsFontEncodeAnsi;  // R3OE01 is a North American game

void LogOnce(bool& logged, const char* message)
{
    if (!logged) {
        logged = true;
        RT_LOGF(RT_TAG_OS, "%s\n", message);
    }
}
} // namespace

// OSGetFontEncode: the current system-font encoding, ANSI unless the game set one.
extern "C" uint32_t OSGetFontEncode_HLE_8010e0c0()
{
    static bool logged = false;
    LogOnce(logged, "OSGetFontEncode: reporting the configured encoding (no VI DTV register read)");
    return g_fontEncode;
}
PPC_NATIVE_OVERRIDE(8010E0C0, OSGetFontEncode_HLE_8010e0c0, uint32_t, (), ());

// OSSetFontEncode: store a valid encoding and return the previous one.
extern "C" uint32_t OSSetFontEncode_HLE_8010e110(uint32_t encode)
{
    const uint32_t previous = g_fontEncode;
    if (encode == kOsFontEncodeAnsi || encode == kOsFontEncodeSjis) {
        g_fontEncode = encode;
    }
    return previous;
}
PPC_NATIVE_OVERRIDE(8010E110, OSSetFontEncode_HLE_8010e110, uint32_t, (uint32_t encode), (encode));

extern "C" uint32_t OSLoadFont_HLE_8010e4a0(uint32_t fontData, uint32_t temp)
{
    static bool logged = false;
    LogOnce(logged, "OSLoadFont: no console ROM font available; reporting failure");
    (void)fontData;
    (void)temp;
    return 0;
}
PPC_NATIVE_OVERRIDE(8010E4A0, OSLoadFont_HLE_8010e4a0, uint32_t, (uint32_t fontData, uint32_t temp), (fontData, temp));

// OSGetFontTexel (0x8010E880): draws one character of the ROM font into a texture and returns the
// next character. OSLoadFont reports no font, so draw nothing: skip one character (two for an SJIS
// lead byte) and report zero width.
extern "C" uint32_t OSGetFontTexel_HLE(uint32_t string, uint32_t image, int32_t pos, int32_t stride, uint32_t widthPtr)
{
    static bool logged = false;
    LogOnce(logged, "OSGetFontTexel: no console ROM font; characters are skipped");
    (void)image; (void)pos; (void)stride;
    try {
        if (widthPtr != 0) Memory::Write32(widthPtr, 0);
        const uint8_t c = Memory::Read8(string);
        if (c == 0) return string;
        const bool lead = g_fontEncode == kOsFontEncodeSjis && ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC));
        return string + (lead ? 2u : 1u);
    } catch (const Memory::AccessViolation&) {
        return string;
    }
}
PPC_NATIVE_OVERRIDE(8010E880, OSGetFontTexel_HLE, uint32_t,
                    (uint32_t string, uint32_t image, int32_t pos, int32_t stride, uint32_t widthPtr),
                    (string, image, pos, stride, widthPtr));
