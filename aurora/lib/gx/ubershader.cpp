#include "ubershader.hpp"

#include "../webgpu/gpu.hpp"
#include "gx.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

#include "tracy/Tracy.hpp"

namespace aurora::gx {
static Module Log("aurora::gx::uber");

// The WGSL below interprets what shader.cpp's build_shader writes out per config, and has to stay in step with it:
// every TEV, texgen, lighting, indirect and fog rule there has its twin here, including the integer rounding (TEV and
// lighting work in bytes, as the hardware does) and which TEV inputs get tev_overflow. Where build_shader decides
// something per config, pack_uber_config decides it on the CPU and leaves a bit in the config words.
//
// Config words (UberConfig, `ubuf.cfg`):
//   0 global: numTexGens [0,4) tevStageCount [4,5) numIndStages [9,3) fogType [12,4) dualTex 16 fogRangeAdjust 17
//     zFreeze 18 alphaFromPackedBlue 19 alphaCompare 21 vtxStride [24,8)
//   1 TEV swap table: entry s at [8s, 8s+8): red [0,2) green [2,2) blue [4,2) alpha [6,2)
//   2 color channels (COLOR0, COLOR1, ALPHA0, ALPHA1) c at [8c, 8c+8): matSrc 0 ambSrc 1 diffFn [2,2) attnFn [4,2)
//     lighting 6
//   3 alpha compare: comp0 [0,3) ref0 [3,8) op [11,2) comp1 [13,3) ref1 [16,8)
//   4 indirect stages s at [8s, 8s+8): texcoord [0,4) (15 = none) texmap [4,4)
//   5 indirect stages s at [8s, 8s+8): scaleS [0,4) scaleT [4,4)
//   6 used indirect stages [0,4); vertex color of channel 0 [4,2) and 1 [6,2) (0 white, 1 CLR0, 2 CLR1)
//   16 + attr (GX_VA_PNMTXIDX .. GX_VA_TEX7): type [0,2) count [2,4) compType [6,3) offset [9,8) stride [17,8)
//     frac [25,5) indexed NBT3 30 array little-endian 31
//   40 + texgen: type [0,4) src [4,5) matrix (0 identity, 1 slot, 2 per-vertex index) [9,2) slot [11,5)
//     post matrix 16, its index [17,5), normalize 22, inputForm AB11 23
//   48 + 4 * stage:
//     +0 color args a,b,c,d [0,16) alpha args a,b,c,d [16,12); tev_overflow on color a 28, b 29, c 30
//     +1 color op [0,4) bias [4,2) scale [6,2) out [8,2) clamp 10; tev_overflow on alpha a 11, b 12, c 13;
//        alpha op [16,4) bias [20,2) scale [22,2) out [24,2) clamp 26
//     +2 kcsel [0,5) kasel [5,5) raster (0 rast0, 1 rast1, 2 zero, 3 bump alpha, 4 normalized bump alpha) [10,3)
//        swap ras [13,2) swap tex [15,2) texcoord [17,4) (15 none) texmap [21,4) (15 none) can sample 25
//        samples 26 fixed texcoord state 27 add previous 28
//     +3 indirect offset (0 none, 1 matrix, 2 dynamic S, 3 dynamic T) [0,2) matrix [2,2) indirect stage [4,2)
//        format shift [6,3) bias STU [9,3) bias -128 12 wrap S [13,3) wrap T [16,3) bump alpha select [19,2)
//        bump format [21,2) bump valid 23
namespace {
constexpr u32 GlobalWord = 0;
constexpr u32 SwapWord = 1;
constexpr u32 ChannelWord = 2;
constexpr u32 AlphaCompareWord = 3;
constexpr u32 IndStageWord = 4;
constexpr u32 IndScaleWord = 5;
constexpr u32 MiscWord = 6;
constexpr u32 AttrWord = 16;
constexpr u32 TevWord = 48;
static_assert(AttrWord + GX_VA_TEX7 < UberTcgWord);
static_assert(UberTcgWord + MaxTexCoord <= TevWord);
static_assert(TevWord + MaxTevStages * 4 == UberConfigWordCount);

std::atomic<UberShaderMode> g_mode{UberShaderMode::Hybrid};
std::atomic<u32> g_generation{1};
std::atomic<u64> g_draws{0};

std::mutex g_moduleMutex;
wgpu::ShaderModule g_module;

constexpr std::string_view kUberSource = R"WGSL(
struct Light {
    pos: vec3f,
    dir: vec3f,
    color: vec4f,
    cos_att: vec3f,
    dist_att: vec3f,
};

struct Fog {
    color: vec4f,
    a: f32,
    b: f32,
    c: f32,
    pad: f32,
    range_base: vec4f,
    range_k: array<vec4f, 3>,
};

struct Uniform {
    vtx_start: u32,
    current_pnmtx: u32,
    render_viewport_size: vec2f,
    logical_viewport_size: vec2f,
    pad: vec2u,
    array_start: array<vec4u, 3>,
    proj: mat4x4f,
    postex_mtx: array<mat3x4f, 20>,
    nrm_mtx: array<mat3x4f, 10>,
    post_mtx: array<mat3x4f, 8>,
    tevregs: array<vec4f, 4>,
    kcolor: array<vec4f, 4>,
    cc_amb: array<vec4f, 4>,
    cc_mat: array<vec4f, 4>,
    lights: array<Light, 8>,
    light_state: vec4u,
    fog: Fog,
    ind_mtx: array<vec4i, 6>,
    texcoord_scale: array<vec4f, 8>,
    tex_size_bias: array<vec4f, 8>,
    zfreeze_plane: vec4f,
    cfg: array<vec4u, 28>,
};

@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;
@group(1) @binding(0)
var<uniform> ubuf: Uniform;
@group(2) @binding(0)
var tex0: texture_2d<f32>;
@group(2) @binding(1)
var tex0_samp: sampler;
@group(2) @binding(2)
var tex1: texture_2d<f32>;
@group(2) @binding(3)
var tex1_samp: sampler;
@group(2) @binding(4)
var tex2: texture_2d<f32>;
@group(2) @binding(5)
var tex2_samp: sampler;
@group(2) @binding(6)
var tex3: texture_2d<f32>;
@group(2) @binding(7)
var tex3_samp: sampler;
@group(2) @binding(8)
var tex4: texture_2d<f32>;
@group(2) @binding(9)
var tex4_samp: sampler;
@group(2) @binding(10)
var tex5: texture_2d<f32>;
@group(2) @binding(11)
var tex5_samp: sampler;
@group(2) @binding(12)
var tex6: texture_2d<f32>;
@group(2) @binding(13)
var tex6_samp: sampler;
@group(2) @binding(14)
var tex7: texture_2d<f32>;
@group(2) @binding(15)
var tex7_samp: sampler;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) cc0: vec4f,
    @location(1) cc1: vec4f,
    @location(2) tex0: vec3f,
    @location(3) tex1: vec3f,
    @location(4) tex2: vec3f,
    @location(5) tex3: vec3f,
    @location(6) tex4: vec3f,
    @location(7) tex5: vec3f,
    @location(8) tex6: vec3f,
    @location(9) tex7: vec3f,
};

fn cfgw(i: u32) -> u32 {
    return ubuf.cfg[i >> 2u][i & 3u];
}

fn fld(v: u32, off: u32, n: u32) -> u32 {
    return extractBits(v, off, n);
}

fn flag(v: u32, b: u32) -> bool {
    return ((v >> b) & 1u) != 0u;
}

// ---- Vertex pulling (shader.cpp's load_* / fetch_* helpers, with the format chosen at run time) ----

fn load_word(src: u32, idx: u32) -> u32 {
    if (src == 0u) {
        //VBUF_LOAD
    }
    //ABUF_LOAD
}

fn ld_u8(src: u32, off: u32) -> u32 {
    return (load_word(src, off >> 2u) >> ((off & 3u) * 8u)) & 0xFFu;
}

