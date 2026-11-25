#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Mods live in <UserData>/mods/<name>: files/ replaces game files (riivolution.cpp), textures/ replaces textures, a
// Riivolution pack (or Other M Redux's own files) says what it replaces. A .zip dropped in the mods folder is
// unpacked into its own mod folder at the next start: one with textures/ or code/ before the window opens (those
// load with it), any other on a worker while the start screen shows its progress; the game boots once it is done.
// A file named "disabled" inside a mod folder turns it off (the Options menu's Mods page).
namespace ModInstall {
struct Mod {
    std::string name;
    bool enabled = true;
};
std::filesystem::path Folder();
// At start, before the window opens.
void StartInstallingDroppedZips();
struct InstallStatus {
    bool busy = false;
    std::string name;        // the zip being unpacked
    float fraction = -1.0f;  // 0..1, -1 while its directory is read
    int queued = 0;          // zips after it
};
InstallStatus Installing();
// Before the game reads its files: waits for the worker. The mods it installed this start.
std::vector<std::string> WaitForInstalls();
std::vector<Mod> List();
void SetEnabled(const std::string& name, bool enabled);
// Unpacks `zip` into `dest`, refusing entries that would land outside it, streaming each file (store or deflate,
// zip64 too). False with `error` on failure.
bool ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string& error,
                const std::function<void(uint64_t done, uint64_t total)>& progress = {});
// OTHERM_SELFTEST=1: a wrapped mod zip unpacks, one slipping out of its folder is refused, Redux's layout is a mod.
bool SelfCheck();
} // namespace ModInstall
