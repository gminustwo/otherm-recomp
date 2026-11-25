#pragma once

// Ubershaders: one generic GX shader that reads a draw's TEV, texgen, lighting, fog and vertex layout from its uniform
// block, so a draw whose own (specialized) pipeline is still compiling is drawn right away instead of waiting for it
// (a stutter) or being skipped (a missing effect). Dolphin's "hybrid ubershaders". The ubershader needs one pipeline
// per fixed-function state (blend, depth, cull, target format), compiled at startup for every state the pipeline
// cache knows (gfx::queue_uber_pipelines_from_cache), so the first draw of a new shader usually finds one ready.
//
// Not covered (those draws keep the old wait / skip): lines and points (lineMode), Z textures. Other M's 7,992 known
// shaders use lines in 12 and Z textures in 2.

#include "pipeline.hpp"
#include "shader_info.hpp"

#include <array>
#include <cstddef>

namespace aurora::gx {
enum class UberShaderMode : u8 {
  // Draws wait for or skip their own pipeline (gfx::skip_unready_pipelines decides which).
  Off = 0,
  // A draw whose pipeline is still compiling is drawn with the ubershader.
  Hybrid = 1,
  // Every supported draw uses the ubershader (a test mode: compare its picture with the specialized shaders').
  Exclusive = 2,
};
UberShaderMode uber_shader_mode() noexcept;
void set_uber_shader_mode(UberShaderMode mode) noexcept;
// Bumped by set_uber_shader_mode, so per-config caches built under another mode are rebuilt.
u32 uber_shader_generation() noexcept;

// The packed ShaderConfig the ubershader interprets (the `cfg` words of UberUniformBlock). Layout in ubershader.cpp.
constexpr u32 UberConfigWordCount = 112;
using UberConfig = std::array<u32, UberConfigWordCount>;
constexpr u32 UberTcgWord = 40;
// The tcg word's post-transform matrix index (into GXState::ptTexMtxs) and its "has one" bit, read by the uniform
// builder to resolve each texgen's post matrix.
constexpr u32 UberTcgPostBit = 16;
constexpr u32 UberTcgPostIndexShift = 17;

// False when the ubershader cannot draw this config (see the file comment); `out` is then meaningless.
bool pack_uber_config(const ShaderConfig& config, const ShaderInfo& info, UberConfig& out) noexcept;
// The key of the ubershader pipeline for this draw's fixed-function state: the config with its ShaderConfig cleared.
PipelineConfig uber_pipeline_config(const PipelineConfig& config) noexcept;
wgpu::RenderPipeline create_uber_pipeline(const PipelineConfig& uberConfig);
void clear_uber_shader_module() noexcept;

// The ubershader's uniform block, mirrored by `struct Uniform` in the WGSL (ubershader.cpp). Every matrix slot is
// uploaded at its absolute index, so per-vertex matrix indices and frame interpolation need no remapping.
struct alignas(16) UberUniformBlock {
  u32 vtxStart;
  u32 currentPnMtx;
  float renderViewportSize[2];
  float logicalViewportSize[2];
  u32 pad[2];
  u32 arrayStart[12];
  Mat4x4<float> proj;
  Mat3x4<float> postex[MaxPostexMtx];
  Mat3x4<float> nrm[MaxPnMtx];
  // Each texgen's post-transform matrix, already resolved from ptTexMtxs.
  Mat3x4<float> post[MaxTexCoord];
  Vec4<float> tevRegs[MaxTevRegs];
  Vec4<float> kcolors[MaxKColors];
  Vec4<float> ccAmb[MaxColorChannels];
  Vec4<float> ccMat[MaxColorChannels];
  Light lights[GX::MaxLights];
  u32 lightState[MaxColorChannels];
  Fog fog;
  Vec4<s32> indMtx[MaxIndTexMtxs * 2];
  Vec4<float> texcoordScale[MaxTexCoord];
  Vec4<float> texSizeBias[MaxTextures];
  Vec4<float> zfreezePlane;
  u32 cfg[UberConfigWordCount];
};
static_assert(offsetof(UberUniformBlock, arrayStart) == 32);
static_assert(offsetof(UberUniformBlock, proj) == 80);
static_assert(offsetof(UberUniformBlock, postex) == 144);
static_assert(offsetof(UberUniformBlock, nrm) == 1104);
static_assert(offsetof(UberUniformBlock, post) == 1584);
static_assert(offsetof(UberUniformBlock, tevRegs) == 1968);
static_assert(offsetof(UberUniformBlock, lights) == 2224);
static_assert(offsetof(UberUniformBlock, lightState) == 2864);
static_assert(offsetof(UberUniformBlock, fog) == 2880);
static_assert(offsetof(UberUniformBlock, indMtx) == 2976);
static_assert(offsetof(UberUniformBlock, texcoordScale) == 3072);
static_assert(offsetof(UberUniformBlock, texSizeBias) == 3200);
static_assert(offsetof(UberUniformBlock, zfreezePlane) == 3328);
static_assert(offsetof(UberUniformBlock, cfg) == 3344);
static_assert(sizeof(UberUniformBlock) == 3792);
static_assert(sizeof(UberUniformBlock) <= MaxUniformSize, "the GX uniform bind group binds MaxUniformSize bytes");

// build_uniform's counterpart for an ubershader draw (shader_info.cpp).
UniformRanges build_uber_uniform(const UberConfig& words, const ShaderInfo& info, u32 vtxStart,
                                 const BindGroupRanges& ranges, const FrameInterpolationDrawIdentity& drawIdentity,
                                 bool perspective, uint16_t usedPnMtxMask = 1) noexcept;

// Draws recorded with the ubershader since the process started (aurora_get_ubershader_draw_count, for the log).
u64 uber_shader_draw_count() noexcept;
void count_uber_shader_draw() noexcept;
} // namespace aurora::gx
