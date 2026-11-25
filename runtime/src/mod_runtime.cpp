// Code mods, Gecko cheat codes and Riivolution memory patches (mod_runtime.h).
#include "mod_runtime.h"

#include "abi_bridge.h"
#include "code_patches.h"
#include "hle/storage/riivolution.h"
#include "hle_stubs.h"
#include "memory.h"
#include "mod_install.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "otherm_mod_api.h"

#include <algorithm>
#include <iterator>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

namespace fs = std::filesystem;

namespace {

// ---- code ranges: a write there changes an instruction, which the translated game never reads ----
struct Range { uint32_t start, end; };
std::vector<Range> g_codeRanges;
bool g_relRangesAdded = false;

void AddDolTextRanges() {
    std::ifstream dol(RuntimeConfigFile::ResolvedDvdRoot() / "sys" / "main.dol", std::ios::binary);
    std::array<uint8_t, 0xE4> header{};
    if (!dol.read(reinterpret_cast<char*>(header.data()), header.size())) return;
    const auto be = [&](size_t at) { return uint32_t(header[at]) << 24 | uint32_t(header[at + 1]) << 16 | uint32_t(header[at + 2]) << 8 | header[at + 3]; };
    for (int i = 0; i < 7; ++i)
        if (const uint32_t size = be(0x90 + 4 * i); size) g_codeRanges.push_back({be(0x48 + 4 * i), be(0x48 + 4 * i) + size});
}

// The loaded REL modules (OS module queue at 0x800030C8): their executable sections, once the game has linked them.
void AddRelTextRanges() {
    uint32_t module = 0;
    if (!Memory::TryRead32(0x800030C8u, module)) return;
    for (int guard = 0; module && guard < 16 && Memory::Contains(module, 0x20); ++guard) {
        const uint32_t count = Memory::Read32(module + 0x0C), table = Memory::Read32(module + 0x10);
        for (uint32_t s = 0; s < count && s < 32 && Memory::Contains(table + 8 * s, 8); ++s) {
            const uint32_t offset = Memory::Read32(table + 8 * s), size = Memory::Read32(table + 8 * s + 4);
            if (!(offset & 1) || !size) continue;
            const uint32_t start = (offset & ~1u) >= 0x80000000u ? (offset & ~1u) : module + (offset & ~1u);
            g_codeRanges.push_back({start, start + size});
            g_relRangesAdded = true;
        }
        module = Memory::Read32(module);  // link.next
    }
}

bool IsCode(uint32_t address) {
    return std::any_of(g_codeRanges.begin(), g_codeRanges.end(), [&](const Range& r) { return address >= r.start && address < r.end; });
}

// ---- Gecko codes ----
// Where a code's own lines appear to the code types that point into the code list (46 / 4E, then reads through ba
// or po): a window outside the guest's memory, 8 bytes a line, one per code (on a Wii the list sits in memory; a
// code such as Redux's third-person missiles keeps a word of its own there).
constexpr uint32_t kGeckoWindowBase = 0xE0000000u;
constexpr uint32_t kGeckoWindowStride = 0x00100000u;

struct Gecko {
    std::string name, note;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    bool enabledByDefault = false;
    bool supported = true;
    bool checkedCode = false;
    // A mod's main.dol code list (GeckoLoader): on while the mod is, not listed under Cheats, and run line by line
    // past what it can't do here (PowerPC blocks C0 / C2 are skipped, an unknown code type ends the list).
    bool fromMain = false;
    uint32_t window = 0;
};
std::mutex g_cheatMutex;
std::vector<Gecko> g_codes;
std::set<std::string> g_enabled;  // names switched on

fs::path CheatDir() { return RuntimeConfigFile::ApplicationDataDirectory() / "cheats"; }

// The code types run here: RAM writes, conditionals, base / pointer loads, terminators.
bool SupportedType(uint8_t cmd) {
    switch (cmd) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x08:
    case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E:
    case 0x40: case 0x42: case 0x44: case 0x46: case 0x48: case 0x4A: case 0x4C: case 0x4E:
    case 0xDE: case 0xE0: case 0xE2: case 0xF0:
        return true;
    default:
        return false;
    }
}

uint8_t Cmd(uint32_t a) { return uint8_t(a >> 24) & 0xEE; }  // without the pointer bit (0x10) and address bit 24

// Lines a code takes after its first one.
size_t ExtraLines(uint32_t a, uint32_t v) {
    switch (Cmd(a)) {
    case 0x06: return (v + 7) / 8;
    case 0x08: return 1;
    case 0xC0: case 0xC2: return v;  // PowerPC: v lines of instructions
    default: return 0;
    }
}

// What a dry run of a code met that it can't do here (for the log).
struct GeckoSkips {
    uint32_t powerpcBlocks = 0;
    uint32_t unknownType = 0;  // the first line of a type this runner does not know (0: none)
};

