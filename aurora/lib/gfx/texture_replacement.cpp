#include "texture_replacement.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "../webgpu/gpu.hpp"
#include "dds_io.hpp"
#include "frame_costs.hpp"
#include "texture_convert.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <SDL3/SDL_cpuinfo.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <functional>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "png_io.hpp"
#include "../fs_helper.hpp"

using namespace aurora::gx;
using aurora::webgpu::g_device;

namespace aurora::gfx::texture_replacement {
Module Log("aurora::gfx::texture_replacement");

struct RuntimeTextureKey {
  uint64_t textureHash = 0;
  uint64_t tlutHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool hasTlut = false;
  uint32_t format = 0;

  bool operator==(const RuntimeTextureKey& rhs) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const RuntimeTextureKey& key) {
    return H::combine(std::move(h), key.textureHash, key.tlutHash, key.width, key.height, key.hasTlut, key.format);
  }
};

struct TlutMetadata {
  uint32_t size = 0;
  uint32_t format = 0;
  uint16_t entries = 0;
  bool valid = false;
  // Object the palette was registered from. GXLoadTlut consumes the registration, so this is what
  // lets the same object be loaded into more than one hardware slot.
  const GXTlutObj* source = nullptr;
  ByteBuffer data;
};

struct CachedReplacement {
  gfx::TextureHandle handle;
  uint64_t bytes = 0;
  std::list<RuntimeTextureKey>::iterator lruIt;
};

struct ReplacementIndexEntry {
  std::filesystem::path path;
  uint8_t marker = 0;  // button_icons/<style>/marker<N>: where these are drawn is reported (marker7: a menu's back legend)
};

absl::flat_hash_map<RuntimeTextureKey, ReplacementIndexEntry> s_replacementIndex;
// <UserData>/button_icons/<style>: per-controller button prompts, looked up before everything else.
// The last, "common", applies whatever the style (also with none): looked up after the style's own set.
constexpr const char* kButtonIconStyles[] = {"xbox", "playstation", "nintendo", "common"};
constexpr int kCommonIcons = 3;
std::array<absl::flat_hash_map<RuntimeTextureKey, ReplacementIndexEntry>, 4> s_buttonIconIndex;
// Metroid: Other M: <style>/menu/, the pictures that differ while a game menu is up (its controls are remapped there,
// see Metroid.ini's $menu); looked up before the style's own.
std::array<absl::flat_hash_map<RuntimeTextureKey, ReplacementIndexEntry>, 3> s_buttonIconMenuIndex;
std::atomic<int> s_buttonIconStyle{-1};
std::atomic<bool> s_buttonIconMenu{false};
std::atomic<bool> s_buttonIconStyleChanged{false};
std::atomic<bool> s_buttonIconReload{false};  // reload_button_icons: read button_icons/ again at the next resolve
int s_appliedButtonIconStyle = -1;
bool s_appliedButtonIconMenu = false;
int s_buttonIconForced = -1;
uint32_t s_buttonIconForcedFrom = 0;  // OTHERM_BUTTON_ICONS=<style>@<frame>: the game's own icons until that frame
// Button icons load even with texture replacements off (they are the port's controller prompts, not a texture
// pack). Then only textures whose size and format match an indexed icon are hashed and looked up.
bool s_haveButtonIcons = false;
absl::flat_hash_set<uint64_t> s_buttonIconShapes;
uint64_t texture_shape(uint32_t width, uint32_t height, uint32_t format) noexcept {
  return (uint64_t(width) << 40) | (uint64_t(height) << 20) | format;
}
absl::flat_hash_map<RuntimeTextureKey, CachedReplacement> s_replacementCache;
absl::flat_hash_set<RuntimeTextureKey> s_failedKeys;
absl::flat_hash_set<RuntimeTextureKey> s_reportedMisses;
absl::flat_hash_map<const GXTlutObj*, TlutMetadata> s_pendingTluts;
std::array<TlutMetadata, MaxTluts> s_loadedTluts{};
std::list<RuntimeTextureKey> s_replacementLru;
std::filesystem::path s_replacementRoot;
std::filesystem::path s_dumpRoot;
// Options > Textures (aurora_set_texture_packs): packs on or off, dumps, and folders indexed after the port's own (a
// Dolphin pack). Taken at the next texture resolve, on the thread that looks replacements up.
struct PackRequest {
  bool enabled = false;
  bool dumps = false;
  std::vector<std::filesystem::path> folders;
};
std::mutex s_packRequestMutex;
std::optional<PackRequest> s_packRequest;
std::vector<std::filesystem::path> s_extraRoots;
std::atomic<size_t> s_indexedCount{0};
std::atomic<size_t> s_loadedCount{0};
uint64_t s_replacementCacheBytes = 0;
constexpr uint64_t kReplacementCacheBudgetBytes = 4294967296; // 4GB, reasonable for modern hardware?
constexpr uint64_t kReplacementWildcardTextureHash = 0xFFFFFFFFFFFFFFFFull;
constexpr uint64_t kReplacementWildcardTlutHash = 0xFFFFFFFFFFFFFFFEull;

bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

bool is_relative_to(const std::filesystem::path& path, const std::filesystem::path& root) noexcept {
  if (root.empty()) {
    return false;
  }
  auto pathIt = path.begin();
  auto rootIt = root.begin();
  for (; rootIt != root.end(); ++rootIt, ++pathIt) {
    if (pathIt == path.end() || !iequals_ascii(fs_path_to_string(*pathIt), fs_path_to_string(*rootIt))) {
      return false;
    }
  }
  return true;
}

bool is_sidecar_mip(std::string_view stem) noexcept {
  constexpr std::string_view tag = "_mip";
  size_t i = stem.size();
  while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') {
    --i;
  }

  if (i == stem.size() || i < tag.size()) {
    return false;
  }

  return stem.substr(i - tag.size(), tag.size()) == tag;
}

