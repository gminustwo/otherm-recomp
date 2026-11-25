#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
// gx_vertex.cpp - Vertex Descriptor and Attribute Functions
#include "det_clock.h"
#include "gx_internal.h"
#include "gx_stream_common.h"
#include "fiber_manager.h"
#include "guest_interrupt_context.h"
#include "runtime_log.h"

#include <cstdio>
#include <cstdlib>

namespace {
uint32_t CanonicalVtxAttr(uint32_t attr) {
    return attr == GX_VA_NBT ? GX_VA_NRM : attr;
}

// There is deliberately no indexed-aurora path here. The immediate begin path
// streams vertex data incrementally, and aurora's indexed-array upload must
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream below (GX_INDEX8/16 -> GX_DIRECT).
void ApplyAuroraVtxStateForBegin(GXVtxFmt fmt) {
    GxStream::PublishAuroraVtxState(fmt,
                                    GxStream::AuroraVtxPublishOptions{
                                        /*includeNbt=*/false,
                                        /*fmtLoopFirst=*/static_cast<int>(GX_VA_POS),
                                        /*fmtLoopLast=*/static_cast<int>(GX_VA_TEX7)});
}
}

// ============================================================================
// Vertex Descriptor
// ============================================================================

extern "C" void GX__ClearVtxDesc_800eec90() {
    bool changed = false;
    for(int i=0; i<26; ++i){
        changed |= g_hleGxState.vtxDesc[i] != GX_NONE;
        g_hleGxState.vtxDesc[i]=GX_NONE;
    }
    if (changed) g_hleGxState.InvalidateVtxLayoutHash();
    // GXClearVtxDesc resets descriptors only; array base/stride state persists.
    GXClearVtxDesc();
}
PPC_NATIVE_OVERRIDE_VOID(800eec90, GX__ClearVtxDesc_800eec90, (), ());

extern "C" void GX__SetVtxDesc_800ee840(uint32_t a, uint32_t t) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr>=26||attr==GX_VA_NULL) return;
    const GXAttrType oldType = g_hleGxState.vtxDesc[attr];
    g_hleGxState.vtxDesc[attr]=(GXAttrType)t;
    if (a == GX_VA_NBT) {
        g_hleGxState.vtxDesc[GX_VA_NBT] = GX_NONE;
    }
    if (g_hleGxState.vtxDesc[attr] != oldType) g_hleGxState.InvalidateVtxLayoutHash();
    if(IsMatrixIndexAttr((GXAttr)attr)) return;
    GXSetVtxDesc((GXAttr)a, (t==GX_INDEX8||t==GX_INDEX16)?GX_DIRECT:(GXAttrType)t);
}
PPC_NATIVE_OVERRIDE_VOID(800ee840, GX__SetVtxDesc_800ee840, (uint32_t a, uint32_t t), (a, t));

// ============================================================================
// Vertex Attribute Format
// ============================================================================

extern "C" void GX__SetVtxAttrFmt_800eecd0(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(vf<8&&attr<26){
        const VtxAttrFmt oldFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        g_hleGxState.vtxAttrFmt[vf][attr].cnt=(GXCompCnt)c;
        g_hleGxState.vtxAttrFmt[vf][attr].type=(GXCompType)t;
        g_hleGxState.vtxAttrFmt[vf][attr].frac=(u8)fr;
        if (a == GX_VA_NBT) {
            g_hleGxState.vtxAttrFmt[vf][GX_VA_NBT] = {};
        }
        const auto& newFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        if (oldFmt.cnt != newFmt.cnt || oldFmt.type != newFmt.type || oldFmt.frac != newFmt.frac) {
            g_hleGxState.InvalidateVtxLayoutHash();
        }
    }
    if (vf >= GX_MAX_VTXFMT || a < GX_VA_POS || a >= GX_VA_MAX_ATTR) {
        return;
    }
    GXSetVtxAttrFmt((GXVtxFmt)vf, (GXAttr)a, (GXCompCnt)c, (GXCompType)t, (u8)fr);
}
PPC_NATIVE_OVERRIDE_VOID(800eecd0, GX__SetVtxAttrFmt_800eecd0, (uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr), (vf, a, c, t, fr));

extern "C" void GX__SetVtxAttrFmtv_800eee70(uint32_t vf, uint32_t la) {
    if(!la) return; uint32_t p=la;
    while(true){
        uint32_t a=Memory::Read32(p);
        if(a==0xFFu) break;
        GX__SetVtxAttrFmt_800eecd0(vf, a, Memory::Read32(p+4), Memory::Read32(p+8), Memory::Read8(p+12));
        p+=16;
    }
}
PPC_NATIVE_OVERRIDE_VOID(800eee70, GX__SetVtxAttrFmtv_800eee70, (uint32_t vf, uint32_t la), (vf, la));

