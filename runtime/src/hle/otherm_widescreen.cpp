// Other M's own wide 3D view (otherm_widescreen.h). How the constant was found (game.rel pre-linked at 0x8052B280;
// NOTES.md 2026-11-19):
//  - The main 3D camera is an nw4r::g3d::Camera. Its SetPerspective (0x801CC450, inlined at every call site) stores
//    fovy / aspect / near / far at CameraData +172 / +176 / +180 / +184, and every matrix the camera hands out is built
//    from them: GXSetProjection (0x801CCB80), the projection matrix (0x801CC680) and the screen-space texture matrix
//    (0x801CC8C0) that the refraction and ripple materials sample the screen copies through.
//  - rel_807CA114 (the scene camera, which sets the projection the 3D scene draws with: the [gx] perspective lr
//    801CCC34 / 807CA630, aspect 1.778, fovy 45) passes as aspect the float at 0x80B54CB0, read through the getter
//    rel_807E6E04. The float is 16/9 in game.rel's data (beside 45.0, 0.1, 16000.0: the camera defaults); no code
//    stores to it. About twenty functions read it: the scene camera, rel_807E6E10..807E6E58, visibility helpers
//    (rel_8057D5B0, rel_8057F7A8, rel_8058109C), the bloom pass (rel_80802AF8, rel_80802BE0) and others. So the
//    screen's aspect written there widens the camera, what the game culls and its effects together (Hor+: the same
//    height, more to the sides).
//  - The scene camera reads that float only when it is set up (a room, a cutscene), and draws from its CameraData
//    copy (at 0x80F4E7D8 in the developer's states: viewport 640x480, fovy 45) every frame after. So a state saved
//    with another width, or a setting changed in a room, would keep the old width until the next setup. 0x801CCB80
//    hands GXSetProjection the matrix it caches at CameraData+48 (`r3 = r31 + 48`, lr 0x801CCC34), so the GX
//    override finds the camera there and brings a screen-sized one up to date (OnCameraProjection). The camera
//    objects keep their own copy too (+168; Apply updates the ones in the slots at 0x80C92810).
//  - The HUD and menus are nw4r::lyt layouts (854x480, their mLayoutSize; Layout methods at 0x801DBxxx, vtable
//    0x802497A8) drawn through rel_8065A3B8 with an orthographic projection of that rect (r29 = the drawer, whose
//    first word is the Layout). aurora narrows that 16:9 projection back to its shape on the wide frame
//    (AuroraSetWidePerspectiveAspect with AuroraSetWideGameNative). In play the HUD's layouts then move to the screen's
//    edges: each pane loads its global matrix with GXLoadPosMtxImm while its layout draws, so the x translations say
//    where the layout's panes sit; energy and missiles (15 layouts, x -451..8) go left, the map (9 layouts, x 35..361)
//    right, the missile numbers (x -127..-15) with the energy frame they sit in. The map's layouts (the HUD elements
//    [0xA3]..[0xB7], otherm_minimap.cpp) all take the side of its frame: the area map's rooms move with Samus.
//  - Samus's visor (first person, the arrival after a save loads, the pause map's frame) is nine layouts whose panes
//    come in mirror pairs: five frame layouts (arcs at y 270 / -260 out to x +-447..+-484) and four side rails
//    (+-227..+-380). They are stretched across the wide screen, as the Wii stretches them to 16:9: the frame is one
//    curve cut into many panes, so moving pieces apart would open gaps. (Their roots' scale is 1.3 only while the
//    visor zooms in, so it cannot tell them.)
//    The stretch widens each pane's position matrix by 1 / aurora's narrowing as it loads (OnPositionMatrix), so the
//    two cancel at any screen shape (a pre-stretched projection would look 4:3 at 21:9 and be narrowed again).
//  - A menu (the B / BACK legend's badge, or a wide layout that is not mirror-symmetric: the pause map's labels,
//    -444..441, 68% mirrored) stops the move to the edges: the pause map's markers are one icon layout drawn at every
//    marker, so the menu's own layouts keep the 16:9 placement. The visor around a menu (the pause map, STATUS)
//    is still stretched; its BACK / SELECT badges move with the frame's corners, their shape kept (g_visorMenu), and
//    full-width bands over it (the scan sweep) are stretched with it (g_bandStretch).
//  - Each layout's place settles over its first 10 frames and then holds while it is drawn: edge layouts reach into
//    the outer fifth of the 16:9 frame (or sit inside one that does), the rest follow the wide view (their middle drawn
//    where it is on the full width, their shape kept). The first-person cursor (rel layout at +-390 when pushed to a
//    side) follows: the game aims it through the wide aspect, so it lands on what it points at, and moving it never
//    sends it, or the HUD, anywhere else.
//  - The controller notice at boot ("hold the Wii Remote horizontally") is recognised by its panes' names; its
//    backdrop, side walls, grids and connectors are stretched, its picture, frame and text box keep their shape
//    (NoticeKeepsShape). Its text is the game's 640x480 text layer, which aurora keeps in shape anyway.
#include "otherm_widescreen.h"

