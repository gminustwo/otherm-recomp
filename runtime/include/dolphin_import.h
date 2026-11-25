#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Metroid: Other M: what Options > Saves and Options > Textures take from a Dolphin install on this computer: the
// game's save in Dolphin's Wii NAND and Dolphin's texture pack for the game (<Load>/Textures/<game ID>).
//
// Dolphin's user folder is the one picked in Options ([paths] dolphin_folder), then the standard places (on Windows
// the registry's UserConfigPath, %APPDATA%\Dolphin Emulator, Documents\Dolphin Emulator). Each user folder's
// Config/Dolphin.ini can move the NAND (NANDRootPath) and the Load folder (LoadPath) elsewhere; those win.
namespace DolphinImport {

struct Install {
    std::filesystem::path user;  // empty when a NAND folder was picked directly
    std::filesystem::path nand;
    std::filesystem::path load;
};

struct Save {
    std::filesystem::path data;  // <nand>/title/00010000/<game code>/data
    std::filesystem::file_time_type newest{};
    int files = 0;
};

// The disc's game ID ("R3OE01") from sys/boot.bin, and the NAND folder name of its code ("52334f45").
std::string GameId();
std::string TitleFolder();

// Every Dolphin install found. Cached for two seconds (Options rebuilds its rows each frame); Refresh() forgets it.
const std::vector<Install>& Installs();
void Refresh();

// This game's save in the Dolphin NANDs, the most recently written one when there are several.
std::optional<Save> FindSave();
// The same facts for this port's own save folder (the folder itself, whether or not it exists yet).
Save PortSave();
// Dolphin's texture pack folder for this game, empty when no install has one.
std::filesystem::path TexturePack();
// Files in a pack folder (counted once per folder per session; the Dolphin pack for Other M is about 15,000).
size_t CountTextures(const std::filesystem::path& folder);

// Copies `from` over this port's save folder. The port's save is first copied to
// <user data>/save_backups/<date>_<time>/<game code>/data; `backup` names that folder. Returns an error, or empty.
std::string ImportSave(const Save& from, std::filesystem::path& backup);

// Hands the texture settings to the renderer: packs on or off, dumps, and Dolphin's pack when it is switched on.
// Called once before aurora_initialize and again whenever Options > Textures changes something.
void ApplyTexturePacks();

// "May 25, 2026"
std::string FormatDate(std::filesystem::file_time_type time, bool withTime = false);  // "Nov 8, 2026" (" 21:30")

// Starts the game again with the same command line, then ends this process (a newly imported save is read at boot).
[[noreturn]] void RestartGame();

} // namespace DolphinImport
