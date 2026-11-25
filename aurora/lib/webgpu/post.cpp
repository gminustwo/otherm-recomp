#include "post.hpp"

#include "../internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace aurora::webgpu::post {
namespace {
Module Log("aurora::post");

// All passes draw one full-screen triangle. uv runs 0..1 over the target, so a pass needs no size uniform: picture
// sizes come from textureDimensions. Every texel is read with textureLoad at clamped integer coordinates.
constexpr const char* kSource = R"(
@group(0) @binding(0) var texA: texture_2d<f32>;
@group(0) @binding(1) var texB: texture_2d<f32>;

override kSharpness: f32 = 0.5;
const kEdgeThreshold: f32 = 0.08;  // luma step (0..1) that counts as an edge
const kMaxSearch: i32 = 32;        // pixels searched each way along an edge

struct VsOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  var o: VsOut;
  o.pos = vec4f(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
  o.uv = p;
  return o;
}

fn luma(c: vec3f) -> f32 { return dot(c, vec3f(0.299, 0.587, 0.114)); }

fn loadA(p: vec2i) -> vec4f {
  let last = vec2i(textureDimensions(texA)) - vec2i(1);
  return textureLoad(texA, clamp(p, vec2i(0), last), 0);
}
fn loadB(p: vec2i) -> vec4f {
  let last = vec2i(textureDimensions(texB)) - vec2i(1);
  return textureLoad(texB, clamp(p, vec2i(0), last), 0);
}
fn lumaA(p: vec2i) -> f32 { return luma(loadA(p).rgb); }

// ---- Anti-aliasing, pass 1 (texA: picture): r = an edge on the pixel's left side, g = on its top side ----
@fragment
fn fs_edges(in: VsOut) -> @location(0) vec4f {
  let p = vec2i(in.pos.xy);
  let c = lumaA(p);
  let l = lumaA(p + vec2i(-1, 0));
  let t = lumaA(p + vec2i(0, -1));
  let d = abs(vec2f(c - l, c - t));
  var e = step(vec2f(kEdgeThreshold), d);
  if (e.x + e.y == 0.0) {
    return vec4f(0.0);
  }
  // A step much weaker than the strongest one around it is the soft side of a gradient or texture detail, not a
  // silhouette: drop it.
  let r = abs(c - lumaA(p + vec2i(1, 0)));
  let b = abs(c - lumaA(p + vec2i(0, 1)));
  let l2 = abs(l - lumaA(p + vec2i(-2, 0)));
  let t2 = abs(t - lumaA(p + vec2i(0, -2)));
  let strongest = max(max(max(d.x, d.y), max(r, b)), max(l2, t2));
  e *= step(vec2f(strongest), 2.0 * d);
  return vec4f(e, 0.0, 0.0);
}

// ---- Anti-aliasing, pass 2 (texB: edges): how much of each neighbour a pixel takes ----
fn edgeLeft(p: vec2i) -> bool { return loadB(p).r > 0.5; }
fn edgeTop(p: vec2i) -> bool { return loadB(p).g > 0.5; }

// Which way the silhouette steps where an edge ends: the crossing edge lies before (above / left of) or after
// (below / right of) the edge line. Neither, both, or a search that ran out: no step.
fn endOffset(before: bool, after: bool, open: bool) -> f32 {
  if (open || before == after) {
    return 0.0;
  }
  return select(0.5, -0.5, before);
}

// The silhouette's distance from the edge line at position x, for an edge from border s0 to border s1 whose ends
// step by o0 and o1. Ends stepping the same way make a U: two lines meeting the edge line in the middle.
fn silhouette(x: f32, s0: f32, s1: f32, o0: f32, o1: f32) -> f32 {
  if (o0 != 0.0 && o0 == o1) {
    let mid = 0.5 * (s0 + s1);
    if (x <= mid) {
      return o0 * clamp((mid - x) / max(mid - s0, 1e-4), 0.0, 1.0);
    }
    return o1 * clamp((x - mid) / max(s1 - mid, 1e-4), 0.0, 1.0);
  }
  return mix(o0, o1, clamp((x - s0) / max(s1 - s0, 1e-4), 0.0, 1.0));
}

// Area on the positive side of zero under a line from h0 to h1 over a unit width.
fn positiveArea(h0: f32, h1: f32) -> f32 {
  if (h0 >= 0.0 && h1 >= 0.0) {
    return 0.5 * (h0 + h1);
  }
  if (h0 <= 0.0 && h1 <= 0.0) {
    return 0.0;
  }
  let t = h0 / (h0 - h1);
  return select(0.5 * (1.0 - t) * h1, 0.5 * t * h0, h0 > 0.0);
}

// x: positive area (past the edge line, on the pixel's own side), y: negative area, over the pixel span [x, x + 1].
fn spanAreas(x: f32, s0: f32, s1: f32, o0: f32, o1: f32) -> vec2f {
  let h0 = silhouette(x, s0, s1, o0, o1);
  let h1 = silhouette(x + 1.0, s0, s1, o0, o1);
  if (o0 != 0.0 && o0 == o1) {
    let mid = 0.5 * (s0 + s1);
    if (mid > x && mid < x + 1.0) {  // a U's apex inside the span: each half on its own
      let w0 = mid - x;
      let w1 = x + 1.0 - mid;
      return vec2f(w0 * positiveArea(h0, 0.0) + w1 * positiveArea(0.0, h1),
                   w0 * positiveArea(-h0, 0.0) + w1 * positiveArea(0.0, -h1));
    }
  }
  return vec2f(positiveArea(h0, h1), positiveArea(-h0, -h1));
}

@fragment
fn fs_weights(in: VsOut) -> @location(0) vec4f {
  let p = vec2i(in.pos.xy);
  let size = vec2i(textureDimensions(texB));
  let e = loadB(p);
  var w = vec4f(0.0);
  if (e.g > 0.5) {  // the border above the pixel (y = p.y), between rows p.y - 1 and p.y
    var left = 0;
    loop {
      let x = p.x - left - 1;
      if (left >= kMaxSearch || x < 0 || !edgeTop(vec2i(x, p.y))) { break; }
      left += 1;
    }
    var right = 0;
    loop {
      let x = p.x + right + 1;
      if (right >= kMaxSearch || x >= size.x || !edgeTop(vec2i(x, p.y))) { break; }
      right += 1;
    }
    let s0 = p.x - left;
    let s1 = p.x + right + 1;
    let o0 = endOffset(edgeLeft(vec2i(s0, p.y - 1)), edgeLeft(vec2i(s0, p.y)), left >= kMaxSearch);
    let o1 = endOffset(edgeLeft(vec2i(s1, p.y - 1)), edgeLeft(vec2i(s1, p.y)), right >= kMaxSearch);
    let a = spanAreas(f32(p.x), f32(s0), f32(s1), o0, o1);
    w.r = a.x;  // the silhouette dips into this pixel: it takes that much of the pixel above
    w.g = a.y;  // it rises into the pixel above: that one takes this much of this pixel
  }
  if (e.r > 0.5) {  // the border left of the pixel (x = p.x), between columns p.x - 1 and p.x
    var up = 0;
    loop {
      let y = p.y - up - 1;
      if (up >= kMaxSearch || y < 0 || !edgeLeft(vec2i(p.x, y))) { break; }
      up += 1;
    }
    var down = 0;
    loop {
      let y = p.y + down + 1;
      if (down >= kMaxSearch || y >= size.y || !edgeLeft(vec2i(p.x, y))) { break; }
      down += 1;
    }
    let s0 = p.y - up;
    let s1 = p.y + down + 1;
    let o0 = endOffset(edgeTop(vec2i(p.x - 1, s0)), edgeTop(vec2i(p.x, s0)), up >= kMaxSearch);
    let o1 = endOffset(edgeTop(vec2i(p.x - 1, s1)), edgeTop(vec2i(p.x, s1)), down >= kMaxSearch);
    let a = spanAreas(f32(p.y), f32(s0), f32(s1), o0, o1);
    w.b = a.x;  // this pixel takes that much of the pixel to its left
    w.a = a.y;  // the pixel to the left takes this much of this one
  }
  return w;
}

// ---- Anti-aliasing, pass 3 (texA: picture, texB: weights): blend with the neighbours the weights name ----
@fragment
fn fs_blend(in: VsOut) -> @location(0) vec4f {
  let p = vec2i(in.pos.xy);
  let size = vec2i(textureDimensions(texA));
  let c = loadA(p);
  let here = loadB(p);
  let wTop = here.r;
  let wLeft = here.b;
  let wBottom = select(0.0, loadB(p + vec2i(0, 1)).g, p.y + 1 < size.y);
  let wRight = select(0.0, loadB(p + vec2i(1, 0)).a, p.x + 1 < size.x);
  let h = wTop + wBottom;
  let v = wLeft + wRight;
  if (max(h, v) <= 0.0) {
    return c;
  }
  // One direction only, the stronger: blending both ways would soften corners twice.
  if (h >= v) {
    return c * (1.0 - h) + loadA(p + vec2i(0, -1)) * wTop + loadA(p + vec2i(0, 1)) * wBottom;
  }
  return c * (1.0 - v) + loadA(p + vec2i(-1, 0)) * wLeft + loadA(p + vec2i(1, 0)) * wRight;
}

// ---- Upscaling, pass 1 (texA: picture): an edge-directed resample to the target's size ----
fn lanczos2(x: f32) -> f32 {
  if (x >= 2.0) {
    return 0.0;
  }
  if (x < 1e-3) {
    return 1.0;
  }
  let px = 3.14159265 * x;
  return 2.0 * sin(px) * sin(0.5 * px) / (px * px);
}

@fragment
fn fs_upscale(in: VsOut) -> @location(0) vec4f {
  let size = vec2f(textureDimensions(texA));
  let src = in.uv * size - vec2f(0.5);
  let base = floor(src);
  let f = src - base;
  let o = vec2i(base);
  // The twelve texels around the 2x2 quad that holds src (a 4x4 block without its corners).
  var offs = array<vec2i, 12>(vec2i(0, -1), vec2i(1, -1), vec2i(-1, 0), vec2i(0, 0), vec2i(1, 0), vec2i(2, 0),
                              vec2i(-1, 1), vec2i(0, 1), vec2i(1, 1), vec2i(2, 1), vec2i(0, 2), vec2i(1, 2));
  var col: array<vec3f, 12>;
  var lum: array<f32, 12>;
  for (var i = 0; i < 12; i++) {
    col[i] = loadA(o + offs[i]).rgb;
    lum[i] = luma(col[i]);
  }
  // Luma gradients at the quad's four texels (central differences), weighted toward src: a structure tensor whose
  // main axis is the direction across the local edge.
  let g00 = vec2f(lum[4] - lum[2], lum[7] - lum[0]);
  let g10 = vec2f(lum[5] - lum[3], lum[8] - lum[1]);
  let g01 = vec2f(lum[8] - lum[6], lum[10] - lum[3]);
  let g11 = vec2f(lum[9] - lum[7], lum[11] - lum[4]);
  let w00 = (1.0 - f.x) * (1.0 - f.y);
  let w10 = f.x * (1.0 - f.y);
  let w01 = (1.0 - f.x) * f.y;
  let w11 = f.x * f.y;
  let tensor = w00 * vec3f(g00.x * g00.x, g00.x * g00.y, g00.y * g00.y) +
               w10 * vec3f(g10.x * g10.x, g10.x * g10.y, g10.y * g10.y) +
               w01 * vec3f(g01.x * g01.x, g01.x * g01.y, g01.y * g01.y) +
               w11 * vec3f(g11.x * g11.x, g11.x * g11.y, g11.y * g11.y);
  let trace = tensor.x + tensor.z;
  let diff = tensor.x - tensor.z;
  // 0 where the area is flat or has no one direction, 1 along a clean edge.
  let aniso = select(0.0, sqrt(diff * diff + 4.0 * tensor.y * tensor.y) / trace, trace > 1e-5);
  let angle = 0.5 * atan2(2.0 * tensor.y, diff);
  let across = vec2f(cos(angle), sin(angle));
  let along = vec2f(-across.y, across.x);
  // The kernel reaches up to twice as far along an edge as across it: the staircase smooths out, the edge stays sharp.
  let stretch = 1.0 + aniso;
  var sum = vec3f(0.0);
  var weight = 0.0;
  for (var i = 0; i < 12; i++) {
    let d = vec2f(offs[i]) - f;
    let w = lanczos2(length(vec2f(dot(d, across), dot(d, along) / stretch)));
    sum += col[i] * w;
    weight += w;
  }
  var rgb = sum / max(weight, 1e-4);
  // Lanczos rings at hard edges: keep the result within the quad's own colours.
  let lo = min(min(col[3], col[4]), min(col[7], col[8]));
  let hi = max(max(col[3], col[4]), max(col[7], col[8]));
  rgb = clamp(rgb, lo, hi);
  return vec4f(rgb, 1.0);
}

// ---- Upscaling, pass 2 (texA: upscaled): sharpening held inside each pixel's neighbourhood ----
@fragment
fn fs_sharpen(in: VsOut) -> @location(0) vec4f {
  let p = vec2i(in.pos.xy);
  let c = loadA(p).rgb;
  let n = loadA(p + vec2i(0, -1)).rgb;
  let s = loadA(p + vec2i(0, 1)).rgb;
  let w = loadA(p + vec2i(-1, 0)).rgb;
  let e = loadA(p + vec2i(1, 0)).rgb;
  let lo = min(c, min(min(n, s), min(w, e)));
  let hi = max(c, max(max(n, s), max(w, e)));
  // An unsharp mask over the cross; clamping to the cross's own range leaves no halos.
  let detail = c - 0.25 * (n + s + w + e);
  return vec4f(clamp(c + 2.0 * kSharpness * detail, lo, hi), 1.0);
}
)";

