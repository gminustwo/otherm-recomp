// gx_copy.cpp - Framebuffer Copy Operations
#include "gx_internal.h"
#include "runtime_log.h"

#include "settings_overlay.h"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <map>
#include <mutex>
#include <vector>

namespace {
// Copy destinations stay GPU-only until an explicit/lazy readback. Track the
// guest ranges that own those results so a later data-cache flush over a reused
// allocation can retire the stale GPU texture before it is considered by a
// subsequent GXLoadTexObj. There is at most one live range per destination;
// rewriting the destination replaces its previous extent.
std::mutex g_efbCopyDestinationsMutex;
std::map<uint32_t, uint32_t> g_efbCopyDestinations;
uint32_t g_largestEfbCopyDestination = 0;

void RememberEfbCopyDestination(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
    g_efbCopyDestinations[CanonicalizeGxMainRamAddress(addr)] = size;
    // A conservative monotonic maximum lets invalidation jump directly to
    // the only map interval that could overlap instead of walking every copy
    // destination on every DCStoreRange.
    g_largestEfbCopyDestination = std::max(g_largestEfbCopyDestination, size);
}

} // namespace

// A data-cache op over a GXCopyTex destination is the game about to read the copy with its CPU (an offscreen
// effect can copy the screen, then DCFlushRange it and move the pixels into its own buffer). Copies are GPU-only,
// so download any that are still pending into RAM first.
void DownloadEfbCopiesForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    const uint64_t start = CanonicalizeGxMainRamAddress(addr);
    const uint64_t end = start + size;
    std::vector<uint32_t> overlapping;
    {
        std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
        const uint64_t earliest = start > g_largestEfbCopyDestination ? start - g_largestEfbCopyDestination : 0;
        for (auto it = g_efbCopyDestinations.lower_bound(static_cast<uint32_t>(earliest));
             it != g_efbCopyDestinations.end() && static_cast<uint64_t>(it->first) < end; ++it) {
            if (static_cast<uint64_t>(it->first) + it->second > start) overlapping.push_back(it->first);
        }
    }
    for (const uint32_t copyAddr : overlapping) {
        aurora_flush_efb_copy_to_ram(GuestToHostPtr(copyAddr));  // no-op unless still pending
    }
}

void InvalidateEfbCopyDestinationsForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }

    const uint64_t dirtyStart = CanonicalizeGxMainRamAddress(addr);
    const uint64_t dirtyEnd = dirtyStart + size;
    std::vector<uint32_t> retired;
    {
        std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
        const uint64_t earliestCandidate =
            dirtyStart > g_largestEfbCopyDestination ? dirtyStart - g_largestEfbCopyDestination : 0;
        for (auto it = g_efbCopyDestinations.lower_bound(static_cast<uint32_t>(earliestCandidate));
             it != g_efbCopyDestinations.end() && static_cast<uint64_t>(it->first) < dirtyEnd;) {
            const uint64_t copyStart = it->first;
            const uint64_t copyEnd = copyStart + it->second;
            if (dirtyEnd <= copyStart || dirtyStart >= copyEnd) {
                ++it;
                continue;
            }
            retired.push_back(it->first);
            it = g_efbCopyDestinations.erase(it);
        }
    }

    // Preserve FIFO ordering: the destroy command is emitted before any later
    // texture load that can consume the freshly flushed RAM bytes.
    for (const uint32_t copyAddr : retired) {
        GXDestroyCopyTex(GuestToHostPtr(copyAddr));
    }
}

// ============================================================================
// Display Copy Source/Destination
// ============================================================================