fn ld_raw32(src: u32, off: u32) -> u32 {
    let word_idx = off >> 2u;
    let sub = off & 3u;
    let lo = load_word(src, word_idx);
    if (sub == 0u) {
        return lo;
    }
    let hi = load_word(src, word_idx + 1u);
    let shift = sub * 8u;
    return (lo >> shift) | (hi << (32u - shift));
}

fn ld_u16(src: u32, off: u32, le: bool) -> u32 {
    let raw = ld_u8(src, off) | (ld_u8(src, off + 1u) << 8u);
    return select(((raw & 0xFFu) << 8u) | (raw >> 8u), raw, le);
}

fn ld_u24(src: u32, off: u32, le: bool) -> u32 {
    let raw = ld_u8(src, off) | (ld_u8(src, off + 1u) << 8u) | (ld_u8(src, off + 2u) << 16u);
    if (le) {
        return raw;
    }
    return ((raw & 0x0000FFu) << 16u) | (raw & 0x00FF00u) | ((raw & 0xFF0000u) >> 16u);
}

fn ld_u32(src: u32, off: u32, le: bool) -> u32 {
    let raw = ld_raw32(src, off);
    if (le) {
        return raw;
    }
    return ((raw & 0x000000FFu) << 24u) | ((raw & 0x0000FF00u) << 8u) | ((raw & 0x00FF0000u) >> 8u) |
           ((raw & 0xFF000000u) >> 24u);
}

fn comp_size(ct: u32) -> u32 {
    switch ct {
        case 0u, 1u: {
            return 1u;
        }
        case 2u, 3u: {
            return 2u;
        }
        default: {
            return 4u;
        }
    }
}

fn fetch_comp(src: u32, off: u32, ct: u32, frac: u32, le: bool) -> f32 {
    switch ct {
        case 0u: {
            return f32(ld_u8(src, off)) / f32(1u << frac);
        }
        case 1u: {
            return f32((bitcast<i32>(ld_u8(src, off)) << 24u) >> 24u) / f32(1u << frac);
        }
        case 2u: {
            return f32(ld_u16(src, off, le)) / f32(1u << frac);
        }
        case 3u: {
            return f32(bitcast<i32>(ld_u16(src, off, le) << 16u) >> 16u) / f32(1u << frac);
        }
        default: {
            return bitcast<f32>(ld_u32(src, off, le));
        }
    }
}

struct Addr {
    src: u32,
    off: u32,
    le: bool,
};

fn attr_word(attr: u32) -> u32 {
    return cfgw(16u + attr);
}

fn attr_present(attr: u32) -> bool {
    return fld(attr_word(attr), 0u, 2u) != 0u;
}

fn array_start(k: u32) -> u32 {
    return ubuf.array_start[k >> 2u][k & 3u];
}

// Where an attribute's data starts: in the vertex itself, or (GX_INDEX8 / GX_INDEX16) in its array. `group` picks an
// NBT3 normal's own index (its three rows are indexed separately).
fn attr_addr(attr: u32, w: u32, vbase: u32, group: u32) -> Addr {
    let kind = fld(w, 0u, 2u);
    let off = vbase + fld(w, 9u, 8u);
    if (kind == 2u || kind == 3u) {
        let nbt3 = flag(w, 30u);
        var index: u32;
        if (kind == 2u) {
            index = ld_u8(0u, off + select(0u, group, nbt3));
        } else {
            index = ld_u16(0u, off + select(0u, group * 2u, nbt3), false);
        }
        return Addr(1u, array_start(attr - 9u) + index * fld(w, 17u, 8u), flag(w, 31u));
    }
    return Addr(0u, off, false);
}

fn fetch_vec(ad: Addr, ct: u32, cnt: u32, frac: u32) -> vec4f {
    if (ct == 5u) {
        return unpack4x8unorm(ld_raw32(ad.src, ad.off));
    }
    let size = comp_size(ct);
    var v = vec4f(0.0);
    for (var k = 0u; k < min(cnt, 4u); k++) {
        v[k] = fetch_comp(ad.src, ad.off + k * size, ct, frac, ad.le);
    }
    return v;
}

fn fetch_color(ad: Addr, ct: u32) -> vec4f {
    switch ct {
        case 0u: {
            let v = ld_u16(ad.src, ad.off, ad.le);
            return vec4f(f32((v >> 11u) & 0x1Fu) / f32(0x1Fu), f32((v >> 5u) & 0x3Fu) / f32(0x3Fu),
                         f32(v & 0x1Fu) / f32(0x1Fu), 1.0);
        }
        case 1u, 2u: {
            return vec4f(f32(ld_u8(ad.src, ad.off)), f32(ld_u8(ad.src, ad.off + 1u)),
                         f32(ld_u8(ad.src, ad.off + 2u)), 255.0) / 255.0;
        }
        case 3u: {
            let v = ld_u16(ad.src, ad.off, ad.le);
            return vec4f(f32((v >> 12u) & 0x0Fu) / f32(0x0Fu), f32((v >> 8u) & 0x0Fu) / f32(0x0Fu),
                         f32((v >> 4u) & 0x0Fu) / f32(0x0Fu), f32(v & 0x0Fu) / f32(0x0Fu));
        }
        case 4u: {
            let v = ld_u24(ad.src, ad.off, ad.le);
            return vec4f(f32((v >> 18u) & 0x3Fu) / f32(0x3Fu), f32((v >> 12u) & 0x3Fu) / f32(0x3Fu),
                         f32((v >> 6u) & 0x3Fu) / f32(0x3Fu), f32(v & 0x3Fu) / f32(0x3Fu));
        }
        default: {
            return vec4f(f32(ld_u8(ad.src, ad.off)), f32(ld_u8(ad.src, ad.off + 1u)),
                         f32(ld_u8(ad.src, ad.off + 2u)), f32(ld_u8(ad.src, ad.off + 3u))) / 255.0;
        }
    }
}

// POS / NRM / TEXn: a missing third (or second) component reads as 0, as build_shader's vec3f(xy, 0.0).
fn load_vec_attr(attr: u32, vbase: u32, cnt_override: u32) -> vec4f {
    let w = attr_word(attr);
    let cnt = select(fld(w, 2u, 4u), cnt_override, cnt_override != 0u);
    return fetch_vec(attr_addr(attr, w, vbase, 0u), fld(w, 6u, 3u), cnt, fld(w, 25u, 5u));
}

fn load_idx_attr(attr: u32, vbase: u32) -> u32 {
    let w = attr_word(attr);
    let ad = attr_addr(attr, w, vbase, 0u);
    return ld_u8(ad.src, ad.off);
}

fn tex_attr(n: u32, vbase: u32) -> vec2f {
    if (!attr_present(13u + n)) {
        return vec2f(0.0, 0.0);
    }
    return load_vec_attr(13u + n, vbase, 0u).xy;
}

// shader.cpp's normal_group_load: row `group` of an NBT normal (1 binormal, 2 tangent in GX's texgen source order).
fn load_nbt(vbase: u32, group: u32) -> vec3f {
    let w = attr_word(10u);
    if (fld(w, 0u, 2u) == 0u) {
        return vec3f(1.0, 0.0, 0.0);
    }
    if (fld(w, 2u, 4u) != 9u) {
        return load_vec_attr(10u, vbase, 0u).xyz;
    }
    var ad = attr_addr(10u, w, vbase, group);
    let ct = fld(w, 6u, 3u);
    if (!flag(w, 30u)) {
        ad.off = ad.off + group * 3u * comp_size(ct);
    }
    return fetch_vec(ad, ct, 3u, fld(w, 25u, 5u)).xyz;
}

// ---- Vertex lighting (shader.cpp's lighting_func) ----