constexpr auto kFormat = wgpu::TextureFormat::RGBA8Unorm;

struct Target {
  wgpu::Texture texture;
  wgpu::TextureView view;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct State {
  wgpu::ShaderModule module;
  wgpu::BindGroupLayout layout;
  wgpu::PipelineLayout pipelineLayout;
  wgpu::RenderPipeline edges;
  wgpu::RenderPipeline weights;
  wgpu::RenderPipeline blend;
  wgpu::RenderPipeline upscale;
  wgpu::RenderPipeline sharpen;
  float sharpenFor = -1.f;
  wgpu::Sampler sampler;
  Target edgesTarget;
  Target weightsTarget;
  Target antiAliased;
  Target upscaled;
  Target sharpened;
  WGPUTexture sourceHandle = nullptr;
  wgpu::TextureView sourceView;
  bool reported = false;
};
State g_state;

wgpu::RenderPipeline make_pipeline(const char* entry, const char* label, float sharpness = 0.5f) {
  const wgpu::ColorTargetState target{.format = kFormat, .writeMask = wgpu::ColorWriteMask::All};
  const wgpu::ConstantEntry constant{.key = "kSharpness", .value = sharpness};
  const bool sharpen = std::string_view(entry) == "fs_sharpen";
  const wgpu::FragmentState fragment{
      .module = g_state.module,
      .entryPoint = entry,
      .constantCount = sharpen ? 1u : 0u,
      .constants = sharpen ? &constant : nullptr,
      .targetCount = 1,
      .targets = &target,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = g_state.pipelineLayout,
      .vertex = {.module = g_state.module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = {.count = 1},
      .fragment = &fragment,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

bool ensure_pipelines() {
  if (g_state.module) {
    return true;
  }
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = kSource;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Post-processing"};
  g_state.module = g_device.CreateShaderModule(&moduleDescriptor);
  const auto textureEntry = [](uint32_t binding) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding,
        .visibility = wgpu::ShaderStage::Fragment,
        .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
    };
  };
  const std::array entries{textureEntry(0), textureEntry(1)};
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{.label = "Post-processing", .entryCount = entries.size(),
                                                        .entries = entries.data()};
  g_state.layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                               .bindGroupLayouts = &g_state.layout};
  g_state.pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  g_state.edges = make_pipeline("fs_edges", "AA edges");
  g_state.weights = make_pipeline("fs_weights", "AA blend weights");
  g_state.blend = make_pipeline("fs_blend", "AA blend");
  g_state.upscale = make_pipeline("fs_upscale", "Upscale");
  const wgpu::SamplerDescriptor samplerDescriptor{.magFilter = wgpu::FilterMode::Linear,
                                                  .minFilter = wgpu::FilterMode::Linear};
  g_state.sampler = g_device.CreateSampler(&samplerDescriptor);
  Log.info("Post-processing pipelines created");
  return true;
}

void ensure_target(Target& target, uint32_t width, uint32_t height, const char* label) {
  if (target.texture && target.width == width && target.height == height) {
    return;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .size = {width, height, 1},
      .format = kFormat,
  };
  target.texture = g_device.CreateTexture(&descriptor);
  target.view = target.texture.CreateView();
  target.width = width;
  target.height = height;
}

void run_pass(const wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipeline, const Target& target,
              const wgpu::TextureView& a, const wgpu::TextureView& b, const char* label) {
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .textureView = a},
      wgpu::BindGroupEntry{.binding = 1, .textureView = b},
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{.layout = g_state.layout, .entryCount = entries.size(),
                                                      .entries = entries.data()};
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const wgpu::RenderPassColorAttachment attachment{
      .view = target.view,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 0.0},
  };
  const wgpu::RenderPassDescriptor descriptor{.label = label, .colorAttachmentCount = 1,
                                              .colorAttachments = &attachment};
  auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.Draw(3);
  pass.End();
}
} // namespace

