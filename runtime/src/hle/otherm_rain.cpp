// Rain drops drawn where they are (Sector 1's rain went missing at times). NOTES.md 2026-11-21.
//
// particle_rain.cpp keeps its drops in a bank (table 0x80C8DCE0; bank +0x2008 -> drops of 0x70 bytes: position +0,
// velocity +0xC, life +0x18, splash count +0x24, splash places from +0x28). rel_807B1CC4 draws them: it takes the camera's
// view matrix once, then rel_807B413C(bank, drop, view) loads view * rel_807B3AAC(drop) for the drop and its splashes.
// rel_807B3AAC builds the drop's billboard: columns cross(up, toward), up = -velocity (normalized), toward = camera
// position (camera object +0x74, from rel_807E6C94(-1)) - drop (normalized), then concatenates the drop's translation in
// front of that rotation. It stores only the nine rotation entries of the 3x4 rotation matrix (sp+0x80..0xA8): the
// translation column (sp+0x8C, 0x9C, 0xAC) is never written, so the concatenation adds whatever is left in those stack
// words to the drop's position. On the Wii they are left by PSMTXConcat's `stfd f14` / `stfd f31` from the previous
// drop, the low halves of two doubles that hold single-precision values: zero. The port keeps a paired-single register
// as two floats in one 64-bit word (PPC_FPR), so the same `stfd` leaves ps1's float there instead: 12784.27 in the
// developer's 21:53 state, which put every drop but the first (drawn at a different stack depth) 12784 units along
// the world's x, off screen. Whether rain showed depended on what f14 held in the code that called the effect draw.
//
// This is rel_807B3AAC with the translation column zero, as the game means it (and as the Wii's stack makes it).
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

constexpr uint32_t kCameraSlots = 0x80C92810u;  // rel_807E6C94(-1) picks one of these four
constexpr uint32_t kCameraPosition = 0x74u;
constexpr float kEpsilon = 1.1920929e-07f;      // 0x80B54004: below this length a vector is left as it is

struct Vec { float x, y, z; };

// rel_807E6C94(-1): slot 2 if it is set, else slot 3, else slot 1 if it is set, else slot 0.
uint32_t CurrentCamera() {
    uint32_t index = 0;
    if (Memory::Read32(kCameraSlots + 8u) != 0) index = 2;
    else if (Memory::Read32(kCameraSlots + 12u) != 0) index = 3;
    else if (Memory::Read32(kCameraSlots + 4u) != 0) index = 1;
    return Memory::Read32(kCameraSlots + 4u * index);
}

// The game's in-place normalization: scaled by 1 / length when the length is over kEpsilon (PSVECMag, fdivs, ps_muls0).
Vec Normalized(Vec v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > kEpsilon)) return v;
    const float s = 1.0f / length;
    return {v.x * s, v.y * s, v.z * s};
}

Vec ReadVec(uint32_t at) {
    return {Memory::ReadFloat32(at), Memory::ReadFloat32(at + 4u), Memory::ReadFloat32(at + 8u)};
}

std::atomic<bool> g_logged{false};

}  // namespace

// 0x807B3AAC: void RainDrop::BillboardMtx(const Drop* drop, Mtx out).
extern "C" void OthermRainDropMatrix_HLE(CpuContext* ctx) {
    const uint32_t drop = ctx->gpr[3];
    const uint32_t out = ctx->gpr[4];
    const Vec position = ReadVec(drop);
    const Vec velocity = ReadVec(drop + 0xCu);
    const uint32_t camera = CurrentCamera();
    const Vec eye = camera != 0 ? ReadVec(camera + kCameraPosition) : position;
    const Vec toward = Normalized({eye.x - position.x, eye.y - position.y, eye.z - position.z});
    const Vec up = Normalized({-velocity.x, -velocity.y, -velocity.z});
    const Vec side = {up.y * toward.z - up.z * toward.y, up.z * toward.x - up.x * toward.z,
                      up.x * toward.y - up.y * toward.x};  // PSVECCrossProduct(up, toward)
    const float m[12] = {side.x, up.x, toward.x, position.x,
                         side.y, up.y, toward.y, position.y,
                         side.z, up.z, toward.z, position.z};
    for (uint32_t i = 0; i < 12; ++i) Memory::WriteFloat32(out + 4u * i, m[i]);
    if (!g_logged.exchange(true, std::memory_order_relaxed)) {
        RT_LOGF(RT_TAG_RUNTIME, "[rain] drop matrices built by the port (rel_807B3AAC, translation column zero)\n");
    }
}
PPC_NATIVE_OVERRIDE_VOID(807B3AAC, OthermRainDropMatrix_HLE, (CpuContext* ctx), (ctx));