extern "C" void GX__SetDispCopySrc_800f0420(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GXSetDispCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
}
PPC_NATIVE_OVERRIDE_VOID(800f0420, GX__SetDispCopySrc_800f0420, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetDispCopyDst_800f04a0(uint32_t w, uint32_t h) { GXSetDispCopyDst((u16)w, (u16)h); }
PPC_NATIVE_OVERRIDE_VOID(800f04a0, GX__SetDispCopyDst_800f04a0, (uint32_t w, uint32_t h), (w, h));

// ============================================================================
// Texture Copy Source/Destination
// ============================================================================

extern "C" void GX__SetTexCopySrc_800f0460(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GXSetTexCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
    g_texCopyState.srcLeft=(u16)l; g_texCopyState.srcTop=(u16)t;
    g_texCopyState.srcWidth=(u16)w; g_texCopyState.srcHeight=(u16)h;
}
PPC_NATIVE_OVERRIDE_VOID(800f0460, GX__SetTexCopySrc_800f0460, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetTexCopyDst_800f04d0(uint32_t w, uint32_t h, uint32_t f, uint32_t m) {
    GXSetTexCopyDst((u16)w, (u16)h, (GXTexFmt)f, (GXBool)m);
    g_texCopyState.dstWidth=(u16)w; g_texCopyState.dstHeight=(u16)h;
    g_texCopyState.dstFormat=f; g_texCopyState.dstMipmap=m;
}
PPC_NATIVE_OVERRIDE_VOID(800f04d0, GX__SetTexCopyDst_800f04d0, (uint32_t w, uint32_t h, uint32_t f, uint32_t m), (w, h, f, m));

extern "C" void GX__SetCopyFilter_800f09b0(uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa) {
    uint8_t sp[12][2]={}, vfb[7]={};
    if(spa) std::memcpy(sp, GuestToHostPtr(spa, 24), 24);
    if(vfa) std::memcpy(vfb, GuestToHostPtr(vfa, 7), 7);
    GXSetCopyFilter((GXBool)aa, sp, (GXBool)vf, vfb);
}
PPC_NATIVE_OVERRIDE_VOID(800f09b0, GX__SetCopyFilter_800f09b0, (uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa), (aa, spa, vf, vfa));

extern "C" void GX__SetDispCopyGamma_800f0ba0(uint32_t g) { GXSetDispCopyGamma((GXGamma)g); }
PPC_NATIVE_OVERRIDE_VOID(800f0ba0, GX__SetDispCopyGamma_800f0ba0, (uint32_t g), (g));

// ============================================================================
// Copy Execution
// ============================================================================

// Time the game spent at the end of its frames waiting for the render worker (vi.cpp's frame log).
std::atomic<uint64_t> g_copyDispWorkerWaitNanos{0};

extern "C" void GX__CopyDisp_800f0bc0(uint32_t da, uint32_t c) {
    g_gxBringupCounters[1].fetch_add(1, std::memory_order_relaxed);
    if (OthermDumpActive()) RT_LOGF(RT_TAG_GX, "[dldump] %u copydisp dest %08X clear %u\n", OthermDumpNextSeq(), da, c);
    EnsureAuroraFrameActive();
    // GX copies are FIFO-ordered on hardware. Drain submitted draws before
    // resolving the EFB so high-level copies see the same contents.
    GXDrawDone();
    GXCopyDisp(GuestToHostPtr(da), (GXBool)c);
    // No second GXDrawDone here: the frame-worker wait below is for the DONE
    // phase, which strictly subsumes the drain this call would perform.
    ++g_gxFrameCount;
    VI_HLE_SetXfbReady(da);
    // Present immediately so post-copy draws don't leak into this frame. Join at the DONE phase
    // (not the cheaper SEALED phase GXDrawDone waits for) because ImGui's draw lists, owned by
    // Aurora's render worker, replay during encode; aurora_end_frame would join here anyway.
    const auto waitStart = std::chrono::steady_clock::now();
    aurora_wait_for_frame_worker();
    g_copyDispWorkerWaitNanos.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()),
        std::memory_order_relaxed);
    settings_overlay::Draw();
    // Seal, pace to the VI retrace boundary (Aurora renders the sealed frame
    // during the wait), and pre-warm the next frame.
    VI_HLE_PresentFrame(/*presentedXfb=*/true, /*paceToRetrace=*/true);
}