// ============================================================================
// Vertex Arrays
// ============================================================================

extern "C" void GX__SetArray_800ef0c0(uint32_t a, uint32_t ba, uint32_t str) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if (OthermDumpActive()) {
        const CpuContext* x = TryGetCpuContext();
        RT_LOGF(RT_TAG_GX, "[dldump] %u setarray attr %u (canonical %u) base %08X stride %u lr %08X\n", OthermDumpNextSeq(), a, attr, ba, str,
                x ? x->lr : 0);
    }
    if(attr<26){ g_hleGxState.vtxArray[attr].base=ba; g_hleGxState.vtxArray[attr].stride=str; }
}
PPC_NATIVE_OVERRIDE_VOID(800ef0c0, GX__SetArray_800ef0c0, (uint32_t a, uint32_t ba, uint32_t str), (a, ba, str));

// ============================================================================
// Texture Coordinate Generation
// ============================================================================

extern "C" void GX__SetNumTexGens_800ef360(uint32_t n) {
    GXSetNumTexGens((u8)n);
}
PPC_NATIVE_OVERRIDE_VOID(800ef360, GX__SetNumTexGens_800ef360, (uint32_t n), (n));

extern "C" void GX__SetTexCoordGen2_800ef110(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm) {
    if (sp >= static_cast<uint32_t>(GX_MAX_TEXGENSRC)) {
        uint32_t pc = 0;
        uint32_t lr = 0;
        if (auto* cpu = TryGetCpuContext()) {
            pc = cpu->pc;
            lr = cpu->lr;
        }
        RT_LOGF(RT_TAG_GX,
                "GXSetTexCoordGen2 invalid src=%u dst=%u type=%u mtx=%u norm=%u post=%u PC=0x%08X LR=0x%08X\n",
                sp, dc, f, m, n, pm, pc, lr);
    }
    GXSetTexCoordGen2((GXTexCoordID)dc, (GXTexGenType)f, (GXTexGenSrc)sp, m, (GXBool)n, pm);
}
PPC_NATIVE_OVERRIDE_VOID(800ef110, GX__SetTexCoordGen2_800ef110, (uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm), (dc, f, sp, m, n, pm));

extern "C" void GX__EnableTexOffsets_800f01e0(uint32_t coord, uint32_t lineEnable, uint32_t pointEnable) {
    GXEnableTexOffsets(static_cast<GXTexCoordID>(coord),
                       lineEnable ? GX_TRUE : GX_FALSE,
                       pointEnable ? GX_TRUE : GX_FALSE);
}
PPC_NATIVE_OVERRIDE_VOID(800f01e0, GX__EnableTexOffsets_800f01e0, (uint32_t coord, uint32_t lineEnable, uint32_t pointEnable), (coord, lineEnable, pointEnable));