// Runs one code against game memory (`write` false: only reports the addresses it would write, for IsCode).
template <class Read32Fn, class WriteFn>
void RunGecko(const Gecko& code, Read32Fn read32Memory, WriteFn writeMemory, GeckoSkips* skips = nullptr) {
    uint32_t ba = 0x80000000u, po = 0x80000000u;
    std::vector<bool> stack;  // conditionals: true while their code runs
    const auto active = [&] { return std::all_of(stack.begin(), stack.end(), [](bool b) { return b; }); };
    // The code's own lines through its window (46 / 4E point there); everything else is game memory.
    const auto inWindow = [&](uint32_t at) { return code.window && at - code.window < kGeckoWindowStride; };
    const auto read32 = [&](uint32_t at) -> uint32_t {
        if (!inWindow(at)) return read32Memory(at);
        const uint32_t word = (at - code.window) / 4u;
        if (word / 2u >= code.lines.size()) return 0u;
        return (word & 1u) ? code.lines[word / 2u].second : code.lines[word / 2u].first;
    };
    const auto write = [&](uint32_t at, uint32_t size, uint32_t value) {
        if (!inWindow(at)) writeMemory(at, size, value);  // the list itself stays as loaded
    };
    for (size_t i = 0; i < code.lines.size(); ++i) {
        const auto [a, v] = code.lines[i];
        const uint8_t cmd = Cmd(a);
        const bool usePo = (a >> 24) & 0x10;
        const uint32_t address = (a & 0x01FFFFFFu) + (usePo ? po : ba);
        const bool run = active();
        switch (cmd) {
        case 0x00: case 0x02: case 0x04: {
            if (!run) break;
            const uint32_t size = cmd == 0x00 ? 1 : cmd == 0x02 ? 2 : 4;
            const uint32_t count = cmd == 0x04 ? 1 : (v >> 16) + 1;
            for (uint32_t n = 0; n < count; ++n) write(address + n * size, size, size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v);
            break;
        }
        case 0x06:
            for (uint32_t n = 0; run && n < v; ++n) {
                const auto& line = code.lines[std::min(code.lines.size() - 1, i + 1 + n / 8)];
                const uint32_t word = (n % 8) < 4 ? line.first : line.second;
                write(address + n, 1, (word >> (24 - 8 * (n % 4))) & 0xFF);
            }
            break;
        case 0x08:
            if (run && i + 1 < code.lines.size()) {
                const auto [p, inc] = code.lines[i + 1];
                const uint32_t type = p >> 28, count = ((p >> 16) & 0xFFF) + 1, step = p & 0xFFFF;
                const uint32_t size = type == 0 ? 1 : type == 1 ? 2 : 4;
                uint32_t value = v;
                for (uint32_t n = 0; n < count; ++n, value += inc)
                    write(address + n * step, size, size == 1 ? (value & 0xFF) : size == 2 ? (value & 0xFFFF) : value);
            }
            break;
        case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E: {
            if (a & 1) { if (!stack.empty()) stack.pop_back(); }  // an endif first
            const bool parent = active();
            bool cond = false;
            const uint32_t at = address & ~1u;
            if (parent) {
                if (cmd <= 0x26) {
                    const uint32_t m = read32(at);
                    cond = cmd == 0x20 ? m == v : cmd == 0x22 ? m != v : cmd == 0x24 ? m > v : m < v;
                } else {
                    const uint32_t m = (read32(at & ~3u) >> ((at & 2) ? 0 : 16)) & 0xFFFF & ~(v >> 16);
                    const uint32_t want = v & 0xFFFF;
                    cond = cmd == 0x28 ? m == want : cmd == 0x2A ? m != want : cmd == 0x2C ? m > want : m < want;
                }
            }
            stack.push_back(parent && cond);
            break;
        }
        case 0x40: case 0x48: {  // ba / po = [X (+ ba or po)]
            if (!run) break;
            uint32_t at = v;
            if (a & 0x00100000u) at += usePo ? po : ba;
            (cmd == 0x40 ? ba : po) = read32(at);
            break;
        }
        case 0x42: case 0x4A: {  // ba / po = X (+ ba or po)
            if (!run) break;
            uint32_t value = v;
            if (a & 0x00100000u) value += cmd == 0x42 ? ba : po;
            (cmd == 0x42 ? ba : po) = value;
            break;
        }
        case 0x44: case 0x4C:
            if (run) write(v, 4, cmd == 0x44 ? ba : po);
            break;
        case 0x46: case 0x4E:  // ba / po = the next line's address in the code list + XXXX
            if (run) (cmd == 0x46 ? ba : po) = code.window + 8u * uint32_t(i + 1) + uint32_t(int32_t(int16_t(a & 0xFFFFu)));
            break;
        case 0xC0: case 0xC2:  // PowerPC, which the recomp can't run: skipped with its lines (main.dol lists only)
            if (skips) ++skips->powerpcBlocks;
            break;
        case 0xDE: {
            const bool parent = active();
            const uint32_t lo = v & 0xFFFF0000u, hi = v << 16;
            stack.push_back(parent && po >= lo && po < hi);
            break;
        }
        case 0xE0:
            stack.clear();
            if (v >> 16) ba = v & 0xFFFF0000u;
            if (v & 0xFFFF) po = v << 16;
            break;
        case 0xE2: {
            for (uint32_t n = 0; n < (a & 0xFF) && !stack.empty(); ++n) stack.pop_back();  // E2T000VV: VV endifs
            if ((a & 0x00100000u) && !stack.empty()) {  // else
                const bool top = stack.back();
                stack.pop_back();
                stack.push_back(active() && !top);
            }
            break;
        }
        case 0xF0:
            return;
        default:
            // Its length is unknown, so nothing after it can be read as codes.
            if (skips && !skips->unknownType) skips->unknownType = a;
            if (code.fromMain) return;
            break;
        }
        i += ExtraLines(a, v);
    }
}

void LoadCheatFile(const fs::path& path, const std::string& source) {
    std::ifstream in(path);
    std::string line, section;
    std::set<std::string> defaults;
    std::vector<Gecko> codes;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') {
            section = line;
            continue;
        }
        if (section == "[Gecko_Enabled]" && line.front() == '$') {
            defaults.insert(line.substr(1));
        } else if (section == "[Gecko]") {
            if (line.front() == '$') {
                codes.push_back({});
                codes.back().name = line.substr(1);
            } else if (!codes.empty() && line.front() == '*') {
                if (!codes.back().note.empty()) codes.back().note += ' ';
                codes.back().note += line.substr(1);
            } else if (!codes.empty()) {
                unsigned a = 0, v = 0;
                char extra = 0;
                if (std::sscanf(line.c_str(), "%8x %8x%c", &a, &v, &extra) == 2) codes.back().lines.emplace_back(a, v);
            }
        }
    }
    for (auto& code : codes) {
        if (code.lines.empty()) continue;
        for (size_t i = 0; i < code.lines.size(); ++i) {
            if (!SupportedType(Cmd(code.lines[i].first))) {
                code.supported = false;
                code.note = "Runs PowerPC code, which this version can't do.";
                break;
            }
            i += ExtraLines(code.lines[i].first, code.lines[i].second);
        }
        code.enabledByDefault = defaults.count(code.name) != 0;
        if (code.note.empty()) code.note = "From " + source;
        const bool duplicate = std::any_of(g_codes.begin(), g_codes.end(), [&](const Gecko& g) { return g.name == code.name; });
        code.window = kGeckoWindowBase + kGeckoWindowStride * uint32_t(g_codes.size() % 0x100u);
        if (!duplicate) g_codes.push_back(std::move(code));
    }
}

