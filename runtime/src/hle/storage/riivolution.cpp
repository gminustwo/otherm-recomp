// Riivolution overlay loading (host-IO shim). See riivolution.h.
//
// Ported behavior from Dolphin Emulator's DiscIO/RiivolutionParser and
// RiivolutionPatcher (https://github.com/dolphin-emu/dolphin).
// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/storage/riivolution.h"

#include "hle/riivolution_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "nand_path.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

struct RiivoState {
    std::vector<RuntimeRiivolution::Overlay> overlays;
    std::optional<RuntimeRiivolution::SaveRedirect> saveRedirect;
};

std::once_flag g_riivoOnce;
RiivoState g_riivoState;

std::optional<std::string> RiivoReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Six-character game ID ("R3OE01") from guest low memory, with the same R3OE
// fallback the rest of the storage HLE uses for boots that have not written
// the disc header yet.
std::string RiivoGameId() {
    const uint32_t code = RuntimeHle::CurrentGameCode(0x52334F45u); // "R3OE"
    std::string id(6, '\0');
    id[0] = static_cast<char>((code >> 24) & 0xffu);
    id[1] = static_cast<char>((code >> 16) & 0xffu);
    id[2] = static_cast<char>((code >> 8) & 0xffu);
    id[3] = static_cast<char>(code & 0xffu);

    uint16_t maker = 0;
    if (Memory::Contains(0x80000004u, 2u)) {
        maker = Memory::Read16(0x80000004u);
    }
    const char makerHi = static_cast<char>((maker >> 8) & 0xffu);
    const char makerLo = static_cast<char>(maker & 0xffu);
    if (std::isalnum(static_cast<unsigned char>(makerHi)) &&
        std::isalnum(static_cast<unsigned char>(makerLo))) {
        id[4] = makerHi;
        id[5] = makerLo;
    } else {
        id[4] = '0';
        id[5] = '1';
    }
    return id;
}

// Every narrow path string here is UTF-8, including the ones the XML halves of
// resolved paths are concatenated with.
using RuntimeConfigFile::PathFromUtf8;
using RuntimeConfigFile::PathToUtf8;

std::string RiivoGenericText(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string RiivoComparablePath(const fs::path& path) {
    std::string text = RiivoGenericText(path.lexically_normal());
#ifdef _WIN32
    RuntimeHle::LowerInPlace(text);
#endif
    return text;
}

void RiivoAddRoot(std::vector<RuntimeRiivolution::Overlay>& overlays, fs::path root,
                  const char* source, std::string label = {}) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        RT_LOG(RT_TAG_RIIVOLUTION) << "rejected overlay root (" << (source ? source : "unknown")
                  << "): " << PathToUtf8(root) << " is not a reachable directory" << std::endl;
        return;
    }

    fs::path normalized = fs::weakly_canonical(root, ec);
    if (ec) {
        normalized = root.lexically_normal();
    }

    const std::string comparable = RiivoComparablePath(normalized);
    const auto duplicate =
        std::find_if(overlays.begin(), overlays.end(), [&](const RuntimeRiivolution::Overlay& overlay) {
            return RiivoComparablePath(overlay.root) == comparable;
        });
    if (duplicate != overlays.end()) {
        return;
    }

    RT_LOG(RT_TAG_RIIVOLUTION) << "overlay root (" << (source ? source : "unknown")
              << "): " << PathToUtf8(normalized) << std::endl;
    overlays.push_back({std::move(normalized), std::nullopt, std::move(label)});
}

// A child folder by name in any case: Dolphin's Load folder has "Riivolution", the SD card layout "riivolution",
// and Linux keeps the difference.
fs::path RiivoChildDir(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    if (fs::is_directory(dir / fs::path(std::string(name)), ec)) return dir / fs::path(std::string(name));
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string child = PathToUtf8(entry.path().filename());
        RuntimeHle::LowerInPlace(child);
        if (child == name && entry.is_directory(ec)) return entry.path();
    }
    return {};
}

