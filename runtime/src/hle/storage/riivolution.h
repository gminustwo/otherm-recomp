// Riivolution overlay loading: host-IO shim around hle/riivolution_contract.h. Discovers DVD
// overlay roots (Config.toml/CLI/recomp mod manifest), parses each root's XML, applies option
// selections, and exposes the resulting disc mappings (dvd.cpp) and savegame redirect (nand_fs.cpp).
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace RuntimeRiivolution {

struct Mapping {
    enum class Kind {
        File,         // discPath is a file path
        Folder,       // discPath is a directory prefix
        FolderByName, // no disc path: replace disc files matching by filename
    };

    Kind kind = Kind::File;
    std::string discPath;
    std::filesystem::path hostPath;
    bool recursive = true;
    // Riivolution 'create': when false, only files already present on the disc
    // may be replaced; nothing new is added.
    bool create = false;
};

struct PatchSet {
    // In Dolphin's application order (per active patch: files, then folders);
    // a later mapping for the same disc path wins.
    std::vector<Mapping> mappings;
    uint32_t skippedExternals = 0; // externals that do not exist on the host
    // <memory> patches with an inline value (applied by mod_runtime.cpp); `original`, when given, must match first.
    struct MemoryWrite {
        uint32_t address = 0;
        std::vector<uint8_t> value, original;
    };
    std::vector<MemoryWrite> memory;
    // A replacement main.dol (<file disc="main.dol">). The port runs its own translation of the game's main.dol, so
    // mod_runtime.cpp reads what it can use from this one (the Gecko codes a loader such as GeckoLoader put in it).
    std::optional<std::filesystem::path> mainDol;
};

struct Overlay {
    std::filesystem::path root;
    // nullopt: the root carries no Riivolution XML and is a plain disc-shaped
    // overlay directory.
    std::optional<PatchSet> patches;
    std::string label;  // the mod's folder name, for the log ("" for roots that are not in the mods folder)
};

struct SaveRedirect {
    std::filesystem::path hostDirectory;
    bool clone = false;
};

// Discovered overlay roots in precedence order (highest priority first), each
// with its active Riivolution mappings. Loaded once, thread-safe.
const std::vector<Overlay>& Overlays();

// The active <savegame> redirect from the highest-priority overlay that has
// one, resolved to a host directory (Dolphin semantics: the whole NAND
// /title/<id>/data directory is redirected there).
const std::optional<SaveRedirect>& GetSaveRedirect();

// Options > Mods: the Riivolution options of the pack in a mod folder, with the choice each gets at the next start.
// A pack is a riivolution/ folder of XMLs (any case), an XML beside the mod's files, or Other M Redux's own files
// (redux.rel / redux.dol beside model/, movie/, ...), which the port describes itself. Empty for other mods.
struct PackOption {
    std::string id;  // Dolphin's config identifier: the option's id, or its section's name and its own
    std::string section, name;
    std::vector<std::string> choices;
    uint32_t selected = 0;  // 0: off, else the 1-based choice
};
std::vector<PackOption> PackOptions(const std::filesystem::path& modFolder);
// Remembers a choice in the mod folder (riivolution/config/<game>.xml, Dolphin's format) for the next start.
bool SetPackOption(const std::filesystem::path& modFolder, const std::string& id, uint32_t choice);
// The folder holding a mod's Riivolution pack or Other M Redux's files, `dir` itself or one up to `depth` levels down
// (mod_install.cpp: where an unpacked zip's mod starts). Empty when there is none.
std::filesystem::path FindPackRoot(const std::filesystem::path& dir, int depth);

} // namespace RuntimeRiivolution