PPC_NATIVE_OVERRIDE_VOID(800f0bc0, GX__CopyDisp_800f0bc0, (uint32_t da, uint32_t c), (da, c));


extern "C" void GX__CopyTex_800f0d00(uint32_t da, uint32_t c) {
    g_gxBringupCounters[2].fetch_add(1, std::memory_order_relaxed);
    EnsureAuroraFrameActive();
    // Match GX FIFO ordering: texture copies observe all prior draws.
    GXDrawDone();
    const uint16_t rawSrcLeft = g_texCopyState.srcLeft;
    const uint16_t rawSrcTop = g_texCopyState.srcTop;
    const uint16_t rawSrcWidth = g_texCopyState.srcWidth;
    const uint16_t rawSrcHeight = g_texCopyState.srcHeight;
    { static int n = 0; if (n++ < 40) RT_LOGF(RT_TAG_GX, "[gxcopy] tex copy src %u,%u %ux%u -> 0x%08X clear=%u\n", rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight, da, c); }

    // Keep the source in guest EFB coordinates. Aurora maps it to the scaled
    // EFB exactly once, matching Dolphin's ConvertEFBRectangle path.
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
    // EFB copies stay GPU-only except probe-sized ones (e.g. the 4x4 lens-flare depth probe),
    // which Aurora reads back asynchronously via efb_ram::schedule and land in guest RAM a frame
    // later. RISK: copies above the probe threshold, or on the offscreen list, are not
    // auto-downloaded, so guest reads see stale RAM; call aurora_flush_efb_copies_to_ram if a
    // copy needs reading back.
    // OTHERM_LOG_COPY_FRAMES: per frame, the texture copies made (size, format, source, destination),
    // logged with the time whenever that list changes from the previous frame's.
    if (static const bool logFrames = std::getenv("OTHERM_LOG_COPY_FRAMES") != nullptr; logFrames) {
        static uint32_t frame = ~0u;
        static std::string current, previous;
        if (frame != static_cast<uint32_t>(g_gxFrameCount)) {
            if (current != previous) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::fprintf(stderr, "[copyframe] t=%lld frame %u:%s\n", static_cast<long long>(ms), frame,
                             current.c_str());
                previous = current;
            }
            current.clear();
            frame = static_cast<uint32_t>(g_gxFrameCount);
        }
        char item[96];
        std::snprintf(item, sizeof(item), " %ux%u/%u@%u,%u>%08X", g_texCopyState.dstWidth, g_texCopyState.dstHeight,
                      g_texCopyState.dstFormat, rawSrcLeft, rawSrcTop, da);
        current += item;
    }
    // OTHERM_LOG_COPIES: each distinct texture copy (destination size, format, source rectangle) once,
    // with the guest code that asked for it (profiling).
    if (static const bool logCopies = std::getenv("OTHERM_LOG_COPIES") != nullptr; logCopies) {
        static std::vector<uint64_t> seen;
        const uint64_t key = (uint64_t(g_texCopyState.dstWidth) << 48) | (uint64_t(g_texCopyState.dstHeight) << 32) |
                             (uint64_t(rawSrcWidth) << 16) | rawSrcHeight;
        if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 64) {
            seen.push_back(key);
            uint32_t lr = 0, ra[4] = {};
            if (const CpuContext* cpu = TryGetCpuContext()) {
                lr = cpu->lr;
                uint32_t sp = cpu->gpr[1];
                for (int d = 0; d < 4 && sp != 0; ++d) {
                    uint32_t back = 0;
                    if (!Memory::TryRead32(sp, back) || back == 0) break;
                    Memory::TryRead32(back + 4, ra[d]);
                    sp = back;
                }
            }
            std::fprintf(stderr, "[copytex] dst %ux%u fmt %u src %u,%u %ux%u dest %08X lr %08X %08X %08X %08X %08X\n",
                         g_texCopyState.dstWidth, g_texCopyState.dstHeight, g_texCopyState.dstFormat, rawSrcLeft,
                         rawSrcTop, rawSrcWidth, rawSrcHeight, da, lr, ra[0], ra[1], ra[2], ra[3]);
        }
    }
    if (OthermDumpActive()) {
        RT_LOGF(RT_TAG_GX, "[dldump] %u copytex src %u,%u %ux%u -> %ux%u fmt %u mip %u dest %08X clear %u\n", OthermDumpNextSeq(),
                rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight, g_texCopyState.dstWidth, g_texCopyState.dstHeight,
                g_texCopyState.dstFormat, g_texCopyState.dstMipmap, da, c);
    }
    GXCopyTex(GuestToHostPtr(da), (GXBool)c);
    // OTHERM_DLDUMP: read each copy of 64x64 or more back to RAM and save it (dldump/copy_<seq>_<dest>_<w>x<h>_f<fmt>.bin,
    // the guest texture layout) to see what each pass produced.
    if (OthermDumpActive() && g_texCopyState.dstWidth >= 64 && g_texCopyState.dstHeight >= 64) {
        if (void* host = GuestToHostPtr(da)) {
            const bool flushed = aurora_flush_efb_copy_to_ram(host);
            const uint32_t bytes = GXGetTexBufferSize(g_texCopyState.dstWidth, g_texCopyState.dstHeight, g_texCopyState.dstFormat, GX_FALSE, 0);
            char name[96];
            std::snprintf(name, sizeof name, "dldump/copy_%04u_%08X_%ux%u_f%u.bin", OthermDumpNextSeq(), da, g_texCopyState.dstWidth,
                          g_texCopyState.dstHeight, g_texCopyState.dstFormat);
            if (FILE* out = std::fopen(name, "wb")) {
                std::fwrite(GuestToHostPtr(da, bytes), 1, bytes, out);
                std::fclose(out);
            }
            RT_LOGF(RT_TAG_GX, "[dldump] saved %s (%u bytes, flushed %d)\n", name, bytes, flushed ? 1 : 0);
        }
    }
    // The extent the copy writes: a destination bigger than the source is a patch into a larger
    // texture (aurora sizes its copy the same way), not a claim on that whole texture's memory.
    const uint32_t copiedWidth = std::max<uint32_t>(rawSrcWidth >> (g_texCopyState.dstMipmap ? 1 : 0), 1);
    const uint32_t copiedHeight = std::max<uint32_t>(rawSrcHeight >> (g_texCopyState.dstMipmap ? 1 : 0), 1);
    RememberEfbCopyDestination(
        da, GXGetTexBufferSize(std::min<uint32_t>(g_texCopyState.dstWidth, copiedWidth),
                               std::min<uint32_t>(g_texCopyState.dstHeight, copiedHeight),
                               g_texCopyState.dstFormat, GX_FALSE, 0));
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
}
PPC_NATIVE_OVERRIDE_VOID(800f0d00, GX__CopyTex_800f0d00, (uint32_t da, uint32_t c), (da, c));

// GXSetDispCopyYScale (0x800F0860) / GXGetYScaleFactor (0x800F0630), for a game that calls them directly
// (unverified for Other M). Translated, they read the SDK's own copy state, which the native copy
// hooks never fill, and programmed a bogus vertical XFB scale (every column smeared top to bottom).
extern "C" uint32_t GX__SetDispCopyYScale_HLE(float vscale) { return GXSetDispCopyYScale(vscale); }
PPC_NATIVE_OVERRIDE(800F0860, GX__SetDispCopyYScale_HLE, uint32_t, (float vscale), (vscale));

extern "C" float GX__GetYScaleFactor_HLE(uint32_t efbHeight, uint32_t xfbHeight) {
    if (efbHeight == 0 || xfbHeight == 0 || xfbHeight > 1024) return 1.0f;
    return GXGetYScaleFactor(static_cast<u16>(efbHeight), static_cast<u16>(xfbHeight));
}
PPC_NATIVE_OVERRIDE(800F0630, GX__GetYScaleFactor_HLE, float, (uint32_t efbHeight, uint32_t xfbHeight), (efbHeight, xfbHeight));
