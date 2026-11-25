// Built-in cheats for Metroid: Other M (otherm_cheats.h). How the values were found (game.rel pre-linked at 0x8052B280,
// guest memory read out of save states with tools/state_mem.py; NOTES.md 2026-11-08):
//  - 0x80C7B06C caches the player object (rel_805C4158, the getter the HUD and the save code use).
//  - player+0x10C is the energy component: vtable 0x80B15440, slot count at +0x60 (1), the player again at +0x68,
//    current energy at +0x40 and max at +0x20 (99 / 99 in every state looked at). The HUD gauge (rel_80648A68) reads
//    it with rel_806951EC(component, 0); rel_80695210(component, value, 0) sets it, clamped to the max, and rehashes it
//    (rel_808AF0A4 on the tamper object 0x80CB7268).
//  - player+0x124 is the item table: +0 points at {id, count, capacity} entries, +4 says how many (1). Missiles are id
//    0: their capacity is the progression record's missile capacity (0x80CA4E20 + 8, pushed into the table by
//    rel_80837CD4 -> rel_80699A1C(table, 0, capacity)); the developer's 11:22 state has 9 of 10, capacity 10 in both
//    places. rel_80699378(table, id, amount, 0) takes `amount` off (clamped at 0) and rehashes (rel_808AEFDC); the
//    community "Infinite Ammo" Gecko code NOPs its store. Power bombs have no count (the game's own text: charged in
//    Morph Ball by holding 1), so there is nothing to refill for them.
//  - Damage to the player is scaled by a per-difficulty pair of floats at 0x80AFCD30 (normal 1.0 1.0, hard 1.4 1.4;
//    rel_80586470 returns the row, rel_8052C548 / rel_8052EDE0 multiply by it only when the target is the player).
//    0 means no damage (the community "Samus Damage modifier" Gecko code writes the same table).
//  - The tamper check rel_808AF7DC runs from the game loop (rel_805AF65C) every frame; a mismatch calls fn_80179C88,
//    which sets the byte at r13-0x6F28 (0x80332AB8). Writing energy or ammo directly would trip it, so the cheats go
//    through the game's setters.
#include "otherm_cheats.h"

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace OthermCheats {
namespace {

constexpr uint32_t kModuleList = 0x800030C8u;    // __OSModuleInfoList: non-zero once game.rel is linked
constexpr uint32_t kPlayerCache = 0x80C7B06Cu;
constexpr uint32_t kEnergyVtable = 0x80B15440u;
constexpr uint32_t kSetEnergy = 0x80695210u;     // rel_80695210(component, value, slot)
constexpr uint32_t kConsumeItem = 0x80699378u;   // rel_80699378(table, id, amount, new capacity or 0 to keep it)
constexpr uint32_t kMissileId = 0;
constexpr uint32_t kDamageScale = 0x80AFCD30u;
// The table as the disc has it (read from the developer's states, none of which had a cheat on).
constexpr std::array<uint32_t, 4> kDamageScaleDisc{0x3F800000u, 0x3F800000u, 0x3FB33333u, 0x3FB33333u};

std::atomic<int> g_energy{-1}, g_missiles{-1};  // -1: not read from Config.toml yet

bool Enabled(std::atomic<int>& flag, bool (*fromConfig)(bool)) {
    int v = flag.load(std::memory_order_relaxed);
    if (v < 0) {
        v = fromConfig(false) ? 1 : 0;
        flag.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

// A heap or static object of the game: MEM1 or MEM2, word aligned.
bool ReadObject(uint32_t at, uint32_t& value) {
    if (!Memory::TryRead32(at, value)) return false;
    const bool mem1 = value >= 0x80000000u && value < 0x81800000u;
    const bool mem2 = value >= 0x90000000u && value < 0x94000000u;
    return (mem1 || mem2) && (value & 3u) == 0;
}

void CallGame(CpuContext* cpu, uint32_t function, std::initializer_list<uint32_t> args) {
    CpuContext call = *cpu;  // the callee's frames go below this thread's stack pointer, as for any call it makes
    int reg = 3;
    for (uint32_t a : args) call.gpr[reg++] = a;
    InvokeIndirectCpu(function, &call);
}

void LogChange(const char* what, int32_t from, int32_t to) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 40)
        RT_LOGF(RT_TAG_RUNTIME, "[cheats] %s %d -> %d\n", what, from, to);
}

int s_scaleLogs = 0;  // a Gecko code holding the table at 0 would otherwise log the restore every retrace

void UpdateDamageScale(bool noDamage) {
    std::array<uint32_t, 4> now{};
    for (size_t i = 0; i < now.size(); ++i)
        if (!Memory::TryRead32(kDamageScale + 4 * uint32_t(i), now[i])) return;
    if (noDamage) {
        if (now != std::array<uint32_t, 4>{}) {
            for (size_t i = 0; i < now.size(); ++i) Memory::TryWrite32(kDamageScale + 4 * uint32_t(i), 0);
            if (s_scaleLogs++ < 10) RT_LOGF(RT_TAG_RUNTIME, "[cheats] damage to Samus scaled by 0\n");
        }
    } else if (now == std::array<uint32_t, 4>{}) {
        // Off, but the table is still zero: the cheat was just switched off, or a save state taken with it on was
        // loaded. Gecko codes run after this, so a code that wants the table at 0 still gets its way.
        for (size_t i = 0; i < now.size(); ++i) Memory::TryWrite32(kDamageScale + 4 * uint32_t(i), kDamageScaleDisc[i]);
        if (s_scaleLogs++ < 10) RT_LOGF(RT_TAG_RUNTIME, "[cheats] damage to Samus back to the game's own scale\n");
    }
}

// Samus's energy component (player + 0x10C), checked against its vtable and back pointer.
struct Energy {
    uint32_t component = 0, current = 0, max = 0;
};
bool FindEnergy(uint32_t player, Energy& out) {
    uint32_t vtable = 0, owner = 0, slots = 0;
    if (!ReadObject(player + 0x10Cu, out.component)) return false;
    if (!Memory::TryRead32(out.component + 0x64u, vtable) || vtable != kEnergyVtable) return false;
    if (!Memory::TryRead32(out.component + 0x68u, owner) || owner != player) return false;
    if (!Memory::TryRead32(out.component + 0x60u, slots) || slots == 0 || slots > 8) return false;
    return Memory::TryRead32(out.component + 0x40u, out.current) && Memory::TryRead32(out.component + 0x20u, out.max) &&
           out.max > 0 && out.max <= 100000u;
}

// The missile entry of the item table (player + 0x124).
struct Missiles {
    uint32_t table = 0;
    int32_t have = 0, capacity = 0;
};
bool FindMissiles(uint32_t player, Missiles& out) {
    uint32_t entries = 0, count = 0;
    if (!ReadObject(player + 0x124u, out.table) || !ReadObject(out.table, entries)) return false;
    if (!Memory::TryRead32(out.table + 4u, count) || count == 0 || count > 32) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = entries + 12u * i;
        uint32_t id = 0, have = 0, capacity = 0;
        if (!Memory::TryRead32(entry, id)) return false;
        if (id != kMissileId) continue;
        if (!Memory::TryRead32(entry + 4u, have) || !Memory::TryRead32(entry + 8u, capacity)) return false;
        out.have = int32_t(have);
        out.capacity = int32_t(capacity);
        return out.capacity > 0 && out.capacity <= 999 && out.have >= 0;
    }
    return false;
}

void SetEnergy(CpuContext* cpu, const Energy& e, uint32_t value) { CallGame(cpu, kSetEnergy, {e.component, value, 0}); }
// rel_80699378 takes an amount off; a negative one adds.
void SetMissiles(CpuContext* cpu, const Missiles& m, int32_t value) {
    CallGame(cpu, kConsumeItem, {m.table, kMissileId, uint32_t(m.have - value), 0});
}

void RefillEnergy(CpuContext* cpu, uint32_t player) {
    Energy e;
    // At 0 the game's own death sequence has started (no damage reaches Samus while this is on, so it was something
    // else, a scripted death say): leave it alone.
    if (!FindEnergy(player, e) || e.current == 0 || e.current >= e.max) return;
    SetEnergy(cpu, e, e.max);
    LogChange("energy", int32_t(e.current), int32_t(e.max));
}

void RefillMissiles(CpuContext* cpu, uint32_t player) {
    Missiles m;
    if (!FindMissiles(player, m) || m.have >= m.capacity) return;
    SetMissiles(cpu, m, m.capacity);
    LogChange("missiles", m.have, m.capacity);
}

// OTHERM_TEST_SET="<frame>:energy:<n>,<frame>:missiles:<n>" (tests): at that presented frame, sets Samus's energy or
// missile count through the same setters, standing in for damage taken or a missile fired.
void TestSets(CpuContext* cpu, uint32_t player) {
    struct Set { uint32_t frame; bool energy; int32_t value; };
    static const std::vector<Set> sets = [] {
        std::vector<Set> out;
        const char* v = std::getenv("OTHERM_TEST_SET");
        std::stringstream items(v ? v : "");
        std::string item;
        while (std::getline(items, item, ',')) {
            const size_t a = item.find(':'), b = a == std::string::npos ? a : item.find(':', a + 1);
            if (b == std::string::npos) continue;
            const std::string what = item.substr(a + 1, b - a - 1);
            if (what != "energy" && what != "missiles") continue;
            out.push_back({uint32_t(std::strtoul(item.c_str(), nullptr, 10)), what == "energy",
                           int32_t(std::strtol(item.c_str() + b + 1, nullptr, 10))});
        }
        return out;
    }();
    static size_t next = 0;
    const uint32_t frame = VI_HLE_PresentedFrames();
    for (; next < sets.size() && frame >= sets[next].frame; ++next) {
        const Set& s = sets[next];
        Energy e;
        Missiles m;
        if (s.energy && s.value > 0 && FindEnergy(player, e)) {
            SetEnergy(cpu, e, uint32_t(s.value));
            RT_LOGF(RT_TAG_RUNTIME, "[cheats] test: energy %u -> %d at frame %u\n", e.current, s.value, frame);
        } else if (!s.energy && s.value >= 0 && FindMissiles(player, m)) {
            SetMissiles(cpu, m, s.value);
            RT_LOGF(RT_TAG_RUNTIME, "[cheats] test: missiles %d -> %d at frame %u\n", m.have, s.value, frame);
        } else {
            RT_LOGF(RT_TAG_RUNTIME, "[cheats] test: nothing to set at frame %u\n", frame);
        }
    }
}

}  // namespace

