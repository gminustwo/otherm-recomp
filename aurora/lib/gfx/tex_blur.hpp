#pragma once

#include "common.hpp"

// A box blur of a texture, horizontal then vertical (src -> tmp -> dst, all three the same size), the
// blur of Dolphin's "Bloom Blurred" graphics mod: each texel the mean of the 2 * radius + 1 texels around it on the
// line, those inside the texture. Used for a copy the embedder marks (aurora_set_next_copy_style).
namespace aurora::gfx::tex_blur {

// The format of the blur's middle step and of the blurred texture: float, so a thin glow spread over many texels
// keeps its light (in 8 bits about half of it was rounded away, at a 4.5x scale), as Dolphin's blur in its shader does.
inline constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RGBA16Float;
// A texture of that format for `width` x `height`, sampled as `gxFormat` (the copy's) by the game's draws.
TextureHandle new_float_texture(uint32_t width, uint32_t height, u32 gxFormat);

struct Request {
  TextureHandle src;
  TextureHandle tmp;
  TextureHandle dst;
  uint32_t radius = 1;
};

void initialize();
void shutdown();
void run(const wgpu::CommandEncoder& cmd, const Request& req);

} // namespace aurora::gfx::tex_blur