void LoadCheats() {
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    g_codes.clear();
    g_enabled.clear();
    std::error_code ec;
    fs::create_directories(CheatDir(), ec);
    std::vector<std::pair<fs::path, std::string>> files;
    for (const auto& entry : fs::directory_iterator(CheatDir(), ec)) {
        const auto ext = entry.path().extension().string();
        if (ext == ".ini" || ext == ".txt") files.emplace_back(entry.path(), RuntimeConfigFile::PathToUtf8(entry.path().filename()));
    }
    for (const auto& mod : ModInstall::List()) {
        if (!mod.enabled) continue;
        for (const auto& dir : {ModInstall::Folder() / mod.name, ModInstall::Folder() / mod.name / "codes"})
            for (const auto& entry : fs::directory_iterator(dir, ec))
                if (entry.path().extension() == ".ini") files.emplace_back(entry.path(), mod.name);
    }
    std::sort(files.begin(), files.end());
    for (const auto& [path, source] : files)
        if (path.filename() != "enabled.txt") LoadCheatFile(path, source);
    // enabled.txt: "+name" / "-name" lines from the Options menu override the files' [Gecko_Enabled].
    std::map<std::string, bool> chosen;
    std::ifstream state(CheatDir() / "enabled.txt");
    for (std::string line; std::getline(state, line);) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && (line[0] == '+' || line[0] == '-')) chosen[line.substr(1)] = line[0] == '+';
    }
    for (const auto& code : g_codes) {
        const auto it = chosen.find(code.name);
        if (it != chosen.end() ? it->second : code.enabledByDefault) g_enabled.insert(code.name);
    }
    if (!g_codes.empty()) RT_LOGF(RT_TAG_RUNTIME, "cheats: %zu code(s), %zu on\n", g_codes.size(), g_enabled.size());
}

// ---- native code mods ----
struct Hook { OthermHookFn fn; void* user; };
std::unordered_map<uint32_t, std::vector<Hook>> g_hooks;
std::vector<uint64_t> g_hookBits((0x01800000u >> 2) / 64);  // hooked addresses in MEM1
std::vector<std::pair<OthermFrameFn, void*>> g_frameFns;
bool g_loading = false;
thread_local uint32_t g_bypass = 0;  // call_original: the next call to this address skips the hooks
std::vector<std::string> g_modFolders;  // kept alive for api.mod_folder

CpuContext* Ctx(OthermCpu* cpu) { return reinterpret_cast<CpuContext*>(cpu); }

uint32_t FindFunction(const char* name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < kGuestMapSymbolCount; ++i)
        if (std::strcmp(kGuestMapSymbolNames[i], name) == 0) return kGuestMapSymbolAddresses[i];
    return 0;
}

uint32_t ApiCall(OthermCpu* cpu, uint32_t fn, const uint32_t* args, int count) {
    if (!cpu || !fn) return 0;
    CpuContext call = *Ctx(cpu);
    for (int i = 0; i < count && i < 8; ++i) call.gpr[3 + i] = args[i];
    InvokeIndirectCpu(fn, &call);
    return call.gpr[3];
}

uint32_t ApiCallOriginal(OthermCpu* cpu, uint32_t fn) {
    if (!cpu || !fn) return 0;
    g_bypass = fn;
    InvokeIndirectCpu(fn, Ctx(cpu));
    g_bypass = 0;
    return Ctx(cpu)->gpr[3];
}

int ApiHook(uint32_t fn, OthermHookFn cb, void* user) {
    if (!g_loading || !cb || !TranslatedFunctionRegistry::FindByAddressPtr(fn)) return 0;
    g_hooks[fn].push_back({cb, user});
    const uint32_t slot = (fn - 0x80000000u) >> 2;
    if (slot < g_hookBits.size() * 64) g_hookBits[slot >> 6] |= uint64_t(1) << (slot & 63);
    ModHooks::g_active = true;
    return 1;
}

void ApiOnFrame(OthermFrameFn cb, void* user) {
    if (g_loading && cb) g_frameFns.emplace_back(cb, user);
}

const OthermModApi& BaseApi() {
    static const OthermModApi api = [] {
        OthermModApi a{};
        a.version = OTHERM_MOD_API_VERSION;
        a.region = 0;  // USA (R3OE01)
        a.log = [](const char* text) { RT_LOGF(RT_TAG_RUNTIME, "[mod] %s\n", text ? text : ""); };
        a.read8 = [](uint32_t at) { return Memory::Contains(at, 1) ? Memory::Read8(at) : uint8_t(0); };
        a.read16 = [](uint32_t at) { return Memory::Contains(at, 2) ? Memory::Read16(at) : uint16_t(0); };
        a.read32 = [](uint32_t at) { return Memory::Contains(at, 4) ? Memory::Read32(at) : 0u; };
        a.read_f32 = [](uint32_t at) { return Memory::Contains(at, 4) ? Memory::ReadFloat32(at) : 0.0f; };
        a.write8 = [](uint32_t at, uint8_t v) { if (Memory::Contains(at, 1)) Memory::Write8(at, v); };
        a.write16 = [](uint32_t at, uint16_t v) { if (Memory::Contains(at, 2)) Memory::Write16(at, v); };
        a.write32 = [](uint32_t at, uint32_t v) { if (Memory::Contains(at, 4)) Memory::Write32(at, v); };
        a.write_f32 = [](uint32_t at, float v) { if (Memory::Contains(at, 4)) Memory::WriteFloat32(at, v); };
        a.gpr = [](OthermCpu* cpu) { return Ctx(cpu)->gpr; };
        a.get_fpr = [](OthermCpu* cpu, int i) { return i >= 0 && i < 32 ? Ctx(cpu)->fpr[i].d : 0.0; };
        a.set_fpr = [](OthermCpu* cpu, int i, double v) { if (i >= 0 && i < 32) Ctx(cpu)->fpr[i].d = v; };
        a.call = ApiCall;
        a.call_original = ApiCallOriginal;
        a.hook = ApiHook;
        a.on_frame = ApiOnFrame;
        a.find_function = FindFunction;
        return a;
    }();
    return api;
}

