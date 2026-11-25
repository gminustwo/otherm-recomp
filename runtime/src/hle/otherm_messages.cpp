// Metroid: Other M: the on-screen hints in the controls the player uses (otherm_messages.h).
//
// message_all.dat is a "tdpack": the total size at 0x10, eight section offsets at 0x30 and sizes at 0x50, one section
// per language (JP, US, DE, FR, ES, IT, USFR, USES). A section is its 1,899 messages joined by "\" CR LF "\", with the
// markup inline (#FONT_SYSTEM, #COLOR_GREEN, #ICON_B for a button picture, ^ for a new line) and zero padding after.
// Only the US English section is rewritten: the phrases below, by exact text. The button pictures (#ICON_A ...) are
// already the controller's own (button_icons); the words for pointing and tilting are what this changes.
//
// The rewritten section goes back in its place when it fits (31 spare bytes follow it), else after the end of the
// file with the header pointing there. The game reads the header, then each section at the offset and size it gives,
// so a rewrite made at a header read stays consistent for the reads that follow.
#include "otherm_messages.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "virtual_remote.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

namespace OthermMessages {
namespace {

std::mutex g_mutex;
std::filesystem::path g_sourcePath;
std::vector<uint8_t> g_source;   // the file as on the disc (or a mod's copy)
std::vector<uint8_t> g_served;   // what reads get
std::string g_lastNames;         // the names of the last rewrite, to log a change once

uint32_t Get32(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3];
}
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24);
    b[at + 1] = uint8_t(v >> 16);
    b[at + 2] = uint8_t(v >> 8);
    b[at + 3] = uint8_t(v);
}

// {POINT}: the input that points the remote at the screen; {VERTICAL}: the one that holds it vertically. A rule whose
// name is unknown leaves its text alone.
struct Rule {
    const char* from;
    const char* to;
};
constexpr Rule kUsEnglish[] = {
    // 290, at the first door that needs the first-person view.
    {"Point the Wii Remote at the screen.", "Hold {POINT} for first-person view."},
    // 277, SEARCH VIEW.
    {"Point the Wii Remote at^the screen and^", "Hold {POINT} for first-person view^and "},
    // 279, LOCK ON.
    {"#COLOR_GREENPress and hold #ICON_B #COLOR_WHITEand point at your^target to lock on.",
     "Hold {POINT}, aim at your target^and #COLOR_GREENpress and hold #ICON_B#COLOR_WHITE^to lock on."},
    // 275, 341 and 340, CONCENTRATION (missiles and energy).
    {"Hold the Wii Remote vertically and ^", "Hold {VERTICAL} and ^"},
    {"Hold the Wii Remote vertically and^", "Hold {VERTICAL} and^"},
    {"hold the ^Wii Remote vertically and^", "hold {VERTICAL} and^"},
    // 265 and 378, MORPH BALL: holding a controller sideways is no instruction.
    {"With the #COLOR_GREENWii Remote held ^sideways, #COLOR_WHITEpress #ICON_A.", "Press #ICON_A."},
    {"Controls: Hold the Wii Remote #COLOR_GREENhorizontally #COLOR_WHITE^and press #ICON_A.", "Controls: Press #ICON_A."},
};

bool LoadSource(const std::filesystem::path& path) {
    if (path == g_sourcePath && !g_source.empty()) return true;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    if (!in.good() && !in.eof()) return false;
    if (bytes.size() < 0x70 || std::memcmp(bytes.data(), "tdpack", 6) != 0) {
        RT_LOGF(RT_TAG_RUNTIME, "[messages] %s is not a tdpack; served as it is\n", RuntimeConfigFile::PathToUtf8(path).c_str());
    }
    g_sourcePath = path;
    g_source = std::move(bytes);
    return !g_source.empty();
}

