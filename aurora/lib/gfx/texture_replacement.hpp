#pragma once

#include "texture.hpp"
#include <filesystem>
#include <optional>
#include <vector>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept;
void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept;
// *pending (if given) is set when a pack file for obj is being read on a worker: the caller draws the game's own
// texture meanwhile, and drops what it cached for it once take_ready_replacements returns true.
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, bool* pending = nullptr) noexcept;
// Uploads pack files the workers finished, within the frame's budget. True if any is now ready to be looked up.
bool take_ready_replacements() noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept;
// The button prompt set for the controller in use (0 xbox, 1 playstation, 2 nintendo,
// -1 the game's own Wii Remote icons). Returns true once after it changed, so cached textures can be re-resolved.
void set_button_icon_style(int style) noexcept;
// Metroid: Other M: a game menu is up; <style>/menu/ is looked up first. A change counts as a style change.
void set_button_icon_menu(bool menu) noexcept;
bool take_button_icon_style_change() noexcept;
// Metroid: Other M: <userPath>/button_icons was written again; read at the next take_button_icon_style_change.
void reload_button_icons() noexcept;
int button_icon_style() noexcept;
// Whether a texture of this size and GX format could be a button icon (some set has one shaped like it).
bool is_button_icon_shape(uint32_t width, uint32_t height, uint32_t format) noexcept;
// Metroid: Other M, Options > Textures: packs on or off, dumps, and folders indexed after the port's own. Applied by
// take_texture_pack_change at the next texture resolve, which reads the folders again (true: caches must be dropped).
void set_texture_packs(bool enabled, bool dumps, std::vector<std::filesystem::path> folders) noexcept;
bool take_texture_pack_change() noexcept;
size_t indexed_count() noexcept;
size_t loaded_count() noexcept;
bool texture_packs_pending() noexcept;
// Options > Textures, prefetch: load every pack file into memory in the background (Dolphin's Prefetch Custom
// Textures). Applied at the next texture resolve, on the thread that looks replacements up.
void set_texture_prefetch(bool enabled) noexcept;
struct TexturePrefetchProgress {
  size_t done = 0;    // files read (failed ones count too)
  size_t total = 0;   // files to read; 0 while the prefetch is off
  uint64_t bytes = 0; // memory held by the copies
  uint64_t budget = 0;
  bool full = false;  // stopped at the memory budget
};
TexturePrefetchProgress texture_prefetch_progress() noexcept;
} // namespace aurora::gfx::texture_replacement