void LoadCodeMods() {
    std::error_code ec;
    for (const auto& mod : ModInstall::List()) {
        if (!mod.enabled) continue;
        for (const auto& entry : fs::directory_iterator(ModInstall::Folder() / mod.name / "code", ec)) {
#ifdef _WIN32
            if (entry.path().extension() != ".dll") continue;
            HMODULE lib = LoadLibraryW(entry.path().wstring().c_str());
            auto init = lib ? reinterpret_cast<int (*)(const OthermModApi*)>(GetProcAddress(lib, "otherm_mod_init")) : nullptr;
#else
            if (entry.path().extension() != ".so") continue;
            void* lib = dlopen(entry.path().c_str(), RTLD_NOW | RTLD_LOCAL);
            auto init = lib ? reinterpret_cast<int (*)(const OthermModApi*)>(dlsym(lib, "otherm_mod_init")) : nullptr;
#endif
            const std::string file = RuntimeConfigFile::PathToUtf8(entry.path().filename());
            if (!init) {
                RT_LOGF(RT_TAG_RUNTIME, "code mod %s/%s: not loaded (no otherm_mod_init)\n", mod.name.c_str(), file.c_str());
                continue;
            }
            g_modFolders.push_back(RuntimeConfigFile::PathToUtf8(ModInstall::Folder() / mod.name));
            OthermModApi api = BaseApi();
            api.mod_folder = g_modFolders.back().c_str();
            static std::vector<std::unique_ptr<OthermModApi>> keep;  // a mod may keep the pointer
            keep.push_back(std::make_unique<OthermModApi>(api));
            g_loading = true;
            const int ok = init(keep.back().get());
            g_loading = false;
            RT_LOGF(RT_TAG_RUNTIME, "code mod %s/%s: %s\n", mod.name.c_str(), file.c_str(), ok ? "loaded" : "init failed");
        }
    }
}

// ---- Riivolution memory patches ----
void ApplyRiivolution(uint32_t below) {
    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        if (!overlay.patches) continue;
        for (const auto& write : overlay.patches->memory) {
            const uint32_t size = uint32_t(write.value.size());
            if (write.address >= below || !Memory::Contains(write.address, size)) continue;
            bool matches = true;
            for (uint32_t i = 0; i < write.original.size() && matches; ++i)
                matches = Memory::Contains(write.address + i, 1) && Memory::Read8(write.address + i) == write.original[i];
            if (!matches) continue;
            for (uint32_t i = 0; i < size; ++i) Memory::Write8(write.address + i, write.value[i]);
        }
    }
}

uint32_t Read32Safe(uint32_t at) { return Memory::Contains(at, 4) ? Memory::Read32(at) : 0u; }

void WriteSafe(uint32_t at, uint32_t size, uint32_t value) {
    if (!Memory::Contains(at, size)) return;
    if (size == 1) Memory::Write8(at, uint8_t(value));
    else if (size == 2) Memory::Write16(at, uint16_t(value));
    else Memory::Write32(at, value);
}

// A code's write: into the game's code it also goes to code_patches.cpp, which follows the instructions it knows.
void WriteGame(uint32_t at, uint32_t size, uint32_t value) {
    if (IsCode(at)) CodePatches::Write(at, size, value);
    WriteSafe(at, size, value);
}

bool g_codesChanged = false;  // the cheats that are on changed: the game's code goes back before they run again

// ---- what a mod's main.dol and game.rel bring ----
struct DolImage {
    struct Section { uint32_t offset, address, size; bool text; };
    std::vector<uint8_t> bytes;
    std::vector<Section> sections;
    const uint8_t* At(uint32_t address, uint32_t size) const {
        for (const Section& s : sections)
            if (address >= s.address && address - s.address + uint64_t(size) <= s.size && s.offset + uint64_t(s.size) <= bytes.size())
                return &bytes[s.offset + (address - s.address)];
        return nullptr;
    }
};

uint32_t ModBe32(const std::vector<uint8_t>& b, size_t at) {
    return at + 4 <= b.size() ? uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3] : 0u;
}

std::vector<uint8_t> ModReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

bool ReadDol(const fs::path& path, DolImage& dol) {
    dol.bytes = ModReadFile(path);
    if (dol.bytes.size() < 0x100) return false;
    for (int i = 0; i < 18; ++i) {
        const uint32_t size = ModBe32(dol.bytes, 0x90 + 4 * i);
        if (size) dol.sections.push_back({ModBe32(dol.bytes, 4 * i), ModBe32(dol.bytes, 0x48 + 4 * i), size, i < 7});
    }
    return !dol.sections.empty();
}

// A mod's main.dol: the port runs its own translation of the game's, so what counts is whether the mod's keeps the
// game's code and which Gecko codes a loader (GeckoLoader: a list after 00D0C0DE 00D0C0DE) added to it.
void LoadMainDolCodes(const std::string& label, const fs::path& path) {
    DolImage mod, disc;
    if (!ReadDol(path, mod) || !ReadDol(RuntimeConfigFile::ResolvedDvdRoot() / "sys" / "main.dol", disc)) {
        RT_LOGF(RT_TAG_MOD, "%s: its main.dol could not be read; left out\n", label.c_str());
        return;
    }
    size_t changedCode = 0, changedData = 0, missing = 0;
    uint32_t firstCode = 0;
    for (const auto& section : disc.sections) {
        const uint8_t* theirs = mod.At(section.address, section.size);
        if (!theirs) {
            missing += section.size;
            continue;
        }
        for (uint32_t i = 0; i < section.size; ++i) {
            if (theirs[i] == disc.bytes[section.offset + i]) continue;
            if (section.text && !changedCode) firstCode = section.address + i;
            ++(section.text ? changedCode : changedData);
        }
    }
    Gecko list;
    list.name = label + " (main.dol)";
    list.fromMain = true;
    static constexpr uint8_t kMagic[8] = {0x00, 0xD0, 0xC0, 0xDE, 0x00, 0xD0, 0xC0, 0xDE};
    const auto found = std::search(mod.bytes.begin(), mod.bytes.end(), std::begin(kMagic), std::end(kMagic));
    for (size_t at = size_t(found - mod.bytes.begin()) + 8; found != mod.bytes.end() && at + 8 <= mod.bytes.size(); at += 8) {
        const uint32_t a = ModBe32(mod.bytes, at), v = ModBe32(mod.bytes, at + 4);
        if (a == 0xF0000000u && v == 0) break;
        list.lines.emplace_back(a, v);
    }
    RT_LOGF(RT_TAG_MOD, "%s: main.dol keeps the game's code%s (%zu code and %zu data bytes differ, %zu missing); "
                        "%zu Gecko code lines in it\n",
            label.c_str(), changedCode || missing ? " only in part" : "", changedCode, changedData, missing, list.lines.size());
    if (changedCode)
        RT_LOGF(RT_TAG_MOD, "WARNING %s: its main.dol changes the game's code (first at 0x%08X); the port runs its own "
                            "translation of the game's, so those changes do nothing\n", label.c_str(), firstCode);
    if (list.lines.empty()) return;
    GeckoSkips skips;
    RunGecko(list, [](uint32_t) { return 0u; }, [](uint32_t, uint32_t, uint32_t) {}, &skips);
    RT_LOGF(RT_TAG_MOD, "%s: running its main.dol's codes every frame (%u PowerPC block(s) skipped: the port can't run "
                        "those%s)\n", label.c_str(), skips.powerpcBlocks,
            skips.unknownType ? ", and the list ends at an unknown code type" : "");
    if (skips.unknownType) RT_LOGF(RT_TAG_MOD, "%s: unknown code type in line %08X\n", label.c_str(), skips.unknownType);
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    list.window = kGeckoWindowBase + kGeckoWindowStride * uint32_t(g_codes.size() % 0x100u);
    g_codes.push_back(std::move(list));
}