bool RiivoIsXmlFile(const fs::directory_entry& entry) {
    std::error_code ec;
    std::string ext = PathToUtf8(entry.path().extension());
    RuntimeHle::LowerInPlace(ext);
    return ext == ".xml" && entry.is_regular_file(ec);
}

// The *.xml files directly in `dir` that are Riivolution discs (<wiidisc>), sorted.
std::vector<fs::path> RiivoDiscXmlsIn(const fs::path& dir) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!RiivoIsXmlFile(entry)) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string head(512, '\0');
        in.read(head.data(), std::streamsize(head.size()));
        head.resize(size_t(in.gcount()));
        if (head.find("<wiidisc") != std::string::npos) found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

// Other M Redux's own files: its release zip is the contents of its "Other M Redux" folder (redux.dol and redux.rel
// beside message/, model/, movie/ and sound/) without the Riivolution XML. `dir` or a folder up to `depth` below it.
fs::path RiivoFindReduxContent(const fs::path& dir, int depth = 2) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "redux.rel", ec) || fs::is_regular_file(dir / "redux.dol", ec)) return dir;
    if (depth <= 0) return {};
    std::vector<fs::path> subs;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_directory(ec)) subs.push_back(entry.path());
    std::sort(subs.begin(), subs.end());
    for (const fs::path& sub : subs)
        if (fs::path found = RiivoFindReduxContent(sub, depth - 1); !found.empty()) return found;
    return {};
}

// The Riivolution description of Other M Redux's files when no XML comes with them: Redux's own XML's section,
// options and patch ids (so a remembered choice means the same either way), its externals relative to the folder
// that holds the files, and its Dolphin descriptors' defaults (everything but the Maxximum Edition soundtrack).
const char* const kReduxBuiltinXml = R"(<wiidisc version="1">
  <id game="R3O"/>
  <options>
    <section name="Other M Redux">
      <option name="Patched DOL and REL" default="1"><choice name="Redux"><patch id="Patched DOL and REL"/></choice></option>
      <option name="3D Models" default="1"><choice name="Redux"><patch id="3D Models"/></choice></option>
      <option name="Audio and SFX" default="1"><choice name="Redux"><patch id="Audio and SFX"/></choice></option>
      <option name="Maxximum OST" default="0"><choice name="Redux"><patch id="Maxximum OST"/></choice></option>
      <option name="Original Title Screen BGM" default="1"><choice name="Redux"><patch id="Original Title Screen BGM"/></choice></option>
      <option name="CG Movie Cutscenes" default="1"><choice name="Redux"><patch id="CG Movie Cutscenes"/></choice></option>
      <option name="Text, Messages and Subtitles" default="1"><choice name="Redux"><patch id="Text, Messages and Subtitles"/></choice></option>
    </section>
  </options>
  <patch id="Patched DOL and REL"><file external="/redux.dol" disc="main.dol"/><file external="/redux.rel" disc="/rel/Release/game.rel"/></patch>
  <patch id="3D Models"><folder external="/model" disc="/" recursive="false"/></patch>
  <patch id="Audio and SFX"><folder external="/sound/bgm" disc="/sound/bgm/"/></patch>
  <patch id="Maxximum OST"><folder external="/sound/ost" disc="/sound/bgm/"/></patch>
  <patch id="Original Title Screen BGM"><folder external="/sound/title" disc="/sound/bgm/"/></patch>
  <patch id="CG Movie Cutscenes"><folder external="/movie" disc="/movie/"/></patch>
  <patch id="Text, Messages and Subtitles"><folder external="/message" disc="/message/"/></patch>
</wiidisc>
)";

// What a mod folder holds that makes it a Riivolution pack.
bool RiivoIsPack(const fs::path& mod) {
    const fs::path xmlDir = RiivoChildDir(mod, "riivolution");
    return (!xmlDir.empty() && !RiivoDiscXmlsIn(xmlDir).empty()) || !RiivoDiscXmlsIn(mod).empty() ||
           !RiivoFindReduxContent(mod).empty();
}