fn light_factor(ccw: u32, light: Light, mv_pos: vec3f, mv_nrm: vec3f) -> f32 {
    let diff_fn = fld(ccw, 2u, 2u);
    let attn_fn = fld(ccw, 4u, 2u);
    var ldir = light.pos - mv_pos;
    let dist2 = dot(ldir, ldir);
    let dist = sqrt(dist2);
    if (attn_fn == 2u) {
        ldir = select(mv_nrm, ldir / max(dist, 1e-20), dist > 0.0);
    } else {
        ldir = ldir / dist;
    }
    var attn: f32;
    if (attn_fn == 2u) {
        attn = 1.0;
    } else if (attn_fn == 1u) {
        let cosine = max(0.0, dot(ldir, light.dir));
        let cos_attn = dot(light.cos_att, vec3f(1.0, cosine, cosine * cosine));
        let dist_attn = dot(light.dist_att, vec3f(1.0, dist, dist2));
        attn = max(0.0, cos_attn / dist_attn);
    } else {
        attn = select(0.0, max(0.0, dot(mv_nrm, light.dir)), dot(mv_nrm, ldir) >= 0.0);
        let cos_attn = dot(light.cos_att, vec3f(1.0, attn, attn * attn));
        var dist_attn: f32;
        if (diff_fn != 0u) {
            dist_attn = dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn));
        } else {
            dist_attn = dot(light.dist_att, vec3f(1.0, attn, attn * attn));
        }
        attn = select(select(0.0, 1.0, cos_attn > 0.0), max(0.0, cos_attn / dist_attn), dist_attn != 0.0);
    }
    var diff = 1.0;
    if (diff_fn == 1u) {
        diff = dot(ldir, mv_nrm);
    } else if (diff_fn == 2u) {
        diff = max(0.0, dot(ldir, mv_nrm));
    }
    return attn * diff;
}

fn lit_color(ccw: u32, mat_src: vec4f, amb_src: vec4f, mask: u32, mv_pos: vec3f, mv_nrm: vec3f) -> vec3f {
    if (!flag(ccw, 6u)) {
        return vec3f(vec3i(round(mat_src.rgb * 255.0))) / 255.0;
    }
    var lighting = vec3i(round(amb_src.rgb * 255.0));
    for (var i = 0u; i < 8u; i++) {
        if ((mask & (1u << i)) == 0u) {
            continue;
        }
        let light = ubuf.lights[i];
        lighting = lighting + vec3i(round(light_factor(ccw, light, mv_pos, mv_nrm) * light.color.rgb * 255.0));
    }
    let lacc = clamp(lighting, vec3i(0), vec3i(255));
    let material = vec3i(round(mat_src.rgb * 255.0));
    return vec3f((material * (lacc + (lacc >> vec3u(7u)))) >> vec3u(8u)) / 255.0;
}

fn lit_alpha(ccw: u32, mat_src: vec4f, amb_src: vec4f, mask: u32, mv_pos: vec3f, mv_nrm: vec3f) -> f32 {
    if (!flag(ccw, 6u)) {
        return f32(i32(round(mat_src.a * 255.0))) / 255.0;
    }
    var lighting = i32(round(amb_src.a * 255.0));
    for (var i = 0u; i < 8u; i++) {
        if ((mask & (1u << i)) == 0u) {
            continue;
        }
        let light = ubuf.lights[i];
        lighting = lighting + i32(round(light_factor(ccw, light, mv_pos, mv_nrm) * light.color.a * 255.0));
    }
    let lacc = clamp(lighting, 0, 255);
    let material = i32(round(mat_src.a * 255.0));
    return f32((material * (lacc + (lacc >> 7u))) >> 8u) / 255.0;
}

fn vertex_color(sel: u32, clr0: vec4f, clr1: vec4f) -> vec4f {
    if (sel == 1u) {
        return clr0;
    }
    if (sel == 2u) {
        return clr1;
    }
    return vec4f(1.0);
}

fn channel_color(ch: u32, clr0: vec4f, clr1: vec4f, mv_pos: vec3f, mv_nrm: vec3f) -> vec4f {
    let vc = vertex_color(fld(cfgw(6u), 4u + ch * 2u, 2u), clr0, clr1);
    let cw = fld(cfgw(2u), ch * 8u, 8u);
    let aw = fld(cfgw(2u), (ch + 2u) * 8u, 8u);
    var c = vec4f(lit_color(cw, select(ubuf.cc_mat[ch], vc, flag(cw, 0u)), select(ubuf.cc_amb[ch], vc, flag(cw, 1u)),
                            ubuf.light_state[ch], mv_pos, mv_nrm), 1.0);
    c.a = lit_alpha(aw, select(ubuf.cc_mat[ch + 2u], vc, flag(aw, 0u)), select(ubuf.cc_amb[ch + 2u], vc, flag(aw, 1u)),
                    ubuf.light_state[ch + 2u], mv_pos, mv_nrm);
    return c;
}

@vertex
fn vs_main(@builtin(vertex_index) vidx: u32) -> VertexOutput {
    var out: VertexOutput;
    let g0 = cfgw(0u);
    let vbase = ubuf.vtx_start + vidx * fld(g0, 24u, 8u);

    var pnmtx = ubuf.current_pnmtx;
    if (attr_present(0u)) {
        pnmtx = load_idx_attr(0u, vbase) / 3u;
    }
    let pos = load_vec_attr(9u, vbase, 0u).xyz;
    var nrm = vec3f(1.0, 0.0, 0.0);
    let nw = attr_word(10u);
    if (fld(nw, 0u, 2u) != 0u) {
        nrm = load_vec_attr(10u, vbase, select(0u, 3u, fld(nw, 2u, 4u) == 9u)).xyz;
    }
    var clr0 = vec4f(1.0);
    if (attr_present(11u)) {
        let w = attr_word(11u);
        clr0 = fetch_color(attr_addr(11u, w, vbase, 0u), fld(w, 6u, 3u));
    }
    var clr1 = vec4f(1.0);
    if (attr_present(12u)) {
        let w = attr_word(12u);
        clr1 = fetch_color(attr_addr(12u, w, vbase, 0u), fld(w, 6u, 3u));
    }

    // Matrix memory is uploaded whole; an index past it reads the last slot, as a clamped access of build_shader's
    // array does.
    let mv_pos = vec4f(pos, 1.0) * ubuf.postex_mtx[min(pnmtx, 19u)];
    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;
    if (flag(g0, 18u)) {
        let zf_ndc = out.pos.xy / out.pos.w;
        out.pos.z = (ubuf.zfreeze_plane.x * zf_ndc.x + ubuf.zfreeze_plane.y * zf_ndc.y + ubuf.zfreeze_plane.z) * out.pos.w;
    }
    let gx_pixel_center_correction = vec2f(-1.0, 1.0) / (6.0 * max(abs(ubuf.render_viewport_size), vec2f(1.0)));
    out.pos = vec4f(out.pos.xy + out.pos.w * gx_pixel_center_correction, out.pos.zw);
    let nrm_tmp = vec4f(nrm, 0.0) * ubuf.nrm_mtx[min(pnmtx, 9u)];
    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);

    out.cc0 = channel_color(0u, clr0, clr1, mv_pos, mv_nrm);
    out.cc1 = channel_color(1u, clr0, clr1, mv_pos, mv_nrm);

    var tex_out: array<vec3f, 8>;
    let num_tex_gens = fld(g0, 0u, 4u);
    for (var i = 0u; i < num_tex_gens; i++) {
        let w = cfgw(40u + i);
        let tg_type = fld(w, 0u, 4u);
        let src = fld(w, 4u, 5u);
        var tc = vec4f(0.0);
        if (src == 0u) {
            tc = vec4f(pos, 1.0);
        } else if (src == 1u) {
            tc = vec4f(nrm, 1.0);
        } else if (src == 2u) {
            tc = vec4f(load_nbt(vbase, 1u), 1.0);
        } else if (src == 3u) {
            tc = vec4f(load_nbt(vbase, 2u), 1.0);
        } else if (src >= 4u && src <= 11u) {
            tc = vec4f(tex_attr(src - 4u, vbase), 1.0, 1.0);
        } else if (src == 19u) {
            tc = vec4f(clr0.rgb, 1.0);
        } else if (src == 20u) {
            tc = vec4f(clr1.rgb, 1.0);
        } else if (src == 21u) {
            tc = vec4f(tex_attr(i, vbase), 1.0, 1.0);
        }
        if (flag(w, 23u)) {
            tc.z = 1.0;
        }
        var tmp = tc.xyz;
        if (tg_type <= 1u) {
            let mode = fld(w, 9u, 2u);
            if (mode == 2u) {
                tmp = tc * ubuf.postex_mtx[min(load_idx_attr(1u + i, vbase) / 3u, 19u)];
            } else if (mode == 1u) {
                tmp = tc * ubuf.postex_mtx[fld(w, 11u, 5u)];
            }
            if (tg_type == 1u) {
                tmp.z = 1.0;
            }
        } else if (tg_type == 10u) {
            tmp = vec3f(tc.xy, 1.0);
        }
        if (flag(w, 22u)) {
            tmp = normalize(tmp);
        }
        var proj = tmp;
        if (flag(w, 16u)) {
            proj = vec4f(tmp.xyz, 1.0) * ubuf.post_mtx[i];
        }
        if (tg_type <= 1u && proj.z == 0.0) {
            proj.x = clamp(proj.x / 2.0, -1.0, 1.0);
            proj.y = clamp(proj.y / 2.0, -1.0, 1.0);
        }
        tex_out[i] = proj;
    }
    out.tex0 = tex_out[0];
    out.tex1 = tex_out[1];
    out.tex2 = tex_out[2];
    out.tex3 = tex_out[3];
    out.tex4 = tex_out[4];
    out.tex5 = tex_out[5];
    out.tex6 = tex_out[6];
    out.tex7 = tex_out[7];
    return out;
}