std::optional<uint64_t> parse_hex(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (const char ch : text) {
    value <<= 4;
    if (ch >= '0' && ch <= '9') {
      value |= static_cast<uint64_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= static_cast<uint64_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value |= static_cast<uint64_t>(ch - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return value;
}

std::optional<uint32_t> parse_u32(std::string_view text, int base = 10) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }

  uint32_t value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value, base);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::pair<uint32_t, uint32_t>> parse_dimensions(std::string_view text) noexcept {
  const size_t sep = text.find('x');
  if (sep == std::string_view::npos) {
    return std::nullopt;
  }

  const auto width = parse_u32(text.substr(0, sep));
  const auto height = parse_u32(text.substr(sep + 1));
  if (!width.has_value() || !height.has_value()) {
    return std::nullopt;
  }
  return std::pair{*width, *height};
}

uint32_t texture_base_level_size(const GXTexObj_& obj) noexcept {
  switch (obj.format()) {
  case GX_TF_R8_PC:
    return obj.width() * obj.height();
  case GX_TF_RGBA8_PC:
    return obj.width() * obj.height() * 4;
  default:
    return GXGetTexBufferSize(obj.width(), obj.height(), obj.format(), false, 0);
  }
}

std::optional<uint64_t> compute_referenced_tlut_hash(const GXTexObj_& obj) noexcept {
  if (!is_palette_format(obj.format()) || obj.tlut >= s_loadedTluts.size()) {
    return std::nullopt;
  }

  const auto& tlut = s_loadedTluts[obj.tlut];
  const uint32_t textureSize = texture_base_level_size(obj);
  const auto* textureData = static_cast<const uint8_t*>(obj.data);
  if (!tlut.valid || textureData == nullptr || textureSize == 0) {
    return std::nullopt;
  }

  uint32_t minIndex = 0xffff;
  uint32_t maxIndex = 0;
  switch (obj.format()) {
  case GX_TF_C4:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t lowNibble = textureData[i] & 0xf;
      const uint32_t highNibble = textureData[i] >> 4;
      minIndex = std::min({minIndex, lowNibble, highNibble});
      maxIndex = std::max({maxIndex, lowNibble, highNibble});
    }
    break;
  case GX_TF_C8:
    for (uint32_t i = 0; i < textureSize; ++i) {
      const uint32_t index = textureData[i];
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  case GX_TF_C14X2:
    for (uint32_t i = 0; i + sizeof(uint16_t) <= textureSize; i += sizeof(uint16_t)) {
      uint16_t value = 0;
      std::memcpy(&value, textureData + i, sizeof(value));
      const uint32_t index = bswap(value) & 0x3fff;
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    break;
  default:
    return std::nullopt;
  }

  size_t tlutSize = 2 * (static_cast<size_t>(maxIndex) + 1 - minIndex);
  const size_t tlutOffset = 2 * static_cast<size_t>(minIndex);
  if (tlutOffset + tlutSize > tlut.data.size()) {
    return std::nullopt;
  }
  return XXH64(tlut.data.data() + tlutOffset, tlutSize, 0);
}

const TlutMetadata* get_loaded_tlut(const GXTexObj_& obj) noexcept {
  if (!is_palette_format(obj.format()) || obj.tlut >= s_loadedTluts.size()) {
    return nullptr;
  }

  const auto& tlut = s_loadedTluts[obj.tlut];
  return tlut.valid ? &tlut : nullptr;
}

bool ensure_directory(const std::filesystem::path& dir) noexcept {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec;
}

RuntimeTextureKey build_runtime_key(const GXTexObj_& obj) noexcept {
  RuntimeTextureKey key{
      .width = obj.width(),
      .height = obj.height(),
      .hasTlut = is_palette_format(obj.format()),
      .format = obj.format(),
  };

  const uint32_t textureSize = texture_base_level_size(obj);
  if (obj.data != nullptr && textureSize != 0) {
    key.textureHash = XXH64(obj.data, textureSize, 0);
  }
  if (key.hasTlut) {
    key.tlutHash = compute_referenced_tlut_hash(obj).value_or(0);
  }
  return key;
}

std::string format_replacement_filename(const RuntimeTextureKey& key) {
  if (key.hasTlut) {
    return fmt::format("tex1_{}x{}_{:016x}_{:016x}_{}.dds", key.width, key.height,
                       key.textureHash, key.tlutHash, key.format);
  }
  return fmt::format("tex1_{}x{}_{:016x}_{}.dds", key.width, key.height, key.textureHash, key.format);
}

std::optional<RuntimeTextureKey> parse_replacement_filename(std::string_view filename) noexcept {
  const size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }

  if (!iequals_ascii(filename.substr(dot), ".dds") && !iequals_ascii(filename.substr(dot), ".png")) {
    return std::nullopt;
  }

  const std::string_view stem = filename.substr(0, dot);
  constexpr std::string_view prefix = "tex1_";
  if (!stem.starts_with(prefix)) {
    return std::nullopt;
  }

  std::array<std::string_view, 6> parts{};
  size_t partCount = 0;
  size_t offset = 0;
  bool consumedAll = false;
  while (offset <= stem.size() && partCount < parts.size()) {
    const size_t next = stem.find('_', offset);
    parts[partCount++] = stem.substr(offset, next == std::string_view::npos ? stem.size() - offset : next - offset);
    if (next == std::string_view::npos) {
      consumedAll = true;
      break;
    }
    offset = next + 1;
  }
  if (!consumedAll || partCount < 4 || partCount > 6 || parts[0] != "tex1") {
    return std::nullopt;
  }

  const auto dimensions = parse_dimensions(parts[1]);
  if (!dimensions.has_value()) {
    return std::nullopt;
  }

  size_t index = 2;
  if (parts[index] == "m") {
    ++index;
  }

  size_t remaining = partCount - index;
  if (remaining != 2 && remaining != 3) {
    return std::nullopt;
  }

  uint64_t textureHash = 0;
  if (parts[index] == "$") {
    textureHash = kReplacementWildcardTextureHash;
  } else {
    const auto parsedTex = parse_hex(parts[index]);
    if (!parsedTex.has_value()) {
      return std::nullopt;
    }
    textureHash = *parsedTex;
  }

  auto formatPart = parts[partCount - 1];
  if (formatPart == "arb") {
    formatPart = parts[partCount - 2];
    remaining -= 1;
  }
  const auto format = parse_u32(formatPart);
  if (!format.has_value()) {
    return std::nullopt;
  }

  uint64_t tlutHash = 0;
  const bool hasTlut = remaining == 3;
  if (hasTlut) {
    const std::string_view tlutPart = parts[index + 1];
    if (tlutPart == "$") {
      tlutHash = kReplacementWildcardTlutHash;
    } else {
      const auto parsedTlutHash = parse_hex(tlutPart);
      if (!parsedTlutHash.has_value()) {
        return std::nullopt;
      }
      tlutHash = *parsedTlutHash;
    }
  }

  return RuntimeTextureKey{
      .textureHash = textureHash,
      .tlutHash = tlutHash,
      .width = dimensions->first,
      .height = dimensions->second,
      .hasTlut = hasTlut,
      .format = *format,
  };
}

static std::optional<ConvertedTexture> load_texture_file(const std::filesystem::path& path) {
  if (path.extension() == ".png") {
    return png::load_png_file(path);
  } else {
    return dds::load_dds_file(path);
  }
}

constexpr bool isUnsupportedTextureFormat(const ConvertedTexture& texture) {
  switch (texture.format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC1RGBAUnormSrgb:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnormSrgb:
  case wgpu::TextureFormat::BC3RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnormSrgb:
  case wgpu::TextureFormat::BC4RUnorm:
  case wgpu::TextureFormat::BC4RSnorm:
  case wgpu::TextureFormat::BC5RGUnorm:
  case wgpu::TextureFormat::BC5RGSnorm:
  case wgpu::TextureFormat::BC6HRGBUfloat:
  case wgpu::TextureFormat::BC6HRGBFloat:
  case wgpu::TextureFormat::BC7RGBAUnorm:
  case wgpu::TextureFormat::BC7RGBAUnormSrgb:
    return !webgpu::g_bcTexturesSupported;
  default:
    return false;
  }
}

// WebGPU takes a block-compressed texture only when its full size is whole 4x4 blocks: Dawn refuses the upload of a
// 40x2 BC3 (Metroid: Other M, Dolphin's pack, tex1_20x1_eae6913edd446e27_0.dds) and that validation error stops the
// game. Such a replacement is decoded to RGBA8 instead (BC1, BC3: Direct3D's block layouts); another format of that
// shape is skipped, so the game's own texture is used.
static std::array<uint8_t, 4> expand_rgb565(uint16_t c) noexcept {
  const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
  return {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)), uint8_t((b << 3) | (b >> 2)), 255};
}

