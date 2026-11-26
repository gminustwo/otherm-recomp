// Rewrites of the game's instructions the port follows (code_patches.h).
#include "code_patches.h"

#include "code_taps.h"
#include "generated/RuntimeConfig.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

extern "C" uint8_t g_othermCodeTapOn[kOthermCodeTapCapacity] = {};

static_assert(RuntimeConfig::kCodeTapCount <= kOthermCodeTapCapacity, "more code taps than code_taps.h has room for");

namespace CodePatches {
namespace {

constexpr uint32_t kCodePatchRelImage = 0x8052B280u;  // game.rel pre-linked here: file offset = address - this
constexpr uint32_t kCodePatchGearWord = 0x80CA4E20u;  // the suit bits (Redux: 0x4 Gravity, 0x2 Varia)

enum class TapMode : uint8_t { Constant, SuitModel };
struct TapValue {
    TapMode mode = TapMode::Constant;
    uint32_t constant = 0;
};
std::array<TapValue, kOthermCodeTapCapacity> g_codeTapValues{};

int FindTap(uint32_t address, bool after, uint8_t gpr) {
    for (uint32_t i = 0; i < RuntimeConfig::kCodeTapCount; ++i) {
        const auto& site = RuntimeConfig::kCodeTaps[i];
        if (site.address == address && site.after == after && site.gpr == gpr) return int(i);
    }
    return -1;
}

void SetTap(int id, bool on, TapValue value = {}) {
    if (id < 0) return;
    g_codeTapValues[size_t(id)] = value;
    g_othermCodeTapOn[id] = on ? 1 : 0;
}

// "li rD, N" (addi rD, 0, N): true with rD and N.
bool DecodeLi(uint32_t word, uint8_t& rd, int32_t& value) {
    if ((word >> 26) != 14u || ((word >> 16) & 31u) != 0u) return false;
    rd = uint8_t((word >> 21) & 31u);
    value = int32_t(int16_t(word & 0xFFFFu));
    return true;
}

uint64_t Fnv1a(const std::vector<uint32_t>& words) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const uint32_t w : words)
        for (int shift = 24; shift >= 0; shift -= 8) {
            h ^= (w >> shift) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    return h;
}

// One site the port follows: `words` instructions from `address` (the game's own in `original`, the current ones in
// `current`, the state Reset() returns to in `committed`), and what the current ones switch on.
struct Site {
    const char* what;
    uint32_t address;
    uint32_t count;
    void (*apply)(Site&);
    std::vector<uint32_t> original, current, committed;
    std::vector<int> taps;  // ids, looked up at Init
    bool on = false;
    std::set<uint64_t> logged;  // (state) already in the log
};

void LogOnce(Site& site, uint64_t key, const char* text) {
    if (site.logged.size() < 16 && site.logged.insert(key).second)
        RT_LOGF(RT_TAG_MOD, "code at 0x%08X (%s): %s\n", site.address, site.what, text);
}

// "li rD, N" over the instruction at a site whose tap gives rD after it (missiles in third person).
void ApplyLi(Site& site) {
    const uint32_t word = site.current[0];
    const int tap = site.taps[0];
    if (word == site.original[0]) {
        SetTap(tap, false);
        site.on = false;
        return;
    }
    uint8_t rd = 0;
    int32_t value = 0;
    if (tap >= 0 && DecodeLi(word, rd, value) && rd == RuntimeConfig::kCodeTaps[tap].gpr) {
        SetTap(tap, true, {TapMode::Constant, uint32_t(value)});
        site.on = true;
        char text[64];
        std::snprintf(text, sizeof text, "li r%u, %d followed", unsigned(rd), int(value));
        LogOnce(site, uint64_t(word), text);
        return;
    }
    SetTap(tap, false);
    site.on = false;
    char text[96];
    std::snprintf(text, sizeof text, "0x%08X is not an instruction this port can follow there; ignored", word);
    LogOnce(site, uint64_t(word), text);
}

// A nop over rel_80571284's branch past concentrating: the compare before it sees "yes" (concentrate at any energy).
void ApplyConcentrationBranch(Site& site) {
    const uint32_t word = site.current[0];
    const bool nop = word == 0x60000000u;
    SetTap(site.taps[0], nop, {TapMode::Constant, 1u});
    site.on = nop;
    if (nop) LogOnce(site, 1, "nop followed (concentration at any energy)");
    else if (word != site.original[0]) LogOnce(site, uint64_t(word), "an instruction this port can't follow there; ignored");
}

// Redux's suit choice over rel_805F602C's room-based one: the 253 branch is taken and the model comes from the suit.
constexpr uint64_t kReduxCutsceneSuitHash = 0x815459AF61F3A22Eull;  // Other M Redux's game.rel, 0x805F6040-0x805F6107
void ApplyCutsceneSuit(Site& site) {
    const bool redux = Fnv1a(site.current) == kReduxCutsceneSuitHash;
    SetTap(site.taps[0], redux, {TapMode::Constant, 1u});
    SetTap(site.taps[1], redux, {TapMode::SuitModel, 0u});
    site.on = redux;
    if (redux) LogOnce(site, 1, "Other M Redux's suit choice followed");
    else if (site.current != site.original) LogOnce(site, 2, "changed in a way this port can't follow; ignored");
}

std::vector<Site> g_codeSites;
bool g_codeSitesReady = false;

std::vector<Site> MakeSites() {
    std::vector<Site> sites;
    const auto add = [&](const char* what, uint32_t address, uint32_t count, void (*apply)(Site&),
                         std::vector<std::array<uint32_t, 3>> taps) {
        Site site{what, address, count, apply};
        for (const auto& [at, after, gpr] : taps) site.taps.push_back(FindTap(at, after != 0, uint8_t(gpr)));
        sites.push_back(std::move(site));
    };
    add("concentration", 0x805713D0u, 1, ApplyConcentrationBranch, {{0x805713CCu, 0, 3}});
    add("shot type", 0x8054D998u, 1, ApplyLi, {{0x8054D998u, 1, 27}});
    add("shot type", 0x8054D9ACu, 1, ApplyLi, {{0x8054D9ACu, 1, 27}});
    add("shot type", 0x8054D9CCu, 1, ApplyLi, {{0x8054D9CCu, 1, 27}});
    add("shot type", 0x8054D9F4u, 1, ApplyLi, {{0x8054D9F4u, 1, 27}});
    add("stored shot type", 0x8054E368u, 1, ApplyLi, {{0x8054E368u, 1, 0}});
    add("cutscene suit", 0x805F6040u, 50, ApplyCutsceneSuit, {{0x805F60FCu, 0, 0}, {0x805F6104u, 1, 3}});
    return sites;
}

Site* FindSite(uint32_t address) {
    for (Site& site : g_codeSites)
        if (address >= site.address && address < site.address + 4u * site.count) return &site;
    return nullptr;
}

}  // namespace
}  // namespace CodePatches