// ---- TEV (shader.cpp's tev_* helpers, then the per-stage logic of build_shader) ----

fn tev_overflow_f32(in: f32) -> f32 {
    let byte_space = in * 255.0;
    return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}

fn tev_overflow_vec3f(in: vec3f) -> vec3f {
    let byte_space = in * 255.0;
    return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}

fn tev_overflow_vec4f(in: vec4f) -> vec4f {
    let byte_space = in * 255.0;
    return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}

fn tev_s24_fixed_vec2i(in: vec2i) -> vec2i {
    return (in << vec2u(8u)) >> vec2u(8u);
}

fn tev_byte_f32(in: f32) -> i32 {
    return i32(round(in * 255.0));
}

fn tev_byte_vec3f(in: vec3f) -> vec3i {
    return vec3i(round(in * 255.0));
}

fn tev_regular_i32(a: i32, b: i32, c: i32, d: i32, bias: i32, scale: u32, is_sub: bool) -> i32 {
    var d_part = d + bias;
    var lerp_part = (a << 8u) + (b - a) * (c + (c >> 7u));
    if (scale == 1u) {
        d_part = d_part << 1u;
        lerp_part = lerp_part << 1u;
    }
    if (scale == 2u) {
        d_part = d_part << 2u;
        lerp_part = lerp_part << 2u;
    }
    if (scale != 3u) {
        lerp_part += select(128, 127, is_sub);
    }
    let lerp = lerp_part >> 8u;
    var result = select(d_part + lerp, d_part - lerp, is_sub);
    if (scale == 3u) {
        result = result >> 1u;
    }
    return result;
}

fn tev_regular_f32(a: f32, b: f32, c: f32, d: f32, bias: i32, scale: u32, is_sub: bool) -> f32 {
    return f32(tev_regular_i32(tev_byte_f32(a), tev_byte_f32(b), tev_byte_f32(c), tev_byte_f32(d), bias, scale, is_sub)) /
           255.0;
}

fn tev_regular_vec3f(a: vec3f, b: vec3f, c: vec3f, d: vec3f, bias: i32, scale: u32, is_sub: bool) -> vec3f {
    let ai = tev_byte_vec3f(a);
    let bi = tev_byte_vec3f(b);
    let ci = tev_byte_vec3f(c);
    let di = tev_byte_vec3f(d);
    return vec3f(
        f32(tev_regular_i32(ai.x, bi.x, ci.x, di.x, bias, scale, is_sub)) / 255.0,
        f32(tev_regular_i32(ai.y, bi.y, ci.y, di.y, bias, scale, is_sub)) / 255.0,
        f32(tev_regular_i32(ai.z, bi.z, ci.z, di.z, bias, scale, is_sub)) / 255.0,
    );
}

fn tev_bias(b: u32) -> i32 {
    if (b == 1u) {
        return 128;
    }
    if (b == 2u) {
        return -128;
    }
    return 0;
}

