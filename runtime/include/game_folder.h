#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// The player's own copy of Metroid: Other M (USA, R3OE01, revision 1): its files unpacked from the player's disc image
// into <game folder>/Disc/DATA, or a folder Dolphin's "Extract Entire Disc" made (files/ and sys/). The start screen
// (settings_overlay.cpp, the launcher) sets it up through this; nothing here asks the player anything.
namespace GameFolder {

// The extracted game at `path` (or its DATA subfolder); empty when there is none.
std::filesystem::path Find(const std::filesystem::path& path);

// Why the game at `root` can't be used ("" when it can): not an extracted disc, or another game, read from
// sys/boot.bin.
std::string Problem(const std::filesystem::path& root);

// The folder the game's own things go in: the one holding the .AppImage in an AppImage, else the program's (its
// parent when the program sits in an engine/ folder); on Android the app's folder on shared storage. Disc/DATA is set
// up here.
std::filesystem::path GameRoot();

// Picks up the game's files without asking: the saved folder, else <game folder>/Disc/DATA (Windows: a "game" folder
// beside the program first). True when they are there and right, so the game can boot.
bool Ready();
// For the start screen: where the game's files are, or what is missing.
std::string Describe();

// A disc image by its name: .iso, .ciso and .wbfs (read here), .rvz, .wia and .gcz (through nodtool beside the
// program).
bool IsImage(const std::filesystem::path& path);
// What a folder the player points at holds: an extracted game (Find), and the disc images in it, largest first.
struct FolderContents {
    std::filesystem::path extracted;
    std::vector<std::filesystem::path> images;
};
FolderContents Scan(const std::filesystem::path& folder);

// Sets the game up from `source`, one at a time: a disc image is unpacked on a worker thread into
// <game folder>/Disc/DATA (a few minutes), an extracted game folder is used where it is. Progress() reports it.
void StartSetUp(const std::filesystem::path& source);
#if defined(__ANDROID__)
// Android: sets the game up from the document the system's file picker returned (AndroidBridge::TakePickedDisc), read
// through nod (every image format); `name` is its display name.
void StartSetUpFromDocument(const std::string& uri, const std::string& name);
#endif
struct SetUpStatus {
    bool running = false;
    bool finished = false;  // the last set-up ended and TakeFinished() was not called since
    bool ok = false;
    float fraction = -1.0f;  // 0-1 while unpacking; -1 while not known yet
    double seconds = 0.0;
    std::string source;      // the image's (or folder's) name
    std::string message;     // when finished: what happened, in a sentence
};
// Called from the start screen's thread every frame: it also saves the finished set-up's folder to Config.toml.
SetUpStatus Progress();
void TakeFinished();

// Quick places for the start screen's folder browser, the ones that exist: a name and a folder.
std::vector<std::pair<std::string, std::filesystem::path>> Places();

} // namespace GameFolder
