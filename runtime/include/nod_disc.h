#pragma once

#include "disc_image.h"

#include <filesystem>
#include <functional>
#include <string>

// A disc image in any format Dolphin writes (.rvz, .wia, .gcz, .iso, .ciso, .wbfs), read through nod's C library
// (github.com/encounter/nod) from an open file descriptor: on Android the game reads the image the system's file picker
// handed over, which it can't open by path. Android builds only (runtime/src/android/nod_disc.cpp).
namespace NodDisc {

// Reads the header and opens the game partition. False with `error` when it isn't a usable Wii image.
bool Probe(int fd, DiscImage::Info& info, std::string& error);
// Writes sys/ and files/ under `outDir`, as DiscImage::Extract does; `progress(done, total)` after each file.
bool Extract(int fd, const std::filesystem::path& outDir, const std::function<void(int, int)>& progress, std::string& error);

} // namespace NodDisc