// GX_TEV_COMP_* with color inputs a / b; `c` and `d` are the combiner's own (vec3 for color, f32 for alpha).
fn tev_compare_true(op: u32, a: vec3f, b: vec3f) -> bool {
    switch op {
        case 8u: {
            return round(a.r * 255.0) > round(b.r * 255.0);
        }
        case 9u: {
            return round(a.r * 255.0) == round(b.r * 255.0);
        }
        case 10u: {
            return round(dot(a.rg * 255.0, vec2(1.0, 256.0))) > round(dot(b.rg * 255.0, vec2(1.0, 256.0)));
        }
        case 11u: {
            return round(dot(a.rg * 255.0, vec2(1.0, 256.0))) == round(dot(b.rg * 255.0, vec2(1.0, 256.0)));
        }
        case 12u: {
            return round(dot(a.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) >
                   round(dot(b.rgb * 255.0, vec3(1.0, 256.0, 65536.0)));
        }
        default: {
            return round(dot(a.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) ==
                   round(dot(b.rgb * 255.0, vec3(1.0, 256.0, 65536.0)));
        }
    }
}

fn konst_color(sel: u32) -> vec3f {
    if (sel < 8u) {
        return vec3f(f32(8u - sel) / 8.0);
    }
    let k = ubuf.kcolor[sel & 3u];
    if (sel < 16u) {
        return k.rgb;
    }
    if (sel < 20u) {
        return vec3f(k.r);
    }
    if (sel < 24u) {
        return vec3f(k.g);
    }
    if (sel < 28u) {
        return vec3f(k.b);
    }
    return vec3f(k.a);
}

fn konst_alpha(sel: u32) -> f32 {
    if (sel < 8u) {
        return f32(8u - sel) / 8.0;
    }
    let k = ubuf.kcolor[sel & 3u];
    if (sel < 20u) {
        return k.r;
    }
    if (sel < 24u) {
        return k.g;
    }
    if (sel < 28u) {
        return k.b;
    }
    return k.a;
}

fn color_arg(sel: u32, prev: vec4f, r0: vec4f, r1: vec4f, r2: vec4f, texc: vec3f, texa: f32, rasc: vec3f, rasa: f32,
             konst: vec3f) -> vec3f {
    switch sel {
        case 0u: {
            return prev.rgb;
        }
        case 1u: {
            return vec3f(prev.a);
        }
        case 2u: {
            return r0.rgb;
        }
        case 3u: {
            return vec3f(r0.a);
        }
        case 4u: {
            return r1.rgb;
        }
        case 5u: {
            return vec3f(r1.a);
        }
        case 6u: {
            return r2.rgb;
        }
        case 7u: {
            return vec3f(r2.a);
        }
        case 8u: {
            return texc;
        }
        case 9u: {
            return vec3f(texa);
        }
        case 10u: {
            return rasc;
        }
        case 11u: {
            return vec3f(rasa);
        }
        case 12u: {
            return vec3f(1.0);
        }
        case 13u: {
            return vec3f(0.5);
        }
        case 14u: {
            return konst;
        }
        default: {
            return vec3f(0.0);
        }
    }
}

fn alpha_arg(sel: u32, prev: vec4f, r0: vec4f, r1: vec4f, r2: vec4f, texa: f32, rasa: f32, konst: f32) -> f32 {
    switch sel {
        case 0u: {
            return prev.a;
        }
        case 1u: {
            return r0.a;
        }
        case 2u: {
            return r1.a;
        }
        case 3u: {
            return r2.a;
        }
        case 4u: {
            return texa;
        }
        case 5u: {
            return rasa;
        }
        case 6u: {
            return konst;
        }
        default: {
            return 0.0;
        }
    }
}

fn swz(v: vec4f, e: u32) -> vec4f {
    return vec4f(v[fld(e, 0u, 2u)], v[fld(e, 2u, 2u)], v[fld(e, 4u, 2u)], v[fld(e, 6u, 2u)]);
}

fn sample_map(map: u32, uv: vec2f, bias: f32) -> vec4f {
    switch map {
        case 0u: {
            return textureSampleBias(tex0, tex0_samp, uv, bias);
        }
        case 1u: {
            return textureSampleBias(tex1, tex1_samp, uv, bias);
        }
        case 2u: {
            return textureSampleBias(tex2, tex2_samp, uv, bias);
        }
        case 3u: {
            return textureSampleBias(tex3, tex3_samp, uv, bias);
        }
        case 4u: {
            return textureSampleBias(tex4, tex4_samp, uv, bias);
        }
        case 5u: {
            return textureSampleBias(tex5, tex5_samp, uv, bias);
        }
        case 6u: {
            return textureSampleBias(tex6, tex6_samp, uv, bias);
        }
        default: {
            return textureSampleBias(tex7, tex7_samp, uv, bias);
        }
    }
}

fn fixed_uv(uv: vec2f, tc: u32) -> vec2i {
    return vec2i(uv * ubuf.texcoord_scale[tc].xy * 128.0);
}

// tev_indirect_wrap_mask: 0 off, 1-5 GX_ITW_256 .. GX_ITW_16, 6 and up GX_ITW_0.
fn tev_wrap(mode: u32, v: i32) -> i32 {
    if (mode == 0u) {
        return v;
    }
    if (mode >= 6u) {
        return 0;
    }
    let mask = ((256 >> (mode - 1u)) << 7u) - 1;
    return v & mask;
}

fn bump_alpha(w3: u32, coord: vec3i) -> f32 {
    if (!flag(w3, 23u)) {
        return 0.0;
    }
    let sel = fld(w3, 19u, 2u);
    var c = coord.x;
    if (sel == 2u) {
        c = coord.y;
    } else if (sel == 3u) {
        c = coord.z;
    }
    let format = fld(w3, 21u, 2u);
    var mask = 0xF8u;
    if (format == 1u) {
        mask = 0xE0u;
    } else if (format == 2u) {
        mask = 0xF0u;
    }
    return f32(u32(c) & mask) / 255.0;
}

fn alpha_test(comp: u32, v: u32, refv: u32) -> bool {
    switch comp {
        case 0u: {
            return false;
        }
        case 1u: {
            return v < refv;
        }
        case 2u: {
            return v == refv;
        }
        case 3u: {
            return v <= refv;
        }
        case 4u: {
            return v > refv;
        }
        case 5u: {
            return v != refv;
        }
        case 6u: {
            return v >= refv;
        }
        default: {
            return true;
        }
    }
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let g0 = cfgw(0u);
    let num_tex_gens = fld(g0, 0u, 4u);
    let tev_count = fld(g0, 4u, 5u);
    let num_ind = fld(g0, 9u, 3u);
    let fog_type = fld(g0, 12u, 4u);
    let disabled_tex = select(1.0, 0.0, num_tex_gens == 0u);

    var uvw = array<vec3f, 8>(in.tex0, in.tex1, in.tex2, in.tex3, in.tex4, in.tex5, in.tex6, in.tex7);
    var uv: array<vec2f, 8>;
    for (var i = 0u; i < 8u; i++) {
        let w = uvw[i];
        // GX_TG_MTX3x4 coordinates are projective.
        uv[i] = select(w.xy, w.xy / w.z, fld(cfgw(40u + i), 0u, 4u) == 0u && w.z != 0.0);
    }

    var ind_coord: array<vec3i, 4>;
    let ind_maps = cfgw(4u);
    let ind_scales = cfgw(5u);
    let misc = cfgw(6u);
    for (var s = 0u; s < num_ind; s++) {
        if (!flag(misc, s)) {
            continue;
        }
        let iw = fld(ind_maps, s * 8u, 8u);
        let tc = fld(iw, 0u, 4u);
        let map = fld(iw, 4u, 4u);
        var fixed_coord = vec2i(0);
        if (tc < 8u) {
            fixed_coord = fixed_uv(uv[tc], tc);
        }
        let sc = fld(ind_scales, s * 8u, 8u);
        let sample_fixed = fixed_coord >> vec2u(fld(sc, 0u, 4u), fld(sc, 4u, 4u));
        let sb = ubuf.tex_size_bias[map];
        ind_coord[s] = tev_byte_vec3f(sample_map(map, vec2f(sample_fixed) / (sb.xy * 128.0), sb.z).abg);
    }

    var regs = ubuf.tevregs;
    var rast = array<vec4f, 2>(in.cc0, in.cc1);
    let swap_table = cfgw(1u);
    var t_tex_coord = vec2i(0);
    var last_color_out = 0u;
    var last_alpha_out = 0u;
    for (var i = 0u; i < tev_count; i++) {
        let base = 48u + i * 4u;
        let w0 = cfgw(base);
        let w1 = cfgw(base + 1u);
        let w2 = cfgw(base + 2u);
        let w3 = cfgw(base + 3u);
        let tex_coord = fld(w2, 17u, 4u);
        let tex_map = fld(w2, 21u, 4u);
        let can_sample = flag(w2, 25u);
        let will_sample = flag(w2, 26u);
        let needs_tc = flag(w2, 27u);

        var sampled = vec4f(0.0);
        if (needs_tc || will_sample) {
            var uv_in = vec2f(0.0);
            var have_uv = false;
            if (needs_tc) {
                var base_uv = vec2i(0);
                if (tex_coord < 8u) {
                    base_uv = fixed_uv(uv[tex_coord], tex_coord);
                }
                var offset = vec2i(0);
                let ind_mode = fld(w3, 0u, 2u);
                if (ind_mode != 0u) {
                    let mtx = fld(w3, 2u, 2u);
                    var coord = ind_coord[fld(w3, 4u, 2u)] >> vec3u(fld(w3, 6u, 3u));
                    let bias_sel = fld(w3, 9u, 3u);
                    if (bias_sel != 0u) {
                        let bias = select(1, -128, flag(w3, 12u));
                        coord = coord + vec3i(i32(bias_sel & 1u), i32((bias_sel >> 1u) & 1u), i32((bias_sel >> 2u) & 1u)) * bias;
                    }
                    if (ind_mode == 1u) {
                        let c0 = ubuf.ind_mtx[mtx * 2u];
                        let c1 = ubuf.ind_mtx[mtx * 2u + 1u];
                        offset = vec2i(
                            (c0.x * coord.x + c0.z * coord.y + c1.x * coord.z) >> 3u,
                            (c0.y * coord.x + c0.w * coord.y + c1.y * coord.z) >> 3u);
                    } else if (ind_mode == 2u) {
                        offset = (base_uv * vec2i(coord.x)) >> vec2u(8u);
                    } else {
                        offset = (base_uv * vec2i(coord.y)) >> vec2u(8u);
                    }
                    let shift = ubuf.ind_mtx[mtx * 2u + 1u].z;
                    if (shift >= 0) {
                        offset = offset >> vec2u(u32(shift));
                    } else {
                        offset = offset << vec2u(u32(-shift));
                    }
                }
                let final_coord = vec2i(tev_wrap(fld(w3, 13u, 3u), base_uv.x), tev_wrap(fld(w3, 16u, 3u), base_uv.y)) + offset;
                if (flag(w2, 28u)) {
                    t_tex_coord = t_tex_coord + final_coord;
                } else {
                    t_tex_coord = final_coord;
                }
                t_tex_coord = tev_s24_fixed_vec2i(t_tex_coord);
                if (will_sample) {
                    uv_in = vec2f(t_tex_coord) / (ubuf.tex_size_bias[tex_map].xy * 128.0);
                    have_uv = true;
                }
            }
            if (will_sample) {
                if (!have_uv) {
                    uv_in = vec2f(tev_s24_fixed_vec2i(fixed_uv(uv[tex_coord], tex_coord))) /
                            (ubuf.tex_size_bias[tex_map].xy * 128.0);
                }
                sampled = sample_map(tex_map, uv_in, ubuf.tex_size_bias[tex_map].z);
            }
        }

        let texv = swz(sampled, fld(swap_table, fld(w2, 15u, 2u) * 8u, 8u));
        let texc = select(vec3f(disabled_tex), texv.rgb, can_sample);
        let texa = select(disabled_tex, texv.a, can_sample);
        let ras_sel = fld(w2, 10u, 3u);
        var rasc = vec3f(0.0);
        var rasa = 0.0;
        if (ras_sel < 2u) {
            let rv = swz(rast[ras_sel], fld(swap_table, fld(w2, 13u, 2u) * 8u, 8u));
            rasc = rv.rgb;
            rasa = rv.a;
        } else if (ras_sel >= 3u) {
            var bump = bump_alpha(w3, ind_coord[fld(w3, 4u, 2u)]);
            if (ras_sel == 4u) {
                bump = bump * (255.0 / 248.0);
            }
            rasc = vec3f(bump);
            rasa = bump;
        }
        let kc = konst_color(fld(w2, 0u, 5u));
        let ka = konst_alpha(fld(w2, 5u, 5u));

        // Every input is read before this stage writes: the color result leaves the registers' alpha alone.
        var ca = color_arg(fld(w0, 0u, 4u), regs[0], regs[1], regs[2], regs[3], texc, texa, rasc, rasa, kc);
        var cb = color_arg(fld(w0, 4u, 4u), regs[0], regs[1], regs[2], regs[3], texc, texa, rasc, rasa, kc);
        var cc = color_arg(fld(w0, 8u, 4u), regs[0], regs[1], regs[2], regs[3], texc, texa, rasc, rasa, kc);
        let cd = color_arg(fld(w0, 12u, 4u), regs[0], regs[1], regs[2], regs[3], texc, texa, rasc, rasa, kc);
        if (flag(w0, 28u)) {
            ca = tev_overflow_vec3f(ca);
        }
        if (flag(w0, 29u)) {
            cb = tev_overflow_vec3f(cb);
        }
        if (flag(w0, 30u)) {
            cc = tev_overflow_vec3f(cc);
        }
        var aa = alpha_arg(fld(w0, 16u, 3u), regs[0], regs[1], regs[2], regs[3], texa, rasa, ka);
        var ab = alpha_arg(fld(w0, 19u, 3u), regs[0], regs[1], regs[2], regs[3], texa, rasa, ka);
        var ac = alpha_arg(fld(w0, 22u, 3u), regs[0], regs[1], regs[2], regs[3], texa, rasa, ka);
        let ad = alpha_arg(fld(w0, 25u, 3u), regs[0], regs[1], regs[2], regs[3], texa, rasa, ka);
        if (flag(w1, 11u)) {
            aa = tev_overflow_f32(aa);
        }
        if (flag(w1, 12u)) {
            ab = tev_overflow_f32(ab);
        }
        if (flag(w1, 13u)) {
            ac = tev_overflow_f32(ac);
        }

        let color_op = fld(w1, 0u, 4u);
        var cres: vec3f;
        if (color_op <= 1u) {
            cres = tev_regular_vec3f(ca, cb, cc, cd, tev_bias(fld(w1, 4u, 2u)), fld(w1, 6u, 2u), color_op == 1u);
        } else if (color_op == 14u) {
            cres = select(vec3(0), cc, round(ca * 255.0) > round(cb * 255.0)) + cd;
        } else if (color_op == 15u) {
            cres = select(vec3(0), cc, round(ca * 255.0) == round(cb * 255.0)) + cd;
        } else {
            cres = select(vec3(0), cc, tev_compare_true(color_op, ca, cb)) + cd;
        }
        if (flag(w1, 10u)) {
            cres = clamp(cres, vec3f(0.0), vec3f(1.0));
        } else {
            cres = clamp(cres, vec3f(-1024.0 / 255.0), vec3f(1023.0 / 255.0));
        }

        let alpha_op = fld(w1, 16u, 4u);
        var ares: f32;
        if (alpha_op <= 1u) {
            ares = tev_regular_f32(aa, ab, ac, ad, tev_bias(fld(w1, 20u, 2u)), fld(w1, 22u, 2u), alpha_op == 1u);
        } else if (alpha_op == 14u) {
            ares = select(0.0, ac, round(aa * 255.0) > round(ab * 255.0)) + ad;
        } else if (alpha_op == 15u) {
            ares = select(0.0, ac, round(aa * 255.0) == round(ab * 255.0)) + ad;
        } else {
            // R8 / GR16 / BGR24 compare the color combiner's A and B.
            ares = select(0.0, ac, tev_compare_true(alpha_op, ca, cb)) + ad;
        }
        if (flag(w1, 26u)) {
            ares = clamp(ares, 0.0, 1.0);
        } else {
            ares = clamp(ares, -1024.0 / 255.0, 1023.0 / 255.0);
        }

        last_color_out = fld(w1, 8u, 2u);
        regs[last_color_out] = vec4f(cres, regs[last_color_out].a);
        last_alpha_out = fld(w1, 24u, 2u);
        regs[last_alpha_out].a = ares;
    }
    var prev = vec4f(regs[last_color_out].rgb, regs[last_alpha_out].a);

    if (fog_type != 0u) {
        let fog_z_coord = u32(round(clamp(1.0 - in.pos.z, 0.0, 1.0) * 16777216.0));
        var fog_ze: f32;
        if ((fog_type & 8u) == 0u) {
            let fog_shifted_z = fog_z_coord >> u32(ubuf.fog.pad);
            fog_ze = (ubuf.fog.a * 16777216.0) / (ubuf.fog.b - f32(fog_shifted_z));
        } else {
            fog_ze = ubuf.fog.a * f32(fog_z_coord) / 16777216.0;
        }
        if (flag(g0, 17u)) {
            let fog_range_offset = (2.0 * (in.pos.x / ubuf.fog.range_base.y)) - 1.0 - ubuf.fog.range_base.x;
            let fog_range_float_index = clamp(9.0 - abs(fog_range_offset) * 9.0, 0.0, 9.0);
            let fog_range_index_lower = u32(fog_range_float_index);
            let fog_range_index_upper = fog_range_index_lower + 1u;
            let fog_range_k_lower = ubuf.fog.range_k[fog_range_index_lower >> 2u][fog_range_index_lower & 3u];
            let fog_range_k_upper = ubuf.fog.range_k[fog_range_index_upper >> 2u][fog_range_index_upper & 3u];
            let fog_range_k = mix(fog_range_k_lower, fog_range_k_upper, fract(fog_range_float_index));
            fog_ze *= sqrt(fog_range_offset * fog_range_offset + fog_range_k * fog_range_k) / fog_range_k;
        }
        var fog_f = clamp(fog_ze - ubuf.fog.c, 0.0, 1.0);
        var fog_z = fog_f;
        switch fog_type {
            case 4u, 12u: {
                fog_z = 1.0 - exp2(-8.0 * fog_f);
            }
            case 5u, 13u: {
                fog_z = 1.0 - exp2(-8.0 * fog_f * fog_f);
            }
            case 6u, 14u: {
                fog_z = exp2(-8.0 * (1.0 - fog_f));
            }
            case 7u, 15u: {
                fog_f = 1.0 - fog_f;
                fog_z = exp2(-8.0 * fog_f * fog_f);
            }
            default: {
            }
        }
        prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fog_z, 0.0, 1.0)), prev.a);
    }
    prev = tev_overflow_vec4f(prev);

    if (flag(g0, 21u)) {
        let ac = cfgw(3u);
        let alpha_value = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));
        let pass0 = alpha_test(fld(ac, 0u, 3u), alpha_value, fld(ac, 3u, 8u));
        let pass1 = alpha_test(fld(ac, 13u, 3u), alpha_value, fld(ac, 16u, 8u));
        var passed = pass0 && pass1;
        let op = fld(ac, 11u, 2u);
        if (op == 1u) {
            passed = pass0 || pass1;
        } else if (op == 2u) {
            passed = pass0 != pass1;
        } else if (op == 3u) {
            passed = pass0 == pass1;
        }
        if (!passed) {
            discard;
        }
    }

    if (flag(g0, 19u)) {
        prev = vec4f(prev.rgb, f32(u32(round(clamp(prev.b, 0.0, 1.0) * 255.0)) & 63u) / 63.0);
    }
    return prev;
}
)WGSL";