// The game.rel the game reads (a mod's replacement, or the disc's): its data is used as it is; its code the port
// follows where code_patches.cpp knows the change.
void InspectRel() {
    const fs::path disc = RuntimeConfigFile::ResolvedDvdRoot() / "files" / "rel" / "Release" / "game.rel";
    CodePatches::Init(disc);
    const std::string servedText = DVD_HLE_HostPath("/rel/Release/game.rel");
    const fs::path served = RuntimeConfigFile::PathFromUtf8(servedText);
    std::error_code ec;
    if (servedText.empty() || fs::equivalent(served, disc, ec)) {
        CodePatches::Commit();
        return;
    }
    const std::vector<uint8_t> theirs = ModReadFile(served), ours = ModReadFile(disc);
    constexpr uint32_t kRelImage = 0x8052B280u;  // pre-linked in place: file offset = address - this
    size_t changedCode = 0, followed = 0, changedData = 0;
    uint32_t firstUnknown = 0;
    const uint32_t count = ModBe32(ours, 0x0C), table = ModBe32(ours, 0x10);
    for (uint32_t s = 0; s < count && s < 32; ++s) {
        const uint32_t word = ModBe32(ours, table + 8 * s), size = ModBe32(ours, table + 8 * s + 4);
        const uint32_t offset = word & ~3u;
        if (!offset || !size || offset + uint64_t(size) > std::min(ours.size(), theirs.size())) continue;
        for (uint32_t i = 0; i < size; i += 4) {
            const uint32_t a = ModBe32(ours, offset + i), b = ModBe32(theirs, offset + i);
            if (a == b) continue;
            if (!(word & 1)) {
                changedData += 4;
                continue;
            }
            changedCode += 4;
            if (CodePatches::Write(kRelImage + offset + i, 4, b, false)) followed += 4;
            else if (!firstUnknown) firstUnknown = kRelImage + offset + i;
        }
    }
    CodePatches::Commit();
    RT_LOGF(RT_TAG_MOD, "game.rel from %s (%zu bytes, the disc's %zu): %zu code bytes differ (%zu followed), about %zu data "
                        "bytes differ (used as they are)\n", servedText.c_str(), theirs.size(), ours.size(), changedCode,
            followed, changedData);
    if (theirs.size() != ours.size())
        RT_LOGF(RT_TAG_MOD, "WARNING: that game.rel is not the disc's size; the game's memory layout moves with it and the "
                            "port's code expects the disc's\n");
    if (firstUnknown)
        RT_LOGF(RT_TAG_MOD, "WARNING: that game.rel changes game code the port can't follow (first at 0x%08X); those "
                            "changes do nothing\n", firstUnknown);
}

} // namespace

namespace ModHooks {
bool g_active = false;
}  // namespace ModHooks

extern "C" int g_othermDrawDump;  // aurora command_processor: the presented frame while drawdump.txt's frames run

