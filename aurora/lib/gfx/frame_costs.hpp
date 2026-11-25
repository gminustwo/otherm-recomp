#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Work that can stall a frame, timed where it happens so the runtime's slow-frame log can say which of it a slow
// frame did (AuroraFrameCostTotals). Totals since start, never reset: readers take deltas. Steady-clock reads only
// around these events (rare: a cache miss, a pack file, a CPU-demanded EFB download), never per draw.
namespace aurora::gfx::frame_costs {

enum Kind : int {
  Texture = 0,  // a game texture converted and uploaded (static-texture cache miss, or an uncacheable texture)
  Pack = 1,     // a texture-pack replacement read from disk, decoded and uploaded (first use, or after eviction)
  EfbSync = 2,  // the CPU waiting for an EFB copy's pixels (the game reading a copy back into RAM)
  KindCount = 3,
};

inline std::atomic<uint64_t> g_nanos[KindCount]{};
inline std::atomic<uint64_t> g_counts[KindCount]{};

struct Scope {
  Kind kind;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  explicit Scope(Kind k) noexcept : kind(k) {}
  ~Scope() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
    g_nanos[kind].fetch_add(static_cast<uint64_t>(ns.count()), std::memory_order_relaxed);
    g_counts[kind].fetch_add(1, std::memory_order_relaxed);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

} // namespace aurora::gfx::frame_costs