wgpu::ShaderModule build_uber_module() {
  ZoneScoped;
  // The GPU compatibility path's bounds-checked storage reads (shader.cpp's load_word), here per buffer.
  std::string source{kUberSource};
  const auto replace = [&](std::string_view marker, std::string_view code) {
    const auto at = source.find(marker);
    CHECK(at != std::string::npos, "ubershader source lost its {} marker", marker);
    source.replace(at, marker.size(), code);
  };
  if (webgpu::g_safeVertexFetch) {
    replace("//VBUF_LOAD", "if (idx < arrayLength(&vbuf)) { return vbuf[idx]; }\n        return 0u;");
    replace("//ABUF_LOAD", "if (idx < arrayLength(&abuf)) { return abuf[idx]; }\n    return 0u;");
  } else {
    replace("//VBUF_LOAD", "return vbuf[idx];");
    replace("//ABUF_LOAD", "return abuf[idx];");
  }
  wgpu::ShaderSourceWGSL wgslDescriptor{};
  wgslDescriptor.code = source.c_str();
  const wgpu::ShaderModuleDescriptor descriptor{
      .nextInChain = &wgslDescriptor,
      .label = "GX Ubershader",
  };
  return webgpu::g_device.CreateShaderModule(&descriptor);
}

wgpu::ShaderModule uber_module() {
  std::lock_guard lock{g_moduleMutex};
  if (!g_module) {
    const auto start = std::chrono::steady_clock::now();
    g_module = build_uber_module();
    Log.info("Ubershader module created in {:.1f} ms", std::chrono::duration<double, std::milli>(
                                                             std::chrono::steady_clock::now() - start)
                                                             .count());
  }
  return g_module;
}