extern "C" uint32_t OthermCodeTapValue(uint32_t id, uint32_t value) {
    if (id >= kOthermCodeTapCapacity) return value;
    const CodePatches::TapValue& tap = CodePatches::g_codeTapValues[id];
    uint32_t result = tap.constant, gear = 0;
    if (tap.mode == CodePatches::TapMode::SuitModel) {
        if (!Memory::TryRead32(CodePatches::kCodePatchGearWord, gear)) return value;
        result = (gear & 4u) ? 254u : (gear & 2u) ? 253u : 252u;
    }
    // OTHERM_LOG_CODE_TAPS=1 (tests): each value a switched-on tap gives the game, when it changes (200 lines at most).
    static const bool logTaps = std::getenv("OTHERM_LOG_CODE_TAPS") != nullptr;
    static uint64_t lastLogged[kOthermCodeTapCapacity] = {};
    static int logged = 0;
    const uint64_t key = (uint64_t(value) << 32 | result) + 1;
    if (logTaps && lastLogged[id] != key && logged < 200) {
        lastLogged[id] = key;
        ++logged;
        RT_LOGF(RT_TAG_MOD, "tap %u at 0x%08X: %u -> %u (gear 0x%08X, frame %u)\n", id, RuntimeConfig::kCodeTaps[id].address,
                value, result, gear, VI_HLE_PresentedFrames());
    }
    return result;
}

