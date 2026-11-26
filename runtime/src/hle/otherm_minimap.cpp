// The HUD map always facing north (Options > Cheats, Config.toml [cheats] minimap_north_up), so it lines up with the
// pause screen's map. NOTES.md 2026-11-20.
//
// Other M's HUD is a set of elements (one nw4r::lyt layout each), kept in the table at 0x80C80DC0 (index * 4). An
// element E holds its layout at E+0, the layout's root pane at E+0x60, its lyt::DrawInfo at E+0x64, its name at E+0x34
// (a length byte, then the text) and where to draw it at E+0xB8: x, y (854 x 480 layout units, y up), the angle in
// radians at E+0xC0 and the scale at E+0xC4. rel_80638CAC(E) draws it: it copies those into the root pane (translate,
// rotate z in degrees, scale) and runs the layout's Animate / CalculateMtx / Draw. An element drawn at several places
// (the map's door and item markers) is drawn by its own loop that sets E+0xB8.. before each rel_80638CAC call.
//
// The HUD map (the update rel_80A61488 gets &table[0xA2] as `this`): table[0xA3] / [0xA4] are area maps (class vtable
// 0x80C71928, named after the area's layout, "A_12"), table[0xA5] is Samus ("map_samus", vtable 0x80BBF6B8, drawn at
// the map's middle), then the doors, items, destination, elevators and the like up to [0xB1]; [0xB2]-[0xB7] are the
// plate, its frame, the mask, the scan ring and two overlays that never turn. The game turns the whole map with the
// view: the area map's angle (developer's 08:24 state: -1.5649 rad, Samus's marker the same) is the map's turn. North
// up undoes that turn for every map element as it is drawn: its place is turned back about the map's middle and the
// map's turn is taken off its angle (icons the game keeps upright, angle 0, stay upright), so the map, the markers on
// it and Samus's heading keep their places relative to each other, the way the pause screen's map shows them.
#include "otherm_minimap.h"

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

