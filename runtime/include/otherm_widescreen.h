#pragma once
// Metroid: Other M on screens wider than 16:9 (Options > Display > Aspect ratio: Fill screen). The game draws its own
// wider 3D view: its camera aspect (a 16:9 constant in game.rel's data) is set to the screen's, so the camera, the
// game's visibility checks and its screen-space effects (bloom, refraction, ripples) all agree. The 640-wide frame is
// then stretched across the screen, aurora narrows the game's 16:9 2D layers (HUD, menus) back to their shape, in
// play the HUD's layouts move out to the screen's edges, and Samus's visor is stretched to fill it.
// How the constant was found: hle/otherm_widescreen.cpp and NOTES.md (2026-11-19).

namespace OthermWidescreen {
// At every frame boundary on the game thread. aspect: the 3D view's width over height (0 = the game's own 16:9).
void Apply(float aspect);
// The 3D view aspect the game is drawing with (0 until game.rel is loaded).
float GameAspect();
// GXSetProjection from nw4r::g3d::Camera::GXSetProjection (it passes the matrix it caches at CameraData+48): a
// screen-sized camera set up earlier with another view aspect (before a state load or a setting change) takes the
// current one, matrix included, before the matrix is read.
void OnCameraProjection(uint32_t matrix);
// The return address of that call inside Camera::GXSetProjection (0x801CCB80).
constexpr uint32_t kCameraSetProjectionReturn = 0x801CCC34u;

// HUD layouts at the screen's edges. Each HUD element is an nw4r::lyt::Layout drawn through rel_8065A3B8, which sets
// a projection of the layout's rect (854x480) and then draws its panes. On a wider screen aurora narrows that
// projection to the 16:9 middle; a layout reaching into the outer part of it moves to that edge (energy and missiles,
// the map), the others follow the wide view (the first-person cursor), the visor (wide, mirror-symmetric layouts) and
// the controller notice's backdrop are stretched to fill the screen; in a menu nothing changes.
// GXSetProjection, before the matrix (row-major 4x4, host floats) goes to aurora: lr says who set it, layout is the
// guest Layout (0 when the projection is not a layout's). May shift or scale m's x terms.
void OnProjection(uint32_t lr, uint32_t layout, float m[16], bool orthographic, float viewportW, float viewportH);
// GXLoadPosMtxImm (host floats, 3x4 row-major; matrixAddress: the guest matrix): a pane's position while a layout
// draws. May widen the matrix (a layout stretched over the wide screen).
void OnPositionMatrix(float m[12], uint32_t matrixAddress);
// The return addresses of the two GXSetProjection calls in rel_8065A3B8 (perspective, orthographic); r29 then holds
// the drawer, whose first word is the Layout.
constexpr uint32_t kLayoutPerspectiveReturn = 0x8065A464u;
constexpr uint32_t kLayoutOrthoReturn = 0x8065A4FCu;
}  // namespace OthermWidescreen
