// In-engine cutscenes ("RTM" demos) for Metroid: Other M: the game's own skip, for every cutscene, and their log lines.
//
// How the game does it (game.rel pre-linked at 0x8052B280; NOTES.md 2026-11-23):
//  - The cutscene director (static object 0x80C7FAE8, "set state" rel_805F5414(state, id)) drives the cutscene player
//    (static 0x80C7F018: +0x00 active, +0x0C the frame, +0x1C the cutscene's id). The director's state, +0x38
//    (0x80C7FB20), is 4 while a cutscene plays, 5 while one is being skipped and -3 once it has ended.
//  - Every frame rel_8083BCD0 (the game's control state at 0x80CA4F30) runs, while a cutscene plays (bit 0 of
//    0x80CA4F48), the skip check rel_8083E588: − pressed on the first Wii Remote, A not held, skips the cutscene when
//    the game allows it there: the game has been cleared, this cutscene was already seen, or a script command
//    (rel_8087F420 -> rel_8083E8D0) has set the skip flag 0x80CA4F54. Never in areas 0x11F-0x13D (0x80C4BDC0) or while
//    one particular object (rel_8065A07C(0x98), +0xF4) is busy. A skip calls the director with state 5, which fades
//    out and ends the cutscene through the same path as its last frame does: a skipped cutscene leaves the game in
//    the state watching it to the end does (the hangar cutscene: 79 words of the game's static data differ, all
//    timers, heap pointers, the random seed and the frame it stopped at).
//
// The port:
//  - [game] skip_cutscenes = "any" (the default) holds the skip flag at 1 while a cutscene plays, so the game lets
//    every cutscene be skipped the way it lets the ones it marks be skipped; the game's own value is put back when
//    the cutscene ends. "seen" leaves the game's own rule.
//  - OthermRequestCutsceneSkip (the pad's Start or A, LB, Backspace: settings_overlay.cpp) presses − on the game's
//    remote for a few retraces while a cutscene plays, so the game's own check decides exactly as for a real −.
//    During a Sofdec movie it asks otherm_movie.cpp instead (the movie ends as at its last frame).
//  - [cutscene] log lines: each cutscene's start (its id, the game's own skip flag), a skip, its end and frame.
#include "hle_stubs.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "virtual_remote.h"

#include <atomic>
#include <cstdint>
#include <cstring>

void OthermRequestMovieSkip();  // otherm_movie.cpp
bool OthermMovieUp();           // otherm_movie.cpp