extern "C" void GX__SetLineWidth_800f0160(uint32_t width, uint32_t texOffsets) {
    GXSetLineWidth(static_cast<u8>(width), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(800f0160, GX__SetLineWidth_800f0160, (uint32_t width, uint32_t texOffsets), (width, texOffsets));

extern "C" void GX__SetPointSize_800f01a0(uint32_t pointSize, uint32_t texOffsets) {
    GXSetPointSize(static_cast<u8>(pointSize), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(800f01a0, GX__SetPointSize_800f01a0, (uint32_t pointSize, uint32_t texOffsets), (pointSize, texOffsets));

// ============================================================================
// Begin/End Drawing
// ============================================================================

namespace {
thread_local bool g_inGxBeginOrFifo = false;
thread_local std::chrono::steady_clock::time_point g_nextDeferredTimingPoll{};

void ServiceDeferredTimingDuringGxWork() {
    // Wall-clock driven, so where it lands in the guest's execution depends on host speed; the
    // deterministic mode (det_clock.h) delivers these events only at their virtual time.
    if (g_inGxBeginOrFifo || DetClock::Enabled()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextDeferredTimingPoll) {
        return;
    }
    g_nextDeferredTimingPoll = now + std::chrono::milliseconds(1);

    g_inGxBeginOrFifo = true;
    try {
        if (Fiber::GuestFiberManager::IsInitialized()) {
            // VI callbacks can drive a game's display timing on NTSC/PAL60,
            // an OS alarm on PAL50. Keep both clocks moving while a slow
            // host frame is still being submitted, but defer fiber switches
            // and Aurora work until the native GX call has unwound.
            VI_HLE_ProcessRetracesDeferred(8);
            OS_HLE_ProcessAlarmsDeferred(8);
        } else if (TryGetCpuContext() != nullptr) {
            // Same interrupt isolation as the deferred paths above: this GX call
            // is mid-execution of a translated function, so the retrace
            // callbacks must not publish their registers into its context.
            GuestInterruptCallbackContext interrupt;
            VI_HLE_PollRetrace(interrupt.get());
        }
    } catch (...) {
        g_inGxBeginOrFifo = false;
        throw;
    }
    g_inGxBeginOrFifo = false;
}
}

uint32_t OthermPresentedFrames();
static int32_t VertexDumpTarget() {
    static int32_t target = [] { std::ifstream in("drawdump.txt"); int32_t f = -1; in >> f; return f; }();
    return target;
}
bool OthermDumpFrame() {
    const int32_t t = VertexDumpTarget();
    if (t < 0) return false;
    const uint32_t f = OthermPresentedFrames();
    return f >= static_cast<uint32_t>(t) && f <= static_cast<uint32_t>(t) + 1;
}
// Bring-up: drawdump.txt beside the exe holds a frame number; that frame's GXBegin calls are logged.
static void OthermDumpBegin(uint32_t t, uint32_t vf, uint32_t nv) {
    static int32_t target = [] {
        std::ifstream in("drawdump.txt"); int32_t f = -1; in >> f; return f;
    }();
    if (target < 0) return;
    const uint32_t frame = OthermPresentedFrames();
    if (frame < static_cast<uint32_t>(target) || frame > static_cast<uint32_t>(target) + 1) return;
    static int n = 0;
    if (n++ > 400) return;
    const auto& d = g_hleGxState.vtxDesc;
    RT_LOGF(RT_TAG_GX, "[drawdump] f%u begin prim 0x%02X fmt %u n %u desc pos%u nrm%u c0%u c1%u t0%u t1%u pnmi%u\n", frame, t, vf, nv,
            d[GX_VA_POS], d[GX_VA_NRM], d[GX_VA_CLR0], d[GX_VA_CLR1], d[GX_VA_TEX0], d[GX_VA_TEX1], d[GX_VA_PNMTXIDX]);
}

// Other M diagnostic: OTHERM_MASK_BEGIN_LR=<hex lr>,... draws the GXBegin primitives issued from those return addresses
// with colour and alpha writes off (restored at the next GXBegin or display-list call), to see what lies under a
// full-screen pass without consuming its vertex stream differently.
static bool g_othermMaskedDraw = false;
void OthermRestoreMaskedDraw() {
    if (!g_othermMaskedDraw) return;
    g_othermMaskedDraw = false;
    ::GXSetColorUpdate(GX_TRUE);
    ::GXSetAlphaUpdate(GX_TRUE);
}
static bool OthermShouldMaskBegin() {
    static const std::vector<uint32_t> lrs = [] {
        std::vector<uint32_t> v;
        if (const char* e = std::getenv("OTHERM_MASK_BEGIN_LR")) {
            std::string text(e);
            size_t start = 0;
            while (start < text.size()) {
                const size_t comma = text.find(',', start);
                v.push_back(static_cast<uint32_t>(std::strtoul(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start).c_str(), nullptr, 16)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        return v;
    }();
    if (lrs.empty()) return false;
    const CpuContext* x = TryGetCpuContext();
    return x && std::find(lrs.begin(), lrs.end(), x->lr) != lrs.end();
}

extern "C" void GX__Begin_800eff30(uint32_t t, uint32_t vf, uint32_t nv) {
    g_gxBringupCounters[0].fetch_add(1, std::memory_order_relaxed);
    OthermDumpBegin(t, vf, nv);
    if (OthermDumpActive()) {
        char vcd[27] = {};
        for (int a = 0; a < 26; ++a) vcd[a] = static_cast<char>('0' + (static_cast<int>(g_hleGxState.vtxDesc[a]) & 7));
        const CpuContext* x = TryGetCpuContext();
        RT_LOGF(RT_TAG_GX, "[dldump] %u begin prim %02X fmt %u n %u lr %08X vcd %s\n", OthermDumpNextSeq(), t, vf, nv, x ? x->lr : 0, vcd);
    }
    ServiceDeferredTimingDuringGxWork();
    OthermRestoreMaskedDraw();
    if (OthermShouldMaskBegin()) {
        ::GXSetColorUpdate(GX_FALSE);
        ::GXSetAlphaUpdate(GX_FALSE);
        g_othermMaskedDraw = true;
    }
    ApplyAuroraVtxStateForBegin(static_cast<GXVtxFmt>(vf));
    g_hleGxState.currentVtxFmt=(GXVtxFmt)vf; g_hleGxState.currentPrim=(GXPrimitive)t;
    g_hleGxState.vertsRemaining=nv; g_hleGxState.inBegin=true; g_hleGxState.auroraBeginCalled=false;
    g_hleGxState.fifoReadOffset = 0;
    g_hleGxState.fifoByteCount=0; g_hleGxState.ResetVertex();
}
PPC_NATIVE_OVERRIDE_VOID(800eff30, GX__Begin_800eff30, (uint32_t t, uint32_t vf, uint32_t nv), (t, vf, nv));