std::vector<RuntimeRiivolution::Overlay> RiivoDiscoverRoots() {
    std::vector<RuntimeRiivolution::Overlay> overlays;

    // Discovery order is precedence order: DVDInit applies the roots in
    // reverse, so an explicitly configured root (command line first, then
    // Config.toml) always outranks the mod manifest.
    for (const auto& root : RuntimeConfigFile::OverlayRoots()) {
        // A relative overlay root resolves against the config file that named
        // it, never against whatever working directory the process happened
        // to be started in.
        RiivoAddRoot(overlays, RuntimeNandPath::ResolveConfiguredPath(root),
                     "command line or Config.toml");
    }

    for (const auto& root : RecompMod::DvdOverlayRoots()) {
        RiivoAddRoot(overlays, root, "recomp mod manifest");
    }

    // Mods: <UserData>/mods/<name>/files mirrors the disc's files/ tree. Folders load
    // in name order and a later name overrides an earlier one (roots apply in reverse, so add them
    // descending); a file named "disabled" inside the mod folder turns it off (Options > Mods).
    std::vector<fs::path> mods;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(RuntimeConfigFile::ApplicationDataDirectory() / "mods", ec)) {
        if (entry.is_directory(ec) && !fs::exists(entry.path() / "disabled", ec) &&
            fs::is_directory(entry.path() / "files", ec)) {
            mods.push_back(entry.path());
        }
    }
    for (const auto& entry : fs::directory_iterator(RuntimeConfigFile::ApplicationDataDirectory() / "mods", ec)) {
        if (entry.is_directory(ec) && !fs::exists(entry.path() / "disabled", ec) &&
            !fs::is_directory(entry.path() / "files", ec) && RiivoIsPack(entry.path())) {
            mods.push_back(entry.path());  // a Riivolution pack: its XML (or Redux's own layout) says what it replaces
        }
    }
    std::sort(mods.begin(), mods.end(), std::greater<>());
    for (const auto& mod : mods) {
        RiivoAddRoot(overlays, fs::is_directory(mod / "files", ec) ? mod / "files" : mod, "mod",
                     PathToUtf8(mod.filename()));
    }
    // Built into the game: <exe folder>/game_files mirrors the disc's files/ tree (the packaged HQ music lives
    // there). Lowest precedence, so any mod can still override it.
    if (const auto exeDir = RuntimeConfigFile::ExecutableDirectory()) {
        if (fs::is_directory(*exeDir / "game_files", ec)) {
            RiivoAddRoot(overlays, *exeDir / "game_files", "built-in game files");
        }
    }

    return overlays;
}

// The Riivolution XMLs describing an overlay root, plus the SD-root directory
// that absolute external paths resolve against.
struct RiivoXmlSource {
    fs::path path;     // the XML file, or the content folder for the built-in Redux description
    std::string text;  // the XML
};
struct RiivoXmlSet {
    fs::path sdRoot;
    std::vector<RiivoXmlSource> xmls;
    fs::path configDirectory;  // riivolution/config: the remembered choices (Dolphin's format)
    bool builtin = false;      // Other M Redux's files, described by the port
};