// The served file for these names: the source with its US English section rewritten.
std::vector<uint8_t> Rewrite(const VirtualRemote::MotionHintNames& names, int& replaced) {
    replaced = 0;
    const std::vector<uint8_t>& src = g_source;
    if (names.wiiRemote || src.size() < 0x70 || std::memcmp(src.data(), "tdpack", 6) != 0) return src;
    static const std::string kUsTag = std::string("\\\\\r\n\\#FONT_SYSTEMUS\\\r\n");
    for (int section = 0; section < 8; ++section) {
        const uint32_t offset = Get32(src, 0x30 + section * 4), size = Get32(src, 0x50 + section * 4);
        if (size == 0 || uint64_t(offset) + size > src.size()) continue;
        std::string text(reinterpret_cast<const char*>(src.data()) + offset, size);
        if (text.compare(0, kUsTag.size(), kUsTag) != 0) continue;

        for (const Rule& rule : kUsEnglish) {
            std::string to = rule.to;
            bool usable = true;
            const std::pair<const char*, const std::string*> fills[] = {{"{POINT}", &names.point},
                                                                        {"{VERTICAL}", &names.vertical}};
            for (const auto& [key, value] : fills) {
                for (size_t at; (at = to.find(key)) != std::string::npos;) {
                    if (value->empty()) usable = false;
                    to.replace(at, std::strlen(key), *value);
                }
            }
            if (!usable) continue;
            for (size_t at = 0; (at = text.find(rule.from, at)) != std::string::npos; at += to.size()) {
                text.replace(at, std::strlen(rule.from), to);
                ++replaced;
            }
        }
        if (replaced == 0) return src;

        std::vector<uint8_t> out = src;
        // Room up to the next section (or the end of the file).
        uint32_t limit = static_cast<uint32_t>(src.size());
        for (int other = 0; other < 8; ++other) {
            const uint32_t o = Get32(src, 0x30 + other * 4);
            if (o > offset && o < limit) limit = o;
        }
        uint32_t place = offset;
        if (text.size() + 4 > limit - offset) {  // keep zero padding after it, as the disc's sections have
            place = static_cast<uint32_t>((src.size() + 31) & ~size_t(31));
            out.resize(place + ((text.size() + 4 + 31) & ~size_t(31)), 0);
            Put32(out, 0x10, static_cast<uint32_t>(out.size()));
        } else {
            std::fill(out.begin() + offset, out.begin() + limit, 0);
        }
        std::memcpy(out.data() + place, text.data(), text.size());
        Put32(out, 0x30 + section * 4, place);
        Put32(out, 0x50 + section * 4, static_cast<uint32_t>(text.size()));
        return out;
    }
    return src;
}

} // namespace

bool Handles(const std::filesystem::path& hostPath) {
    const auto name = hostPath.filename().u8string();
    const auto dir = hostPath.parent_path().filename().u8string();
    return name == u8"message_all.dat" && dir == u8"message";
}

bool Read(const std::filesystem::path& hostPath, uint32_t offset, uint32_t length, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!LoadSource(hostPath)) return false;
    if (offset == 0 || g_served.empty()) {
        const VirtualRemote::MotionHintNames names = VirtualRemote::HintNames();
        int replaced = 0;
        g_served = Rewrite(names, replaced);
        const std::string summary = names.wiiRemote ? std::string("a Wii Remote: the game's own text")
                                                    : "point '" + names.point + "', vertical '" + names.vertical + "': " +
                                                          std::to_string(replaced) + " phrase(s) rewritten";
        if (summary != g_lastNames) {
            g_lastNames = summary;
            RT_LOGF(RT_TAG_RUNTIME, "[messages] hints for %s\n", summary.c_str());
        }
    }
    out.clear();
    if (offset >= g_served.size()) return true;
    const size_t count = std::min<size_t>(length, g_served.size() - offset);
    out.assign(g_served.begin() + offset, g_served.begin() + offset + count);
    return true;
}

} // namespace OthermMessages
