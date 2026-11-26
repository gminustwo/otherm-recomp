// Metroid: Other M: where the player is, for the log ([area] lines) and Discord's status (discord_presence.h).
//
// The HUD map draws the room the player is in as an area map element named after its layout (otherm_minimap.cpp
// DrawnAreaMap: "A_36"). The disc keeps those layouts in one map archive per region (U8 archives among the numbered
// files): 3139 holds MA_ / MB_ / MC_, 3140 A_ / AB_, 3141 B_ / E_, 3142 C_, 3143 D_, and the opening's training room
// is T_00 (3694, alone). Seen in save states: T_00 in the training-room tutorial, MA_ in the first rooms after the
// Bottle Ship's hangar cutscene, A_36 in Sector 1's rain jungle. The other regions are a guess from the order of the
// game's own map labels (message_all.dat 218-256: the Main Sector's rooms, then Sector 1's, Sector 2's, Sector 3's and
// the Bioweapon Research Center's): B_ / E_ Sector 2, C_ Sector 3, D_ the Bioweapon Research Center.
#include "discord_presence.h"
#include "otherm_minimap.h"
#include "runtime_log.h"

#include <cstdint>
#include <string>

bool OthermMovieUp();         // otherm_movie.cpp
bool OthermCutscenePlaying(); // otherm_cutscene.cpp

namespace {

struct AreaRegion {
    const char* prefix;  // the layout's name up to its '_'
    const char* name;
};
constexpr AreaRegion kAreaRegions[] = {
    {"T", "Training room"},
    {"MA", "Main Sector"},
    {"MB", "Main Sector"},
    {"MC", "Main Sector"},
    {"A", "Sector 1 (Biosphere)"},
    {"AB", "Sector 1 (Biosphere)"},
    {"B", "Sector 2 (Cryosphere)"},
    {"E", "Sector 2 (Cryosphere)"},
    {"C", "Sector 3 (Pyrosphere)"},
    {"D", "Bioweapon Research Center"},
};

const char* AreaRegionOf(const std::string& map) {
    const std::string prefix = map.substr(0, map.find('_'));
    for (const AreaRegion& region : kAreaRegions)
        if (prefix == region.prefix) return region.name;
    return nullptr;
}

constexpr uint32_t kAreaCheckEvery = 15;    // retraces: 4 times a second at 60
constexpr uint32_t kAreaDrawnWithin = 30;   // presented frames: the HUD map is up
constexpr int kMaxAreaLines = 1000;

struct AreaWhere {
    std::string map;        // the room's map now ("A_36"), empty before the first
    const char* region = nullptr;
    std::string candidate;  // a new map, taken when the next check still draws it
    int lines = 0;
};
AreaWhere g_areaWhere;

}  // namespace

// Each retrace (vi.cpp AdvanceRetrace), on the game's thread.
void OthermAreaFrame() {
    static uint32_t s_retraces = 0;
    if (++s_retraces % kAreaCheckEvery != 0) return;
    AreaWhere& w = g_areaWhere;
    const std::string map = OthermMinimap::DrawnAreaMap(kAreaDrawnWithin);
    if (!map.empty() && map != w.map) {
        if (map != w.candidate) {
            w.candidate = map;  // once more first: a door can show the next room's map for a moment
        } else {
            w.map = map;
            w.candidate.clear();
            w.region = AreaRegionOf(map);
            if (w.lines < kMaxAreaLines) {
                ++w.lines;
                RT_LOGF(RT_TAG_RUNTIME, "[area] %s, room map %s%s\n", w.region != nullptr ? w.region : "region not known",
                        map.c_str(), w.lines == kMaxAreaLines ? " (no more [area] lines this session)" : "");
            }
        }
    } else {
        w.candidate.clear();
    }
    const char* doing = OthermMovieUp() ? "Watching a movie" : OthermCutscenePlaying() ? "In a cutscene" : "";
    DiscordPresence::Publish(w.region != nullptr ? w.region : "", doing);
}