// `forceBuiltin`: Other M Redux's files with the port's own description even when an XML is there too (used when
// that XML's files are not where it says).
std::optional<RiivoXmlSet> RiivoFindXmls(const fs::path& overlayRoot, bool forceBuiltin = false) {
    std::error_code ec;
    const auto read = [](const std::vector<fs::path>& files) {
        std::vector<RiivoXmlSource> sources;
        for (const fs::path& file : files)
            if (auto text = RiivoReadFile(file)) sources.push_back({file, std::move(*text)});
        return sources;
    };
    fs::path riivolutionDir = RiivoChildDir(overlayRoot, "riivolution");
    const fs::path configDirectory = (riivolutionDir.empty() ? overlayRoot / "riivolution" : riivolutionDir) / "config";

    if (!forceBuiltin) {
        // Distribution-pinned XML from recomp.yml: a path relative to the pack
        // folder. The pack folder sits inside the virtual SD root (e.g.
        // <sd>/<pack>), so externals resolve against the root's parent.
        const std::string& configured = RecompMod::RiivolutionXml();
        if (!configured.empty()) {
            const fs::path configuredXml = overlayRoot / PathFromUtf8(configured);
            if (fs::is_regular_file(configuredXml, ec)) {
                return RiivoXmlSet{overlayRoot.parent_path(), read({configuredXml}), configDirectory};
            }
        }

        // Dolphin-style discovery: the overlay root is itself a virtual SD root carrying <root>/riivolution/*.xml
        // (any case); otherwise an XML beside the mod's files (a pack unpacked as it was downloaded).
        if (!riivolutionDir.empty()) {
            if (auto xmls = read(RiivoDiscXmlsIn(riivolutionDir)); !xmls.empty()) {
                return RiivoXmlSet{overlayRoot, std::move(xmls), configDirectory};
            }
        }
        if (auto xmls = read(RiivoDiscXmlsIn(overlayRoot)); !xmls.empty()) {
            return RiivoXmlSet{overlayRoot, std::move(xmls), configDirectory};
        }
    }

    // Other M Redux's files without its XML (its release zip): the port's description of them.
    if (const fs::path content = RiivoFindReduxContent(overlayRoot); !content.empty()) {
        return RiivoXmlSet{content, {{content, kReduxBuiltinXml}}, configDirectory, true};
    }
    return std::nullopt;
}