constexpr u32 bit(bool value, u32 shift) noexcept { return static_cast<u32>(value) << shift; }

constexpr u32 field(u32 value, u32 shift, u32 width) noexcept { return (value & ((1u << width) - 1u)) << shift; }

bool is_bump(GXChannelID id) noexcept { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

// The raster color a stage reads: 0 / 1 the lit channels, 2 zero, 3 / 4 the (normalized) bump alpha.
u32 raster_select(GXChannelID id) noexcept {
  switch (id) {
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    return 0;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    return 1;
  case GX_ALPHA_BUMP:
    return 3;
  case GX_ALPHA_BUMPN:
    return 4;
  default:
    return 2;
  }
}

// build_shader skips tev_overflow on inputs that always lie in [0, 1] (tev_unit_range): everything but the TEV
// registers and the bump alpha.
bool color_arg_needs_overflow(GXTevColorArg arg, const TevStage& stage) noexcept {
  if (arg <= GX_CC_A2) {
    return true;
  }
  return (arg == GX_CC_RASC || arg == GX_CC_RASA) && is_bump(stage.channelId);
}

bool alpha_arg_needs_overflow(GXTevAlphaArg arg, const TevStage& stage) noexcept {
  if (arg <= GX_CA_A2) {
    return true;
  }
  return arg == GX_CA_RASA && is_bump(stage.channelId);
}

bool alpha_op_uses_color_inputs(GXTevOp op) noexcept { return op >= GX_TEV_COMP_R8_GT && op <= GX_TEV_COMP_BGR24_EQ; }

bool valid_tev_op(GXTevOp op) noexcept { return op == GX_TEV_ADD || op == GX_TEV_SUB || (op >= GX_TEV_COMP_R8_GT && op <= GX_TEV_COMP_RGB8_EQ); }
} // namespace

UberShaderMode uber_shader_mode() noexcept { return g_mode.load(std::memory_order_relaxed); }

void set_uber_shader_mode(UberShaderMode mode) noexcept {
  if (g_mode.exchange(mode, std::memory_order_relaxed) != mode) {
    g_generation.fetch_add(1, std::memory_order_relaxed);
  }
}

u32 uber_shader_generation() noexcept { return g_generation.load(std::memory_order_relaxed); }

u64 uber_shader_draw_count() noexcept { return g_draws.load(std::memory_order_relaxed); }

void count_uber_shader_draw() noexcept { g_draws.fetch_add(1, std::memory_order_relaxed); }