namespace {
// OTHERM_CALLCOUNT=1 (diagnostics): over the drawdump.txt frames, counts every guest call by (callee, return address) and
// writes one file per presented frame, calls_<frame>.txt in the working directory ("<callee> <lr> <count>" lines). Diffing
// the files of two frames shows which game code ran in only one of them (an effect, a shot, a state change). Inlined leaf
// functions are not calls and do not appear.
struct CallCounter {
    int frame = 0;
    std::unordered_map<uint64_t, uint32_t> counts;
    void Flush() {
        if (!frame || counts.empty()) return;
        const std::string name = "calls_" + std::to_string(frame) + ".txt";
        if (FILE* f = std::fopen(name.c_str(), "a")) {
            for (const auto& [key, n] : counts)
                std::fprintf(f, "%08X %08X %u\n", static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key), n);
            std::fclose(f);
        }
        counts.clear();
    }
};
thread_local CallCounter t_calls;
// OTHERM_CALLSTACK=<hex>[:<bytes>][,...] (with OTHERM_CALLCOUNT, during the drawdump.txt frames): logs the guest call
// stack (the EABI back chain from r1, saved LRs) each time one of these functions is called, and with ":<bytes>" also
// that many bytes at r3 (the object a method runs on) as big-endian words.
struct CallStackTarget { uint32_t address; uint32_t dumpBytes; std::vector<uint32_t> hops; int reg = 3; };
const std::vector<CallStackTarget>& CallStackTargets() {
    static const std::vector<CallStackTarget> list = [] {
        std::vector<CallStackTarget> out;
        const char* v = std::getenv("OTHERM_CALLSTACK");
        for (const char* p = v; p && *p;) {
            char* end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 16);
            if (end == p) break;
            uint32_t bytes = 0;
            if (*end == ':' && end[1] == 's') {
                bytes = UINT32_MAX;  // ":s": print the C strings r5 and r6 point at (a format and its first argument)
                end += 2;
            } else if (*end == ':') {
                bytes = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
            }
            // ":<bytes>@<off>[@<off>...]": follow pointers (r3+off, then that pointer+off, ...) and dump there
            std::vector<uint32_t> hops;
            while (*end == '@') hops.push_back(static_cast<uint32_t>(std::strtoul(end + 1, &end, 0)));
            int reg = 3;  // "#<n>": start from register r<n> instead of r3
            if (*end == '#') reg = static_cast<int>(std::strtoul(end + 1, &end, 10)) & 31;
            out.push_back({static_cast<uint32_t>(a), bytes, std::move(hops), reg});
            p = *end ? end + 1 : end;
        }
        return out;
    }();
    return list;
}
void LogCallStack(uint32_t target, const CpuContext* ctx, uint32_t dumpBytes, const std::vector<uint32_t>& hops = {},
                  int reg = 3) {
    std::string chain;
    uint32_t sp = ctx->gpr[1];
    for (int depth = 0; depth < 32; ++depth) {
        if (!Memory::Contains(sp, 8)) break;
        const uint32_t back = Memory::Read32(sp);
        if (back <= sp || !Memory::Contains(back, 8)) break;
        char item[16];
        std::snprintf(item, sizeof(item), " %08X", Memory::Read32(back + 4));
        chain += item;
        sp = back;
    }
    RT_LOGF(RT_TAG_RUNTIME, "[callstack] f %d %08X lr %08X r3 %08X r4 %08X r5 %08X r6 %08X f1 %g f2 %g f3 %g f31 %g (%016llX) chain%s\n", g_othermDrawDump, target, ctx->lr,
            ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->fpr[1].d, ctx->fpr[2].d, ctx->fpr[3].d, ctx->fpr[31].d,
            static_cast<unsigned long long>(ctx->fpr[31].raw), chain.c_str());
    if (dumpBytes == UINT32_MAX) {
        const auto text = [](uint32_t at) {
            std::string out;
            for (uint32_t i = 0; i < 160 && Memory::Contains(at + i, 1); ++i) {
                const char c = static_cast<char>(Memory::Read8(at + i));
                if (!c) break;
                out += (c >= 32 && c < 127) ? c : '?';
            }
            return out;
        };
        RT_LOGF(RT_TAG_RUNTIME, "[callstr] f %d %08X r5 \"%s\" r6 \"%s\" r7 %08X" "\n", g_othermDrawDump, target,
                text(ctx->gpr[5]).c_str(), text(ctx->gpr[6]).c_str(), ctx->gpr[7]);
        return;
    }
    uint32_t base = ctx->gpr[reg];
    for (const uint32_t hop : hops) base = Memory::Contains(base + hop, 4) ? Memory::Read32(base + hop) : 0;
    if (dumpBytes && Memory::Contains(base, dumpBytes)) {
        std::string words;
        for (uint32_t at = 0; at + 4 <= dumpBytes; at += 4) {
            char item[12];
            std::snprintf(item, sizeof(item), " %08X", Memory::Read32(base + at));
            words += item;
        }
        RT_LOGF(RT_TAG_RUNTIME, "[calldump] f %d %08X this %08X%s" "\n", g_othermDrawDump, target, base, words.c_str());
    }
}
// OTHERM_VMTRACE=<hex> (with OTHERM_CALLCOUNT, during the drawdump.txt frames): traces Other M's script interpreter.
// Every operation its run loop (fn_80093740) dispatches, every resume (fn_80092100) and every native call is recorded,
// and the frame's trace is written to vmtrace_<frame>.txt only if the native at <hex> ran in that frame (so one script's
// run can be picked out by a native only it calls); OTHERM_VMTRACE=1 writes every frame's. Line format: "op <vm> d<depth> <pc> <opcode> <next 12 bytes>".
struct VmTrace {
    uint32_t native = 0;
    int frame = 0;
    bool keep = false;
    std::vector<std::string> lines;
    int keepNext = 0;  // frames after a matching one are written too (the run that follows the native)
    void Flush() {
        if (keep) keepNext = 2;
        else if (keepNext > 0) {
            --keepNext;
            keep = true;
        }
        if (keep && frame && !lines.empty()) {
            const std::string name = "vmtrace_" + std::to_string(frame) + ".txt";
            if (FILE* f = std::fopen(name.c_str(), "w")) {
                for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
                std::fclose(f);
            }
        }
        lines.clear();
        keep = false;
    }
};
thread_local VmTrace t_vm;
const uint32_t g_vmTraceNative = [] {
    const char* v = std::getenv("OTHERM_VMTRACE");
    return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : 0u;
}();
void TraceVm(uint32_t target, const CpuContext* ctx) {
    VmTrace& v = t_vm;
    if (g_othermDrawDump != v.frame) {
        v.Flush();
        v.frame = g_othermDrawDump;
    }
    if (!v.frame) return;
    if (g_vmTraceNative == 1u) v.keep = true;
    char line[160];
    // The run loop's handler dispatch (reached through __ptmf_scall, inlined or not; the return address stays the loop's).
    if (ctx->lr == 0x80093838u && target != 0x8001320Cu) {
        const uint32_t vm = ctx->gpr[3];
        if (!Memory::Contains(vm + 0xB0, 8)) return;
        const uint32_t depth = Memory::Read32(vm + 0xB4);
        const uint32_t frame = Memory::Read32(vm + 0xB0) + (depth - 1) * 16;
        if (!Memory::Contains(frame, 4)) return;
        const uint32_t pc = Memory::Read32(frame);
        if (!Memory::Contains(pc - 2, 14)) return;
        std::snprintf(line, sizeof(line), "op %08X d%u %08X %02X %08X %08X %08X h %08X", vm, depth, pc - 2,
                      Memory::Read16(pc - 2), Memory::Read32(pc), Memory::Read32(pc + 4), Memory::Read32(pc + 8), target);
        v.lines.push_back(line);
    } else if (target == 0x80092100u) {
        std::snprintf(line, sizeof(line), "resume %08X lr %08X", ctx->gpr[3], ctx->lr);
        v.lines.push_back(line);
    } else if (ctx->lr == 0x800A18C4u) {
        std::snprintf(line, sizeof(line), "native %08X", target);
        v.lines.push_back(line);
        if (target == g_vmTraceNative) v.keep = true;
    }
}
// OTHERM_WATCH=<hex address> (with OTHERM_CALLCOUNT, during the drawdump.txt frames): a watchpoint with call
// granularity. The word is compared on every guest call; when it changed, the call that noticed it is logged with the
// guest call stack, so the writer is the function running just before (usually the caller in that stack).
uint32_t g_watchAddress = [] {
    const char* v = std::getenv("OTHERM_WATCH");
    return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : 0u;
}();
// OTHERM_WATCH_ARM=<func>[/<return address>]@<off>[@<off>...]+<off>: sets the watched word when that function is
// called (from that call site): r3, then each "@" loads the pointer at base+off, and "+" adds the final offset. For
// objects whose address changes from run to run (heap layout follows timing).
struct WatchArm { uint32_t function = 0, lr = 0, add = 0; std::vector<uint32_t> hops; };
const WatchArm g_watchArm = [] {
    WatchArm arm;
    const char* v = std::getenv("OTHERM_WATCH_ARM");
    if (!v) return arm;
    char* end = nullptr;
    arm.function = static_cast<uint32_t>(std::strtoul(v, &end, 16));
    if (*end == '/') arm.lr = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
    while (*end == '@') arm.hops.push_back(static_cast<uint32_t>(std::strtoul(end + 1, &end, 0)));
    if (*end == '+') arm.add = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
    return arm;
}();
uint32_t g_watchLast = 0;
bool g_watchPrimed = false;
const bool g_callCountRequested = [] {
    if (!std::getenv("OTHERM_CALLCOUNT")) return false;
    ModHooks::g_active = true;  // routes every translated call through ModHooks::Run
    return true;
}();
}  // namespace