// Defaults for the options a pack's XML gives no default (applied before the remembered choices): the choices of a
// Dolphin game-mod descriptor in the pack (*.json, "dolphin-game-mod-descriptor"), else what the port knows of the
// pack, else the option's first choice (as picking it in the Riivolution menu would).
std::vector<RiivolutionContract::OptionSelection> RiivoDescriptorSelections(const fs::path& dir) {
    std::vector<RiivolutionContract::OptionSelection> selections;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string ext = PathToUtf8(entry.path().extension());
        RuntimeHle::LowerInPlace(ext);
        if (ext != ".json" || !entry.is_regular_file(ec)) continue;
        const auto text = RiivoReadFile(entry.path());
        if (!text || text->find("dolphin-game-mod-descriptor") == std::string::npos) continue;
        // The options are flat objects: {"choice": N, "option-name": "...", "section-name": "..."}, in any order.
        static const std::regex object(R"(\{[^{}]*\})");
        static const std::regex choice(R"re("choice"\s*:\s*(\d+))re");
        static const std::regex option(R"re("option-name"\s*:\s*"((?:[^"\\]|\\.)*)")re");
        static const std::regex section(R"re("section-name"\s*:\s*"((?:[^"\\]|\\.)*)")re");
        const auto unescape = [](std::string value) {
            std::string out;
            for (size_t i = 0; i < value.size(); ++i) out += (value[i] == '\\' && i + 1 < value.size()) ? value[++i] : value[i];
            return out;
        };
        for (auto it = std::sregex_iterator(text->begin(), text->end(), object); it != std::sregex_iterator(); ++it) {
            const std::string body = it->str();
            std::smatch c, o, s;
            if (!std::regex_search(body, c, choice) || !std::regex_search(body, o, option)) continue;
            RiivolutionContract::OptionSelection selection;
            selection.option = unescape(o[1].str());
            if (std::regex_search(body, s, section)) selection.section = unescape(s[1].str());
            selection.choice = static_cast<uint32_t>(std::stoul(c[1].str()));
            selections.push_back(std::move(selection));
        }
        if (!selections.empty()) break;
    }
    return selections;
}

void RiivoApplyDefaults(RiivolutionContract::Disc& disc, const fs::path& sdRoot, const fs::path& overlayRoot) {
    auto descriptor = RiivoDescriptorSelections(overlayRoot);
    if (descriptor.empty() && sdRoot != overlayRoot) descriptor = RiivoDescriptorSelections(sdRoot);
    for (auto& section : disc.sections) {
        for (auto& option : section.options) {
            if (option.choices.empty()) continue;
            const auto chosen = std::find_if(descriptor.begin(), descriptor.end(), [&](const auto& selection) {
                return selection.option == option.name && (selection.section.empty() || selection.section == section.name);
            });
            if (chosen != descriptor.end() && chosen->choice <= option.choices.size()) {
                option.selectedChoice = chosen->choice;
            } else if (option.hasDefault) {
                // the XML's own default stands
            } else if (section.name == "Other M Redux" && option.name == "Maxximum OST") {
                option.selectedChoice = 0;  // Redux's optional soundtrack: off in its own Dolphin descriptors
            } else if (option.selectedChoice == 0) {
                option.selectedChoice = 1;
            }
        }
    }
}

std::vector<RiivolutionContract::OptionSelection> RiivoManifestSelections();

// The pack's discs for `gameId`, with the defaults, the remembered choices and the manifest's pins applied.
struct RiivoParsedXml {
    fs::path path;
    RiivolutionContract::Disc disc;
};
std::vector<RiivoParsedXml> RiivoParseXmls(const RiivoXmlSet& xmlSet, const fs::path& overlayRoot,
                                          const std::string& gameId, bool log) {
    std::optional<RiivolutionContract::Config> config;
    if (const auto configText = RiivoReadFile(xmlSet.configDirectory / (gameId.substr(0, 4) + ".xml"))) {
        config = RiivolutionContract::ParseConfigString(*configText);
    }
    const auto manifestSelections = RiivoManifestSelections();
    std::vector<RiivoParsedXml> parsed;
    for (const auto& source : xmlSet.xmls) {
        auto disc = RiivolutionContract::ParseString(source.text);
        if (!disc) {
            if (log) {
                RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: " << PathToUtf8(source.path)
                          << " is not a valid Riivolution XML (version 1 wiidisc); ignoring it" << std::endl;
            }
            continue;
        }
        if (!disc->IsValidForGame(gameId, std::nullopt, std::nullopt)) {
            if (log) {
                RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(source.path) << ": not valid for " << gameId
                          << ", skipped" << std::endl;
            }
            continue;
        }
        RiivoApplyDefaults(*disc, xmlSet.sdRoot, overlayRoot);
        if (config) RiivolutionContract::ApplyConfigDefaults(*disc, *config);
        RiivolutionContract::ApplySelections(*disc, manifestSelections);
        parsed.push_back({source.path, std::move(*disc)});
    }
    return parsed;
}

std::vector<RiivolutionContract::OptionSelection> RiivoManifestSelections() {
    std::vector<RiivolutionContract::OptionSelection> selections;
    for (const auto& selection : RecompMod::RiivolutionOptionSelections()) {
        selections.push_back({selection.section, selection.option, selection.choice});
    }
    return selections;
}

void RiivoCollectMappings(const RiivolutionContract::Patch& patch, const std::string& sdRootGeneric,
                          const std::string& xmlDirGeneric, RuntimeRiivolution::PatchSet& set) {
    const std::string patchRoot =
        RiivolutionContract::ResolvePatchRoot(sdRootGeneric, xmlDirGeneric, patch.root);
    std::error_code ec;

    // Dolphin applies each patch's file patches first, then its folder
    // patches; with last-registration-wins that means folders shadow files
    // within one patch, and later patches shadow earlier ones.
    for (const auto& file : patch.filePatches) {
        if (file.disc.empty()) {
            ++set.skippedExternals;
            continue;
        }
        const auto resolved =
            RiivolutionContract::MakeAbsoluteFromRelative(sdRootGeneric, patchRoot, file.external);
        if (!resolved) {
            ++set.skippedExternals;
            continue;
        }
        const fs::path hostFile = PathFromUtf8(*resolved);
        if (!fs::is_regular_file(hostFile, ec)) {
            ++set.skippedExternals;
            continue;
        }
        // "main.dol" is the program itself in Riivolution (Dolphin swaps the DOL). The port runs its own translation
        // of the game's main.dol; mod_runtime.cpp takes what it can from this one.
        std::string disc = file.disc;
        while (!disc.empty() && disc.front() == '/') disc.erase(0, 1);
        RuntimeHle::LowerInPlace(disc);
        if (disc == "main.dol" || disc == "sys/main.dol") {
            set.mainDol = hostFile;
            continue;
        }
        set.mappings.push_back(
            {RuntimeRiivolution::Mapping::Kind::File, file.disc, hostFile, true, file.create});
    }

    for (const auto& folder : patch.folderPatches) {
        const auto resolved =
            RiivolutionContract::MakeAbsoluteFromRelative(sdRootGeneric, patchRoot, folder.external);
        if (!resolved) {
            ++set.skippedExternals;
            continue;
        }
        const fs::path hostFolder = PathFromUtf8(*resolved);
        if (!fs::is_directory(hostFolder, ec)) {
            ++set.skippedExternals;
            continue;
        }
        const auto kind = folder.disc.empty() ? RuntimeRiivolution::Mapping::Kind::FolderByName
                                              : RuntimeRiivolution::Mapping::Kind::Folder;
        set.mappings.push_back({kind, folder.disc, hostFolder, folder.recursive, folder.create});
    }
}

// The mappings of a pack's active options. `state` (the save redirect) and the log only at the real load.
RuntimeRiivolution::PatchSet RiivoBuildPatchSet(const RiivoXmlSet& xmlSet, const fs::path& overlayRoot,
                                               RiivoState* state, bool log) {
    const std::string gameId = RiivoGameId();
    const std::string sdRootGeneric = RiivoGenericText(xmlSet.sdRoot);

    RuntimeRiivolution::PatchSet set;
    for (const auto& parsed : RiivoParseXmls(xmlSet, overlayRoot, gameId, log)) {
        const auto* disc = &parsed.disc;
        const fs::path& xmlFile = parsed.path;
        const auto activePatches = disc->GeneratePatches(gameId);
        // Externals relative to the XML's folder (the built-in description's "path" is the content folder itself).
        const std::string xmlDirGeneric =
            RiivoGenericText(xmlSet.builtin ? xmlSet.sdRoot : xmlFile.parent_path());

        const size_t before = set.mappings.size();
        for (const auto& patch : activePatches) {
            RiivoCollectMappings(patch, sdRootGeneric, xmlDirGeneric, set);
            for (const auto& memory : patch.memoryPatches) {
                const auto hex = [](std::string text, std::vector<uint8_t>& out) {
                    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) text = text.substr(2);
                    if (text.size() % 2) return false;
                    for (size_t i = 0; i < text.size(); i += 2) {
                        const int hi = std::isxdigit(static_cast<unsigned char>(text[i])) ? std::stoi(text.substr(i, 1), nullptr, 16) : -1;
                        const int lo = std::isxdigit(static_cast<unsigned char>(text[i + 1])) ? std::stoi(text.substr(i + 1, 1), nullptr, 16) : -1;
                        if (hi < 0 || lo < 0) return false;
                        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
                    }
                    return !out.empty();
                };
                RuntimeRiivolution::PatchSet::MemoryWrite write;
                write.address = memory.offset;
                if (memory.ocarina || memory.search || !hex(memory.value, write.value) ||
                    (!memory.original.empty() && !hex(memory.original, write.original))) {
                    if (log) {
                        RT_LOGF(RT_TAG_RIIVOLUTION, "memory patch at 0x%08X skipped (only plain value patches are supported)\n",
                                memory.offset);
                    }
                    continue;
                }
                set.memory.push_back(std::move(write));
            }
        }

        // Highest-priority overlay with a <savegame> wins.
        if (state && !state->saveRedirect) {
            if (const auto* savegame = RiivolutionContract::FindSavegamePatch(activePatches)) {
                if (const auto resolvedSave = RiivolutionContract::MakeAbsoluteFromRelative(
                        sdRootGeneric, xmlDirGeneric, savegame->external)) {
                    state->saveRedirect =
                        RuntimeRiivolution::SaveRedirect{PathFromUtf8(*resolvedSave),
                                                         savegame->clone};
                    RT_LOG(RT_TAG_RIIVOLUTION) << "savegame redirect: "
                              << PathToUtf8(state->saveRedirect->hostDirectory)
                              << (savegame->clone ? " (clone)" : "") << std::endl;
                }
            }
        }

        if (!log) continue;
        // A pack whose XML parses but activates nothing is the most confusing
        // failure this layer has: the game boots, plays, and quietly shows
        // vanilla content. Always say what happened.
        RT_LOG(RT_TAG_RIIVOLUTION) << (xmlSet.builtin ? "Other M Redux's files (described by the port) in " : "")
                  << PathToUtf8(xmlFile) << ": " << activePatches.size()
                  << " active patch(es), " << (set.mappings.size() - before) << " mapping(s)"
                  << (set.mainDol ? ", a main.dol" : "") << std::endl;
        for (const auto& section : disc->sections) {
            for (const auto& option : section.options) {
                const bool on = option.selectedChoice > 0 && option.selectedChoice <= option.choices.size();
                RT_LOG(RT_TAG_RIIVOLUTION) << "  option " << section.name << " / " << option.name << ": "
                          << (on ? option.choices[option.selectedChoice - 1].name : std::string("off")) << std::endl;
            }
        }
        if (activePatches.empty()) {
            RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: " << PathToUtf8(xmlFile)
                      << " has no enabled options for " << gameId
                      << "; turn them on in Options > Mods (" << PathToUtf8(xmlSet.configDirectory) << ")"
                      << std::endl;
        }
    }

    if (log && set.skippedExternals != 0) {
        RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(overlayRoot) << ": skipped "
                  << set.skippedExternals << " mapping(s) whose external path does not exist"
                  << std::endl;
    }

    return set;
}

// The pack an overlay root holds, falling back to the port's description of Other M Redux's files when the pack's
// own XML maps nothing (its files are not where it says, for example the release zip's files beside Redux's XML).
std::optional<RiivoXmlSet> RiivoResolvePack(const fs::path& overlayRoot, RiivoState* state, bool log,
                                            RuntimeRiivolution::PatchSet* out) {
    auto xmlSet = RiivoFindXmls(overlayRoot);
    if (!xmlSet) return std::nullopt;
    auto set = RiivoBuildPatchSet(*xmlSet, overlayRoot, nullptr, false);
    if (!xmlSet->builtin && set.mappings.empty() && !set.mainDol) {
        if (auto builtin = RiivoFindXmls(overlayRoot, true); builtin && builtin->builtin) {
            if (log) {
                RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(overlayRoot) << ": its XML finds none of its files; "
                          << "using the port's description of Other M Redux's files in "
                          << PathToUtf8(builtin->sdRoot) << std::endl;
            }
            xmlSet = std::move(builtin);
        }
    }
    if (out) *out = RiivoBuildPatchSet(*xmlSet, overlayRoot, state, log);
    return xmlSet;
}

std::optional<RuntimeRiivolution::PatchSet> RiivoLoadPatchSet(const fs::path& overlayRoot,
                                                              RiivoState& state) {
    RuntimeRiivolution::PatchSet set;
    if (!RiivoResolvePack(overlayRoot, &state, true, &set)) return std::nullopt;
    return set;
}

void RiivoInitialize() {
    g_riivoState.overlays = RiivoDiscoverRoots();
    for (auto& overlay : g_riivoState.overlays) {
        overlay.patches = RiivoLoadPatchSet(overlay.root, g_riivoState);
    }
}

} // namespace

namespace RuntimeRiivolution {

const std::vector<Overlay>& Overlays() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.overlays;
}

const std::optional<SaveRedirect>& GetSaveRedirect() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.saveRedirect;
}

namespace {
// Options > Mods reads the options every frame it is up: kept for a moment, and dropped when a choice changes.
std::mutex g_packOptionsMutex;
std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::vector<PackOption>>> g_packOptionsCache;

std::vector<PackOption> ReadPackOptions(const fs::path& modFolder) {
    std::vector<PackOption> options;
    std::error_code ec;
    if (fs::is_directory(modFolder / "files", ec)) return options;  // a disc-shaped mod: no options
    const auto xmlSet = RiivoResolvePack(modFolder, nullptr, false, nullptr);
    if (!xmlSet) return options;
    for (const auto& parsed : RiivoParseXmls(*xmlSet, modFolder, RiivoGameId(), false)) {
        for (const auto& section : parsed.disc.sections) {
            for (const auto& option : section.options) {
                PackOption out;
                out.id = option.id.empty() ? section.name + option.name : option.id;
                if (std::any_of(options.begin(), options.end(), [&](const PackOption& o) { return o.id == out.id; }))
                    continue;
                out.section = section.name;
                out.name = option.name;
                for (const auto& choice : option.choices) out.choices.push_back(choice.name);
                out.selected = option.selectedChoice <= option.choices.size() ? option.selectedChoice : 0;
                options.push_back(std::move(out));
            }
        }
    }
    return options;
}

std::string XmlEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}
}  // namespace

std::vector<PackOption> PackOptions(const fs::path& modFolder) {
    const std::string key = PathToUtf8(modFolder);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_packOptionsMutex);
        const auto it = g_packOptionsCache.find(key);
        if (it != g_packOptionsCache.end() && now - it->second.first < std::chrono::seconds(2)) return it->second.second;
    }
    auto options = ReadPackOptions(modFolder);
    std::lock_guard<std::mutex> lock(g_packOptionsMutex);
    g_packOptionsCache[key] = {now, options};
    return options;
}