bool InfiniteEnergy() { return Enabled(g_energy, RuntimeConfigFile::InfiniteEnergy); }
bool InfiniteMissiles() { return Enabled(g_missiles, RuntimeConfigFile::InfiniteMissiles); }

void SetInfiniteEnergy(bool on) {
    g_energy.store(on ? 1 : 0, std::memory_order_relaxed);
    RuntimeConfigFile::SetInfiniteEnergy(on);
    RT_LOGF(RT_TAG_RUNTIME, "[cheats] infinite energy %s\n", on ? "on" : "off");
}

void SetInfiniteMissiles(bool on) {
    g_missiles.store(on ? 1 : 0, std::memory_order_relaxed);
    RuntimeConfigFile::SetInfiniteMissiles(on);
    RT_LOGF(RT_TAG_RUNTIME, "[cheats] infinite missiles %s\n", on ? "on" : "off");
}

void Frame(CpuContext* cpu) {
    uint32_t linked = 0;
    if (!cpu || !Memory::TryRead32(kModuleList, linked) || linked == 0) return;
    const bool energy = InfiniteEnergy(), missiles = InfiniteMissiles();
    UpdateDamageScale(energy);
    static const bool tests = std::getenv("OTHERM_TEST_SET") != nullptr;
    if (!energy && !missiles && !tests) return;
    uint32_t player = 0;
    if (!ReadObject(kPlayerCache, player)) return;
    if (energy) RefillEnergy(cpu, player);
    if (missiles) RefillMissiles(cpu, player);
    if (tests) TestSets(cpu, player);  // after the refills, so the game plays one frame with the value set
}

}  // namespace OthermCheats