// One 4x4 block, its texels row by row as RGBA8. `alpha`: BC3's alpha half, which also puts the colour half in its
// four-colour mode whatever the order of its end points.
static void decode_bc_block(const uint8_t* color, const uint8_t* alpha, uint8_t out[16][4]) noexcept {
  const uint16_t c0 = uint16_t(color[0] | (color[1] << 8)), c1 = uint16_t(color[2] | (color[3] << 8));
  std::array<std::array<uint8_t, 4>, 4> palette{expand_rgb565(c0), expand_rgb565(c1)};
  if (c0 > c1 || alpha != nullptr) {
    for (int ch = 0; ch < 3; ++ch) {
      palette[2][ch] = uint8_t((2 * palette[0][ch] + palette[1][ch] + 1) / 3);
      palette[3][ch] = uint8_t((palette[0][ch] + 2 * palette[1][ch] + 1) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int ch = 0; ch < 3; ++ch) palette[2][ch] = uint8_t((palette[0][ch] + palette[1][ch]) / 2);
    palette[2][3] = 255;
    palette[3] = {0, 0, 0, 0};
  }
  const uint32_t indices = uint32_t(color[4]) | (uint32_t(color[5]) << 8) | (uint32_t(color[6]) << 16) |
                           (uint32_t(color[7]) << 24);
  for (int i = 0; i < 16; ++i) std::memcpy(out[i], palette[(indices >> (2 * i)) & 3u].data(), 4);
  if (alpha == nullptr) return;
  const uint32_t a0 = alpha[0], a1 = alpha[1];
  uint8_t levels[8] = {uint8_t(a0), uint8_t(a1)};
  if (a0 > a1) {
    for (uint32_t k = 1; k <= 6; ++k) levels[k + 1] = uint8_t(((7 - k) * a0 + k * a1 + 3) / 7);
  } else {
    for (uint32_t k = 1; k <= 4; ++k) levels[k + 1] = uint8_t(((5 - k) * a0 + k * a1 + 2) / 5);
    levels[6] = 0;
    levels[7] = 255;
  }
  uint64_t alphaIndices = 0;
  for (int i = 0; i < 6; ++i) alphaIndices |= uint64_t(alpha[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i][3] = levels[(alphaIndices >> (3 * i)) & 7u];
}

static std::optional<ConvertedTexture> make_uploadable(ConvertedTexture tex, const std::filesystem::path& path) noexcept {
  const auto info = format_info(tex.format);
  if (!info.compressed || (tex.width % info.blockWidth == 0 && tex.height % info.blockHeight == 0)) {
    return tex;
  }
  const bool bc1 = tex.format == wgpu::TextureFormat::BC1RGBAUnorm;
  if (!bc1 && tex.format != wgpu::TextureFormat::BC3RGBAUnorm) {
    Log.warn("texture_replacement: {} is {}x{}, not whole 4x4 blocks, in a format that is not decoded here: the game's "
             "own texture is used",
             fs_path_to_string(path), tex.width, tex.height);
    return std::nullopt;
  }
  uint64_t outSize = 0;
  for (uint32_t mip = 0; mip < tex.mips; ++mip) {
    outSize += uint64_t(std::max(tex.width >> mip, 1u)) * std::max(tex.height >> mip, 1u) * 4u;
  }
  ByteBuffer out{static_cast<size_t>(outSize)};
  size_t in = 0, at = 0;
  for (uint32_t mip = 0; mip < tex.mips; ++mip) {
    const uint32_t w = std::max(tex.width >> mip, 1u), h = std::max(tex.height >> mip, 1u);
    const uint32_t blocksWide = (w + 3) / 4, blocksHigh = (h + 3) / 4;
    if (in + size_t(blocksWide) * blocksHigh * info.blockSize > tex.data.size()) {
      Log.warn("texture_replacement: {} is shorter than its size says: the game's own texture is used",
               fs_path_to_string(path));
      return std::nullopt;
    }
    for (uint32_t by = 0; by < blocksHigh; ++by) {
      for (uint32_t bx = 0; bx < blocksWide; ++bx, in += info.blockSize) {
        const uint8_t* block = tex.data.data() + in;
        uint8_t texels[16][4];
        decode_bc_block(bc1 ? block : block + 8, bc1 ? nullptr : block, texels);
        for (uint32_t y = 0; y < 4 && by * 4 + y < h; ++y) {
          for (uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x) {
            std::memcpy(out.data() + at + (size_t(by * 4 + y) * w + bx * 4 + x) * 4, texels[y * 4 + x], 4);
          }
        }
      }
    }
    at += size_t(w) * h * 4;
  }
  Log.info("texture_replacement: {} ({}x{}, {}) decoded to RGBA8: the GPU takes it compressed only in whole 4x4 blocks",
           fs_path_to_string(path), tex.width, tex.height, bc1 ? "BC1" : "BC3");
  return ConvertedTexture{
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .width = tex.width,
      .height = tex.height,
      .mips = tex.mips,
      .data = std::move(out),
  };
}

std::optional<ConvertedTexture> load_replacement(const ReplacementIndexEntry& entry) noexcept {
  auto base = load_texture_file(entry.path);
  if (!base.has_value()) {
    Log.warn("texture_replacement: failed to load texture {}", fs_path_to_string(entry.path));
    return std::nullopt;
  }
  if (isUnsupportedTextureFormat(base.value())) {
    Log.warn(
      "texture_replacement: failed to load texture {} due to unsupported format: {}",
      fs_path_to_string(entry.path),
      static_cast<uint32_t>(base->format));
    return std::nullopt;
  }

  std::vector<ConvertedTexture> more;
  std::error_code ec;
  for (uint32_t mipLevel = 1;; ++mipLevel) {
    const auto mipPath = entry.path.parent_path() / fmt::format("{}_mip{}{}", fs_path_to_string(entry.path.stem()), mipLevel, fs_path_to_string(entry.path.extension()));
    if (!std::filesystem::is_regular_file(mipPath, ec)) {
      break;
    }

    auto lvl = load_texture_file(mipPath);
    const uint32_t ew = std::max(base->width >> mipLevel, 1u);
    const uint32_t eh = std::max(base->height >> mipLevel, 1u);
    const bool ok = lvl.has_value() && lvl->format == base->format && lvl->width == ew && lvl->height == eh;
    if (!ok) {
      if (!lvl.has_value()) {
        Log.warn("texture_replacement: could not load mip {}", fs_path_to_string(mipPath));
      } else {
        Log.warn("texture_replacement: expected {}x{} for mip {}, got {}x{}", ew, eh, fs_path_to_string(mipPath),
                 lvl->width, lvl->height);
      }

      break;
    }
    more.push_back(std::move(*lvl));
  }

  if (more.empty()) {
    return make_uploadable(std::move(*base), entry.path);
  }

  const uint32_t mips = 1u + static_cast<uint32_t>(more.size());
  const uint64_t n = calc_texture_size(base->format, base->width, base->height, mips);
  if (n == 0) {
    return std::nullopt;
  }

  ByteBuffer blob{static_cast<size_t>(n)};
  uint8_t* const dst = blob.data();
  uint64_t o = 0;
  const auto append = [&](const ByteBuffer& d) noexcept -> bool {
    if (o + d.size() > n) {
      return false;
    }
    std::memcpy(dst + o, d.data(), d.size());
    o += d.size();
    return true;
  };
  if (!append(base->data)) {
    return std::nullopt;
  }
  for (const auto& mip : more) {
    if (!append(mip.data)) {
      return std::nullopt;
    }
  }
  if (o != n) {
    return std::nullopt;
  }

  return make_uploadable(ConvertedTexture{
                             .format = base->format,
                             .width = base->width,
                             .height = base->height,
                             .mips = mips,
                             .data = std::move(blob),
                         },
                         entry.path);
}

void touch_cached_replacement(decltype(s_replacementCache)::iterator it) noexcept {
  if (it->second.lruIt != s_replacementLru.begin()) {
    s_replacementLru.splice(s_replacementLru.begin(), s_replacementLru, it->second.lruIt);
    it->second.lruIt = s_replacementLru.begin();
  }
}

void evict_replacement_cache_if_needed() noexcept {
  while (s_replacementCacheBytes > kReplacementCacheBudgetBytes && !s_replacementLru.empty()) {
    const RuntimeTextureKey key = s_replacementLru.back();
    s_replacementLru.pop_back();

    const auto it = s_replacementCache.find(key);
    if (it == s_replacementCache.end()) {
      continue;
    }

    const uint64_t entryBytes = it->second.bytes;
    s_replacementCache.erase(it);
    s_replacementCacheBytes -= std::min(s_replacementCacheBytes, entryBytes);
  }
  s_loadedCount.store(s_replacementCache.size(), std::memory_order_relaxed);
}

void index_button_icons(const std::filesystem::path& userPath) noexcept {
  std::error_code ec;
  s_buttonIconShapes.clear();
  for (auto& icons : s_buttonIconIndex) icons.clear();
  for (auto& icons : s_buttonIconMenuIndex) icons.clear();
  for (size_t style = 0; style < s_buttonIconIndex.size(); ++style) {
    for (uint8_t marker = 0; marker < 8; ++marker) {
      auto dir = userPath / "button_icons" / kButtonIconStyles[style];
      if (marker != 0) dir /= fmt::format("marker{}", marker);
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (const auto parsed = parse_replacement_filename(fs_path_to_string(entry.path().filename())); parsed.has_value())
          s_buttonIconIndex[style].try_emplace(*parsed, ReplacementIndexEntry{entry.path(), marker});
      }
      ec.clear();
    }
    if (style < s_buttonIconMenuIndex.size()) {
      for (const auto& entry : std::filesystem::directory_iterator(userPath / "button_icons" / kButtonIconStyles[style] / "menu", ec)) {
        if (const auto parsed = parse_replacement_filename(fs_path_to_string(entry.path().filename())); parsed.has_value())
          s_buttonIconMenuIndex[style].try_emplace(*parsed, ReplacementIndexEntry{entry.path(), 0});
      }
      ec.clear();
    }
    Log.info("Button icons '{}': {} textures, {} for menus", kButtonIconStyles[style], s_buttonIconIndex[style].size(),
             style < s_buttonIconMenuIndex.size() ? s_buttonIconMenuIndex[style].size() : 0);
  }
  // OTHERM_BUTTON_ICONS=xbox|playstation|nintendo pins a set (tests; scripted input never reaches the gamepad code).
  // "<style>@<frame>" keeps the game's own icons until that presented frame, as when a pad connects late.
  if (const char* forced = std::getenv("OTHERM_BUTTON_ICONS")) {
    const std::string_view text(forced);
    const size_t at = text.find('@');
    const std::string_view name = text.substr(0, at);
    if (at != std::string_view::npos) std::from_chars(text.data() + at + 1, text.data() + text.size(), s_buttonIconForcedFrom);
    for (size_t style = 0; style < kCommonIcons; ++style)
      if (name == kButtonIconStyles[style]) s_buttonIconForced = static_cast<int>(style);
    set_button_icon_style(s_buttonIconForced);
  }
  s_haveButtonIcons = false;
  const auto addShapes = [](const auto& icons) {
    for (const auto& [key, _] : icons) {
      s_buttonIconShapes.insert(texture_shape(key.width, key.height, key.format));
      s_haveButtonIcons = true;
    }
  };
  for (const auto& icons : s_buttonIconIndex) addShapes(icons);
  for (const auto& icons : s_buttonIconMenuIndex) addShapes(icons);
}

void build_index() noexcept {
  auto userPath = fs_path_from_string(g_config.userPath);
  index_button_icons(userPath);
  s_replacementIndex.clear();
  s_indexedCount.store(0, std::memory_order_relaxed);
  if (!g_config.allowTextureReplacements) {
    return;
  }

  auto cachePath = fs_path_from_string(g_config.cachePath);

  s_replacementRoot = userPath / "texture_replacements";
  s_dumpRoot = cachePath / "texture_dumps";

  if (!ensure_directory(s_replacementRoot)) {
    return;
  }
  if (g_config.allowTextureDumps && !ensure_directory(s_dumpRoot)) {
    return;
  }

  std::error_code ec;
  // Mods: <UserData>/mods/<name>/textures (skipped when the mod folder holds a file
  // named "disabled"). The user's own folder is indexed first and try_emplace keeps the first entry, so
  // it wins over mods; among mods the alphabetically later one wins, like their disc file overrides.
  std::vector<std::filesystem::path> roots{s_replacementRoot};
  std::vector<std::filesystem::path> mods;
  for (const auto& entry : std::filesystem::directory_iterator(userPath / "mods", ec)) {
    if (entry.is_directory(ec) && !std::filesystem::exists(entry.path() / "disabled", ec) &&
        std::filesystem::is_directory(entry.path() / "textures", ec)) {
      mods.push_back(entry.path() / "textures");
    }
  }
  std::sort(mods.begin(), mods.end(), std::greater<>());
  roots.insert(roots.end(), mods.begin(), mods.end());
  // Metroid: Other M: then the folders Options > Textures adds (Dolphin's pack for this game), so the port's own
  // textures and mods win over them.
  const size_t firstExtraRoot = roots.size();
  for (const auto& folder : s_extraRoots) {
    if (std::filesystem::is_directory(folder, ec)) roots.push_back(folder);
    ec.clear();
  }
  ec.clear();
  size_t skippedStrips = 0;
  for (size_t rootIndex = 0; rootIndex < roots.size(); ++rootIndex) {
  const auto& root = roots[rootIndex];
  for (std::filesystem::recursive_directory_iterator it(
           root,
           std::filesystem::directory_options::skip_permission_denied |
               std::filesystem::directory_options::follow_directory_symlink,
           ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }

    // A pack (a folder directly in texture_replacements) holding a file named "disabled" is switched off in Options.
    if (root == s_replacementRoot && it.depth() == 0 && it->is_directory(ec) &&
        std::filesystem::exists(it->path() / "disabled", ec)) {
      it.disable_recursion_pending();
      continue;
    }
    ec.clear();

    if (!it->is_regular_file()) {
      continue;
    }

    const auto& path = it->path();

    if (is_relative_to(path, s_dumpRoot)) {
      continue;
    }

    if (!iequals_ascii(fs_path_to_string(path.extension()), ".dds") && !iequals_ascii(fs_path_to_string(path.extension()), ".png")) {
      continue;
    }

    if (is_sidecar_mip(fs_path_to_string(path.stem()))) {
      continue;
    }

    const auto parsed = parse_replacement_filename(fs_path_to_string(path.filename()));
    if (!parsed.has_value()) {
      continue;
    }

    // Dolphin's pack: a texture 1 texel wide is the vertical profile of a HUD frame line or glow, stretched sideways.
    // The developer's pack redrew those profiles (alpha up to 64 of 255 off the game's own in 10 of the 11 the jungle
    // HUD draws), which put faint bands above and below the HUD's numbers and a second line under the energy gauge
    // (NOTES.md 2026-11-18). An upscale cannot add detail to a 1-texel-wide strip, so the game's own stays; the
    // port's own folder and mods still replace them.
    if (rootIndex >= firstExtraRoot && parsed->width == 1 && parsed->height > 1) {
      ++skippedStrips;
      continue;
    }

    s_replacementIndex.try_emplace(*parsed, path);
  }
  }

  s_indexedCount.store(s_replacementIndex.size(), std::memory_order_relaxed);
  Log.info("Indexed {} texture replacements from {} folder(s) (texture_replacements + mods + {} more); {} 1-texel-wide "
           "strip(s) of the added folders left to the game",
           s_replacementIndex.size(), roots.size(), s_extraRoots.size(), skippedStrips);
}

const ReplacementIndexEntry* find_button_icon_path(const RuntimeTextureKey& key) noexcept {
  if (s_appliedButtonIconStyle >= 0) {
    if (s_appliedButtonIconMenu) {
      const auto& menu = s_buttonIconMenuIndex[s_appliedButtonIconStyle];
      if (const auto it = menu.find(key); it != menu.end()) return &it->second;
    }
    const auto& icons = s_buttonIconIndex[s_appliedButtonIconStyle];
    if (const auto it = icons.find(key); it != icons.end()) return &it->second;
  }
  if (const auto it = s_buttonIconIndex[kCommonIcons].find(key); it != s_buttonIconIndex[kCommonIcons].end()) return &it->second;
  return nullptr;
}

const ReplacementIndexEntry* find_replacement_path(const RuntimeTextureKey& key) noexcept {
  if (const auto* icon = find_button_icon_path(key)) return icon;
  if (const auto it = s_replacementIndex.find(key); it != s_replacementIndex.end()) {
    return &it->second;
  }

  if (key.hasTlut) {
    RuntimeTextureKey tlutWildcardKey = key;
    tlutWildcardKey.tlutHash = kReplacementWildcardTlutHash;
    if (const auto it = s_replacementIndex.find(tlutWildcardKey); it != s_replacementIndex.end()) {
      return &it->second;
    }
  }

  RuntimeTextureKey textureWildcardKey = key;
  textureWildcardKey.textureHash = kReplacementWildcardTextureHash;
  if (const auto it = s_replacementIndex.find(textureWildcardKey); it != s_replacementIndex.end()) {
    return &it->second;
  }

  return nullptr;
}

const gfx::TextureHandle* find_cached_replacement(const RuntimeTextureKey& key) noexcept {
  const auto cached = s_replacementCache.find(key);
  if (cached == s_replacementCache.end()) {
    return nullptr;
  }

  touch_cached_replacement(cached);
  return &cached->second.handle;
}

gfx::TextureHandle upload_replacement_texture(const RuntimeTextureKey& key, const ReplacementIndexEntry& entry,
                                              const ConvertedTexture* replacement) noexcept {
  const auto label = fmt::format("TextureReplacement {}", format_replacement_filename(key));
  const wgpu::Extent3D size{
      .width = replacement->width,
      .height = replacement->height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label.c_str(),
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = replacement->format,
      .mipLevelCount = replacement->mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const auto viewLabel = fmt::format("{} view", label);
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = replacement->format,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = replacement->mips,
  };
  auto textureView = texture.CreateView(&textureViewDescriptor);
  auto handle = std::make_shared<gfx::TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{}, size,
                                                  replacement->format, replacement->mips, gfx::InvalidTextureFormat);
  handle->isReplacement = true;
  handle->othermMarker = entry.marker;
  gfx::write_texture(*handle, replacement->data);
  return handle;
}