namespace ModHooks {
bool Run(uint32_t target, CpuContext* ctx) {
    if (g_callCountRequested) {
        if (g_vmTraceNative && ctx) TraceVm(target, ctx);
        if (g_watchArm.function == target && ctx && (!g_watchArm.lr || g_watchArm.lr == ctx->lr)) {
            uint32_t at = ctx->gpr[3];
            for (const uint32_t hop : g_watchArm.hops) at = Memory::Contains(at + hop, 4) ? Memory::Read32(at + hop) : 0;
            g_watchAddress = at + g_watchArm.add;
            g_watchPrimed = false;
            RT_LOGF(RT_TAG_RUNTIME, "[watch] f %d armed at %08X\n", g_othermDrawDump, g_watchAddress);
        }
        if (g_watchAddress && ctx && g_othermDrawDump && Memory::Contains(g_watchAddress, 4)) {
            const uint32_t now = Memory::Read32(g_watchAddress);
            if (g_watchPrimed && now != g_watchLast) {
                RT_LOGF(RT_TAG_RUNTIME, "[watch] f %d %08X: %08X -> %08X\n", g_othermDrawDump, g_watchAddress, g_watchLast, now);
                LogCallStack(target, ctx, 0);
            }
            g_watchLast = now;
            g_watchPrimed = true;
        }
        CallCounter& c = t_calls;
        if (g_othermDrawDump != c.frame) {
            c.Flush();
            c.frame = g_othermDrawDump;
        }
        if (c.frame) {
            ++c.counts[(static_cast<uint64_t>(target) << 32) | (ctx ? ctx->lr : 0u)];
            if (ctx) {
                for (const CallStackTarget& t : CallStackTargets())
                    if (t.address == target) LogCallStack(target, ctx, t.dumpBytes, t.hops, t.reg);
            }
        }
    }
    // Most calls are to functions no mod hooked: one bit per 4-byte address of MEM1 answers that.
    const uint32_t slot = (target - 0x80000000u) >> 2;
    if (slot >= g_hookBits.size() * 64 || !(g_hookBits[slot >> 6] >> (slot & 63) & 1)) return false;
    if (g_bypass == target) {
        g_bypass = 0;
        return false;
    }
    const auto it = g_hooks.find(target);
    if (it == g_hooks.end()) return false;
    for (const Hook& hook : it->second)
        if (hook.fn(reinterpret_cast<OthermCpu*>(ctx), hook.user)) return true;
    return false;
}
} // namespace ModHooks