namespace {

constexpr uint32_t kControlFlags = 0x80CA4F48u;   // bit 0: a cutscene plays (the game's control state + 0x18)
constexpr uint32_t kSkipFlag = 0x80CA4F54u;       // byte: a script allows this cutscene to be skipped (+0x24)
constexpr uint32_t kDirectorState = 0x80C7FB20u;  // 4 plays, 5 skipping, -3 ended
constexpr uint32_t kPlayer = 0x80C7F018u;         // +0x00 active, +0x0C frame (float), +0x1C id
constexpr uint32_t kArea = 0x80C4BDC0u;           // the skip check refuses 0x11F-0x13D
constexpr int32_t kStatePlaying = 4;
constexpr int32_t kStateSkipping = 5;
constexpr uint32_t kWpadMinus = 0x1000u;
constexpr uint32_t kTapRetraces = 6;      // − held this long: the game reads its buttons once a retrace
constexpr uint32_t kVerdictRetraces = 30;  // a skip the game took shows as state 5 well within this

struct Cutscene {
    bool playing = false;
    uint32_t id = 0;
    float frame = 0.0f;
    bool forcing = false;      // the port holds the skip flag at 1
    uint8_t gameFlag = 0;      // the game's own value of it, put back afterwards
    bool skipLogged = false;
    uint32_t requestedAt = 0;  // retrace of a pending skip request, 0: none
};
Cutscene g_cutscene;
std::atomic<bool> g_requested{false};

float ReadFloat(uint32_t addr) {
    const uint32_t bits = Memory::Read32(addr);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

bool CutscenePlaying(int32_t& state) {
    uint32_t flags = 0, raw = 0, active = 0;
    if (!Memory::TryRead32(kControlFlags, flags) || !Memory::TryRead32(kDirectorState, raw) ||
        !Memory::TryRead32(kPlayer, active)) {
        return false;
    }
    state = static_cast<int32_t>(raw);
    return (flags & 1u) != 0 && (active >> 24) != 0 && (state == kStatePlaying || state == kStateSkipping);
}

}  // namespace

bool OthermCutscenePlaying() { return g_cutscene.playing; }

void OthermRequestCutsceneSkip() {
    if (OthermMovieUp()) {
        OthermRequestMovieSkip();
        return;
    }
    if (g_cutscene.playing) g_requested.store(true, std::memory_order_relaxed);
}

// Each retrace (vi.cpp AdvanceRetrace), before the game reads its controllers for the frame.
void OthermCutsceneFrame() {
    Cutscene& c = g_cutscene;
    int32_t state = 0;
    const bool playing = CutscenePlaying(state);
    const uint32_t retrace = VI_HLE_RetraceCount();
    if (playing && !c.playing) {
        c = Cutscene{};
        c.playing = true;
        c.id = Memory::Read32(kPlayer + 0x1Cu);
        c.gameFlag = Memory::Read8(kSkipFlag);
        RT_LOGF(RT_TAG_RUNTIME, "[cutscene] in-engine cutscene %u starts (area 0x%X, the game's skip flag %u)\n", c.id,
                Memory::Read32(kArea), c.gameFlag);
    }
    if (!playing) {
        if (c.playing) {
            if (c.forcing && Memory::Read8(kSkipFlag) == 1u) Memory::Write8(kSkipFlag, c.gameFlag);
            RT_LOGF(RT_TAG_RUNTIME, "[cutscene] cutscene %u ended at frame %.0f%s\n", c.id, c.frame,
                    c.skipLogged ? " (skipped)" : "");
            c = Cutscene{};
        }
        g_requested.store(false, std::memory_order_relaxed);
        return;
    }
    c.frame = ReadFloat(kPlayer + 0x0Cu);

    // [game] skip_cutscenes = "any": the game's skip flag stays set while the cutscene plays.
    if (RuntimeConfigFile::SkipAnyCutscene()) {
        const uint8_t flag = Memory::Read8(kSkipFlag);
        if (!c.forcing) {
            c.gameFlag = flag;
            c.forcing = true;
        } else if (flag != 1u) {
            c.gameFlag = flag;  // the game wrote it meanwhile: that is its value now
        }
        if (flag != 1u) Memory::Write8(kSkipFlag, 1u);
    } else if (c.forcing) {
        if (Memory::Read8(kSkipFlag) == 1u) Memory::Write8(kSkipFlag, c.gameFlag);
        c.forcing = false;
    }

    if (state == kStateSkipping && !c.skipLogged) {
        c.skipLogged = true;
        c.requestedAt = 0;
        RT_LOGF(RT_TAG_RUNTIME, "[cutscene] cutscene %u skipped at frame %.0f\n", c.id, c.frame);
    }
    if (g_requested.exchange(false, std::memory_order_relaxed) && state == kStatePlaying) {
        // The port's skip controls press − for the game, which then decides as for a real press.
        VirtualRemote::HoldButtonsUntil(kWpadMinus, retrace + kTapRetraces);
        c.requestedAt = retrace;
        RT_LOGF(RT_TAG_RUNTIME, "[cutscene] skip asked for at frame %.0f: minus pressed for the game\n", c.frame);
    }
    if (c.requestedAt != 0 && retrace - c.requestedAt > kVerdictRetraces && state == kStatePlaying) {
        RT_LOGF(RT_TAG_RUNTIME, "[cutscene] the game did not skip cutscene %u here (area 0x%X, skip flag %u)\n", c.id,
                Memory::Read32(kArea), Memory::Read8(kSkipFlag));
        c.requestedAt = 0;
    }
}