gfx::TextureHandle load_replacement_texture(const RuntimeTextureKey& key, const ReplacementIndexEntry& entry) noexcept {
  const auto replacement = load_replacement(entry);
  if (!replacement.has_value()) {
    s_failedKeys.insert(key);
    return {};
  }
  return upload_replacement_texture(key, entry, &*replacement);
}

void cache_replacement(const RuntimeTextureKey& key, const gfx::TextureHandle& handle) noexcept {
  const uint64_t replacementBytes =
      calc_texture_size(handle->format, handle->size.width, handle->size.height, handle->mipCount);
  s_replacementLru.push_front(key);
  s_replacementCache.emplace(
      key, CachedReplacement{.handle = handle, .bytes = replacementBytes, .lruIt = s_replacementLru.begin()});
  s_replacementCacheBytes += replacementBytes;
  evict_replacement_cache_if_needed();
  // A few lines show a pack matching the game's textures (Options > Textures shows the count too).
  if (const size_t loaded = s_replacementCache.size(); loaded <= 3 || loaded % 200 == 0)
    Log.info("texture packs: {} replacement(s) loaded, the latest {}", loaded, format_replacement_filename(key));
}

// Pack files are read and decoded on worker threads, so a room full of new textures does not stall the frame that
// first draws them (2026-11-17: one frame spent 114 ms loading 37 pack textures, ~3 ms each from disk). Until a file
// is ready the game's own texture is drawn; gx.cpp drops the stand-ins it cached when take_ready_replacements says
// so. Uploading finished files takes at most about kFrameBudgetMs of the GX thread a frame (at least one file a
// frame). No pack file loads in place: a single one took up to 32 ms (2026-11-17, warm file cache). Button icons
// always load in place (small, and swapped with the game's menus). AURORA_PACK_SYNC=1 loads everything in place.
constexpr double kFrameBudgetMs = 2.0;
constexpr size_t kLoadWorkers = 4;
struct LoadJob {
  RuntimeTextureKey key;
  ReplacementIndexEntry entry;
  uint64_t generation = 0;
};
struct LoadResult {
  RuntimeTextureKey key;
  ReplacementIndexEntry entry;
  uint64_t generation = 0;
  std::optional<ConvertedTexture> texture;
};
std::mutex s_loadMutex;
std::condition_variable s_loadWake;
std::deque<LoadJob> s_loadQueue;    // s_loadMutex
std::deque<LoadResult> s_loadDone;  // s_loadMutex
bool s_loadStop = false;            // s_loadMutex
std::vector<std::thread> s_loadWorkers;
std::atomic<bool> s_loadDoneAny{false};
// The rest belongs to the thread that resolves textures.
uint64_t s_loadGeneration = 1;  // bumped when the packs change: results for the old index are dropped
absl::flat_hash_set<RuntimeTextureKey> s_pendingKeys;
uint32_t s_budgetFrame = 0;
double s_budgetUsedMs = 0.0;