wgpu::BindGroup process(const wgpu::CommandEncoder& encoder, const PresentSource& source, uint32_t outputWidth,
                        uint32_t outputHeight, const Settings& settings) {
  if ((!settings.upscale && !settings.antiAlias) || !source.texture) {
    return {};
  }
  if (source.format != wgpu::TextureFormat::RGBA8Unorm && source.format != wgpu::TextureFormat::BGRA8Unorm) {
    if (!g_state.reported) {
      g_state.reported = true;
      Log.warn("Post-processing skipped: the picture's format {} is not 8-bit RGBA",
               static_cast<uint32_t>(source.format));
    }
    return {};
  }
  // The whole texture is the picture, as the copy to the screen samples it.
  const uint32_t width = source.texture.GetWidth();
  const uint32_t height = source.texture.GetHeight();
  const bool upscale = settings.upscale && outputWidth >= width && outputHeight >= height &&
                       (outputWidth > width || outputHeight > height);
  if (!upscale && !settings.antiAlias) {
    return {};
  }
  ensure_pipelines();
  if (g_state.sourceHandle != source.texture.Get()) {
    g_state.sourceHandle = source.texture.Get();
    g_state.sourceView = source.texture.CreateView();
  }
  wgpu::TextureView picture = g_state.sourceView;
  if (settings.antiAlias) {
    ensure_target(g_state.edgesTarget, width, height, "AA edges");
    ensure_target(g_state.weightsTarget, width, height, "AA weights");
    ensure_target(g_state.antiAliased, width, height, "AA result");
    run_pass(encoder, g_state.edges, g_state.edgesTarget, picture, picture, "AA edges");
    run_pass(encoder, g_state.weights, g_state.weightsTarget, g_state.edgesTarget.view, g_state.edgesTarget.view,
             "AA blend weights");
    run_pass(encoder, g_state.blend, g_state.antiAliased, picture, g_state.weightsTarget.view, "AA blend");
    picture = g_state.antiAliased.view;
  }
  if (upscale) {
    const float sharpness = std::clamp(settings.sharpness, 0.f, 1.f);
    if (!g_state.sharpen || std::fabs(sharpness - g_state.sharpenFor) > 1e-4f) {
      g_state.sharpen = make_pipeline("fs_sharpen", "Sharpen", sharpness);
      g_state.sharpenFor = sharpness;
    }
    ensure_target(g_state.upscaled, outputWidth, outputHeight, "Upscaled");
    ensure_target(g_state.sharpened, outputWidth, outputHeight, "Sharpened");
    run_pass(encoder, g_state.upscale, g_state.upscaled, picture, picture, "Upscale");
    run_pass(encoder, g_state.sharpen, g_state.sharpened, g_state.upscaled.view, g_state.upscaled.view, "Sharpen");
    picture = g_state.sharpened.view;
  }
  return create_copy_bind_group(picture, g_state.sampler);
}

void reset() { g_state = {}; }
} // namespace aurora::webgpu::post
