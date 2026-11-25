#pragma once

#include "gpu.hpp"

namespace aurora::webgpu::post {
// Post-processing between the finished game picture and the copy to the screen (aurora.cpp
// encode_presentation_snapshot): morphological anti-aliasing at the picture's own size, then edge-adaptive upscaling
// and sharpening to the screen's size. Each step is aurora's own WGSL (post.cpp), modelled on published techniques:
// "smaa" after SMAA 1x's three passes (edges, blend weights from edge-length search, neighbourhood blending; areas
// computed in the shader for horizontal and vertical edges, no lookup textures, no diagonals), "fsr" after FSR 1's
// two passes (an edge-directed upscale clamped to its centre texels, then contrast-limited sharpening).
struct Settings {
  bool upscale = false;    // FSR-style upscaling when the screen is bigger than the picture
  bool antiAlias = false;  // SMAA-style anti-aliasing
  float sharpness = 0.5f;  // 0..1, the upscale's sharpening pass
};

// Belongs to the serialized presentation encoder. Returns a bind group for g_CopyPipeline that samples the processed
// picture (outputWidth x outputHeight when upscaled, else the picture's size), or a null one to present the source
// as is (nothing enabled, an unsupported format, or nothing to upscale with anti-aliasing off).
wgpu::BindGroup process(const wgpu::CommandEncoder& encoder, const PresentSource& source, uint32_t outputWidth,
                        uint32_t outputHeight, const Settings& settings);
// Drops the pipelines and textures (device loss, shutdown).
void reset();
} // namespace aurora::webgpu::post