bool pack_uber_config(const ShaderConfig& config, const ShaderInfo& info, UberConfig& out) noexcept {
  out.fill(0);
  if (config.lineMode != 0 || tev_z_texture_enabled(config) || config.tevStageCount == 0 ||
      config.tevStageCount > MaxTevStages || config.numTexGens > MaxTexCoord || config.numIndStages > MaxIndStages ||
      config.fogType > 15) {
    return false;
  }

  out[GlobalWord] = field(config.numTexGens, 0, 4) | field(config.tevStageCount, 4, 5) |
                    field(config.numIndStages, 9, 3) | field(config.fogType, 12, 4) |
                    bit(config.dualTexEnabled, 16) | bit(config.fogRangeAdjust, 17) | bit(config.zFreeze, 18) |
                    bit(config.alphaFromPackedBlue, 19) | bit(static_cast<bool>(config.alphaCompare), 21) |
                    field(config.vtxStride, 24, 8);

  for (u32 s = 0; s < MaxTevSwap; ++s) {
    const auto& swap = config.tevSwapTable[s];
    out[SwapWord] |= field(field(swap.red, 0, 2) | field(swap.green, 2, 2) | field(swap.blue, 4, 2) |
                               field(swap.alpha, 6, 2),
                           s * 8, 8);
  }
  for (u32 c = 0; c < MaxColorChannels; ++c) {
    const auto& cc = config.colorChannels[c];
    out[ChannelWord] |= field(bit(cc.matSrc == GX_SRC_VTX, 0) | bit(cc.ambSrc == GX_SRC_VTX, 1) |
                                  field(cc.diffFn, 2, 2) | field(cc.attnFn, 4, 2) | bit(cc.lightingEnabled, 6),
                              c * 8, 8);
  }
  const auto& ac = config.alphaCompare;
  out[AlphaCompareWord] = field(ac.comp0, 0, 3) | field(ac.ref0, 3, 8) | field(ac.op, 11, 2) |
                          field(ac.comp1, 13, 3) | field(ac.ref1, 16, 8);

  for (u32 s = 0; s < config.numIndStages; ++s) {
    const auto& indStage = config.indStages[s];
    if (info.usedIndStages.test(s)) {
      if (indStage.texMapId >= MaxTextures) {
        return false;
      }
      out[MiscWord] |= bit(true, s);
    }
    const int texCoord = tev_effective_texcoord(config, indStage.texCoordId);
    out[IndStageWord] |= field(field(texCoord < 0 ? 15u : static_cast<u32>(texCoord), 0, 4) |
                                   field(indStage.texMapId, 4, 4),
                               s * 8, 8);
    out[IndScaleWord] |= field(field(indStage.scaleS, 0, 4) | field(indStage.scaleT, 4, 4), s * 8, 8);
  }
  const auto vertexColorSelect = [&](u32 channel) -> u32 {
    switch (shader_vertex_color_attr(config, channel)) {
    case GX_VA_CLR0:
      return 1;
    case GX_VA_CLR1:
      return 2;
    default:
      return 0;
    }
  };
  out[MiscWord] |= field(vertexColorSelect(0), 4, 2) | field(vertexColorSelect(1), 6, 2);

  for (u32 attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr) {
    const auto& mapping = config.attrs[attr];
    if (mapping.attrType == GX_NONE) {
      continue;
    }
    const bool indexed = mapping.attrType == GX_INDEX8 || mapping.attrType == GX_INDEX16;
    if (indexed && attr < GX_VA_POS) {
      return false;  // build_shader's array_start index would be negative
    }
    out[AttrWord + attr] = field(mapping.attrType, 0, 2) | field(mapping.cnt, 2, 4) | field(mapping.compType, 6, 3) |
                           field(mapping.offset, 9, 8) | field(mapping.stride, 17, 8) | field(mapping.frac, 25, 5) |
                           bit(indexed && attr == GX_VA_NRM && mapping.nrmIndexCount == 3, 30) | bit(mapping.le, 31);
  }

  for (u32 i = 0; i < config.numTexGens; ++i) {
    const auto& tcg = config.tcgs[i];
    u32 matrixMode = 0;
    u32 slot = 0;
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        matrixMode = 2;
      } else if (tcg.mtx != GX_IDENTITY) {
        matrixMode = 1;
        slot = std::min<u32>(static_cast<u32>(tcg.mtx) / 3, MaxPostexMtx - 1);
      }
    }
    const bool post = config.dualTexEnabled && tcg.postMtx != GX_PTIDENTITY;
    const u32 postIndex = post ? (static_cast<u32>(tcg.postMtx) - GX_PTTEXMTX0) / 3 : 0;
    if (post && postIndex >= MaxPTTexMtx) {
      return false;
    }
    out[UberTcgWord + i] = field(tcg.type, 0, 4) | field(tcg.src, 4, 5) | field(matrixMode, 9, 2) |
                           field(slot, 11, 5) | bit(post, UberTcgPostBit) | field(postIndex, UberTcgPostIndexShift, 5) |
                           bit(config.dualTexEnabled && tcg.normalize, 22) | bit(tcg.inputFormAB11, 23);
  }

  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    if (!valid_tev_op(stage.colorOp.op) || !valid_tev_op(stage.alphaOp.op) || stage.colorOp.bias > GX_TB_SUBHALF ||
        stage.alphaOp.bias > GX_TB_SUBHALF || stage.kcSel > GX_TEV_KCSEL_K3_A || stage.kaSel > GX_TEV_KASEL_K3_A ||
        (stage.kcSel > GX_TEV_KCSEL_1_8 && stage.kcSel < GX_TEV_KCSEL_K0) ||
        (stage.kaSel > GX_TEV_KASEL_1_8 && stage.kaSel < GX_TEV_KASEL_K0_R)) {
      return false;
    }
    const auto dependency = tev_stage_texture_dependency(config, i);
    const bool willSample = dependency.combinerUsesTexture && dependency.canSampleTexture;
    const bool needsTexCoord = dependency.needsFixedTexcoordState;
    const bool colorInputsNamed = alpha_op_uses_color_inputs(stage.alphaOp.op);
    const auto& colorPass = stage.colorPass;
    const auto& alphaPass = stage.alphaPass;
    const u32 base = TevWord + i * 4;
    out[base] = field(colorPass.a, 0, 4) | field(colorPass.b, 4, 4) | field(colorPass.c, 8, 4) |
                field(colorPass.d, 12, 4) | field(alphaPass.a, 16, 3) | field(alphaPass.b, 19, 3) |
                field(alphaPass.c, 22, 3) | field(alphaPass.d, 25, 3) |
                bit(colorInputsNamed || color_arg_needs_overflow(colorPass.a, stage), 28) |
                bit(colorInputsNamed || color_arg_needs_overflow(colorPass.b, stage), 29) |
                bit(color_arg_needs_overflow(colorPass.c, stage), 30);
    out[base + 1] = field(stage.colorOp.op, 0, 4) | field(stage.colorOp.bias, 4, 2) |
                    field(stage.colorOp.scale, 6, 2) | field(stage.colorOp.outReg, 8, 2) |
                    bit(stage.colorOp.clamp, 10) | bit(alpha_arg_needs_overflow(alphaPass.a, stage), 11) |
                    bit(alpha_arg_needs_overflow(alphaPass.b, stage), 12) |
                    bit(alpha_arg_needs_overflow(alphaPass.c, stage), 13) | field(stage.alphaOp.op, 16, 4) |
                    field(stage.alphaOp.bias, 20, 2) | field(stage.alphaOp.scale, 22, 2) |
                    field(stage.alphaOp.outReg, 24, 2) | bit(stage.alphaOp.clamp, 26);
    out[base + 2] = field(stage.kcSel, 0, 5) | field(stage.kaSel, 5, 5) | field(raster_select(stage.channelId), 10, 3) |
                    field(stage.tevSwapRas, 13, 2) | field(stage.tevSwapTex, 15, 2) |
                    field(dependency.texCoordId < 0 ? 15u : static_cast<u32>(dependency.texCoordId), 17, 4) |
                    field(dependency.texMapId < 0 ? 15u : static_cast<u32>(dependency.texMapId), 21, 4) |
                    bit(dependency.canSampleTexture, 25) | bit(willSample, 26) | bit(needsTexCoord, 27) |
                    bit(stage.indTexAddPrev, 28);

    u32 indirectMode = 0;
    u32 matrix = 0;
    u32 formatShift = 0;
    const bool hasIndirectStage = stage.indTexStage < config.numIndStages;
    const bool hasBaseCoord = dependency.texCoordId >= 0;
    if (needsTexCoord && stage.indTexMtxId != GX_ITM_OFF && hasIndirectStage) {
      if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
        indirectMode = 1;
        matrix = stage.indTexMtxId - GX_ITM_0;
      } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
        indirectMode = 2;
        matrix = stage.indTexMtxId - GX_ITM_S0;
      } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
        indirectMode = 3;
        matrix = stage.indTexMtxId - GX_ITM_T0;
      }
      switch (stage.indTexFormat) {
      case GX_ITF_8:
        formatShift = 0;
        break;
      case GX_ITF_5:
        formatShift = 3;
        break;
      case GX_ITF_4:
        formatShift = 4;
        break;
      case GX_ITF_3:
        formatShift = 5;
        break;
      default:
        return false;
      }
    }
    const bool bumpValid = hasIndirectStage && stage.indTexAlphaSel != GX_ITBA_OFF;
    out[base + 3] = field(indirectMode, 0, 2) | field(matrix, 2, 2) | field(stage.indTexStage, 4, 2) |
                    field(formatShift, 6, 3) | field(stage.indTexBiasSel, 9, 3) |
                    bit(stage.indTexFormat == GX_ITF_8, 12) | field(stage.indTexWrapS, 13, 3) |
                    field(stage.indTexWrapT, 16, 3) | field(stage.indTexAlphaSel, 19, 2) |
                    field(stage.indTexFormat, 21, 2) | bit(bumpValid, 23);
  }
  return true;
}

PipelineConfig uber_pipeline_config(const PipelineConfig& config) noexcept {
  PipelineConfig uber = config;
  uber.shaderConfig = ShaderConfig{};
  return uber;
}

wgpu::RenderPipeline create_uber_pipeline(const PipelineConfig& uberConfig) {
  ZoneScoped;
  return build_pipeline(uberConfig, {}, uber_module(), "GX Uber Pipeline");
}

void clear_uber_shader_module() noexcept {
  std::lock_guard lock{g_moduleMutex};
  g_module = {};
}
} // namespace aurora::gx