double frame_budget_left() noexcept {
  if (const uint32_t frame = current_frame(); frame != s_budgetFrame) {
    s_budgetFrame = frame;
    s_budgetUsedMs = 0.0;
  }
  return kFrameBudgetMs - s_budgetUsedMs;
}

double elapsed_ms(std::chrono::steady_clock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void load_worker() noexcept {
  for (;;) {
    LoadJob job;
    {
      std::unique_lock lock(s_loadMutex);
      s_loadWake.wait(lock, [] { return s_loadStop || !s_loadQueue.empty(); });
      if (s_loadStop) return;
      job = std::move(s_loadQueue.front());
      s_loadQueue.pop_front();
    }
    auto texture = load_replacement(job.entry);
    {
      std::lock_guard lock(s_loadMutex);
      s_loadDone.push_back({job.key, std::move(job.entry), job.generation, std::move(texture)});
    }
    s_loadDoneAny.store(true, std::memory_order_release);
  }
}

void queue_load(const RuntimeTextureKey& key, const ReplacementIndexEntry& entry) {
  {
    std::lock_guard lock(s_loadMutex);
    if (s_loadWorkers.empty()) {
      s_loadStop = false;
      for (size_t i = 0; i < kLoadWorkers; ++i) s_loadWorkers.emplace_back(load_worker);
    }
    s_loadQueue.push_back({key, entry, s_loadGeneration});
  }
  s_loadWake.notify_one();
  s_pendingKeys.insert(key);
}

// Queued loads are forgotten; one a worker is reading finishes and is dropped by its generation.
void forget_queued_loads() noexcept {
  ++s_loadGeneration;
  s_pendingKeys.clear();
  std::lock_guard lock(s_loadMutex);
  s_loadQueue.clear();
}

void stop_load_workers() noexcept {
  {
    std::lock_guard lock(s_loadMutex);
    s_loadStop = true;
    s_loadQueue.clear();
  }
  s_loadWake.notify_all();
  for (auto& worker : s_loadWorkers) {
    if (worker.joinable()) worker.join();
  }
  s_loadWorkers.clear();
  {
    std::lock_guard lock(s_loadMutex);
    s_loadDone.clear();
    s_loadStop = false;
  }
  s_loadDoneAny.store(false, std::memory_order_relaxed);
  s_pendingKeys.clear();
}

// Prefetch (Options > Textures, off by default): Dolphin's "Prefetch Custom Textures" (bCacheHiresTextures), which
// loads every pack file into memory when the packs are read. Here two background threads do it while the game runs
// (Dolphin's asset loader has two as well), up to Dolphin's memory budget: half the PC's memory, leaving at least
// 2 GiB unused (CustomAssetCache.cpp). A texture already in memory is uploaded in place the first time it is drawn,
// so it never shows the game's own texture first; its memory copy stays for the session, so a texture the GPU cache
// evicted comes back without the disk. Textures not prefetched yet load on demand as without the option.
constexpr size_t kPrefetchWorkers = 2;
std::atomic<bool> s_prefetchWanted{false};
bool s_prefetchActive = false;                      // the thread that resolves textures
std::vector<std::thread> s_prefetchWorkers;         // ditto
std::vector<ReplacementIndexEntry> s_prefetchList;  // read-only while the workers run
std::atomic<size_t> s_prefetchNext{0};
std::atomic<size_t> s_prefetchDone{0};
std::atomic<size_t> s_prefetchTotal{0};
std::atomic<uint64_t> s_prefetchBytes{0};
uint64_t s_prefetchBudget = 0;
std::atomic<bool> s_prefetchStop{false};
std::atomic<bool> s_prefetchFull{false};
std::chrono::steady_clock::time_point s_prefetchStart;
std::mutex s_prefetchMutex;
absl::flat_hash_map<std::string, std::shared_ptr<const ConvertedTexture>> s_prefetched;  // s_prefetchMutex, by path

uint64_t prefetch_budget() noexcept {
  const uint64_t total = static_cast<uint64_t>(std::max(SDL_GetSystemRAM(), 0)) << 20;  // MiB
  const uint64_t keepUnused = std::max(total / 2, std::min<uint64_t>(total, uint64_t{2} << 30));
  return total - keepUnused;
}

void prefetch_worker() noexcept {
  while (!s_prefetchStop.load(std::memory_order_acquire)) {
    const size_t index = s_prefetchNext.fetch_add(1, std::memory_order_relaxed);
    if (index >= s_prefetchList.size()) {
      return;
    }
    if (s_prefetchBytes.load(std::memory_order_relaxed) >= s_prefetchBudget) {
      if (!s_prefetchFull.exchange(true)) {
        Log.warn("texture prefetch: stopped at the memory budget ({:.1f} GB) after {} of {} textures",
                 s_prefetchBudget / 1e9, s_prefetchDone.load(), s_prefetchList.size());
      }
      return;
    }
    const auto& entry = s_prefetchList[index];
    if (auto texture = load_replacement(entry)) {
      const uint64_t bytes = texture->data.size();
      auto shared = std::make_shared<const ConvertedTexture>(std::move(*texture));
      {
        std::lock_guard lock(s_prefetchMutex);
        s_prefetched.emplace(fs_path_to_string(entry.path), std::move(shared));
      }
      s_prefetchBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    if (s_prefetchDone.fetch_add(1, std::memory_order_acq_rel) + 1 == s_prefetchList.size()) {
      Log.info("texture prefetch: {} textures in memory ({:.2f} GB) after {:.1f} s", s_prefetched.size(),
               s_prefetchBytes.load() / 1e9, elapsed_ms(s_prefetchStart) / 1000.0);
    }
  }
}

void stop_prefetch() noexcept {
  s_prefetchStop.store(true, std::memory_order_release);
  for (auto& worker : s_prefetchWorkers) {
    if (worker.joinable()) worker.join();
  }
  s_prefetchWorkers.clear();
  s_prefetchList.clear();
  {
    std::lock_guard lock(s_prefetchMutex);
    s_prefetched.clear();
  }
  s_prefetchNext.store(0);
  s_prefetchDone.store(0);
  s_prefetchTotal.store(0);
  s_prefetchBytes.store(0);
  s_prefetchFull.store(false);
  s_prefetchActive = false;
}

// Every file of the current index once (wildcard names can point several keys at one file).
void start_prefetch() noexcept {
  stop_prefetch();
  absl::flat_hash_set<std::string> seen;
  for (const auto& [key, entry] : s_replacementIndex) {
    if (seen.insert(fs_path_to_string(entry.path)).second) s_prefetchList.push_back(entry);
  }
  s_prefetchTotal.store(s_prefetchList.size());
  s_prefetchBudget = prefetch_budget();
  s_prefetchStop.store(false, std::memory_order_release);
  s_prefetchStart = std::chrono::steady_clock::now();
  s_prefetchActive = true;
  Log.info("texture prefetch: loading {} textures into memory (budget {:.1f} GB)", s_prefetchList.size(),
           s_prefetchBudget / 1e9);
  if (s_prefetchList.empty()) return;
  for (size_t i = 0; i < kPrefetchWorkers; ++i) s_prefetchWorkers.emplace_back(prefetch_worker);
}

// On the thread that resolves textures: start or stop to match the option and the packs; `indexChanged` restarts a
// running prefetch for the new index.
void apply_prefetch_request(bool indexChanged) noexcept {
  const bool wanted = s_prefetchWanted.load(std::memory_order_acquire) && g_config.allowTextureReplacements;
  if (wanted && (!s_prefetchActive || indexChanged)) {
    start_prefetch();
  } else if (!wanted && s_prefetchActive) {
    stop_prefetch();
    Log.info("texture prefetch: off, memory copies released");
  }
}

std::shared_ptr<const ConvertedTexture> find_prefetched(const ReplacementIndexEntry& entry) noexcept {
  if (!s_prefetchActive) return {};
  std::lock_guard lock(s_prefetchMutex);
  const auto it = s_prefetched.find(fs_path_to_string(entry.path));
  return it != s_prefetched.end() ? it->second : nullptr;
}

bool dump_editable_texture_dds(const RuntimeTextureKey& key, const GXTexObj_& obj) noexcept {
  const ArrayRef<uint8_t> texData{static_cast<const uint8_t*>(obj.data), UINT32_MAX};
  const uint32_t texWidth = obj.width();
  const uint32_t texHeight = obj.height();

  ConvertedTexture pixels;
  if (is_palette_format(obj.format())) {
    const TlutMetadata* tlut = get_loaded_tlut(obj);
    if (tlut == nullptr) {
      return false;
    }
    pixels =
        convert_texture_palette(obj.format(), texWidth, texHeight, 1, texData, static_cast<GXTlutFmt>(tlut->format),
                                tlut->entries, {tlut->data.data(), tlut->data.size()});
  } else {
    pixels = convert_texture(obj.format(), texWidth, texHeight, 1, texData);
  }

  const uint64_t rgbaBytes = calc_texture_size(wgpu::TextureFormat::RGBA8Unorm, texWidth, texHeight, 1);

  if (pixels.data.empty() || pixels.format != wgpu::TextureFormat::RGBA8Unorm || pixels.data.size() != rgbaBytes) {
    return false;
  }

  const auto path = s_dumpRoot / format_replacement_filename(key);
  return dds::write_rgba8_dds(path, texWidth, texHeight, pixels.data);
}

bool report_missing_key(const RuntimeTextureKey& key, const GXTexObj_& obj) noexcept {
  if (!s_reportedMisses.insert(key).second) {
    return false;
  }

  if (g_config.allowTextureDumps) {
    dump_editable_texture_dds(key, obj);
  }
  return true;
}

void set_texture_packs(bool enabled, bool dumps, std::vector<std::filesystem::path> folders) noexcept {
  std::lock_guard lock(s_packRequestMutex);
  s_packRequest = PackRequest{.enabled = enabled, .dumps = dumps, .folders = std::move(folders)};
}

bool take_texture_pack_change() noexcept {
  std::optional<PackRequest> request;
  {
    std::lock_guard lock(s_packRequestMutex);
    request.swap(s_packRequest);
  }
  if (!request.has_value()) {
    apply_prefetch_request(false);
    return false;
  }
  g_config.allowTextureReplacements = request->enabled;
  g_config.allowTextureDumps = request->enabled && request->dumps;
  s_extraRoots = std::move(request->folders);
  // Textures already on screen keep their handles; everything is looked up again (gx.cpp drops its caches).
  s_replacementCache.clear();
  s_replacementLru.clear();
  s_replacementCacheBytes = 0;
  s_loadedCount.store(0, std::memory_order_relaxed);
  s_failedKeys.clear();
  s_reportedMisses.clear();
  forget_queued_loads();
  if (!request->enabled) {
    s_pendingTluts.clear();
    for (auto& tlut : s_loadedTluts) tlut = {};
  }
  s_replacementRoot.clear();
  s_dumpRoot.clear();
  build_index();
  Log.info("Texture packs {}{}: {} textures", request->enabled ? "on" : "off",
           g_config.allowTextureDumps ? ", dumping" : "", s_replacementIndex.size());
  apply_prefetch_request(true);
  return true;
}

void set_texture_prefetch(bool enabled) noexcept { s_prefetchWanted.store(enabled, std::memory_order_release); }

TexturePrefetchProgress texture_prefetch_progress() noexcept {
  return {
      .done = s_prefetchDone.load(std::memory_order_relaxed),
      .total = s_prefetchTotal.load(std::memory_order_relaxed),
      .bytes = s_prefetchBytes.load(std::memory_order_relaxed),
      .budget = s_prefetchBudget,
      .full = s_prefetchFull.load(std::memory_order_relaxed),
  };
}

size_t indexed_count() noexcept { return s_indexedCount.load(std::memory_order_relaxed); }
size_t loaded_count() noexcept { return s_loadedCount.load(std::memory_order_relaxed); }
bool texture_packs_pending() noexcept {
  std::lock_guard lock(s_packRequestMutex);
  return s_packRequest.has_value();
}

// A request made before the renderer started (the saved settings) is the first index.
void initialize() noexcept {
  if (!take_texture_pack_change()) build_index();
}

void shutdown() noexcept {
  stop_load_workers();
  stop_prefetch();
  s_replacementIndex.clear();
  for (auto& icons : s_buttonIconIndex) icons.clear();
  for (auto& icons : s_buttonIconMenuIndex) icons.clear();
  s_buttonIconShapes.clear();
  s_haveButtonIcons = false;
  s_replacementCache.clear();
  s_failedKeys.clear();
  s_reportedMisses.clear();
  s_pendingTluts.clear();
  for (auto& tlut : s_loadedTluts) {
    tlut = {};
  }
  s_replacementLru.clear();
  s_replacementCacheBytes = 0;
  s_replacementRoot.clear();
  s_dumpRoot.clear();
  s_indexedCount.store(0, std::memory_order_relaxed);
  s_loadedCount.store(0, std::memory_order_relaxed);
}

// Mirrors of the guest palettes, kept only so find_replacement can hash and the DDS dump decode a
// CI palette. The renderer uploads from the guest pointer, so skip this work when replacement is off.
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept {
  if (!g_config.allowTextureReplacements) {
    return;
  }
  if (obj == nullptr || data == nullptr) {
    return;
  }

  // A registration is only consumed by a matching load_tlut, and the map is keyed by a guest address
  // the game may free and reuse, so a table this large means stale entries: drop them wholesale.
  constexpr size_t kMaxPendingTluts = 4096;
  if (s_pendingTluts.size() >= kMaxPendingTluts && !s_pendingTluts.contains(obj)) {
    s_pendingTluts.clear();
  }

  const size_t sz = static_cast<size_t>(entries) * 2;
  ByteBuffer buffer{sz};
  std::memcpy(buffer.data(), static_cast<const uint8_t*>(data), sz);
  s_pendingTluts[obj] = {
      .size = static_cast<uint32_t>(entries) * 2,
      .format = static_cast<uint32_t>(format),
      .entries = entries,
      .valid = true,
      .source = obj,
      .data = std::move(buffer),
  };
}

void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept {
  if (!g_config.allowTextureReplacements) {
    return;
  }
  if (idx >= s_loadedTluts.size()) {
    return;
  }

  // Consume the pending registration. The map is keyed by a guest object address that can be freed
  // or reused at any time, and entries used to live until shutdown.
  if (const auto it = s_pendingTluts.find(obj); it != s_pendingTluts.end()) {
    s_loadedTluts[idx] = std::move(it->second);
    s_pendingTluts.erase(it);
    return;
  }

  // The same object may legally be loaded into several hardware slots; only the first load finds the
  // pending entry, so recover the palette from whichever slot already holds it.
  for (const auto& loaded : s_loadedTluts) {
    if (!loaded.valid || loaded.source != obj) {
      continue;
    }
    s_loadedTluts[idx] = {
        .size = loaded.size,
        .format = loaded.format,
        .entries = loaded.entries,
        .valid = loaded.valid,
        .source = loaded.source,
        .data = loaded.data.clone(),
    };
    return;
  }

  s_loadedTluts[idx] = {};
}

std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, bool* pending) noexcept {
  ZoneScoped;

  const bool packs = g_config.allowTextureReplacements;
  if (!packs && (!s_haveButtonIcons ||
                 !s_buttonIconShapes.contains(texture_shape(obj.width(), obj.height(), obj.format())))) {
    return std::nullopt;
  }

  // AURORA_TEST_REPLACEMENTS=<file>[,<file>...] (tests): those pack files are loaded and uploaded at the first lookup
  // once the packs are indexed, as they would be for a texture the game draws, whether or not it draws them here.
  static const char* testFiles = std::getenv("AURORA_TEST_REPLACEMENTS");
  static bool testDone = false;
  if (testFiles != nullptr && packs && !testDone && !s_replacementIndex.empty()) {
    testDone = true;
    const std::string list = std::string(",") + testFiles + ",";
    int matched = 0;
    for (const auto& [testKey, entry] : s_replacementIndex) {
      const std::string name = fs_path_to_string(entry.path.filename());
      if (list.find("," + name + ",") == std::string::npos) continue;
      ++matched;
      auto handle = load_replacement_texture(testKey, entry);
      Log.info("[test] replacement {}: {}", name,
               handle ? fmt::format("uploaded {}x{}, format {}", handle->size.width, handle->size.height,
                                    static_cast<uint32_t>(handle->format))
                      : std::string("not loaded"));
      if (handle) cache_replacement(testKey, handle);
    }
    Log.info("[test] {} of the named replacement(s) found in the packs", matched);
  }

  const RuntimeTextureKey key = build_runtime_key(obj);
  const auto* path = packs ? find_replacement_path(key) : find_button_icon_path(key);
  // OTHERM_LOG_ICONS=1: every lookup of a texture shaped like a button icon, once per key and style.
  static const bool logIcons = std::getenv("OTHERM_LOG_ICONS") != nullptr;
  if (logIcons && s_buttonIconShapes.contains(texture_shape(obj.width(), obj.height(), obj.format()))) {
    static absl::flat_hash_set<std::pair<uint64_t, int>> s_logged;
    if (s_logged.insert({key.textureHash, s_appliedButtonIconStyle * 2 + int(s_appliedButtonIconMenu)}).second)
      Log.info("[icons] frame {} {}x{} fmt {} key {:016x} style {}{}: {}", current_frame(), key.width, key.height,
               key.format, key.textureHash, s_appliedButtonIconStyle, s_appliedButtonIconMenu ? " menu" : "",
               path != nullptr ? fs_path_to_string(path->path.parent_path().filename()) + "/" +
                                     fs_path_to_string(path->path.filename())
                               : std::string("no replacement"));
  }
  if (path == nullptr) {
    if (packs) report_missing_key(key, obj);
    return std::nullopt;
  }

  if (const auto* cached = find_cached_replacement(key); cached != nullptr) {
    return *cached;
  }

  if (s_failedKeys.contains(key)) {
    return std::nullopt;
  }

  if (s_pendingKeys.contains(key)) {
    if (pending != nullptr) *pending = true;
    return std::nullopt;
  }
  static const bool syncLoads = std::getenv("AURORA_PACK_SYNC") != nullptr;
  const bool inPlace = syncLoads || !packs || find_button_icon_path(key) != nullptr;
  if (!inPlace) {
    if (const auto prefetched = find_prefetched(*path)) {
      gfx::TextureHandle handle;
      {
        const frame_costs::Scope cost{frame_costs::Pack};
        handle = upload_replacement_texture(key, *path, prefetched.get());
      }
      if (handle) {
        cache_replacement(key, handle);
        return handle;
      }
    }
    queue_load(key, *path);
    if (pending != nullptr) *pending = true;
    return std::nullopt;
  }

  gfx::TextureHandle handle;
  {
    const frame_costs::Scope cost{frame_costs::Pack};
    handle = load_replacement_texture(key, *path);
  }
  if (!handle) {
    return std::nullopt;
  }

  cache_replacement(key, handle);
  return handle;
}

bool take_ready_replacements() noexcept {
  if (!s_loadDoneAny.load(std::memory_order_acquire)) {
    return false;
  }
  bool uploaded = false;
  while (frame_budget_left() > 0.0) {
    LoadResult result;
    {
      std::lock_guard lock(s_loadMutex);
      if (s_loadDone.empty()) {
        s_loadDoneAny.store(false, std::memory_order_relaxed);
        break;
      }
      result = std::move(s_loadDone.front());
      s_loadDone.pop_front();
    }
    s_pendingKeys.erase(result.key);
    if (result.generation != s_loadGeneration) {
      continue;
    }
    if (!result.texture.has_value()) {
      s_failedKeys.insert(result.key);  // load_replacement said why
      continue;
    }
    const auto start = std::chrono::steady_clock::now();
    {
      const frame_costs::Scope cost{frame_costs::Pack};
      if (auto handle = upload_replacement_texture(result.key, result.entry, &*result.texture)) {
        cache_replacement(result.key, handle);
        uploaded = true;
      }
    }
    s_budgetUsedMs += elapsed_ms(start);
  }
  return uploaded;
}

bool is_button_icon_shape(uint32_t width, uint32_t height, uint32_t format) noexcept {
  return s_buttonIconShapes.contains(texture_shape(width, height, format));
}

void set_button_icon_style(int style) noexcept {
  if (s_buttonIconForced >= 0) style = current_frame() >= s_buttonIconForcedFrom ? s_buttonIconForced : -1;
  if (style < -1 || style >= kCommonIcons) style = -1;
  if (s_buttonIconStyle.exchange(style) != style) s_buttonIconStyleChanged.store(true);
}

// Called on the renderer's thread before textures are resolved: drops the cached icon textures of the old style.
int button_icon_style() noexcept { return s_buttonIconStyle.load(); }

void set_button_icon_menu(bool menu) noexcept {
  if (s_buttonIconMenu.exchange(menu) != menu) s_buttonIconStyleChanged.store(true);
}

bool take_button_icon_style_change() noexcept {
  const bool reload = s_buttonIconReload.exchange(false);
  if (!s_buttonIconStyleChanged.exchange(false) && !reload) return false;
  s_appliedButtonIconStyle = s_buttonIconStyle.load();
  s_appliedButtonIconMenu = s_buttonIconMenu.load();
  // The cache is keyed by the game's texture, which every set shares: drop each icon so the new set's picture loads.
  const auto drop = [](const auto& icons) {
    for (const auto& [key, _] : icons) {
      if (const auto it = s_replacementCache.find(key); it != s_replacementCache.end()) {
        s_replacementLru.erase(it->second.lruIt);
        s_replacementCacheBytes -= it->second.bytes;
        s_replacementCache.erase(it);
      }
      s_failedKeys.erase(key);
    }
  };
  for (const auto& icons : s_buttonIconIndex) drop(icons);
  for (const auto& icons : s_buttonIconMenuIndex) drop(icons);
  if (reload) {
    // The pictures were drawn again (Metroid: Other M, button_icons.cpp): read the folders, then drop what the new
    // index names too. The caller drops the game textures shaped like an icon, which now include any new shapes.
    index_button_icons(fs_path_from_string(g_config.userPath));
    for (const auto& icons : s_buttonIconIndex) drop(icons);
    for (const auto& icons : s_buttonIconMenuIndex) drop(icons);
  }
  return true;
}

void reload_button_icons() noexcept { s_buttonIconReload.store(true); }

std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept {
  const RuntimeTextureKey key = build_runtime_key(obj);
  return format_replacement_filename(key);
}

} // namespace aurora::gfx::texture_replacement
