// 360-degree movement (Options > Cheats, Config.toml [cheats] analog_movement). NOTES.md 2026-11-20.
//
// Other M reads the sideways remote's D-pad once a frame per input slot (slots at 0x80C76220, 0x9BC bytes each,
// filled by rel_805719E4; slot+0 -> the controller, whose first word is that channel's KPAD buffer, 0x802F3390 +
// channel * 0xF30). rel_80572110(slot) turns the held buttons (slot+4) into a direction:
//   index 0-8 at +0x184 (the remote's own directions: 0 Right, 1 Left, 2 Up, 3 Down, 4 Right+Up, 5 Right+Down,
//   6 Left+Up, 7 Left+Down, 8 none) and in a 10-entry ring (+0x148 newest, +0x14C count, +0x144 oldest, entries from
//   +0x150); the unit vector rel_80573DA8(index) at +0x178 / +0x17C (its table at 0x80C78918: Up (1, 0), Right (0, 1),
//   Right+Up (0.707, 0.707)); its angle atan2(x, y) from the table at 0x80AFCA74 at +0x180; "a direction is held" at
//   +0x188. (+0x189 and +0x18A end up 0 every time: the game compares the index it has just stored with itself.)
// Samus's behavior programs take the direction from the vector (rel_8054DDF8, rel_8054DF20 and other handlers read
// slot+0x178 / +0x17C through rel_80577D68). With the cheat on, this does what the game does and then, for player 1's
// slot while a direction is held, puts the stick's direction (VirtualRemote::MoveDirection: the D-pad bindings'
// analog values, in the same frame) in the vector and the angle. The index stays the D-pad's (VirtualRemote picks its
// bits from the stick's direction, nearest of eight), so whatever goes by the index sees the nearest of the eight.
#include "otherm_move.h"

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "virtual_remote.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kKpadBufferChannel0 = 0x802F3390u;
constexpr uint32_t kDemoPlayback = 0x80C78AE1u;  // non-zero: the slots read a recorded demo (rel_805AFD9C)
constexpr uint32_t kAngleTable = 0x80AFCA74u;
constexpr float kMinimumDeflection = 0.2f;

std::atomic<int> g_enabled{-1};  // -1: not read from Config.toml yet

void WriteFloat(uint32_t at, float value) { Memory::WriteFloat32(at, value); }

}  // namespace

namespace OthermMove {

bool Enabled() {
    int v = g_enabled.load(std::memory_order_relaxed);
    if (v < 0) {
        v = RuntimeConfigFile::AnalogMovement(false) ? 1 : 0;
        g_enabled.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

void SetEnabled(bool on) {
    g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    RuntimeConfigFile::SetAnalogMovement(on);
    RT_LOGF(RT_TAG_RUNTIME, "[cheats] 360-degree movement %s\n", on ? "on" : "off");
}

}  // namespace OthermMove

// 0x80572110: void InputSlot::UpdateDirection(InputSlot* slot).
extern "C" void OthermDpadDirection_HLE(CpuContext* ctx) {
    const uint32_t slot = ctx->gpr[3];
    const uint32_t hold = Memory::Read32(slot + 4u);
    const bool up = (hold & 8u) != 0, down = (hold & 4u) != 0, left = (hold & 1u) != 0, right = (hold & 2u) != 0;
    const bool any = up || down || left || right;
    int32_t dir = 8;
    if (any) {
        if (right) dir = up ? 4 : down ? 5 : 0;
        else if (left) dir = up ? 6 : down ? 7 : 1;
        else dir = up ? 2 : 3;
    }
    const int32_t newest = (static_cast<int32_t>(Memory::Read32(slot + 0x148u)) + 1) % 10;
    Memory::Write32(slot + 0x148u, static_cast<uint32_t>(newest));
    Memory::Write32(slot + 0x150u + 4u * static_cast<uint32_t>(newest), static_cast<uint32_t>(dir));
    const uint32_t count = Memory::Read32(slot + 0x14Cu) + 1u;
    Memory::Write32(slot + 0x14Cu, count);
    if (count > 10u) {
        Memory::Write32(slot + 0x14Cu, count - 1u);
        const int32_t oldest = (static_cast<int32_t>(Memory::Read32(slot + 0x144u)) + 1) % 10;
        Memory::Write32(slot + 0x144u, static_cast<uint32_t>(oldest));
    }
    {
        CpuContext call = *ctx;
        call.gpr[3] = static_cast<uint32_t>(dir);
        call.lr = 0x8057226Cu;
        InvokeDirectCpu<0x80573DA8u>(&call);
        const uint32_t vector = call.gpr[3];
        Memory::Write32(slot + 0x178u, Memory::Read32(vector));
        Memory::Write32(slot + 0x17Cu, Memory::Read32(vector + 4u));
    }
    const uint32_t row = dir >= 0 && dir < 9 ? static_cast<uint32_t>(dir) : 0u;
    Memory::Write32(slot + 0x180u, Memory::Read32(kAngleTable + 4u * row));
    Memory::Write32(slot + 0x184u, static_cast<uint32_t>(dir));
    Memory::Write8(slot + 0x188u, any ? 1u : 0u);
    Memory::Write8(slot + 0x189u, 0);
    Memory::Write8(slot + 0x18Au, 0);
    ctx->gpr[3] = 0;

    if (!any || !OthermMove::Enabled()) return;
    uint32_t controller = 0, buffer = 0;
    if (!Memory::TryRead32(slot, controller) || !Memory::TryRead32(controller, buffer) || buffer != kKpadBufferChannel0)
        return;
    if (Memory::Read8(kDemoPlayback) != 0) return;
    float x = 0.0f, y = 0.0f;
    VirtualRemote::MoveDirection(x, y);
    const float length = std::hypot(x, y);
    if (!(length >= kMinimumDeflection)) return;  // digital input (keyboard, touch): the game's own eight
    x /= length;
    y /= length;
    WriteFloat(slot + 0x178u, x);
    WriteFloat(slot + 0x17Cu, y);
    WriteFloat(slot + 0x180u, std::atan2(x, y));
    // OTHERM_LOG_MOVE=1 (tests): the stick's direction as the game gets it, when it changes by a degree or more.
    static const bool logMove = std::getenv("OTHERM_LOG_MOVE") != nullptr;
    static float s_lastAngle = 100.0f;
    const float angle = std::atan2(x, y);
    if (logMove && std::fabs(angle - s_lastAngle) > 0.0175f) {
        s_lastAngle = angle;
        RT_LOGF(RT_TAG_RUNTIME, "[move] frame %u: D-pad index %d, stick (%.3f, %.3f) angle %.1f degrees\n",
                unsigned(VI_HLE_PresentedFrames()), int(dir), x, y, angle * 57.29578f);
    }
}
PPC_NATIVE_OVERRIDE_VOID(80572110, OthermDpadDirection_HLE, (CpuContext* ctx), (ctx));
