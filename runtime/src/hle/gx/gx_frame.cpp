#include "gx_internal.h"
#include "runtime_log.h"

#include <cstdio>

void BeginNextAuroraFrameWithRetry(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    uint32_t attempts = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        UpdateAuroraAndProcessEvents();
        ++attempts;
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Retry window expired: the aurora frame stays INACTIVE and every GX
    // command until the next EnsureAuroraFrameActive is silently dropped
    // (draws, one-shot bakes, copies). This is a frame-loss event.
    static uint32_t s_beginExpiredLogCount = 0;
    if (s_beginExpiredLogCount < 256 || (s_beginExpiredLogCount & 255) == 0) {
        RT_LOGF(RT_TAG_GX, "BeginAuroraFrame retry EXPIRED after %u attempts; frame remains inactive (n=%u)\n",
                attempts, s_beginExpiredLogCount + 1);
    }
    ++s_beginExpiredLogCount;
}

void EnsureAuroraFrameActive() {
    if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
        BeginNextAuroraFrameWithRetry();
    }
}
