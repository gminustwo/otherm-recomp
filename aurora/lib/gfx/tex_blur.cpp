#include "tex_blur.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "texture.hpp"

#include <map>
#include <tuple>

namespace aurora::gfx::tex_blur {
static Module Log("aurora::gfx::tex_blur");

using webgpu::g_device;

// The radius and the direction are pipeline constants: a few pipelines, no uniform buffer.
static constexpr std::string_view Shader = R"(
struct VertexOutput {
    @builtin(position) pos: vec4f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    return out;
}

override RADIUS: i32 = 1;
override HORIZONTAL: bool = true;

@group(0) @binding(0) var src: texture_2d<f32>;

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let size = vec2i(textureDimensions(src));
    let at = vec2i(in.pos.xy);
    var sum = vec4f(0.0);
    var count = 0.0;
    for (var o = -RADIUS; o <= RADIUS; o++) {
        var p = at;
        if (HORIZONTAL) {
            p.x += o;
        } else {
            p.y += o;
        }
        if (p.x >= 0 && p.y >= 0 && p.x < size.x && p.y < size.y) {
            sum += textureLoad(src, p, 0);
            count += 1.0;
        }
    }
    return sum / max(count, 1.0);
}
)"sv;

static wgpu::ShaderModule g_module;
static wgpu::BindGroupLayout g_bindGroupLayout;
static wgpu::PipelineLayout g_pipelineLayout;
static std::map<std::tuple<wgpu::TextureFormat, uint32_t, bool>, wgpu::RenderPipeline> g_pipelines;

static const wgpu::RenderPipeline& pipeline_for(wgpu::TextureFormat format, uint32_t radius, bool horizontal) {
  auto& pipeline = g_pipelines[{format, radius, horizontal}];
  if (pipeline) {
    return pipeline;
  }
  const std::array constants{
      wgpu::ConstantEntry{.key = "RADIUS", .value = static_cast<double>(radius)},
      wgpu::ConstantEntry{.key = "HORIZONTAL", .value = horizontal ? 1.0 : 0.0},
  };
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = format,
  }};
  const wgpu::FragmentState fragmentState{
      .module = g_module,
      .entryPoint = "fs_main",
      .constantCount = constants.size(),
      .constants = constants.data(),
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = horizontal ? "TexBlur Horizontal" : "TexBlur Vertical",
      .layout = g_pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = g_module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .fragment = &fragmentState,
  };
  pipeline = g_device.CreateRenderPipeline(&pipelineDescriptor);
  return pipeline;
}

TextureHandle new_float_texture(uint32_t width, uint32_t height, u32 gxFormat) {
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Blurred Copy",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = kFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Blurred Copy view",
      .format = kFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);
  return std::make_shared<TextureRef>(std::move(texture), view, view, size, kFormat, 1, gxFormat);
}

void initialize() {
  wgpu::ShaderSourceWGSL wgslSource{};
  wgslSource.code = Shader.data();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &wgslSource,
      .label = "TexBlur",
  };
  g_module = g_device.CreateShaderModule(&moduleDescriptor);

  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .label = "TexBlur Bind Group Layout",
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_bindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_bindGroupLayout,
  };
  g_pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
}

void shutdown() {
  g_pipelines.clear();
  g_pipelineLayout = {};
  g_bindGroupLayout = {};
  g_module = {};
}

static void pass(const wgpu::CommandEncoder& cmd, const TextureHandle& src, const TextureHandle& dst, uint32_t radius,
                 bool horizontal) {
  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .textureView = src->sampleTextureView,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_bindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const std::array colorAttachments{
      wgpu::RenderPassColorAttachment{
          .view = dst->attachmentTextureView,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
          .clearValue = {0.0, 0.0, 0.0, 0.0},
      },
  };
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = "TexBlur Pass",
      .colorAttachmentCount = colorAttachments.size(),
      .colorAttachments = colorAttachments.data(),
  };
  const auto renderPass = cmd.BeginRenderPass(&renderPassDescriptor);
  renderPass.SetPipeline(pipeline_for(dst->format, radius, horizontal));
  renderPass.SetBindGroup(0, bindGroup);
  renderPass.Draw(3);
  renderPass.End();
}

void run(const wgpu::CommandEncoder& cmd, const Request& req) {
  if (!req.src || !req.tmp || !req.dst || req.src->size.width != req.dst->size.width ||
      req.src->size.height != req.dst->size.height || req.tmp->size.width != req.dst->size.width ||
      req.tmp->size.height != req.dst->size.height) {
    Log.warn("Skipping a blur whose textures don't match");
    return;
  }
  pass(cmd, req.src, req.tmp, req.radius, true);
  pass(cmd, req.tmp, req.dst, req.radius, false);
}

} // namespace aurora::gfx::tex_blur