namespace {

constexpr uint32_t kElementTable = 0x80C80DC0u;
constexpr uint32_t kElementSlots = 0x140u;  // up to the table's end (0x80C812C0, the first non-pointer words)
constexpr uint32_t kMapFirst = 0xA2u, kMapLast = 0xB1u;  // the elements that turn with the HUD map
constexpr uint32_t kAreaMapVtable = 0x80C71928u;
constexpr uint32_t kSamusMarker = 0xA5u;
constexpr uint32_t kSamusMarkerVtable = 0x80BBF6B8u;
constexpr uint32_t kDrawConstants = 0x80B07EB8u;  // +0x14 the root's z (-1), +0x18 degrees per radian, +0x1C 0.0

std::atomic<int> g_northUp{-1};  // -1: not read from Config.toml yet

bool GuestObject(uint32_t value) {
    const bool mem1 = value >= 0x80000000u && value < 0x81800000u;
    const bool mem2 = value >= 0x90000000u && value < 0x94000000u;
    return (mem1 || mem2) && (value & 3u) == 0;
}

uint32_t Element(uint32_t index) {
    uint32_t e = 0;
    return Memory::TryRead32(kElementTable + 4u * index, e) && GuestObject(e) ? e : 0;
}

std::string ElementName(uint32_t e) {
    std::string name;
    const uint8_t length = Memory::Read8(e + 0x34u);
    for (uint32_t i = 0; i < length && i < 11u; ++i) {
        const char c = static_cast<char>(Memory::Read8(e + 0x35u + i));
        name += c >= 32 && c < 127 ? c : '?';
    }
    return name;
}

// The table, read again once a presented frame. An element can be in it more than once (the HUD map's are at
// 0x05-0x0D too).
const uint32_t* Elements() {
    static uint32_t s_frame = UINT32_MAX;
    static uint32_t s_elements[kElementSlots] = {};
    const uint32_t frame = VI_HLE_PresentedFrames();
    if (frame != s_frame) {
        s_frame = frame;
        for (uint32_t i = 0; i < kElementSlots; ++i) s_elements[i] = Element(i);
    }
    return s_elements;
}

// The area map drawn last, for where the player is (OthermMinimap::DrawnAreaMap): its layout's name and the presented
// frame it was drawn in.
std::mutex g_areaMapMutex;
std::string g_areaMap;
uint32_t g_areaMapFrame = 0;

void NoteAreaMap(uint32_t e) {
    uint32_t vtable = 0;
    if (!Memory::TryRead32(e + 0x114u, vtable) || vtable != kAreaMapVtable) return;
    std::string name = ElementName(e);
    std::lock_guard<std::mutex> lock(g_areaMapMutex);
    g_areaMap = std::move(name);
    g_areaMapFrame = VI_HLE_PresentedFrames();
}

// The element's first index in the table, -1 if it is not in it (for the log).
int ElementIndex(uint32_t e) {
    const uint32_t* elements = Elements();
    for (uint32_t i = 0; i < kElementSlots; ++i)
        if (elements[i] == e) return static_cast<int>(i);
    return -1;
}

bool OnMap(uint32_t e) {
    const uint32_t* elements = Elements();
    for (uint32_t i = kMapFirst; i <= kMapLast; ++i)
        if (elements[i] == e) return true;
    return false;
}

// The HUD map's elements as a wide screen places them: the area maps through the two overlays ([0xA3]..[0xB7]), its
// frame map_01 at [0xB3]. Not the pause screen's big map ([0xA2]) or map_scan ([0xB5]: drawn twice as the pause screen
// opens, its panes at the screen's middle, 0,-80; developer's 20:48 log). Their layouts, read again once a presented
// frame (an area change swaps the area maps).
constexpr uint32_t kHudMapFirst = 0xA3u, kHudMapLast = 0xB7u;
constexpr uint32_t kHudMapFrame = 0xB3u;
constexpr uint32_t kHudMapScan = 0xB5u;
const uint32_t* HudMapLayouts() {
    static uint32_t s_frame = UINT32_MAX;
    static uint32_t s_layouts[kHudMapLast - kHudMapFirst + 1] = {};
    const uint32_t frame = VI_HLE_PresentedFrames();
    if (frame != s_frame) {
        s_frame = frame;
        const uint32_t* elements = Elements();
        for (uint32_t i = kHudMapFirst; i <= kHudMapLast; ++i) {
            uint32_t layout = 0;
            const bool ok = elements[i] != 0 && i != kHudMapScan && Memory::TryRead32(elements[i], layout) &&
                            GuestObject(layout);
            s_layouts[i - kHudMapFirst] = ok ? layout : 0;
        }
    }
    return s_layouts;
}

// The HUD map's turn and middle: the visible area map's angle, Samus's marker's place.
bool MapTurn(float& angle, float& cx, float& cy) {
    const uint32_t samus = Element(kSamusMarker);
    uint32_t vtable = 0;
    if (samus == 0 || !Memory::TryRead32(samus + 0x114u, vtable) || vtable != kSamusMarkerVtable) return false;
    for (uint32_t index : {0xA3u, 0xA4u}) {
        const uint32_t map = Element(index);
        if (map == 0 || !Memory::TryRead32(map + 0x114u, vtable) || vtable != kAreaMapVtable) continue;
        if (Memory::Read8(map + 0xC8u) == 0) continue;
        angle = Memory::ReadFloat32(map + 0xC0u);
        cx = Memory::ReadFloat32(samus + 0xB8u);
        cy = Memory::ReadFloat32(samus + 0xBCu);
        return std::isfinite(angle) && std::isfinite(cx) && std::isfinite(cy);
    }
    return false;
}

// OTHERM_LOG_ELEMENTS=<first frame>-<last frame> (bring-up): every element drawn in those presented frames, with its
// table index, name, vtable and place, before and after the north-up turn.
bool LogFrame(uint32_t frame) {
    static const std::pair<uint32_t, uint32_t> range = [] {
        std::pair<uint32_t, uint32_t> out{1u, 0u};
        if (const char* v = std::getenv("OTHERM_LOG_ELEMENTS")) {
            char* end = nullptr;
            out.first = static_cast<uint32_t>(std::strtoul(v, &end, 10));
            out.second = end && *end == '-' ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 10)) : out.first;
        }
        return out;
    }();
    return frame >= range.first && frame <= range.second;
}

}  // namespace