namespace ModRuntime {

void Load() {
    AddDolTextRanges();
    LoadCodeMods();
    LoadCheats();
}

void ReloadCheats() { LoadCheats(); }

void ApplyEarlyPatches() {
    uint32_t arenaLo = 0;  // the end of the main program (boot info ArenaLo)
    if (Memory::TryRead32(0x80000030u, arenaLo) && arenaLo > 0x80004000u) ApplyRiivolution(arenaLo);  /* OTHERM-UNMAPPED */
}

void Frame(CpuContext* cpu) {
    // Other M: game.rel is linked during the boot steps; __OSModuleInfoList (0x800030C8) is non-zero from then on.
    uint32_t linked = 0;
    Memory::TryRead32(0x800030C8u, linked);
    if (linked == 0) return;
    static bool patched = false;
    static bool described = false;
    if (!patched) {
        patched = true;
        AddRelTextRanges();
        ApplyRiivolution(0xFFFFFFFFu);
        // The game's code as the mods bring it, then the Gecko codes their main.dol carries (enabled mods only:
        // a disabled mod is not an overlay).
        InspectRel();
        for (const auto& overlay : RuntimeRiivolution::Overlays())
            if (overlay.patches && overlay.patches->mainDol)
                LoadMainDolCodes(overlay.label.empty() ? RuntimeConfigFile::PathToUtf8(overlay.root.filename()) : overlay.label,
                                 *overlay.patches->mainDol);
    }
    for (const auto& [fn, user] : g_frameFns) fn(reinterpret_cast<OthermCpu*>(cpu), user);
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    if (g_codesChanged) {
        g_codesChanged = false;
        CodePatches::Reset();
    }
    for (auto& code : g_codes) {
        if (!code.supported || (!code.fromMain && !g_enabled.count(code.name))) continue;
        if (!code.fromMain && !code.checkedCode && g_relRangesAdded) {
            // A code that only rewrites the game's instructions does nothing in a recomp, unless the port follows
            // those instructions (code_patches.cpp): say so instead.
            code.checkedCode = true;
            bool any = false, data = false;
            RunGecko(code, Read32Safe,
                     [&](uint32_t at, uint32_t, uint32_t) { any = true; data |= !IsCode(at) || CodePatches::Knows(at); });
            if (any && !data) {
                code.supported = false;
                code.note = "Changes the game's code, which this version can't do.";
                RT_LOGF(RT_TAG_RUNTIME, "cheat \"%s\" only rewrites game code: turned off\n", code.name.c_str());
                continue;
            }
        }
        RunGecko(code, Read32Safe, WriteGame);
    }
    if (!described) {
        described = true;
        const std::string on = CodePatches::Describe();
        if (!on.empty()) RT_LOGF(RT_TAG_MOD, "game code the mods change, followed: %s\n", on.c_str());
    }
    // OTHERM_TEST_CALL="<frame>:<function>[:<address>=<value>],..." (tests): at that presented frame, calls a game
    // function that takes no arguments (rel_805F602C, the cutscene suit model) and logs r3; with a word set for the
    // call only (put back right after), e.g. the suit bits at 0x80CA4E20.
    struct TestCall { uint32_t frame, function, poke, value; bool hasPoke; };
    static const std::vector<TestCall> calls = [] {
        std::vector<TestCall> out;
        std::stringstream items(std::getenv("OTHERM_TEST_CALL") ? std::getenv("OTHERM_TEST_CALL") : "");
        for (std::string item; std::getline(items, item, ',');) {
            TestCall c{};
            unsigned frame = 0, function = 0, poke = 0, value = 0;
            const int n = std::sscanf(item.c_str(), "%u:%x:%x=%x", &frame, &function, &poke, &value);
            if (n < 2) continue;
            out.push_back({frame, function, poke, value, n == 4});
        }
        return out;
    }();
    static size_t nextCall = 0;
    for (const uint32_t frame = VI_HLE_PresentedFrames(); cpu && nextCall < calls.size() && frame >= calls[nextCall].frame;
         ++nextCall) {
        const TestCall& c = calls[nextCall];
        uint32_t old = 0;
        if (c.hasPoke && Memory::TryRead32(c.poke, old)) Memory::Write32(c.poke, c.value);
        CpuContext call = *cpu;
        InvokeIndirectCpu(c.function, &call);
        if (c.hasPoke) Memory::Write32(c.poke, old);
        char with[48] = "";
        if (c.hasPoke) std::snprintf(with, sizeof with, " (with 0x%08X = 0x%08X)", c.poke, c.value);
        RT_LOGF(RT_TAG_MOD, "test call 0x%08X at frame %u: r3 = %u%s\n", c.function, frame, call.gpr[3], with);
    }
}

std::vector<CheatCode> CheatCodes() {
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    std::vector<CheatCode> out;
    for (const auto& code : g_codes)
        if (!code.fromMain) out.push_back({code.name, code.note, g_enabled.count(code.name) != 0, code.supported});
    return out;
}

void SetCheatEnabled(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(g_cheatMutex);
    if (enabled) g_enabled.insert(name);
    else g_enabled.erase(name);
    g_codesChanged = true;
    std::ofstream state(CheatDir() / "enabled.txt", std::ios::trunc);
    for (const auto& code : g_codes) state << (g_enabled.count(code.name) ? '+' : '-') << code.name << '\n';
}

std::string CheatFolder() { return RuntimeConfigFile::PathToUtf8(CheatDir()); }

bool SelfCheck() {
    // 04 write, a 16-bit fill, a string, and an if-equal that is false (its write must not happen) then an endif.
    std::array<uint8_t, 64> ram{};
    const uint32_t base = 0x80001000u;
    auto read32 = [&](uint32_t at) {
        const uint32_t o = at - base;
        return o + 4 <= ram.size() ? uint32_t(ram[o]) << 24 | uint32_t(ram[o + 1]) << 16 | uint32_t(ram[o + 2]) << 8 | ram[o + 3] : 0u;
    };
    auto write = [&](uint32_t at, uint32_t size, uint32_t value) {
        const uint32_t o = at - base;
        for (uint32_t i = 0; i < size && o + i < ram.size(); ++i) ram[o + i] = uint8_t(value >> (8 * (size - 1 - i)));
    };
    Gecko code;
    code.lines = {{0x04001000u, 0x11223344u}, {0x02001004u, 0x0001ABCDu}, {0x06001008u, 0x00000003u}, {0x41424300u, 0},
                  {0x20001000u, 0x00000000u}, {0x04001010u, 0xDEADBEEFu}, {0xE2000001u, 0}, {0x04001014u, 0x55555555u}};
    RunGecko(code, read32, write);
    const bool basic = read32(base) == 0x11223344u && read32(base + 4) == 0xABCDABCDu && ram[8] == 'A' &&
                       ram[10] == 'C' && read32(base + 0x10) == 0 && read32(base + 0x14) == 0x55555555u;
    // A main.dol list: po pointed at a word of the list itself (4E), a PowerPC block skipped with its lines (C2), a
    // po-based if-not-equal reading that word, then a write it allows; an unknown type ends the list.
    ram.fill(0);
    Gecko list;
    list.fromMain = true;
    list.window = kGeckoWindowBase;
    list.lines = {{0x4E000010u, 0}, {0xC2000000u, 2}, {0x11111111u, 0x22222222u}, {0xDDDDDDDDu, 0x33333333u},
                  {0x32000000u, 0}, {0x04001018u, 0xAABBCCDDu}, {0xE0000000u, 0x80008000u},
                  {0x8C000000u, 0}, {0x0400101Cu, 0x12345678u}};
    GeckoSkips skips;
    RunGecko(list, read32, write, &skips);
    const bool mainList = read32(base + 0x18) == 0xAABBCCDDu && read32(base + 0x1C) == 0 && skips.powerpcBlocks == 1 &&
                          skips.unknownType == 0x8C000000u;
    return basic && mainList && CodePatches::SelfCheck();
}

} // namespace ModRuntime
