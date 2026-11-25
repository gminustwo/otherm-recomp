#pragma once
// 360-degree movement (Options > Cheats): Samus moves in the stick's own direction instead of the nearest of the
// D-pad's eight. hle/otherm_move.cpp works out the game's D-pad direction (rel_80572110) natively and puts the stick's
// direction in its place; VirtualRemote::MoveDirection supplies it. NOTES.md 2026-11-20.

namespace OthermMove {
bool Enabled();
void SetEnabled(bool on);  // saved to Config.toml [cheats] analog_movement
}  // namespace OthermMove