#include "memory.h"
#include "otherm_minimap.h"
#include <aurora/aurora.h>
#include "runtime_log.h"

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace OthermWidescreen {
namespace {

constexpr uint32_t kModuleList = 0x800030C8u;  // __OSModuleInfoList: non-zero once game.rel is linked
constexpr uint32_t kViewAspect = 0x80B54CB0u;  // f32 16/9: the scene camera's aspect (rel_807E6E04 returns it)
constexpr float kGameAspect = 16.0f / 9.0f;
// What this file may find there: the game's 16:9, or a wider value this run (or the run a save state came from)
// wrote. Anything else is not the camera's aspect (game.rel not loaded yet, another revision): left alone.
constexpr float kLowest = kGameAspect - 0.001f;
constexpr float kHighest = 6.0f;

// The game's camera objects: a manager at 0x80C92810 (rel bss; rel_807E60CC, rel_807E69AC and the other camera code
// address it) holds them in its first four words (one in use in every state looked at: 0x815968E0). An object starts
// with its g3d Camera (the CameraData pointer) and keeps fovy / near / far / aspect at +156 / +160 / +164 / +168,
// copied from the defaults beside kViewAspect when it is made; rel_807E522C passes them to SetPerspective every frame.
constexpr uint32_t kCameraSlots = 0x80C92810u;
constexpr int kCameraSlotCount = 4;
constexpr uint32_t kObjAspect = 168;
constexpr uint32_t kCameraDataSize = 0x10C;

// nw4r::g3d::CameraData (the offsets SetPerspective 0x801CC450 and SetViewport 0x801CC510 write).
constexpr uint32_t kCamProjMtx = 48;     // Mtx44 the camera caches for GXSetProjection
constexpr uint32_t kCamProjType = 168;   // GX_PERSPECTIVE = 0
constexpr uint32_t kCamAspect = 176;
constexpr uint32_t kCamViewportW = 228;  // in EFB pixels (640 x 480 for the scene)
constexpr uint32_t kCamViewportH = 232;

std::atomic<float> g_gameAspect{0.0f};
// The aspect aurora widens the frame to (0: the game's own 16:9), as DisplaySettings passes it.
float g_wideAspect = 0.0f;

// HUD layouts (OnProjection / OnPositionMatrix; game thread only). A layout can be drawn several times a frame (the
// map draws one icon layout at every marker), so a draw is keyed by the layout and its count within the frame. Pane
// positions in layout units per draw, this frame and the last; the side each draw was put on (-1 left, 0 where the
// game put it, 1 right).
constexpr uint32_t kLayoutWidth = 0x18;  // nw4r::lyt::Layout mLayoutSize (854 x 480 for the HUD)
struct PaneSpread {
    float minX = FLT_MAX;
    float maxX = -FLT_MAX;
    float minY = FLT_MAX;
    float maxY = -FLT_MAX;
    bool symmetric = false;  // every pane has a mirror twin (x -> -x, same row)
    bool Empty() const { return minX > maxX; }
    float Spread() const { return Empty() ? 0.0f : maxX - minX; }
};
using DrawKey = uint64_t;  // count within the frame << 32 | Layout
std::unordered_map<DrawKey, PaneSpread> g_spreadThisFrame;
std::unordered_map<DrawKey, PaneSpread> g_spreadLastFrame;
std::unordered_map<uint32_t, uint32_t> g_drawsThisFrame;
std::unordered_map<DrawKey, int> g_anchor;  // a draw missing here has not been placed yet
struct Placement {
    uint32_t lastFrame = 0;  // the frame (g_frameNo) it was last drawn in
    int settled = 0;         // frames placed in a row; from kSettleFrames on the side stays
};
constexpr int kSettleFrames = 10;
std::unordered_map<DrawKey, Placement> g_placement;
// Where a layout goes: -1 / 1 the left / right edge (static HUD: energy, missiles, the map), kFollow its own place on
// the wide view (the first-person cursor and anything the game positions over the 3D view: the game converts with
// the wide camera aspect, rel_807E6E28 / 807E6E58, so a cursor at +390 of +-427 aims at 390/427 of the screen's half
// width), kKeep the narrowed 16:9 placement (menus, layouts spread across the screen).
constexpr int kKeep = 0;
constexpr int kFollow = 2;
// A layout whose panes reach this far out (of the half width) belongs to that edge.
constexpr float kEdgeZone = 0.8f;
DrawKey g_drawing = 0;  // 0: no layout drawing
// While a layout is drawn stretched over the wide screen (the visor, the controller notice's background), each pane's
// position matrix is widened by this factor (1 / aurora's narrowing), so aurora's narrowing cancels whatever the
// screen's shape. 0: not stretched. g_noticePanes: the controller notice, stretched pane by pane (NoticeKeepsShape).
float g_stretch = 0.0f;
bool g_noticePanes = false;
// The visor in a menu (the pause map and its pages) is stretched as in play. The BACK / SELECT badges in its lower
// corners are panes of one of its layouts (after the full-screen `back_color`: `button_1`, `button_2`,
// `button_*_flash`, `back`, `select`); they move with the frame's corner they sit in and keep their shape. Each
// side's group moves by what its label (`back` left, `select` right) moves, from that label's last place, so the icon
// and the label stay together and a flash pane coming and going does not shift them. That layout is placed this way
// from its first frame (`back_color` drawn first), before it has counted as the visor for kVisorRun frames: the
// badges appear as the pause screen's zoom ends and would otherwise jump out three frames later.
bool g_visorMenu = false;
bool g_backdropLayout = false;  // the layout being drawn began with `back_color` while the visor shows in a menu
uint32_t g_lastVisorFrame = 0;  // the frame a visor layout was last drawn in
float g_badgeLabelX[2] = {};    // `back` / `select` x (left / right) as last drawn
bool g_badgeLabelSeen[2] = {};
// In a menu over the visor, a pane as wide as its layout (the pause screen's scan sweep: `scan_line`, `white_line`,
// 854 wide) is a full-screen band: it is stretched with the visor. 0: not stretching bands.
float g_bandStretch = 0.0f;
float g_layoutWidth = 0.0f;  // of the layout being drawn (854 for the HUD's)
// Layouts told apart by their panes' names (read once, on a layout's first drawn frame). The controller notice
// ("hold the Wii Remote horizontally", at boot): one layout with the picture (`image`) in a frame in the middle, side
// walls (`plate_02_*` at +-427, `grid_*`), connectors (`frame_03_*`) and the text box (`frame_04_*`; its text is the
// game's 640x480 text layer, which keeps its shape anyway). Its animated triangles make it mirror-symmetric only on
// some frames, so it is neither the visor nor a menu.
enum class LayoutKind : uint8_t { Unknown, Other, Notice };
std::unordered_map<uint32_t, LayoutKind> g_layoutKind;
std::unordered_map<uint32_t, int> g_kindChecks;  // frames a layout's names were looked at
std::unordered_map<uint32_t, int> g_noticeSigns; // the notice's pane names seen so far (bits)
LayoutKind g_drawingKind = LayoutKind::Unknown;
std::vector<std::string> g_classNames;  // the panes' names of the layout being drawn while its kind is unknown
std::vector<std::pair<float, float>> g_panes;  // the pane positions of the layout being drawn
// Samus's visor (first person, the story recap, the pause map's frame): wide layouts whose panes come in mirror
// pairs (the frame's arcs at y 270 / -260 out to +-447, the side rails at +-227..+-380: four rail and five frame
// layouts in first person). The HUD's panel, the reticle and menu labels are not both wide and symmetric.
constexpr float kVisorSpread = 1.2f;  // of the half width
// One of the game's menus is up: the B / BACK legend's badge was drawn (virtual_remote.cpp's $menu; only with a
// controller's pictures in use), or a wide layout that is not the visor (the pause map's labels: -444..441 of
// +-427). Then nothing moves to the edges and the visor keeps its shape, so the map's markers stay on the map and
// its labels on the frame; the HUD layouts themselves spread to about 1.0.
constexpr float kMenuSpread = 1.5f;
// A menu holds for half a second after its last sign, so a screen whose layouts change from frame to frame does not
// flicker between its menu and play placements.
constexpr uint32_t kMenuHold = 30;
uint32_t g_menuUntil = 0;
bool g_menuLegend = false;
int g_anchorLogs = 0;
uint32_t g_frameNo = 0;

bool MenuUp() {
    return g_menuLegend || (g_menuUntil != 0 && g_frameNo <= g_menuUntil);
}

// The visor's layouts are mirror-symmetric on every frame; a wide layout seen asymmetric even once is not taken for
// it for two seconds after, and a new one counts from its third symmetric frame.
struct VisorTrack {
    int symmetricRun = 0;
    uint32_t lastAsymmetric = 0;
    uint32_t lastFrame = 0;  // the frame it was last drawn in
    int visorFrames = 0;     // frames in a row it has counted as the visor
    bool visor = false;      // as of its last draw
};
constexpr int kVisorRun = 3;
constexpr uint32_t kAsymmetricMemory = 120;
// Once a layout has been the visor for half a second it stays the visor while it is drawn without a break: its
// animations (the side rails tilt against each other as Samus turns: -326,-8 / 326,8 in the developer's 23:09 log)
// may not take it for a menu.
constexpr int kVisorSticky = 30;
std::unordered_map<DrawKey, VisorTrack> g_visorTrack;

// Mirror-symmetric: at least 80% of the panes off the middle line have a twin (x -> -x, same row). The visor's
// layouts are 100% (also while it zooms in), the pause map's labels 68%, the file select's 0-29%; the share leaves
// room for a pane or two caught mid-animation.
bool Symmetric(const std::vector<std::pair<float, float>>& panes) {
    if (panes.size() < 2) return false;
    int offCentre = 0, twinned = 0;
    for (const auto& [x, y] : panes) {
        if (std::fabs(x) < 4.0f) continue;  // on the middle line: its own twin
        ++offCentre;
        for (const auto& [x2, y2] : panes) {
            // The same row (x -> -x), or point-mirrored (x, y -> -x, -y): the side rails tilt against each other.
            if (std::fabs(x + x2) < 4.0f && (std::fabs(y - y2) < 8.0f || std::fabs(y + y2) < 8.0f)) {
                ++twinned;
                break;
            }
        }
    }
    return offCentre >= 2 && twinned * 5 >= offCentre * 4;
}

// nw4r::lyt::Pane::LoadMtx (0x801DB360) hands GXLoadPosMtxImm the pane's global matrix at +0x84, so that address
// minus 0x84 is the pane when its first word is one of the layout library's vtables (main.dol data around the Pane
// vtable 0x80249720). Its name (16 characters) is at +0xBC. Other callers pass a matrix on the stack.
constexpr uint32_t kPaneGlobalMtx = 0x84;
constexpr uint32_t kPaneName = 0xBC;
uint32_t PaneAt(uint32_t matrix) {
    if (matrix < 0x80000000u + kPaneGlobalMtx || (matrix & 3u) != 0) return 0;
    const uint32_t pane = matrix - kPaneGlobalMtx;
    if (!Memory::Contains(pane, kPaneName + 16)) return 0;
    const uint32_t vtable = Memory::Read32(pane);
    return vtable >= 0x80240000u && vtable < 0x80250000u ? pane : 0;
}
std::string PaneName(uint32_t pane) {
    std::string name;
    for (uint32_t i = 0; i < 16; ++i) {
        const char c = static_cast<char>(Memory::Read8(pane + kPaneName + i));
        if (c == 0) break;
        name += (c >= 32 && c < 127) ? c : '?';
    }
    return name;
}

bool IsVisor(DrawKey key) {
    const auto it = g_visorTrack.find(key);
    return it != g_visorTrack.end() && it->second.visor;
}

bool VisorShowing() {
    return g_lastVisorFrame != 0 && g_frameNo - g_lastVisorFrame <= 1;
}

// The visor's corner badges (see g_visorMenu); `back_00`.. are the frame's own panes, so `back` is matched whole.
bool VisorBadge(const std::string& name) {
    return name == "back" || name == "select" || name.rfind("button_", 0) == 0;
}

// The controller notice's panes that keep their shape and place in the middle: the picture, its frame and shadows,
// and the text box. The rest (backdrop, side walls, grids, connectors) is stretched over the screen.
bool NoticeKeepsShape(const std::string& name) {
    static const char* const kKept[] = {"image",    "shadow_",  "plate_00", "plate_01", "plate_flash",
                                        "frame_00", "frame_01", "frame_02", "frame_04", "line_01",
                                        "line_03",  "line_04",  "line_05",  "triangle_"};
    for (const char* prefix : kKept) {
        if (name.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

// OTHERM_HUD_DUMP=<frame>[,<frame>...] (bring-up): every layout draw of those frames (counted at Apply) with its
// pane positions, for working out which layouts make up a screen.
bool g_dumpFrame = false;
std::vector<std::string> g_dumpPaneNames;  // per pane of the layout being drawn, in dump frames: "vtable:name"
bool DumpFrame(uint32_t frame) {
    static const std::vector<uint32_t> frames = [] {
        std::vector<uint32_t> out;
        if (const char* v = std::getenv("OTHERM_HUD_DUMP")) {
            for (const char* p = v; *p != 0;) {
                char* end = nullptr;
                const unsigned long f = std::strtoul(p, &end, 10);
                if (end == p) break;
                out.push_back(static_cast<uint32_t>(f));
                p = *end == ',' ? end + 1 : end;
            }
        }
        return out;
    }();
    return std::find(frames.begin(), frames.end(), frame) != frames.end();
}

// A layout that sits inside an edge layout's panes follows it: the missile numbers are their own small layout near
// the middle, inside the energy panel's frame (which spans -427..8).
int ContainerAnchor(DrawKey key, const PaneSpread& own) {
    constexpr float kMargin = 24.0f;
    for (const auto& [other, spread] : g_spreadLastFrame) {
        if (other == key || spread.Empty()) {
            continue;
        }
        const auto anchor = g_anchor.find(other);
        if (anchor == g_anchor.end() || (anchor->second != -1 && anchor->second != 1)) {
            continue;
        }
        if (own.minX >= spread.minX - kMargin && own.maxX <= spread.maxX + kMargin &&
            own.minY >= spread.minY - kMargin && own.maxY <= spread.maxY + kMargin) {
            return anchor->second;
        }
    }
    return 0;
}

// The layout being drawn is complete (another projection, or the frame's end): a menu's full-screen layout shows
// itself here.
void FinishLayoutDraw() {
    if (g_drawing != 0) {
        auto& done = g_spreadThisFrame[g_drawing];
        done.symmetric = Symmetric(g_panes);
        const uint32_t layout = static_cast<uint32_t>(g_drawing);
        if (g_drawingKind == LayoutKind::Unknown && !g_classNames.empty()) {
            // A layout fading in draws only some panes at a time: the notice's three are gathered over its frames,
            // and a layout is looked at for ten seconds before it counts as something else.
            int& signs = g_noticeSigns[layout];
            for (const auto& name : g_classNames) {
                signs |= name == "plate_02_LT" ? 1 : name == "frame_04_back" ? 2 : name == "image" ? 4 : 0;
            }
            const bool notice = signs == 7;
            if (notice || ++g_kindChecks[layout] >= 600) {
                g_drawingKind = notice ? LayoutKind::Notice : LayoutKind::Other;
                g_layoutKind[layout] = g_drawingKind;
            }
            if (g_drawingKind == LayoutKind::Notice) {
                RT_LOGF(RT_TAG_VI, "[hud] layout %08X is the controller notice: its backdrop is stretched\n", layout);
            } else if (g_drawingKind == LayoutKind::Other && std::getenv("OTHERM_HUD_KINDS") != nullptr) {
                std::string names;
                for (size_t i = 0; i < g_classNames.size() && i < 8; ++i) names += " " + g_classNames[i];
                RT_LOGF(RT_TAG_VI, "[hud] layout %08X: %zu panes named%s\n", layout, g_classNames.size(), names.c_str());
            }
        }
        auto& track = g_visorTrack[g_drawing];
        const bool continuing = track.lastFrame != 0 && track.lastFrame + 1 == g_frameNo;
        track.lastFrame = g_frameNo;
        if (!continuing) {
            track.visorFrames = 0;
        }
        if (done.Spread() > kVisorSpread * 427.0f && g_drawingKind != LayoutKind::Notice) {
            if (done.symmetric) {
                ++track.symmetricRun;
            } else {
                track.symmetricRun = 0;
                track.lastAsymmetric = g_frameNo;
            }
        } else {
            track.symmetricRun = 0;
        }
        track.visor = g_drawingKind != LayoutKind::Notice &&
                      ((track.symmetricRun >= kVisorRun &&
                        (track.lastAsymmetric == 0 || g_frameNo - track.lastAsymmetric > kAsymmetricMemory)) ||
                       (continuing && track.visorFrames >= kVisorSticky));
        track.visorFrames = track.visor ? track.visorFrames + 1 : 0;
        const bool visor = track.visor;
        // A menu shows itself by a wide layout that is not mirror-symmetric; a symmetric one is the visor (or becomes
        // it after kVisorRun frames: not a menu while it zooms in).
        // Menus' layouts have many panes (16-137 seen); a small one (the visor's 4-pane side rails) never counts.
        if (!visor && !done.symmetric && g_drawingKind != LayoutKind::Notice && g_panes.size() >= 6 &&
            done.Spread() > kMenuSpread * 427.0f) {
            if (!MenuUp()) {
                static int logs = 0;
                if (logs < 32) {
                    ++logs;
                    std::string panes;
                    for (size_t i = 0; i < g_panes.size() && i < 16; ++i) {
                        char b[32];
                        std::snprintf(b, sizeof(b), " %.0f,%.0f", g_panes[i].first, g_panes[i].second);
                        panes += b;
                    }
                    RT_LOGF(RT_TAG_VI, "[hud] a menu is up: layout %08X #%u spreads x %.0f..%.0f%s, panes%s\n",
                            static_cast<uint32_t>(g_drawing), static_cast<uint32_t>(g_drawing >> 32), done.minX,
                            done.maxX, done.symmetric ? "" : " (not mirrored)", panes.c_str());
                }
            }
            g_menuUntil = g_frameNo + kMenuHold;
        }
        if (g_dumpFrame) {
            std::string panes;
            for (size_t i = 0; i < g_panes.size() && i < 64; ++i) {
                char b[32];
                std::snprintf(b, sizeof(b), " %.0f,%.0f", g_panes[i].first, g_panes[i].second);
                panes += b;
                if (i < g_dumpPaneNames.size() && !g_dumpPaneNames[i].empty()) panes += "[" + g_dumpPaneNames[i] + "]";
            }
            const auto a = g_anchor.find(g_drawing);
            RT_LOGF(RT_TAG_VI, "[huddump] f%u layout %08X #%u side %d%s%s%s panes %zu:%s\n", g_frameNo,
                    static_cast<uint32_t>(g_drawing), static_cast<uint32_t>(g_drawing >> 32),
                    a != g_anchor.end() ? a->second : 0, done.symmetric ? " symmetric" : "", visor ? " visor" : "",
                    MenuUp() ? " menu" : "", g_panes.size(), panes.c_str());
        }
    }
    g_panes.clear();
    g_dumpPaneNames.clear();
    g_classNames.clear();
    g_drawing = 0;
    g_drawingKind = LayoutKind::Unknown;
    g_stretch = 0.0f;
    g_noticePanes = false;
    g_visorMenu = false;
    g_backdropLayout = false;
    g_bandStretch = 0.0f;
}

// The edge a layout's panes reach into (the outer part of the 16:9 frame), 0 for none.
int EdgeSide(const PaneSpread& s, float halfWidth) {
    if (s.minX <= -kEdgeZone * halfWidth) return -1;  // the energy panel's frame (-427..8)
    if (s.maxX >= kEdgeZone * halfWidth) return 1;    // the map's frame (to 361)
    return 0;
}

// The HUD map moves as one, to the side its frame (map_01) goes to: its plate, mask, area maps, markers and Samus's
// marker are layouts of their own, and the area map's panes (its rooms, most of them masked off) move with Samus, so
// placed by its own panes it could follow the view, stay or go left while the frame went right (the developer's
// 20:51 recording: A_32 at 241..262, y 54..243, reached past the frame's 46..201 and followed the view, the rooms
// about 80 px at 1080p left of Samus's marker). 0: not a map layout, or the frame's side is not known.
int HudMapSide(DrawKey key, float halfWidth) {
    if (!OthermMinimap::IsHudMapLayout(static_cast<uint32_t>(key))) return 0;
    const DrawKey frame = OthermMinimap::HudMapFrameLayout();  // drawn once a frame: count 0
    if (frame == 0) return 0;
    const auto drawn = g_spreadLastFrame.find(frame);
    if (drawn != g_spreadLastFrame.end() && !drawn->second.Empty()) {
        if (const int side = EdgeSide(drawn->second, halfWidth); side != 0) return side;
    }
    const auto placed = g_anchor.find(frame);  // not drawn last frame (fading in or out): the side it had
    return placed != g_anchor.end() && (placed->second == -1 || placed->second == 1) ? placed->second : 0;
}

// The side each layout was last logged on in play (menus are one `a menu is up` line, not one per layout), so the
// log keeps every change of place without filling with each pause's coming and going.
std::unordered_map<DrawKey, int> g_loggedSide;

int DecideAnchor(DrawKey key, float halfWidth) {
    // A layout is placed over its first frames, then keeps that side while it is drawn: the first-person cursor
    // starts in the middle and must not be sent to an edge when it is moved there (it marks where Samus aims). A
    // layout that was not drawn last frame, or a menu, places it again.
    Placement& p = g_placement[key];
    const bool continuing = p.lastFrame + 1 == g_frameNo && p.lastFrame != 0;
    p.lastFrame = g_frameNo;
    if (!continuing || MenuUp()) {
        p.settled = 0;
    }
    const auto placed = g_anchor.find(key);
    const int anchor = placed != g_anchor.end() ? placed->second : 0;
    const auto it = g_spreadLastFrame.find(key);
    const bool drawnLastFrame = it != g_spreadLastFrame.end() && !it->second.Empty();
    const int mapSide = MenuUp() ? 0 : HudMapSide(key, halfWidth);
    int next = kKeep;
    if (mapSide != 0) {
        next = mapSide;  // with the HUD map's frame, every frame
    } else {
        if (continuing && !MenuUp() && p.settled >= kSettleFrames && placed != g_anchor.end()) {
            return anchor;
        }
        if (!drawnLastFrame) {
            return anchor;  // nothing drawn last frame (paused, hidden, just made): keep the side it had
        }
        ++p.settled;
        const PaneSpread& own = it->second;
        const float spread = (own.maxX - own.minX) / halfWidth;
        if (MenuUp() || spread > 1.2f) {
            next = kKeep;  // a menu is up, or panes across most of the screen: kept whole
        } else if (const int side = EdgeSide(own, halfWidth); side != 0) {
            next = side;  // reaches into the outer part of the 16:9 frame
        } else if (const int container = ContainerAnchor(key, own); container != 0) {
            next = container;  // inside an edge layout: the missile numbers
        } else {
            next = kFollow;  // the cursor, markers, hints: where they are on the wide view
        }
    }
    if (placed == g_anchor.end() || next != anchor) {
        g_anchor[key] = next;
    }
    if (!MenuUp()) {
        const auto logged = g_loggedSide.find(key);
        if ((logged == g_loggedSide.end() || logged->second != next) && g_anchorLogs < 256) {
            ++g_anchorLogs;
            g_loggedSide[key] = next;
            char panes[96] = "not drawn last frame";
            if (drawnLastFrame) {
                const PaneSpread& own = it->second;
                std::snprintf(panes, sizeof(panes), "panes at x %.0f..%.0f, y %.0f..%.0f of +-%.0f", own.minX,
                              own.maxX, own.minY, own.maxY, halfWidth);
            }
            RT_LOGF(RT_TAG_VI, "[hud] layout %08X #%u %s%s (%s)\n", static_cast<uint32_t>(key),
                    static_cast<uint32_t>(key >> 32),
                    next == -1 ? "to the left edge" : next == 1 ? "to the right edge"
                    : next == kFollow ? "follows the view" : "stays as drawn",
                    mapSide != 0 ? " with the HUD map" : "", panes);
        }
    }
    return next;
}

// Wider-than-16:9 aspects the port put in the game's memory: this run's, and one a loaded state brought (seen in the
// float at kViewAspect). A camera holding one of them, or 16:9, is the port's to change; others are the game's own.
// Game thread only (frame boundaries and GX calls).
float g_portAspects[8] = {};
int g_portAspectCount = 0;

void RememberPortAspect(float aspect) {
    if (aspect <= kGameAspect + 0.001f) {
        return;
    }
    for (int i = 0; i < g_portAspectCount; ++i) {
        if (std::fabs(g_portAspects[i] - aspect) < 1e-4f) {
            return;
        }
    }
    g_portAspects[g_portAspectCount < 8 ? g_portAspectCount++ : 7] = aspect;
}

bool PortManaged(float aspect) {
    if (std::fabs(aspect - kGameAspect) < 1e-3f) {
        return true;
    }
    for (int i = 0; i < g_portAspectCount; ++i) {
        if (std::fabs(g_portAspects[i] - aspect) < 1e-4f) {
            return true;
        }
    }
    return false;
}

}  // namespace

void Apply(float aspect) {
    g_wideAspect = aspect;
    FinishLayoutDraw();
    g_dumpFrame = DumpFrame(++g_frameNo);
    g_spreadLastFrame.swap(g_spreadThisFrame);
    g_spreadThisFrame.clear();
    g_drawsThisFrame.clear();
    g_menuLegend = aurora_button_icon_marker(7);
    if (Memory::Read32(kModuleList) == 0) {
        return;
    }
    const float current = Memory::ReadFloat32(kViewAspect);
    if (!(current >= kLowest && current <= kHighest)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            RT_LOGF(RT_TAG_VI, "display: the camera aspect at %08X reads %g, not 16:9; the 3D view keeps the game's "
                    "own width\n", kViewAspect, current);
        }
        g_gameAspect.store(0.0f, std::memory_order_relaxed);
        return;
    }
    RememberPortAspect(current);  // a state saved with another width brings it here
    const float want = aspect > kGameAspect ? std::fmin(aspect, kHighest) : kGameAspect;
    if (std::fabs(current - want) > 1e-5f) {
        Memory::WriteFloat32(kViewAspect, want);
        RememberPortAspect(want);
        RT_LOGF(RT_TAG_VI, "display: 3D view aspect %.3f (was %.3f)%s\n", want, current,
                want > kGameAspect ? ", wider than 16:9: more to the sides" : ", the game's own 16:9");
    }
    g_gameAspect.store(want, std::memory_order_relaxed);
    // Camera objects made before the change (or brought by a state) still hold the old aspect.
    for (int i = 0; i < kCameraSlotCount; ++i) {
        const uint32_t obj = Memory::Read32(kCameraSlots + 4u * i);
        if (obj == 0 || (obj & 3u) != 0 || !Memory::Contains(obj, kObjAspect + 4)) {
            continue;
        }
        const uint32_t cameraData = Memory::Read32(obj);
        if ((cameraData & 3u) != 0 || !Memory::Contains(cameraData, kCameraDataSize)) {
            continue;
        }
        const float held = Memory::ReadFloat32(obj + kObjAspect);
        if (std::fabs(held - want) > 1e-5f && PortManaged(held)) {
            Memory::WriteFloat32(obj + kObjAspect, want);
            RT_LOGF(RT_TAG_VI, "display: camera object %08X aspect %.3f -> %.3f\n", obj, held, want);
        }
    }
}

float GameAspect() {
    return g_gameAspect.load(std::memory_order_relaxed);
}

void OnProjection(uint32_t lr, uint32_t layout, float m[16], bool orthographic, float viewportW, float viewportH) {
    FinishLayoutDraw();
    if ((lr != kLayoutOrthoReturn && lr != kLayoutPerspectiveReturn) || layout == 0) {
        return;
    }
    const DrawKey key = static_cast<DrawKey>(g_drawsThisFrame[layout]++) << 32 | layout;
    g_drawing = key;
    const auto kind = g_layoutKind.find(layout);
    g_drawingKind = kind != g_layoutKind.end() ? kind->second : LayoutKind::Unknown;
    g_spreadThisFrame.try_emplace(key);
    const float wide = g_wideAspect;
    if (wide <= kGameAspect + 0.001f || m[0] == 0.0f || m[5] == 0.0f || viewportW < 600.0f || viewportH < 440.0f) {
        return;
    }
    // How much aurora narrows this projection (command_processor.cpp apply_xf_projection, AuroraSetWideGameNative).
    const float shape = std::fabs(m[5] / m[0]);
    float widen = 0.0f;
    if (shape > 1.68f && shape < 1.88f) {
        widen = shape / wide;
    } else if (orthographic && shape > 1.30f && shape < 1.37f) {
        widen = kGameAspect / wide;
    }
    if (!(widen > 0.0f && widen < 1.0f)) {
        return;
    }
    float halfWidth = Memory::Contains(layout + kLayoutWidth, 4) ? Memory::ReadFloat32(layout + kLayoutWidth) * 0.5f
                                                                 : 0.0f;
    if (!(halfWidth > 1.0f && halfWidth < 10000.0f)) {
        halfWidth = orthographic ? 1.0f / std::fabs(m[0]) : 427.0f;
    }
    if (g_drawingKind == LayoutKind::Notice) {
        if (orthographic) {
            g_stretch = 1.0f / widen;  // its backdrop panes are widened as they load (OnPositionMatrix)
            g_noticePanes = true;
        }
        return;
    }
    if (IsVisor(key)) {
        // The visor fills the wide screen, stretched as the Wii stretches it to 16:9 (its frame is one curve in
        // many panes, so moving them apart would open gaps): aurora's narrowing is undone. In a menu too (the pause
        // map): its corner badges move with the frame (g_visorMenu), the menu's own layouts keep their places.
        g_lastVisorFrame = g_frameNo;
        if (orthographic) {
            g_stretch = 1.0f / widen;  // its panes are widened as they load (OnPositionMatrix)
            g_visorMenu = MenuUp();
        }
        return;
    }
    if (orthographic && MenuUp() && VisorShowing()) {
        g_bandStretch = 1.0f / widen;
        g_layoutWidth = 2.0f * halfWidth;
    }
    const int anchor = DecideAnchor(key, halfWidth);
    float d = 0.0f;
    if (anchor == -1 || anchor == 1) {
        // The 16:9 frame's edge (at widen on the screen) moves to the screen's edge: widen * d = 1 - widen.
        d = static_cast<float>(anchor) * (1.0f / widen - 1.0f);
    } else if (anchor == kFollow && orthographic) {
        // The layout's middle (last frame's panes) lands where it is on the full width, its shape kept: a point at
        // c in the game's NDC is drawn at widen * (c + d) = c.
        const auto last = g_spreadLastFrame.find(key);
        if (last == g_spreadLastFrame.end() || last->second.Empty()) {
            return;
        }
        const float c = m[0] * 0.5f * (last->second.minX + last->second.maxX) + m[3];
        d = c * (1.0f / widen - 1.0f);
    }
    if (d == 0.0f) {
        return;
    }
    if (orthographic) {
        m[3] += d;  // x_ndc = m0 x + m3
    } else {
        m[2] -= d;  // x_ndc = (m0 x + m2 z) / -z
    }
}

void OnPositionMatrix(float m[12], uint32_t matrixAddress) {
    if (g_drawing == 0) {
        return;
    }
    const float x = m[3], y = m[7];
    if (g_panes.size() < 256) g_panes.emplace_back(x, y);
    const bool wantName = g_drawingKind == LayoutKind::Unknown || g_noticePanes || g_visorMenu || g_bandStretch > 0.0f;
    const uint32_t pane = (g_dumpFrame || wantName || g_bandStretch > 0.0f) ? PaneAt(matrixAddress) : 0;
    const std::string name = pane != 0 && wantName ? PaneName(pane) : std::string();
    if (g_drawingKind == LayoutKind::Unknown && !name.empty() && g_classNames.size() < 128) {
        g_classNames.push_back(name);
    }
    if (g_dumpFrame && g_dumpPaneNames.size() < 256) {
        char b[16];
        std::snprintf(b, sizeof(b), "%08X:", pane != 0 ? Memory::Read32(pane) : 0u);
        char size[48] = {};
        if (pane != 0) {
            std::snprintf(size, sizeof(size), " %.0fx%.0f*%.2f", Memory::ReadFloat32(pane + 0x4C),
                          Memory::ReadFloat32(pane + 0x50), Memory::ReadFloat32(pane + 0x44));
        }
        g_dumpPaneNames.push_back(pane != 0 ? b + PaneName(pane) + size : std::string());
    }
    auto& spread = g_spreadThisFrame[g_drawing];
    if (x == x && y == y && std::fabs(x) < 100000.0f && std::fabs(y) < 100000.0f) {
        spread.minX = std::fmin(spread.minX, x);
        spread.maxX = std::fmax(spread.maxX, x);
        spread.minY = std::fmin(spread.minY, y);
        spread.maxY = std::fmax(spread.maxY, y);
    }
    if (g_bandStretch > 0.0f && name == "back_color") {
        g_backdropLayout = true;
    }
    const float badgeStretch = g_visorMenu ? g_stretch : g_backdropLayout ? g_bandStretch : 0.0f;
    if (badgeStretch > 0.0f && VisorBadge(name)) {
        // Moved with the frame's corner, shape kept: the group follows its label's stretched place.
        const int side = x < 0.0f ? 0 : 1;
        if (name == (side == 0 ? "back" : "select")) {
            g_badgeLabelX[side] = x;
            g_badgeLabelSeen[side] = true;
        }
        const float label = g_badgeLabelSeen[side] ? g_badgeLabelX[side] : x;
        m[3] += (badgeStretch - 1.0f) * label;
        return;
    }
    float stretch = g_stretch > 0.0f && !(g_noticePanes && NoticeKeepsShape(name)) ? g_stretch : 0.0f;
    if (stretch == 0.0f && g_bandStretch > 0.0f && pane != 0 &&
        std::fabs(Memory::ReadFloat32(pane + 0x4C) * Memory::ReadFloat32(pane + 0x44)) >= g_layoutWidth - 1.0f) {
        stretch = g_bandStretch;  // a full-width band over the visor
    }
    if (stretch > 0.0f) {
        m[0] *= stretch;
        m[1] *= stretch;
        m[2] *= stretch;
        m[3] *= stretch;
    }
}

void OnCameraProjection(uint32_t matrix) {
    const float want = g_gameAspect.load(std::memory_order_relaxed);
    const uint32_t cam = matrix - kCamProjMtx;
    if (want <= 0.0f || matrix < 0x80000000u + kCamProjMtx || !Memory::Contains(cam, kCamViewportH + 4) ||
        Memory::Read32(cam + kCamProjType) != 0) {
        return;
    }
    const float aspect = Memory::ReadFloat32(cam + kCamAspect);
    if (std::fabs(aspect - want) < 1e-5f || !PortManaged(aspect)) {
        return;
    }
    // Render-to-texture cameras (smaller viewports) keep the shape of what they draw into.
    if (Memory::ReadFloat32(cam + kCamViewportW) < 600.0f || Memory::ReadFloat32(cam + kCamViewportH) < 400.0f) {
        return;
    }
    // C_MTXPerspective: m[0][0] = cot(fovy / 2) / aspect, m[1][1] = cot(fovy / 2), m[0][2] = 0.
    const float m11 = Memory::ReadFloat32(matrix + 20);
    Memory::WriteFloat32(cam + kCamAspect, want);
    Memory::WriteFloat32(matrix, m11 / want);
    static uint32_t logged = 0;
    if (logged < 16) {
        ++logged;
        RT_LOGF(RT_TAG_VI, "display: camera %08X aspect %.3f -> %.3f (set up before the change)\n", cam, aspect, want);
    }
}

}  // namespace OthermWidescreen
