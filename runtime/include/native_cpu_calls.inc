// Native call catalog used by InvokeDirectCpu. Keep project-specific HLE
// address mappings here; abi_bridge.h contains only the generic ABI machinery.
//
// This is a .inc, not a header: it has no includes of its own and references
// CpuContext, KnownTypedNativeCpuCall<> and KnownNativeCpuCall<>, all of which
// are declared above its single mid-file include site in abi_bridge.h. It
// cannot compile standalone and must never be included anywhere else.

extern "C" void GX__SetVtxDesc_800ee840(uint32_t a, uint32_t t);
extern "C" void GX__ClearVtxDesc_800eec90();
extern "C" void GX__SetVtxAttrFmt_800eecd0(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr);
extern "C" void GX__SetTexCoordGen2_800ef110(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm);
extern "C" void GX__SetNumTexGens_800ef360(uint32_t n);
extern "C" void GX__DrawDone_800ef860();
extern "C" void GX__Begin_800eff30(uint32_t t, uint32_t vf, uint32_t nv);
extern "C" void GX__SetLineWidth_800f0160(uint32_t width, uint32_t texOffsets);
extern "C" void GX__SetPointSize_800f01a0(uint32_t pointSize, uint32_t texOffsets);
extern "C" void GX__SetCullMode_800f0220(uint32_t m);
extern "C" void GX__SetNumChans_800f1530(uint32_t n);
extern "C" void GX__SetChanCtrl_800F1560(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af);
extern "C" void GX__InitTexObj_800f1800(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m);
extern "C" void GX__InitTexObjLOD_800f1a60(uint32_t oa, uint32_t mif, uint32_t maf, float mil, float mal, float lb, uint32_t bc, uint32_t el, uint32_t ma);
extern "C" void GX__InvalidateTexAll_800F2190();
extern "C" void GX__LoadTexObj_800f1f90(uint32_t oa, uint32_t tid);
extern "C" void GX__SetNumIndStages_800f2be0(uint32_t n);
extern "C" void GX__SetTevDirect_800f2c00(uint32_t s);
extern "C" void GX__SetTevColorIn_800f2dc0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevAlphaIn_800f2e00(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevColorOp_800f2e40(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevAlphaOp_800f2ea0(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevColorS10_800f2f60(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColor_800f2fd0(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColorSel_800f3030(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevKAlphaSel_800f3080(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevSwapMode_800f30d0(uint32_t s, uint32_t rs, uint32_t ts);
extern "C" void GX__SetTevSwapModeTable_800f3110(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
extern "C" void GX__SetTevOrder_800f3260(uint32_t s, uint32_t c, uint32_t m, uint32_t col);
extern "C" void GX__SetNumTevStages_800f33c0(uint32_t n);
extern "C" void GX__SetBlendMode_800f3890(uint32_t t, uint32_t s, uint32_t d, uint32_t op);
extern "C" void GX__LoadPosMtxImm_800f3eb0(uint32_t ma, uint32_t id);
extern "C" void GX__SetCurrentMtx_800F3FD0(uint32_t id);

#define OTHERM_KNOWN_TYPED_NATIVE(addr, fn, ...) \
template <> struct KnownTypedNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static void Invoke(CpuContext* cpu) { (void)cpu; fn(__VA_ARGS__); } \
}

OTHERM_KNOWN_TYPED_NATIVE(0x800EE840u, GX__SetVtxDesc_800ee840, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800EEC90u, GX__ClearVtxDesc_800eec90);
OTHERM_KNOWN_TYPED_NATIVE(0x800EECD0u, GX__SetVtxAttrFmt_800eecd0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
OTHERM_KNOWN_TYPED_NATIVE(0x800EF110u, GX__SetTexCoordGen2_800ef110, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
OTHERM_KNOWN_TYPED_NATIVE(0x800EF360u, GX__SetNumTexGens_800ef360, cpu->gpr[3]);
OTHERM_KNOWN_TYPED_NATIVE(0x800EF860u, GX__DrawDone_800ef860);
OTHERM_KNOWN_TYPED_NATIVE(0x800EFF30u, GX__Begin_800eff30, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F0160u, GX__SetLineWidth_800f0160, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F01A0u, GX__SetPointSize_800f01a0, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F0220u, GX__SetCullMode_800f0220, cpu->gpr[3]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F1530u, GX__SetNumChans_800f1530, cpu->gpr[3]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F1560u, GX__SetChanCtrl_800F1560, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F1800u, GX__InitTexObj_800f1800, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F1A60u, GX__InitTexObjLOD_800f1a60, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2190u, GX__InvalidateTexAll_800F2190);
OTHERM_KNOWN_TYPED_NATIVE(0x800F1F90u, GX__LoadTexObj_800f1f90, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2BE0u, GX__SetNumIndStages_800f2be0, cpu->gpr[3]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2C00u, GX__SetTevDirect_800f2c00, cpu->gpr[3]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2DC0u, GX__SetTevColorIn_800f2dc0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2E00u, GX__SetTevAlphaIn_800f2e00, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2E40u, GX__SetTevColorOp_800f2e40, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2EA0u, GX__SetTevAlphaOp_800f2ea0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2F60u, GX__SetTevColorS10_800f2f60, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F2FD0u, GX__SetTevKColor_800f2fd0, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F3030u, GX__SetTevKColorSel_800f3030, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F3080u, GX__SetTevKAlphaSel_800f3080, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F30D0u, GX__SetTevSwapMode_800f30d0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F3110u, GX__SetTevSwapModeTable_800f3110, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F3260u, GX__SetTevOrder_800f3260, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F33C0u, GX__SetNumTevStages_800f33c0, cpu->gpr[3]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F3890u, GX__SetBlendMode_800f3890, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F3EB0u, GX__LoadPosMtxImm_800f3eb0, cpu->gpr[3], cpu->gpr[4]);
OTHERM_KNOWN_TYPED_NATIVE(0x800F3FD0u, GX__SetCurrentMtx_800F3FD0, cpu->gpr[3]);

#undef OTHERM_KNOWN_TYPED_NATIVE

#define OTHERM_KNOWN_NATIVE_CPU_CALL(addr, fn) \
template <> struct KnownNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr uint32_t kNonvolatileFprWriteMask = 0; \
    static constexpr void (*Entry)(CpuContext*) = &fn; \
}

#undef OTHERM_KNOWN_NATIVE_CPU_CALL