bool SetPackOption(const fs::path& modFolder, const std::string& id, uint32_t choice) {
    auto options = ReadPackOptions(modFolder);
    const auto xmlSet = RiivoResolvePack(modFolder, nullptr, false, nullptr);
    if (options.empty() || !xmlSet) return false;
    bool found = false;
    for (auto& option : options) {
        if (option.id != id) continue;
        option.selected = choice <= option.choices.size() ? choice : 0;
        found = true;
    }
    if (!found) return false;
    // Every option is written, so the next start has the same choices whatever its defaults would be.
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<riivolution version=\"2\">\n";
    for (const auto& option : options)
        xml << "\t<option id=\"" << XmlEscape(option.id) << "\" default=\"" << option.selected << "\" />\n";
    xml << "</riivolution>\n";
    std::error_code ec;
    fs::create_directories(xmlSet->configDirectory, ec);
    const fs::path file = xmlSet->configDirectory / (RiivoGameId().substr(0, 4) + ".xml");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << xml.str();
    if (!out) return false;
    out.close();
    {
        std::lock_guard<std::mutex> lock(g_packOptionsMutex);
        g_packOptionsCache.erase(PathToUtf8(modFolder));
    }
    RT_LOG(RT_TAG_RIIVOLUTION) << "option " << id << " of " << PathToUtf8(modFolder.filename()) << " set to "
              << choice << " (" << PathToUtf8(file) << "); applies at the next start" << std::endl;
    return true;
}

fs::path FindPackRoot(const fs::path& dir, int depth) {
    if (RiivoIsPack(dir)) {
        // Redux's files found below: the folder that holds them is the mod (its own description starts there).
        const fs::path xmlDir = RiivoChildDir(dir, "riivolution");
        if ((xmlDir.empty() || RiivoDiscXmlsIn(xmlDir).empty()) && RiivoDiscXmlsIn(dir).empty())
            return RiivoFindReduxContent(dir);
        return dir;
    }
    if (depth <= 0) return {};
    std::vector<fs::path> subs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_directory(ec)) subs.push_back(entry.path());
    std::sort(subs.begin(), subs.end());
    for (const fs::path& sub : subs)
        if (fs::path found = FindPackRoot(sub, depth - 1); !found.empty()) return found;
    return {};
}

} // namespace RuntimeRiivolution
