// Skippable movies (CRI Sofdec) for Other M.
//
// The game keeps up to eight movie players in the slots at 0x80C7AA28; its per-frame manager (game.rel
// 0x805B7608) calls the player update 0x80158B0C for each live slot. That update asks Sofdec for the playback
// status (0x80052CFC, mwPlyGetStat: 0 stop, 1 prep, 2 playing, 3 play end, 4 error) and, on 3 or 4, destroys
// the Sofdec handle (0x80051544) and clears the player's handle fields (+0x14, +0x1C, +0x6C); a 0 result makes
// the manager run the slot's normal "movie finished" handler (0x805B6D3C). This override is that update with one
// addition: a skip request ends a movie that is playing (status 2) through the same path as its natural end.
// Nothing skips a movie still preparing (status 1), whose stream reads are in flight.
//
// Requests: Backspace or a controller's left shoulder button (settings_overlay.cpp) while a movie plays; a press
// with no movie playing lapses after a second, so it cannot skip a later one. OTHERM_SKIP_MOVIES=1 skips every
// movie as soon as it plays (unattended test runs).
//
// Ultrawide: a movie's frame is a full-screen picture (640x480 RGBA8) kept in its 16:9 shape; while a movie is preparing
// or playing aurora leaves the bars beside it black instead of filling them with the picture's edge columns
// (AuroraSetFullScreenMovie, cleared a few frames after the last update of a live player: OthermMovieFrame).
#include "hle_stubs.h"

#include "abi_bridge.h"
#include "memory.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kPlayerUpdate = 0x80158B0Cu;
constexpr int32_t kStatPrep = 1;
constexpr int32_t kStatPlaying = 2;
constexpr uint32_t kMovieLingerFrames = 4;  // presented frames after the last live update before the bars go back

std::atomic<uint32_t> g_movieSeen{0};  // VI_HLE_PresentedFrames() + 1 at the last update of a live movie, 0: none
constexpr int32_t kStatPlayEnd = 3;
constexpr int32_t kStatError = 4;

std::atomic<int64_t> g_skipRequestedAt{0};  // steady_clock ticks of the last request, 0 = none

int64_t NowTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

bool TakeSkipRequest() {
    static const bool autoSkip = std::getenv("OTHERM_SKIP_MOVIES") != nullptr;
    if (autoSkip) return true;
    const int64_t at = g_skipRequestedAt.load(std::memory_order_relaxed);
    if (at == 0) return false;
    const auto age = std::chrono::steady_clock::duration(NowTicks() - at);
    if (age > std::chrono::seconds(1)) {
        g_skipRequestedAt.store(0, std::memory_order_relaxed);
        return false;
    }
    return g_skipRequestedAt.exchange(0, std::memory_order_relaxed) != 0;
}

}  // namespace

void OthermRequestMovieSkip() { g_skipRequestedAt.store(NowTicks(), std::memory_order_relaxed); }

// A movie is preparing or playing (otherm_cutscene.cpp sends skip requests here then).
bool OthermMovieUp() { return g_movieSeen.load(std::memory_order_relaxed) != 0; }

// Each retrace (vi.cpp): a movie no longer updated gives its bars back.
void OthermMovieFrame() {
    const uint32_t seen = g_movieSeen.load(std::memory_order_relaxed);
    if (seen != 0 && VI_HLE_PresentedFrames() + 1u - seen > kMovieLingerFrames) {
        g_movieSeen.store(0, std::memory_order_relaxed);
        AuroraSetFullScreenMovie(false);
        RT_LOGF(RT_TAG_RUNTIME, "[movie] no movie up: full-screen pictures fill the bars with their edges again\n");
    }
}

// 0x80158B0C: int MoviePlayer::Update(MoviePlayer* player); 1 while the movie plays, 0 once it is over.
extern "C" void OthermMoviePlayerUpdate_HLE(CpuContext* ctx) {
    const uint32_t player = ctx->gpr[3];
    uint32_t handle = 0;
    if (!Memory::TryRead32(player + 0x14u, handle) || handle == 0) {
        ctx->gpr[3] = 0;
        return;
    }
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = handle;
    ctx->lr = 0x80158B3Cu;
    InvokeDirectCpu<0x80052CFCu>(ctx);  // mwPlyGetStat
    const int32_t stat = static_cast<int32_t>(ctx->gpr[3]);
    if (stat == kStatPrep || stat == kStatPlaying) {
        if (g_movieSeen.exchange(VI_HLE_PresentedFrames() + 1u, std::memory_order_relaxed) == 0) {
            AuroraSetFullScreenMovie(true);
            RT_LOGF(RT_TAG_RUNTIME, "[movie] a movie is up in player 0x%08X: black bars beside it in Fill\n", player);
        }
    }
    bool skipped = false;
    if (stat == kStatPlaying && TakeSkipRequest()) {
        skipped = true;
        RT_LOGF(RT_TAG_RUNTIME, "[movie] skipped the movie playing in player 0x%08X\n", player);
    }
    if (stat == kStatPlayEnd || stat == kStatError || skipped) {
        ctx->gpr[3] = handle;
        ctx->lr = 0x80158B54u;
        InvokeDirectCpu<0x80051544u>(ctx);  // destroys the Sofdec handle
        Memory::Write32(player + 0x14u, 0);
        Memory::Write32(player + 0x1Cu, 0);
        Memory::Write32(player + 0x6Cu, 0);
        ctx->gpr[3] = 0;
    } else {
        ctx->gpr[3] = 1;
    }
    ctx->lr = savedLr;
    (void)kPlayerUpdate;
}
PPC_NATIVE_OVERRIDE_VOID(80158B0C, OthermMoviePlayerUpdate_HLE, (CpuContext* ctx), (ctx));