namespace CodePatches {

void Init(const std::filesystem::path& discRel) {
    g_codeSites = MakeSites();
    g_codeSitesReady = false;
    std::ifstream rel(discRel, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(rel)), {});
    for (Site& site : g_codeSites) {
        const size_t offset = site.address - kCodePatchRelImage;
        if (offset + 4u * site.count > bytes.size()) {
            RT_LOGF(RT_TAG_MOD, "code patches: the disc's game.rel is too short (%zu bytes); none followed\n", bytes.size());
            g_codeSites.clear();
            return;
        }
        for (uint32_t i = 0; i < site.count; ++i) {
            const uint8_t* p = &bytes[offset + 4u * i];
            site.original.push_back(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
        }
        site.current = site.committed = site.original;
        if (std::any_of(site.taps.begin(), site.taps.end(), [](int id) { return id < 0; }))
            RT_LOGF(RT_TAG_MOD, "code patches: no code tap for 0x%08X (%s) in this build; project/code_taps.txt and "
                                "the translation disagree\n", site.address, site.what);
    }
    g_codeSitesReady = true;
}

bool Knows(uint32_t address) { return g_codeSitesReady && FindSite(address) != nullptr; }

bool Write(uint32_t address, uint32_t size, uint32_t value, bool apply) {
    if (!g_codeSitesReady) return false;
    Site* site = FindSite(address);
    if (!site) return false;
    // Merge the bytes into the site's words (big-endian), then let the site decide what they mean.
    bool changed = false;
    for (uint32_t i = 0; i < size; ++i) {
        const uint32_t at = address + i;
        if (at < site->address || at >= site->address + 4u * site->count) continue;
        const uint32_t index = (at - site->address) / 4u, shift = 8u * (3u - (at & 3u));
        const uint32_t byte = (value >> (8u * (size - 1u - i))) & 0xFFu;
        const uint32_t word = (site->current[index] & ~(0xFFu << shift)) | (byte << shift);
        changed |= word != site->current[index];
        site->current[index] = word;
    }
    if (changed && apply) site->apply(*site);
    return true;
}

void Commit() {
    for (Site& site : g_codeSites) {
        site.apply(site);
        site.committed = site.current;
    }
}

void Reset() {
    for (Site& site : g_codeSites) {
        if (site.current == site.committed) continue;
        site.current = site.committed;
        site.apply(site);
    }
}

bool SetPortTap(uint32_t address, bool after, uint8_t gpr, bool on, uint32_t value) {
    const int id = FindTap(address, after, gpr);
    SetTap(id, on, {TapMode::Constant, value});
    return id >= 0;
}

std::string Describe() {
    std::string text;
    for (const Site& site : g_codeSites) {
        if (!site.on) continue;
        char part[64];
        std::snprintf(part, sizeof part, "%s0x%08X (%s)", text.empty() ? "" : ", ", site.address, site.what);
        text += part;
    }
    return text;
}

bool SelfCheck() {
    uint8_t rd = 0;
    int32_t value = 0;
    const bool li = DecodeLi(0x3B600006u, rd, value) && rd == 27 && value == 6 && DecodeLi(0x3860FFFFu, rd, value) &&
                    rd == 3 && value == -1 && !DecodeLi(0x801E0018u, rd, value);
    // A site switched on by a write and off again by the original word (no taps needed: ids stay -1).
    Site site{"test", 0x80001000u, 1, ApplyLi};
    site.taps = {-1};
    site.original = site.current = site.committed = {0x801E0018u};
    site.current[0] = 0x38000001u;
    ApplyLi(site);
    const bool onWithoutTap = !site.on;  // no tap in this build: nothing switched on
    return li && onWithoutTap;
}

}  // namespace CodePatches
