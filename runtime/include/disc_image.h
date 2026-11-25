#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// A Wii disc image (.iso, .ciso or .wbfs): decodes the container, decrypts the game partition and extracts it to
// the layout the game reads (sys/ and files/).
namespace DiscImage {
struct Info {
    std::string id;      // "R3OE01"
    int revision = 0;
    std::string title;
};
// Reads the header and checks the game partition decrypts. False with `error` when it isn't a usable Wii image.
bool Probe(const std::filesystem::path& image, Info& info, std::string& error);
// Writes sys/ and files/ under `outDir`; `progress(done, total)` after each file.
bool Extract(const std::filesystem::path& image, const std::filesystem::path& outDir,
             const std::function<void(int, int)>& progress, std::string& error);
// Reads `size` bytes of the game partition's decrypted data at `offset` (0: its boot.bin) into `out`; throws
// std::runtime_error when it can't.
using PartitionReader = std::function<void(int64_t offset, void* out, size_t size)>;
// Extract's second half, from a game partition another reader decrypts (Android: nod, android/nod_disc.cpp).
bool ExtractPartition(const PartitionReader& read, const std::filesystem::path& outDir,
                      const std::function<void(int, int)>& progress, std::string& error);
} // namespace DiscImage
