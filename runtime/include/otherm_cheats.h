#pragma once
// Built-in cheats for Metroid: Other M (Options > Cheats): infinite energy and infinite missiles. They work through
// the game's own setters, which keep its save-data tamper hashes (the object at 0x80CB7268) in step; how each value
// was found is in hle/otherm_cheats.cpp and NOTES.md (2026-11-08).
#include <cstdint>

struct CpuContext;

namespace OthermCheats {
// Each retrace, on the guest thread that advanced it (vi.cpp AdvanceRetrace), before the Gecko codes run.
void Frame(CpuContext* cpu);

bool InfiniteEnergy();
void SetInfiniteEnergy(bool on);  // saved to Config.toml [cheats] infinite_energy
bool InfiniteMissiles();
void SetInfiniteMissiles(bool on);  // saved to Config.toml [cheats] infinite_missiles
}  // namespace OthermCheats