namespace OthermMinimap {

bool NorthUp() {
    int v = g_northUp.load(std::memory_order_relaxed);
    if (v < 0) {
        v = RuntimeConfigFile::MinimapNorthUp(false) ? 1 : 0;
        g_northUp.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

void SetNorthUp(bool on) {
    g_northUp.store(on ? 1 : 0, std::memory_order_relaxed);
    RuntimeConfigFile::SetMinimapNorthUp(on);
    RT_LOGF(RT_TAG_RUNTIME, "[cheats] minimap north up %s\n", on ? "on" : "off");
}

bool IsHudMapLayout(uint32_t layout) {
    if (layout == 0) return false;
    const uint32_t* layouts = HudMapLayouts();
    for (uint32_t i = 0; i <= kHudMapLast - kHudMapFirst; ++i)
        if (layouts[i] == layout) return true;
    return false;
}

uint32_t HudMapFrameLayout() {
    return HudMapLayouts()[kHudMapFrame - kHudMapFirst];
}

std::string DrawnAreaMap(uint32_t withinFrames) {
    const uint32_t frame = VI_HLE_PresentedFrames();
    std::lock_guard<std::mutex> lock(g_areaMapMutex);
    if (g_areaMap.empty() || frame - g_areaMapFrame > withinFrames) return {};
    return g_areaMap;
}

}  // namespace OthermMinimap

// 0x80638CAC: void HudElement::Draw(HudElement* e), natively, with the north-up turn added. Same steps as the game's:
// nothing unless E+0xC8; root alpha = E+0x108 * E+0xCC; flag 2 in the root's +0xBB; translate (E+0xB8, E+0xBC, z);
// scale E+0x100 / E+0x104 times E+0xC4; rotate (0, 0, degrees(E+0xC0)); with an animation (E+4): the frame E+0x5C
// goes to the animation picked by E+0x24 from the tree at E+0x18 (rel_8063A3C8), or straight into E+4's frame, then
// layout->Animate(0); then layout->CalculateMtx(E+0x64) and layout->Draw(E+0x64).
extern "C" void OthermHudElementDraw_HLE(CpuContext* ctx) {
    const uint32_t e = ctx->gpr[3];
    if (Memory::Read8(e + 0xC8u) == 0) return;
    NoteAreaMap(e);
    float x = Memory::ReadFloat32(e + 0xB8u);
    float y = Memory::ReadFloat32(e + 0xBCu);
    float angle = Memory::ReadFloat32(e + 0xC0u);
    const float scale = Memory::ReadFloat32(e + 0xC4u);

    const uint32_t frame = VI_HLE_PresentedFrames();
    const bool log = LogFrame(frame);
    const bool northUp = OthermMinimap::NorthUp();
    const int index = log ? ElementIndex(e) : -1;
    const bool onMap = northUp && OnMap(e);
    float turn = 0.0f, cx = 0.0f, cy = 0.0f;
    const bool turned = northUp && onMap && MapTurn(turn, cx, cy) && turn != 0.0f;
    const float x0 = x, y0 = y, angle0 = angle;
    if (turned) {
        const float c = std::cos(-turn), s = std::sin(-turn);
        const float dx = x - cx, dy = y - cy;
        x = cx + c * dx - s * dy;
        y = cy + s * dx + c * dy;
        // Icons the game keeps upright (navigation booths, elevators, items, sector names: angle 0) stay upright;
        // the map, doors, Samus's marker and the destination arrow carry the map's turn in their angle.
        if (angle != 0.0f) angle -= turn;
    }
    if (log) {
        static int s_lines = 0;
        if (s_lines++ < 4000) {
            uint32_t vtable = 0, layout = 0;
            Memory::TryRead32(e + 0x114u, vtable);
            Memory::TryRead32(e, layout);
            RT_LOGF(RT_TAG_RUNTIME,
                    "[minimap] f%u elem %02X %08X %-12s vt %08X layout %08X at %.2f,%.2f angle %.4f scale %.3f%s\n", frame,
                    index < 0 ? 0xFF : index, e, ElementName(e).c_str(), vtable, layout, x0, y0, angle0, scale,
                    turned ? " (north up: turned back)" : "");
            if (turned)
                RT_LOGF(RT_TAG_RUNTIME, "[minimap] f%u   -> %.2f,%.2f angle %.4f (map turn %.4f about %.2f,%.2f)\n", frame,
                        x, y, angle, turn, cx, cy);
        }
    }

    const uint32_t pane = Memory::Read32(e + 0x60u);
    const float rootZ = Memory::ReadFloat32(kDrawConstants + 0x14u);
    const float degreesPerRadian = Memory::ReadFloat32(kDrawConstants + 0x18u);
    const float zero = Memory::ReadFloat32(kDrawConstants + 0x1Cu);
    // fctiwz of E+0x108 * E+0xCC (a byte value in practice); clamped where the PowerPC instruction saturates.
    const float alpha = Memory::ReadFloat32(e + 0x108u) * static_cast<float>(static_cast<int32_t>(Memory::Read32(e + 0xCCu)));
    const int32_t alphaInt = !(alpha == alpha) ? INT32_MIN
                             : alpha >= 2147483647.0f ? INT32_MAX
                             : alpha <= -2147483648.0f ? INT32_MIN
                                                       : static_cast<int32_t>(alpha);
    Memory::Write8(pane + 0xB8u, static_cast<uint8_t>(alphaInt));
    Memory::Write8(pane + 0xBBu, static_cast<uint8_t>((Memory::Read8(pane + 0xBBu) & 0xFDu) | 2u));
    Memory::WriteFloat32(pane + 0x2Cu, x);
    Memory::WriteFloat32(pane + 0x30u, y);
    Memory::WriteFloat32(pane + 0x34u, rootZ);
    Memory::WriteFloat32(pane + 0x44u, Memory::ReadFloat32(e + 0x100u) * scale);
    Memory::WriteFloat32(pane + 0x48u, Memory::ReadFloat32(e + 0x104u) * scale);
    Memory::WriteFloat32(pane + 0x38u, zero);
    Memory::WriteFloat32(pane + 0x3Cu, zero);
    Memory::WriteFloat32(pane + 0x40u, degreesPerRadian * angle);

    const uint32_t savedLr = ctx->lr;
    const auto layoutCall = [&](uint32_t slot, uint32_t arg) {
        CpuContext call = *ctx;  // the callee's frames go below this thread's stack pointer, as for any call
        const uint32_t layout = Memory::Read32(e);
        call.gpr[3] = layout;
        call.gpr[4] = arg;
        call.lr = 0x80638E8Cu;
        InvokeIndirectCpu(Memory::Read32(Memory::Read32(layout) + slot), &call);
    };
    const uint32_t animation = Memory::Read32(e + 4u);
    if (animation != 0) {
        const int32_t key = static_cast<int32_t>(Memory::Read32(e + 0x24u));
        if (Memory::Read8(e + 0x28u) != 0 && key != -1) {
            // The first node of the tree at E+0x18 (left +0, right +4, key +0xC) whose key is not below E+0x24.
            const uint32_t end = e + 0x18u;
            uint32_t node = Memory::Read32(end), found = end;
            while (node != 0) {
                if (static_cast<int32_t>(Memory::Read32(node + 0xCu)) >= key) {
                    found = node;
                    node = Memory::Read32(node);
                } else {
                    node = Memory::Read32(node + 4u);
                }
            }
            if (found != end && key < static_cast<int32_t>(Memory::Read32(found + 0xCu))) found = end;
            if (found != end) {
                CpuContext call = *ctx;
                call.gpr[3] = e;
                call.gpr[4] = Memory::Read32(e + 0x60u);
                call.gpr[5] = found + 0x10u;
                call.fpr[1].d = Memory::ReadFloat32(e + 0x5Cu);
                call.lr = 0x80638E38u;
                InvokeDirectCpu<0x8063A3C8u>(&call);
            }
        } else {
            Memory::WriteFloat32(animation + 0x10u, Memory::ReadFloat32(e + 0x5Cu));
        }
        layoutCall(0x38u, 0);  // Animate
    }
    layoutCall(0x30u, e + 0x64u);  // CalculateMtx(drawInfo)
    layoutCall(0x34u, e + 0x64u);  // Draw(drawInfo)
    ctx->lr = savedLr;
}
PPC_NATIVE_OVERRIDE_VOID(80638CAC, OthermHudElementDraw_HLE, (CpuContext* ctx), (ctx));
