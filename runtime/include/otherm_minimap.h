#pragma once
// The HUD map facing north (Options > Cheats): hle/otherm_minimap.cpp draws the HUD's elements (rel_80638CAC) and,
// while this is on, turns the map's elements back by the map's own turn. NOTES.md 2026-11-20.
#include <cstdint>
#include <string>

namespace OthermMinimap {
bool NorthUp();
void SetNorthUp(bool on);  // saved to Config.toml [cheats] minimap_north_up

// The HUD map on a wide screen (otherm_widescreen.cpp): whether a layout is one of the HUD map's elements (area maps,
// markers, Samus's marker, plate, mask, frame, rings), which move to an edge together, and its frame's layout (0 when
// not made yet). Game thread only.
bool IsHudMapLayout(uint32_t layout);
uint32_t HudMapFrameLayout();

// The name of the area map the HUD map drew within the last `withinFrames` presented frames (its layout: "A_36",
// "MA_01"), empty when none was (pause screen, cutscenes, menus). Read by hle/otherm_area.cpp. Any thread.
std::string DrawnAreaMap(uint32_t withinFrames);
}  // namespace OthermMinimap
