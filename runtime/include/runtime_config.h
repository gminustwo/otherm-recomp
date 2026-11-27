#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml.hpp>
#include "platform/host_platform.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

struct RuntimeUserConfig {
    std::optional<bool> widescreen;
    std::optional<bool> forceAspect169;
    std::optional<int32_t> aspectMode;  // 0 original 4:3, 1 16:9, 2 fill the window (DisplaySettings)
    std::optional<std::string> language;  // [game] language: en fr de es it (the Wii system language SCGetLanguage reports)
    std::optional<int32_t> gpuCompat;  // 0 auto (by graphics adapter), 1 on, 2 off (AuroraGpuCompat)
    std::optional<int32_t> windowPosX;
    std::optional<int32_t> windowPosY;
    std::optional<uint32_t> windowWidth;
    std::optional<uint32_t> windowHeight;
    std::optional<float> resolutionMultiplier;
    std::optional<std::string> graphicsApi;
    std::optional<std::string> displayMode;
    std::optional<uint32_t> frameInterpolationFps;
    std::optional<bool> metalFxSpatialUpscaling;
    std::optional<bool> skipUnreadyPipelines;
    // [video] shader_compilation: "ubershaders", "skip" or "wait" (what a draw does while its shader compiles)
    std::optional<std::string> shaderCompilation;
    std::optional<bool> disableCopyFilter;
    std::optional<int32_t> bloomMode;  // [video] bloom: "on" (default), "native", "blurred", "off" (otherm_bloom.cpp)
    std::optional<std::string> upscaler;      // [video] upscaler: "off" or "fsr" (aurora webgpu/post.cpp)
    std::optional<float> fsrSharpness;        // [video] fsr_sharpness: 0..1
    // [video] dynamic_resolution: the render scale follows the GPU's load to keep 60 FPS (dynamic_resolution.cpp), up to
    // dynamic_resolution_max; resolution_multiplier stays the fixed choice it falls back to.
    std::optional<bool> dynamicResolution;
    std::optional<float> dynamicResolutionMax;
    std::optional<std::string> antiAliasing;  // [video] anti_aliasing: "off" or "smaa"
    std::optional<bool> textureReplacements;
    std::optional<bool> textureDumps;
    // [video] dolphin_textures: also use Dolphin's own pack for this game (<Load>/Textures/<game ID>).
    std::optional<bool> dolphinTextures;
    // [video] prefetch_textures: load every pack file into memory in the background (default off).
    std::optional<bool> prefetchTextures;
    std::optional<bool> showFps;
    std::optional<bool> mousePointer;   // [input] mouse_pointer: the mouse aims the Wii pointer (default on)
    std::optional<bool> frameGraph;     // [video] frame_graph, frame times under the FPS counter
    std::optional<bool> vsync;          // [video] vsync, presents wait for the display
    std::optional<uint32_t> disabledPostProcessingPaths;
    std::optional<float> audioVolume;
    std::optional<bool> audioMuted;
    std::optional<bool> audioMixWorker;
    // Real Wii Remotes (with or without Nunchuk / Classic Controller) and Wii U Pro
    // Controllers paired over Bluetooth, driven by SDL's HIDAPI Wii driver. The driver
    // is opt-in on SDL's side, so this decides whether the runtime turns it on.
    std::optional<bool> wiiRemotes;
    // Keep re-enumerating Bluetooth HID devices while no Wii controller is connected
    // (Dolphin's "continuous scanning"), so a remote that dropped or was switched on
    // after launch shows up without restarting.
    std::optional<bool> wiiContinuousScan;
    // Accelerometer zero-point correction for the Bluetooth Wii Remote, in g and in
    // SDL's sensor frame (x right, y out of the button face, z towards the user).
    // SDL's Wii driver falls back to a nominal zero point when its read of the
    // remote's calibration block times out (common over Bluetooth), so this is
    // measured in the overlay with the remote at rest.
    std::optional<double> wiiAccelOffsetX;
    std::optional<double> wiiAccelOffsetY;
    std::optional<double> wiiAccelOffsetZ;
    // Debugging aid: append every KPAD sample of the Bluetooth remote (raw and
    // corrected accelerometer, buttons) to wii_accel_trace.csv next to Config.toml.
    std::optional<bool> wiiAccelTrace;
    std::optional<bool> networkEnabled;
    std::optional<bool> discordPresence;      // [discord] enabled: Rich Presence in a Discord running on this PC
    std::optional<std::string> nandRoot;
    // [paths] dolphin_folder: a Dolphin user folder picked in Options (a portable install's User folder).
    std::optional<std::string> dolphinFolder;
    std::optional<std::string> dvdRoot;
    std::vector<std::string> overlayRoots;
    // Controller mappings use Wii/GameCube button names as keys and up to two
    // comma-separated SDL-style physical button names ("south", or
    // "dpad_up,left_shoulder") as values; pressing either bound button counts.
    std::array<std::optional<std::string>, 12> controllerButtons;
    std::optional<bool> rumbleEnabled;
    std::optional<int32_t> rumbleStrength;  // [controller] rumble_strength, percent 0-200 (Options > Controls)
    std::optional<int32_t> muteHotkey;
    std::optional<int32_t> fastForwardSpeed;  // [game] fast_forward_speed: 2..8 times, 0 = as fast as the PC goes
    std::optional<bool> skipAnyCutscene;      // [game] skip_cutscenes: "any" (default) or "seen" (the game's own rule)
    std::optional<bool> japaneseVoices;       // [game] voices: "en" (default) or "jp" (hle/otherm_language.cpp)
    // [game] text_language / subtitles: a code of kGameLanguageCodes, "game" (default: the game's own setting) or,
    // for subtitles, "off". Kept as the game's language index (-1 the game's, -2 off) so the game thread reads an int.
    std::optional<int32_t> textLanguage;
    std::optional<int32_t> subtitleLanguage;
    std::optional<bool> fastForwardToggle;    // [game] fast_forward_toggle: a press toggles it, else hold
    std::optional<bool> fasterMenus;          // [game] faster_menus: menus, saves and loads run faster
    std::optional<bool> saveAnywhere;         // [game] save_anywhere: Options > Game has "Save game"
    std::optional<bool> checkUpdates;         // [game] check_updates: look for a new release at start
    std::optional<bool> showLauncher;         // [game] show_launcher: the start screen before the game boots
    std::optional<bool> infiniteEnergy;       // [cheats] infinite_energy (otherm_cheats.h)
    std::optional<bool> infiniteMissiles;     // [cheats] infinite_missiles
    std::optional<bool> analogMovement;       // [cheats] analog_movement: 360-degree movement from the stick
    std::optional<bool> minimapNorthUp;       // [cheats] minimap_north_up: the HUD map does not turn with the view
    std::map<std::string, std::string> controllerExpressions;
    // [keyboard] action ="comma-separated SDL scancode names" (keybinds.h).
    std::map<std::string, std::string> keyboardBindings;
    std::map<std::string, std::string> gamepadBindings;  // [gamepad]: type + per-action overrides
};

namespace RuntimeConfigFile {

// [video] bloom (hle/otherm_bloom.cpp): the game's glow as it draws it, at the Wii's resolution, blurred, or none.
inline constexpr int32_t kBloomOn = 0, kBloomNative = 1, kBloomBlurred = 2, kBloomOff = 3;
inline constexpr const char* kBloomModeNames[] = {"on", "native", "blurred", "off"};

// The game's text languages by its own index: message/message_all.dat's sections (all eight are on the USA disc).
inline constexpr const char* kGameLanguageCodes[] = {"jp", "en", "de", "fr", "es", "it", "fr_us", "es_us"};

// A code of kGameLanguageCodes -> its index; "off" (subtitles only) -> -2; anything else ("game") -> -1.
inline int32_t GameLanguageIndex(const std::string& code, bool allowOff) {
    for (int32_t i = 0; i < 8; ++i) {
        if (code == kGameLanguageCodes[i]) return i;
    }
    return allowOff && code == "off" ? -2 : -1;
}

inline const char* GameLanguageCode(int32_t index) {
    return index >= 0 && index < 8 ? kGameLanguageCodes[index] : index == -2 ? "off" : "game";
}

// Narrow path strings are UTF-8 everywhere in the runtime; string() and the
// char path constructor would use the ANSI codepage on Windows, which drops
// characters the codepage cannot represent.
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline constexpr const char* kConfigFileName = "Config.toml";
inline constexpr const char* kApplicationDirectoryName = "OtherMRecompiled";

// Always portable on Windows and Linux: the player's files (Config.toml, NAND, saves, caches, logs) live in UserData
// beside the program (beside the .AppImage file in an AppImage), so a folder can be moved or carried as it is.
// macOS and Android use the system's app data folder.
inline constexpr const char* kPortableUserDataDirectoryName = "UserData";

inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline std::string RemoveComment(std::string_view line) {
    bool inSingle = false;
    bool inDouble = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inDouble && ch == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (ch == '"' && !inSingle && !escaped) {
            inDouble = !inDouble;
        } else if (ch == '#' && !inSingle && !inDouble) {
            return std::string(line.substr(0, i));
        }
        escaped = false;
    }
    return std::string(line);
}

inline bool IsSupportedResolutionMultiplier(float value) {
    static constexpr std::array values{0.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.25f, 3.0f, 4.0f, 4.5f, 6.0f, 8.0f};
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Must stay in step with the backend table in main.cpp, which is what actually
// maps these to AuroraBackend.
inline bool IsSupportedGraphicsApi(std::string_view value) {
#if defined(__APPLE__)
    static constexpr std::array<std::string_view, 2> values{"auto", "metal"};
// only vulkan for linux
#elif defined(__linux__)
    static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};
#elif defined(_WIN32)
    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};
#endif
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline bool IsSupportedDisplayMode(std::string_view value) {
    static constexpr std::array<std::string_view, 3> values{
        "windowed", "borderless", "exclusive",
    };
    return std::find(values.begin(), values.end(), value) != values.end();
}

// 240 needs a 240 Hz screen and a fast PC (four presented images per game frame); offered again for #42.
inline bool IsSupportedFrameInterpolationFps(uint32_t value) {
    return value == 0 || value == 90 || value == 120 || value == 144 || value == 180 || value == 240;
}

inline std::optional<std::filesystem::path> ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    // A macOS .app keeps its program in Contents/MacOS and the program's own files (dsp_coef.bin, wii_bootstrap,
    // startup, nodtool, ...) in Contents/Resources, which is also what SDL_GetBasePath answers there: that folder is
    // returned, so every "beside the program" lookup finds them (tools/package-macos.sh). A bare executable in a build
    // folder keeps its own folder.
    auto directory = RuntimePlatform::ExecutableDirectory();
    if (directory && directory->filename() == "MacOS" && directory->parent_path().filename() == "Contents" &&
        directory->parent_path().parent_path().extension() == ".app") {
        return directory->parent_path() / "Resources";
    }
    return directory;
#elif defined(__ANDROID__)
    return RuntimePlatform::ExecutableDirectory();
#else
    // /proc/self/exe is a Linux-specific magic symlink to the running executable; readlink()
    // does not NUL-terminate and silently truncates if the buffer is too small, so this grows
    // the buffer until the result no longer fills it completely, the same doubling strategy as
    // the Windows branch above uses for GetModuleFileNameW.
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

// Linux AppImage: the folder holding the .AppImage file. The program runs from the image's read-only mount, so this
// folder, not the program's, holds what the player owns: UserData, Metroid.ini, the disc image and the Disc/DATA
// set up from it. The AppImage runtime sets APPIMAGE (the file) and APPDIR (its mount); a program that inherited them
// from another AppImage is not inside that APPDIR, so it is not taken for one. nullopt outside an AppImage.
inline const std::optional<std::filesystem::path>& AppImageDirectory() {
    static const std::optional<std::filesystem::path> dir = []() -> std::optional<std::filesystem::path> {
#if defined(__linux__) && !defined(__ANDROID__)
        const char* image = std::getenv("APPIMAGE");
        const char* mount = std::getenv("APPDIR");
        const auto executableDirectory = ExecutableDirectory();
        if (image == nullptr || *image == '\0' || mount == nullptr || *mount == '\0' || !executableDirectory) {
            return std::nullopt;
        }
        std::error_code ec;
        const std::filesystem::path mountPath = std::filesystem::weakly_canonical(mount, ec);
        const std::filesystem::path exePath = std::filesystem::weakly_canonical(*executableDirectory, ec);
        const auto [mountEnd, exeIt] = std::mismatch(mountPath.begin(), mountPath.end(), exePath.begin(), exePath.end());
        if (mountPath.empty() || mountEnd != mountPath.end() || !std::filesystem::is_regular_file(image, ec)) {
            return std::nullopt;
        }
        return std::filesystem::absolute(image, ec).parent_path();
#else
        return std::nullopt;
#endif
    }();
    return dir;
}

// Linux under RecompLauncher: RECOMPLAUNCHER_DATA ({install}/recompdata), which the launcher keeps across updates. It
// always runs the AppImage unpacked, from a folder whose AppRun and usr/ every update replaces, so the program's own
// folder would lose UserData, Metroid.ini and Disc/DATA. (On Windows they stay beside the program in {install}, where
// the launcher keeps them; macOS uses Application Support.) nullopt elsewhere.
inline const std::optional<std::filesystem::path>& LauncherDataDirectory() {
    static const std::optional<std::filesystem::path> dir = []() -> std::optional<std::filesystem::path> {
#if defined(__linux__) && !defined(__ANDROID__)
        const char* data = std::getenv("RECOMPLAUNCHER_DATA");
        if (data != nullptr && *data != '\0') return std::filesystem::path(data);
#endif
        return std::nullopt;
    }();
    return dir;
}

// Where the player's own files beside the program go (Metroid.ini): the AppImage's folder, else the program's. On
// Android the app's data folder: the program's folder there holds the runtime files the app unpacks, which an update
// replaces. On macOS the app data folder too, where UserData's files go there: a downloaded .app can run from a
// read-only copy (App Translocation) and an update replaces it whole.
inline std::optional<std::filesystem::path> PlayerDirectory() {
#if defined(__ANDROID__) || defined(__APPLE__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    if (const auto& launcher = LauncherDataDirectory()) return launcher;
    if (const auto& appImage = AppImageDirectory()) return appImage;
    return ExecutableDirectory();
#endif
}

// The folder whose UserData holds the player's files: the AppImage's folder, else the program's (nullopt on macOS
// and Android, which use the app data folder). Resolved once: every user-state path derives from it.
inline const std::optional<std::filesystem::path>& PortableRootDirectory() {
#if defined(__APPLE__) || defined(__ANDROID__)
    static const std::optional<std::filesystem::path> root;
#else
    static const std::optional<std::filesystem::path> root = PlayerDirectory();
#endif
    return root;
}

inline std::filesystem::path ApplicationDataDirectory() {
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / kPortableUserDataDirectoryName;
    }
#if defined(__APPLE__) || defined(__ANDROID__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    return std::filesystem::current_path() / kPortableUserDataDirectoryName;
#endif
}

inline std::filesystem::path ResolveConfigPath() {
    return ApplicationDataDirectory() / kConfigFileName;
}

inline void EnsureConfigFile() {
    const std::filesystem::path path = ResolveConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path, ec)) {
        return;
    }

    std::ofstream output(path);
    if (!output) {
        return;
    }
    // Defaults: borderless 1080p (2.25 x 480 lines; 4.5 = 4K, 0 = the screen's own size), fill the screen (16:9,
    // wider on ultrawide screens), 120 fps interpolation, HD textures on. Options writes the same keys.
    output << "# Other M Recompiled user configuration\n"
              "# paths.dvd_root is your Metroid: Other M (USA, R3OE01) disc extracted to a DATA folder; the\n"
              "# start screen sets it up from your disc image.\n\n"
              "[video]\n"
              "widescreen = true\n"
              "force_16_9 = false\n"
              "# render height = 480 x resolution_multiplier: 1.5 = 720p, 2.25 = 1080p, 4.5 = 4K, 0 = screen size\n"
              "resolution_multiplier = 2.25\n"
              "# follow the GPU's load to keep 60 FPS, from 1x up to dynamic_resolution_max\n"
              "dynamic_resolution = false\n"
              "dynamic_resolution_max = 4.5\n"
              "# 0 (off), 120 or 180\n"
              "frame_interpolation_fps = 120\n"
              "# wait for the display when presenting: no screen tearing\n"
              "vsync = true\n"
              "# 0 = original 4:3, 1 = 16:9, 2 = fill the screen, 3 = cinematic 2.39:1\n"
              "aspect_mode = 2\n"
              "display_mode = \"borderless\"\n"
              "graphics_api = \"auto\"\n"
              "# while a shader compiles: \"ubershaders\" (drawn by a generic shader), \"skip\" (missing for a\n"
              "# moment) or \"wait\" (a stutter)\n"
              "shader_compilation = \"ubershaders\"\n"
              "disable_copy_filter = true\n"
              "show_fps = false\n"
              "# Dolphin-style custom textures. When enabled, the renderer indexes\n"
              "# texture_replacements/ next to this file at startup and substitutes\n"
              "# any tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>.dds or .png it finds\n"
              "# there for the matching game texture. texture_dumps writes every\n"
              "# unmatched texture to Cache/texture_dumps under the name a\n"
              "# replacement would need. Options > Textures switches both.\n"
              "texture_replacements = true\n"
              "texture_dumps = false\n"
              "# also use Dolphin's pack for this game (its Load/Textures/<game ID> folder)\n"
              "dolphin_textures = false\n"
              "# load every pack texture into memory in the background, up to half this PC's\n"
              "# memory, so none shows the game's own texture first (Dolphin's Prefetch\n"
              "# Custom Textures)\n"
              "prefetch_textures = false\n\n"
              "[audio]\n"
              "volume = 1.0\n"
              "muted = false\n"
              "# Runs the AX/DSP voice mix on its own thread. Off: in Other M it\n"
              "# made the reverb crackle after fast-forward or a stall.\n"
              "mix_worker = false\n\n"
              "[network]\n"
              "enabled = true\n\n"
              "[paths]\n"
              "# dvd_root = \"D:\\\\Games\\\\OtherM\\\\DATA\"\n"
              "# nand_root = \"D:\\\\WiiNand\"\n"
              "# overlay_roots = [\"D:\\\\Mods\\\\MyPack\"]\n";
}

template <typename T>
inline std::optional<T> FindConfigValue(
    const toml::value& document, std::string_view section, std::string_view key) {
    try {
        return toml::find<T>(document, std::string(section), std::string(key));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::optional<uint32_t> FindConfigUint(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < 0 || static_cast<uint64_t>(*value) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

inline std::optional<int32_t> FindConfigInt(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < INT32_MIN || *value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*value);
}

inline std::optional<float> FindConfigFloat(
    const toml::value& document, std::string_view section, std::string_view key) {
    std::optional<double> value = FindConfigValue<double>(document, section, key);
    if (!value) {
        if (const auto integer = FindConfigValue<int64_t>(document, section, key)) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) ||
        *value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *value > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

inline void AppendOverlayRoots(RuntimeUserConfig& config, const std::string& roots) {
    size_t begin = 0;
    while (begin < roots.size()) {
        const size_t end = roots.find(';', begin);
        std::string root = Trim(std::string_view(roots).substr(begin, end - begin));
        if (!root.empty()) {
            config.overlayRoots.push_back(std::move(root));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

// Reads every supported setting out of a parsed Config.toml document.
inline RuntimeUserConfig ParseConfigDocument(const toml::value& document) {
    RuntimeUserConfig config;

    static constexpr std::array<std::string_view, 12> buttonKeys = {
        "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
    };
    for (size_t index = 0; index < buttonKeys.size(); ++index) {
        config.controllerButtons[index] =
            FindConfigValue<std::string>(document, "controller", buttonKeys[index]);
    }

    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");
    config.rumbleStrength = FindConfigInt(document, "controller", "rumble_strength");
    if (auto value = FindConfigInt(document, "audio", "mute_key")) {
        config.muteHotkey = *value;
    }
    if (auto value = FindConfigInt(document, "game", "fast_forward_speed")) {
        config.fastForwardSpeed = *value;
    }
    config.fastForwardToggle = FindConfigValue<bool>(document, "game", "fast_forward_toggle");
    if (auto value = FindConfigValue<std::string>(document, "game", "skip_cutscenes")) {
        config.skipAnyCutscene = *value != "seen";
    }
    if (auto value = FindConfigValue<std::string>(document, "game", "voices")) {
        config.japaneseVoices = *value == "jp";
    }
    if (auto value = FindConfigValue<std::string>(document, "game", "text_language")) {
        config.textLanguage = GameLanguageIndex(*value, false);
    }
    if (auto value = FindConfigValue<std::string>(document, "game", "subtitles")) {
        config.subtitleLanguage = GameLanguageIndex(*value, true);
    }
    config.fasterMenus = FindConfigValue<bool>(document, "game", "faster_menus");
    config.saveAnywhere = FindConfigValue<bool>(document, "game", "save_anywhere");
    config.checkUpdates = FindConfigValue<bool>(document, "game", "check_updates");
    config.showLauncher = FindConfigValue<bool>(document, "game", "show_launcher");
    config.infiniteEnergy = FindConfigValue<bool>(document, "cheats", "infinite_energy");
    config.infiniteMissiles = FindConfigValue<bool>(document, "cheats", "infinite_missiles");
    config.analogMovement = FindConfigValue<bool>(document, "cheats", "analog_movement");
    config.minimapNorthUp = FindConfigValue<bool>(document, "cheats", "minimap_north_up");

    if (const auto* section = document.contains("controller") ? &document.at("controller") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (key.rfind("expr_", 0) == 0 && value.is_string()) {
                config.controllerExpressions[key] = value.as_string();
            }
        }
    }

    if (const auto* section = document.contains("keyboard") ? &document.at("keyboard") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (value.is_string()) {
                config.keyboardBindings[key] = value.as_string();
            }
        }
    }
    if (const auto* section = document.contains("gamepad") ? &document.at("gamepad") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (value.is_string()) {
                config.gamepadBindings[key] = value.as_string();
            }
        }
    }

    config.language = FindConfigValue<std::string>(document, "game", "language");
    config.widescreen = FindConfigValue<bool>(document, "video", "widescreen");
    config.forceAspect169 = FindConfigValue<bool>(document, "video", "force_16_9");
    if (auto value = FindConfigInt(document, "video", "aspect_mode"); value && *value >= 0 && *value <= 3) {
        config.aspectMode = *value;
    }
    if (auto value = FindConfigInt(document, "video", "gpu_compat"); value && *value >= 0 && *value <= 2) {
        config.gpuCompat = *value;
    }
    config.windowPosX = FindConfigInt(document, "video", "window_x");
    config.windowPosY = FindConfigInt(document, "video", "window_y");
    if (auto value = FindConfigUint(document, "video", "window_width"); value && *value != 0) {
        config.windowWidth = *value;
    }
    if (auto value = FindConfigUint(document, "video", "window_height"); value && *value != 0) {
        config.windowHeight = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "resolution_multiplier");
        value && IsSupportedResolutionMultiplier(*value)) {
        config.resolutionMultiplier = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "graphics_api")) {
        if (IsSupportedGraphicsApi(*value)) {
            config.graphicsApi = *value;
        } else {
            std::cerr << "[runtime] Unknown video.graphics_api=\"" << *value
                      << "\", using the automatic backend" << std::endl;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "display_mode");
        value && IsSupportedDisplayMode(*value)) {
        config.displayMode = *value;
    }
    if (auto value = FindConfigUint(document, "video", "frame_interpolation_fps")) {
        const uint32_t migrated = *value;
        if (IsSupportedFrameInterpolationFps(migrated)) {
            config.frameInterpolationFps = migrated;
        }
    }
    config.metalFxSpatialUpscaling =
        FindConfigValue<bool>(document, "video", "metalfx_spatial_upscaling");
    config.skipUnreadyPipelines = FindConfigValue<bool>(document, "video", "skip_unready_pipelines");
    if (auto value = FindConfigValue<std::string>(document, "video", "shader_compilation");
        value && (*value == "ubershaders" || *value == "skip" || *value == "wait"))
        config.shaderCompilation = *value;
    config.disableCopyFilter = FindConfigValue<bool>(document, "video", "disable_copy_filter");
    if (auto value = FindConfigValue<std::string>(document, "video", "bloom")) {
        for (int32_t mode = 0; mode < 4; ++mode) {
            if (*value == kBloomModeNames[mode]) config.bloomMode = mode;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "upscaler"); value && (*value == "off" || *value == "fsr"))
        config.upscaler = *value;
    if (auto value = FindConfigFloat(document, "video", "fsr_sharpness"); value && *value >= 0.0f && *value <= 1.0f)
        config.fsrSharpness = *value;
    config.dynamicResolution = FindConfigValue<bool>(document, "video", "dynamic_resolution");
    if (auto value = FindConfigFloat(document, "video", "dynamic_resolution_max"); value && *value >= 1.0f && *value <= 8.0f)
        config.dynamicResolutionMax = *value;
    if (auto value = FindConfigValue<std::string>(document, "video", "anti_aliasing"); value && (*value == "off" || *value == "smaa"))
        config.antiAliasing = *value;
    config.showFps = FindConfigValue<bool>(document, "video", "show_fps");
    config.mousePointer = FindConfigValue<bool>(document, "input", "mouse_pointer");
    config.frameGraph = FindConfigValue<bool>(document, "video", "frame_graph");
    config.vsync = FindConfigValue<bool>(document, "video", "vsync");
    config.textureReplacements = FindConfigValue<bool>(document, "video", "texture_replacements");
    config.textureDumps = FindConfigValue<bool>(document, "video", "texture_dumps");
    config.dolphinTextures = FindConfigValue<bool>(document, "video", "dolphin_textures");
    config.prefetchTextures = FindConfigValue<bool>(document, "video", "prefetch_textures");
    if (auto value = FindConfigUint(document, "video", "disabled_post_processing_paths");
        value && (*value & ~0x10u) == 0) {
        config.disabledPostProcessingPaths = *value & 0x10u;
    }

    auto readVolume = [&](std::string_view key) -> std::optional<float> {
        auto value = FindConfigFloat(document, "audio", key);
        return value && *value >= 0.0f && *value <= 1.0f ? value : std::nullopt;
    };
    config.audioVolume = readVolume("volume");
    config.audioMuted = FindConfigValue<bool>(document, "audio", "muted");
    config.audioMixWorker = FindConfigValue<bool>(document, "audio", "mix_worker");
    config.wiiRemotes = FindConfigValue<bool>(document, "controller", "wii_remotes");
    config.wiiContinuousScan = FindConfigValue<bool>(document, "controller", "wii_continuous_scan");
    config.wiiAccelOffsetX = FindConfigValue<double>(document, "controller", "wii_accel_offset_x");
    config.wiiAccelOffsetY = FindConfigValue<double>(document, "controller", "wii_accel_offset_y");
    config.wiiAccelOffsetZ = FindConfigValue<double>(document, "controller", "wii_accel_offset_z");
    config.wiiAccelTrace = FindConfigValue<bool>(document, "controller", "wii_accel_trace");
    config.networkEnabled = FindConfigValue<bool>(document, "network", "enabled");
    config.discordPresence = FindConfigValue<bool>(document, "discord", "enabled");

    config.nandRoot = FindConfigValue<std::string>(document, "paths", "nand_root");
    config.dolphinFolder = FindConfigValue<std::string>(document, "paths", "dolphin_folder");
    config.dvdRoot = FindConfigValue<std::string>(document, "paths", "dvd_root");
    if (auto roots = FindConfigValue<std::vector<std::string>>(document, "paths", "overlay_roots")) {
        for (auto& root : *roots) {
            root = Trim(root);
            if (!root.empty()) {
                config.overlayRoots.push_back(std::move(root));
            }
        }
    } else if (auto roots = FindConfigValue<std::string>(document, "paths", "overlay_roots")) {
        AppendOverlayRoots(config, *roots);
    }

    return config;
}

inline RuntimeUserConfig ParseConfig(std::istream& input, std::string sourceName = "Config.toml") {
    // A UTF-8 byte order mark (some editors add one) is not TOML: skip it rather than lose every setting.
    if (input.peek() == 0xEF) {
        char bom[3] = {};
        input.read(bom, 3);
        if (static_cast<unsigned char>(bom[1]) != 0xBB || static_cast<unsigned char>(bom[2]) != 0xBF) {
            input.clear();
            input.seekg(0);
        }
    }
    try {
        return ParseConfigDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML; using built-in defaults: "
                  << exception.what() << std::endl;
        return {};
    }
}

inline RuntimeUserConfig LoadConfigFile() {
    EnsureConfigFile();
    std::ifstream file(ResolveConfigPath(), std::ios::binary);
    return file ? ParseConfig(file, PathToUtf8(ResolveConfigPath())) : RuntimeUserConfig{};
}

inline const RuntimeUserConfig& Get() {
    static RuntimeUserConfig config = LoadConfigFile();
    return config;
}

inline RuntimeUserConfig& Mutable() {
    return const_cast<RuntimeUserConfig&>(Get());
}

inline constexpr std::array<std::string_view, 12> kControllerButtonKeys = {
    "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
};

inline const std::optional<std::string>& ControllerButton(size_t index) {
    static const std::optional<std::string> empty;
    return index < Get().controllerButtons.size() ? Get().controllerButtons[index] : empty;
}

// Update one TOML value without discarding comments, unrelated settings, or
// user-specific paths. This is used by the in-game Options menu (F1).
inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value) {
    const auto path = ResolveConfigPath();
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    const std::string normalizedSection = Trim(section);
    const std::string normalizedKey = Trim(key);
    size_t sectionStart = lines.size();
    size_t sectionEnd = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(RemoveComment(lines[i]));
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string found = Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            if (sectionStart != lines.size()) {
                sectionEnd = i;
                break;
            }
            if (found == normalizedSection) {
                sectionStart = i;
            }
        }
    }

    const std::string replacement = normalizedKey + " = " + std::string(value);
    if (sectionStart == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[" + normalizedSection + "]");
        lines.push_back(replacement);
    } else {
        bool replaced = false;
        for (size_t i = sectionStart + 1; i < sectionEnd; ++i) {
            const std::string uncommented = Trim(RemoveComment(lines[i]));
            const size_t equals = uncommented.find('=');
            if (equals != std::string::npos && Trim(std::string_view(uncommented).substr(0, equals)) == normalizedKey) {
                lines[i] = replacement;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            // Append after the section's last real line rather than after the blank line that
            // separates it from the next header: this file is edited by hand as well, and a key
            // parked below the separator reads as if it belonged to the next section.
            size_t insertAt = sectionEnd;
            while (insertAt > sectionStart + 1 && Trim(lines[insertAt - 1]).empty()) {
                --insertAt;
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), replacement);
        }
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << PathToUtf8(path) << std::endl;
        return false;
    }
    for (const auto& outputLine : lines) {
        output << outputLine << '\n';
    }
    return static_cast<bool>(output);
}

inline std::string FormatString(std::string_view value) {
    return toml::format(toml::value(std::string(value)));
}

inline bool SetResolutionMultiplier(float value) {
    Mutable().resolutionMultiplier = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("video", "resolution_multiplier", formatted.str());
}

inline bool SetWindowSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    Mutable().windowWidth = width;
    Mutable().windowHeight = height;
    const bool wroteWidth = WriteSetting("video", "window_width", std::to_string(width));
    const bool wroteHeight = WriteSetting("video", "window_height", std::to_string(height));
    return wroteWidth && wroteHeight;
}

inline bool SetWindowPosition(int32_t x, int32_t y) {
    Mutable().windowPosX = x;
    Mutable().windowPosY = y;
    const bool wroteX = WriteSetting("video", "window_x", std::to_string(x));
    const bool wroteY = WriteSetting("video", "window_y", std::to_string(y));
    return wroteX && wroteY;
}

inline bool SetFrameInterpolationFps(uint32_t value) {
    if (!IsSupportedFrameInterpolationFps(value)) {
        return false;
    }
    Mutable().frameInterpolationFps = value;
    return WriteSetting("video", "frame_interpolation_fps", std::to_string(value));
}

inline bool SetMetalFxSpatialUpscaling(bool value) {
    Mutable().metalFxSpatialUpscaling = value;
    return WriteSetting("video", "metalfx_spatial_upscaling", value ? "true" : "false");
}

inline bool SetLanguage(std::string value) {
    static constexpr const char* kCodes[] = {"en", "de", "fr", "es", "it"};
    bool known = false;
    for (const char* code : kCodes) known = known || value == code;
    if (!known) {
        return false;
    }
    Mutable().language = value;
    return WriteSetting("game", "language", FormatString(value));
}

inline bool SetDisplayMode(std::string value) {
    if (!IsSupportedDisplayMode(value)) {
        return false;
    }
    Mutable().displayMode = value;
    return WriteSetting("video", "display_mode", FormatString(value));
}

inline bool SetSkipUnreadyPipelines(bool value) {
    Mutable().skipUnreadyPipelines = value;
    return WriteSetting("video", "skip_unready_pipelines", value ? "true" : "false");
}

inline bool SetDisableCopyFilter(bool value) {
    Mutable().disableCopyFilter = value;
    return WriteSetting("video", "disable_copy_filter", value ? "true" : "false");
}

// Options > Display > Bloom (hle/otherm_bloom.cpp): the game's glow around bright lights, a kBloom* mode.
inline int32_t BloomMode() {
    return Get().bloomMode.value_or(kBloomOn);
}

inline bool SetBloomMode(int32_t mode) {
    Mutable().bloomMode = mode;
    return WriteSetting("video", "bloom", FormatString(kBloomModeNames[mode >= 0 && mode < 4 ? mode : kBloomOn]));
}

inline bool SetShaderCompilation(const std::string& value) {
    if (value != "ubershaders" && value != "skip" && value != "wait") return false;
    Mutable().shaderCompilation = value;
    return WriteSetting("video", "shader_compilation", FormatString(value));
}

inline bool SetUpscaler(const std::string& value) {
    if (value != "off" && value != "fsr") return false;
    Mutable().upscaler = value;
    return WriteSetting("video", "upscaler", FormatString(value));
}

inline bool SetDynamicResolution(bool value) {
    Mutable().dynamicResolution = value;
    return WriteSetting("video", "dynamic_resolution", value ? "true" : "false");
}

inline bool SetDynamicResolutionMax(float value) {
    if (!(value >= 1.0f && value <= 8.0f)) return false;
    Mutable().dynamicResolutionMax = value;
    char text[16];
    std::snprintf(text, sizeof(text), "%g", value);
    return WriteSetting("video", "dynamic_resolution_max", text);
}

inline bool SetFsrSharpness(float value) {
    if (!(value >= 0.0f && value <= 1.0f)) return false;
    Mutable().fsrSharpness = value;
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return WriteSetting("video", "fsr_sharpness", text);
}

inline bool SetAntiAliasing(const std::string& value) {
    if (value != "off" && value != "smaa") return false;
    Mutable().antiAliasing = value;
    return WriteSetting("video", "anti_aliasing", FormatString(value));
}

inline bool MousePointer(bool fallback = true) {
    return Get().mousePointer.value_or(fallback);
}

inline bool SetMousePointer(bool value) {
    Mutable().mousePointer = value;
    return WriteSetting("input", "mouse_pointer", value ? "true" : "false");
}

inline bool FrameGraph(bool fallback = false) {
    return Get().frameGraph.value_or(fallback);
}

inline bool SetFrameGraph(bool value) {
    Mutable().frameGraph = value;
    return WriteSetting("video", "frame_graph", value ? "true" : "false");
}

inline bool VSync(bool fallback = false) {
    return Get().vsync.value_or(fallback);
}

inline bool SetVSync(bool value) {
    Mutable().vsync = value;
    return WriteSetting("video", "vsync", value ? "true" : "false");
}

inline bool SetShowFps(bool value) {
    Mutable().showFps = value;
    return WriteSetting("video", "show_fps", value ? "true" : "false");
}

// Options > Textures switches these while the game runs (aurora_set_texture_packs).
inline bool SetTextureReplacements(bool value) {
    Mutable().textureReplacements = value;
    return WriteSetting("video", "texture_replacements", value ? "true" : "false");
}

inline bool SetTextureDumps(bool value) {
    Mutable().textureDumps = value;
    return WriteSetting("video", "texture_dumps", value ? "true" : "false");
}

inline bool SetDolphinTextures(bool value) {
    Mutable().dolphinTextures = value;
    return WriteSetting("video", "dolphin_textures", value ? "true" : "false");
}

inline bool SetPrefetchTextures(bool value) {
    Mutable().prefetchTextures = value;
    return WriteSetting("video", "prefetch_textures", value ? "true" : "false");
}

inline bool SetDolphinFolder(std::string value) {
    Mutable().dolphinFolder = value;
    return WriteSetting("paths", "dolphin_folder", FormatString(value));
}

inline bool SetDisabledPostProcessingPaths(uint32_t value) {
    Mutable().disabledPostProcessingPaths = value;
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::uppercase << value;
    return WriteSetting("video", "disabled_post_processing_paths", formatted.str());
}

inline bool SetControllerButton(size_t index, std::string value) {
    if (index >= kControllerButtonKeys.size()) {
        return false;
    }
    Mutable().controllerButtons[index] = value;
    return WriteSetting("controller", kControllerButtonKeys[index], FormatString(value));
}

inline std::string ControllerExpression(const std::string& key) {
    const auto it = Get().controllerExpressions.find(key);
    return it == Get().controllerExpressions.end() ? std::string() : it->second;
}

inline bool SetControllerExpression(const std::string& key, const std::string& value) {
    Mutable().controllerExpressions[key] = value;
    return WriteSetting("controller", key, FormatString(value));
}

// Config.toml [gamepad]; "" when the key is absent.
inline std::string GamepadBinding(const std::string& key) {
    const auto it = Get().gamepadBindings.find(key);
    return it == Get().gamepadBindings.end() ? std::string() : it->second;
}

inline std::string KeyboardBinding(const std::string& key) {
    const auto it = Get().keyboardBindings.find(key);
    return it == Get().keyboardBindings.end() ? std::string() : it->second;
}

inline bool SetKeyboardBinding(const std::string& key, const std::string& value) {
    Mutable().keyboardBindings[key] = value;
    return WriteSetting("keyboard", key, FormatString(value));
}

inline bool RumbleEnabled(bool fallback = true) {
    return Get().rumbleEnabled.value_or(fallback);
}

inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

// Vibration strength in percent (0 = off, 100 = a Wii Remote's, up to 200); an old `rumble = false` reads as 0.
inline int32_t RumbleStrength() {
    const auto& config = Get();
    if (config.rumbleStrength) return std::clamp(*config.rumbleStrength, 0, 200);
    return config.rumbleEnabled.value_or(true) ? 100 : 0;
}

inline bool SetRumbleStrength(int32_t percent) {
    percent = std::clamp(percent, 0, 200);
    Mutable().rumbleStrength = percent;
    Mutable().rumbleEnabled = percent > 0;
    WriteSetting("controller", "rumble", percent > 0 ? "true" : "false");
    return WriteSetting("controller", "rumble_strength", std::to_string(percent));
}

inline int32_t MuteHotkey(int32_t fallback) {
    return Get().muteHotkey.value_or(fallback);
}

inline bool SetMuteHotkey(int32_t value) {
    Mutable().muteHotkey = value;
    return WriteSetting("audio", "mute_key", std::to_string(value));
}

inline int32_t FastForwardSpeed(int32_t fallback = 3) {
    const int32_t speed = Get().fastForwardSpeed.value_or(fallback);
    return speed == 0 ? 0 : std::clamp(speed, 2, 8);  // 0: Max
}

inline bool SetFastForwardSpeed(int32_t value) {
    Mutable().fastForwardSpeed = value;
    return WriteSetting("game", "fast_forward_speed", std::to_string(value));
}

// Options > Game > Skip cutscenes (hle/otherm_cutscene.cpp): every in-engine cutscene, or only the ones the game
// itself lets be skipped (seen before, the game cleared, or marked by its scripts).
inline bool SkipAnyCutscene(bool fallback = true) {
    return Get().skipAnyCutscene.value_or(fallback);
}

inline bool SetSkipAnyCutscene(bool value) {
    Mutable().skipAnyCutscene = value;
    return WriteSetting("game", "skip_cutscenes", FormatString(value ? "any" : "seen"));
}

// Options > Game > Voices (hle/otherm_language.cpp): the Japanese voice acting, which the USA disc carries too.
inline bool JapaneseVoices() {
    return Get().japaneseVoices.value_or(false);
}

inline bool SetJapaneseVoices(bool value) {
    Mutable().japaneseVoices = value;
    return WriteSetting("game", "voices", FormatString(value ? "jp" : "en"));
}

// Options > Game > Text language and Subtitles (hle/otherm_language.cpp): the game's own language index, -1 for the
// game's own setting, -2 for subtitles off.
inline int32_t TextLanguage() {
    return Get().textLanguage.value_or(-1);
}

inline int32_t SubtitleLanguage() {
    return Get().subtitleLanguage.value_or(-1);
}

inline bool SetTextLanguage(int32_t index) {
    Mutable().textLanguage = index;
    return WriteSetting("game", "text_language", FormatString(GameLanguageCode(index)));
}

inline bool SetSubtitleLanguage(int32_t index) {
    Mutable().subtitleLanguage = index;
    return WriteSetting("game", "subtitles", FormatString(GameLanguageCode(index)));
}

inline bool FastForwardToggle(bool fallback = false) {
    return Get().fastForwardToggle.value_or(fallback);
}

inline bool SetFastForwardToggle(bool value) {
    Mutable().fastForwardToggle = value;
    return WriteSetting("game", "fast_forward_toggle", value ? "true" : "false");
}

inline bool CheckUpdates(bool fallback = true) {
    return Get().checkUpdates.value_or(fallback);
}

inline bool SetCheckUpdates(bool value) {
    Mutable().checkUpdates = value;
    return WriteSetting("game", "check_updates", value ? "true" : "false");
}

// The start screen (settings_overlay.h, launcher): shown before the game boots unless turned off here. It shows anyway
// while the game's files are not set up.
inline bool ShowLauncher(bool fallback = true) {
    return Get().showLauncher.value_or(fallback);
}

inline bool SetShowLauncher(bool value) {
    Mutable().showLauncher = value;
    return WriteSetting("game", "show_launcher", value ? "true" : "false");
}

inline bool SaveAnywhere(bool fallback = true) {
    return Get().saveAnywhere.value_or(fallback);
}

inline bool SetSaveAnywhere(bool value) {
    Mutable().saveAnywhere = value;
    return WriteSetting("game", "save_anywhere", value ? "true" : "false");
}

inline bool InfiniteEnergy(bool fallback = false) {
    return Get().infiniteEnergy.value_or(fallback);
}

inline bool SetInfiniteEnergy(bool value) {
    Mutable().infiniteEnergy = value;
    return WriteSetting("cheats", "infinite_energy", value ? "true" : "false");
}

inline bool InfiniteMissiles(bool fallback = false) {
    return Get().infiniteMissiles.value_or(fallback);
}

inline bool SetInfiniteMissiles(bool value) {
    Mutable().infiniteMissiles = value;
    return WriteSetting("cheats", "infinite_missiles", value ? "true" : "false");
}

inline bool AnalogMovement(bool fallback = false) {
    return Get().analogMovement.value_or(fallback);
}

inline bool SetAnalogMovement(bool value) {
    Mutable().analogMovement = value;
    return WriteSetting("cheats", "analog_movement", value ? "true" : "false");
}

inline bool MinimapNorthUp(bool fallback = false) {
    return Get().minimapNorthUp.value_or(fallback);
}

inline bool SetMinimapNorthUp(bool value) {
    Mutable().minimapNorthUp = value;
    return WriteSetting("cheats", "minimap_north_up", value ? "true" : "false");
}

inline bool FasterMenus(bool fallback = true) {
    return Get().fasterMenus.value_or(fallback);
}

inline bool SetFasterMenus(bool value) {
    Mutable().fasterMenus = value;
    return WriteSetting("game", "faster_menus", value ? "true" : "false");
}

inline bool SetAudioVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "volume", formatted.str());
}

inline bool SetAudioMuted(bool value) {
    Mutable().audioMuted = value;
    return WriteSetting("audio", "muted", value ? "true" : "false");
}

inline bool SetAudioMixWorker(bool value) {
    Mutable().audioMixWorker = value;
    return WriteSetting("audio", "mix_worker", value ? "true" : "false");
}

inline bool WidescreenEnabled(bool fallback = false) {
    return Get().widescreen.value_or(fallback);
}

inline bool ForceAspect169Enabled(bool fallback = false) {
    return Get().forceAspect169.value_or(fallback);
}

// SC language code the game sees (SCGetLanguage): 0 jp, 1 en, 2 de, 3 fr, 4 es, 5 it, 6 nl. The PAL disc picks its
// msg/ folder from it; the USA disc only has English.
inline uint32_t Language() {
    static constexpr const char* kCodes[] = {"jp", "en", "de", "fr", "es", "it", "nl"};
    const std::string value = Get().language.value_or("en");
    for (uint32_t i = 0; i < 7; ++i) {
        if (value == kCodes[i]) return i;
    }
    return 1;
}

// Display aspect (DisplaySettings): 0 original 4:3, 1 16:9, 2 fill the window or screen, 3 cinematic 2.39:1.
inline int32_t AspectMode(int32_t fallback) {
    return Get().aspectMode.value_or(fallback);
}

inline int32_t GpuCompat() {
    return Get().gpuCompat.value_or(0);
}

inline bool SetGpuCompat(int32_t value) {
    Mutable().gpuCompat = value;
    return WriteSetting("video", "gpu_compat", std::to_string(value));
}

inline bool SetAspectMode(int32_t value) {
    Mutable().aspectMode = value;
    return WriteSetting("video", "aspect_mode", std::to_string(value));
}

inline bool SetForceAspect169(bool value) {
    Mutable().forceAspect169 = value;
    return WriteSetting("video", "force_16_9", value ? "true" : "false");
}

inline bool WindowPosition(int32_t& x, int32_t& y) {
    if (!Get().windowPosX || !Get().windowPosY) {
        return false;
    }
    x = *Get().windowPosX;
    y = *Get().windowPosY;
    return true;
}

inline uint32_t WindowWidth(uint32_t fallback) {
    return Get().windowWidth.value_or(fallback);
}

inline uint32_t WindowHeight(uint32_t fallback) {
    return Get().windowHeight.value_or(fallback);
}

inline float ResolutionMultiplier(float fallback = 1.0f) {
    return std::max(0.0f, Get().resolutionMultiplier.value_or(fallback));
}

inline bool MetalFxSpatialUpscaling(bool fallback = false) {
    return Get().metalFxSpatialUpscaling.value_or(fallback);
}

inline float AudioVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVolume.value_or(fallback), 0.0f, 1.0f);
}

inline bool AudioMuted(bool fallback = false) {
    return Get().audioMuted.value_or(fallback);
}

// Off-thread AX/DSP mix. Default off for Other M: the deferred aux (reverb) hand-over assumes one AI block per
// tick, and runs of blocks (fast-forward, a stall's catch-up) flipped it into crackling for tens of seconds
// (NOTES.md 2026-11-08). true brings the worker back.
inline bool AudioMixWorkerEnabled(bool fallback = false) {
    return Get().audioMixWorker.value_or(fallback);
}

// Bluetooth Wii Remotes / Wii U Pro Controllers. Read once before SDL's joystick
// subsystem comes up, so a change only takes effect on the next launch.
inline bool WiiRemotesEnabled(bool fallback = true) {
    return Get().wiiRemotes.value_or(fallback);
}

// Persists the Bluetooth Wii Remote driver switch.
inline bool SetWiiRemotesEnabled(bool value) {
    Mutable().wiiRemotes = value;
    return WriteSetting("controller", "wii_remotes", value ? "true" : "false");
}

// Whether to keep rescanning Bluetooth while no Wii controller is connected.
inline bool WiiContinuousScanEnabled(bool fallback = false) {
    return Get().wiiContinuousScan.value_or(fallback);
}

// Persists the continuous scanning switch.
inline bool SetWiiContinuousScanEnabled(bool value) {
    Mutable().wiiContinuousScan = value;
    return WriteSetting("controller", "wii_continuous_scan", value ? "true" : "false");
}

// Wii Remote accelerometer zero-point correction (g, SDL sensor frame); all zero
// when the remote has not been calibrated.
inline std::array<double, 3> WiiAccelOffset() {
    const RuntimeUserConfig& config = Get();
    return {config.wiiAccelOffsetX.value_or(0.0), config.wiiAccelOffsetY.value_or(0.0),
            config.wiiAccelOffsetZ.value_or(0.0)};
}

// Whether to write the per-frame accelerometer trace (off unless asked for).
inline bool WiiAccelTraceEnabled(bool fallback = false) {
    return Get().wiiAccelTrace.value_or(fallback);
}

// True while a non-zero correction is stored ("Clear calibration" writes zeros).
inline bool HasWiiAccelOffset() {
    const std::array<double, 3> offset = WiiAccelOffset();
    return offset[0] != 0.0 || offset[1] != 0.0 || offset[2] != 0.0;
}

// Persists the accelerometer correction measured by the overlay's calibration.
inline bool SetWiiAccelOffset(const std::array<double, 3>& offset) {
    Mutable().wiiAccelOffsetX = offset[0];
    Mutable().wiiAccelOffsetY = offset[1];
    Mutable().wiiAccelOffsetZ = offset[2];
    bool ok = true;
    const char* keys[3] = {"wii_accel_offset_x", "wii_accel_offset_y", "wii_accel_offset_z"};
    for (size_t i = 0; i < 3; ++i) {
        // Always a float literal, so a whole-number offset does not come back as a TOML integer.
        std::ostringstream formatted;
        formatted << std::fixed << std::setprecision(4) << offset[i];
        ok = WriteSetting("controller", keys[i], formatted.str()) && ok;
    }
    return ok;
}

// Target frame rate for frame interpolation, or 0 to disable it.
inline uint32_t FrameInterpolationFps(uint32_t fallback = 0) {
    return Get().frameInterpolationFps.value_or(fallback);
}

// Whether to skip draws whose graphics pipeline has not finished compiling yet.
inline bool SkipUnreadyPipelines(bool fallback = true) {
    return Get().skipUnreadyPipelines.value_or(fallback);
}

inline bool DisableCopyFilter(bool fallback = true) {
    return Get().disableCopyFilter.value_or(fallback);
}

// "ubershaders", "skip" or "wait". A config from before the setting existed keeps its choice: skip_unready_pipelines
// = false was waiting; skipping (the old default) becomes ubershaders, which neither waits nor skips.
inline std::string ShaderCompilation() {
    if (Get().shaderCompilation) return *Get().shaderCompilation;
    return Get().skipUnreadyPipelines.value_or(true) ? "ubershaders" : "wait";
}

inline std::string Upscaler(std::string fallback = "off") { return Get().upscaler.value_or(std::move(fallback)); }
inline float FsrSharpness(float fallback = 0.5f) { return Get().fsrSharpness.value_or(fallback); }
inline bool DynamicResolution(bool fallback = false) { return Get().dynamicResolution.value_or(fallback); }
inline float DynamicResolutionMax(float fallback = 4.5f) { return Get().dynamicResolutionMax.value_or(fallback); }
inline std::string AntiAliasing(std::string fallback = "off") { return Get().antiAliasing.value_or(std::move(fallback)); }

inline bool ShowFps(bool fallback = true) {
    return Get().showFps.value_or(fallback);
}

inline bool TextureReplacements(bool fallback = true) {
    return Get().textureReplacements.value_or(fallback);
}

// Dumping only produces the names a replacement would need, so main.cpp
// gates it on TextureReplacements() as well.
inline bool TextureDumps(bool fallback = false) {
    return Get().textureDumps.value_or(fallback);
}

inline bool DolphinTextures(bool fallback = false) {
    return Get().dolphinTextures.value_or(fallback);
}

inline bool PrefetchTextures(bool fallback = false) {
    return Get().prefetchTextures.value_or(fallback);
}

inline std::string DolphinFolder(std::string fallback = "") {
    return Get().dolphinFolder.value_or(std::move(fallback));
}

inline uint32_t DisabledPostProcessingPaths(uint32_t fallback = 0) {
    return Get().disabledPostProcessingPaths.value_or(fallback) & 0x10u;
}

inline std::string GraphicsApi(std::string fallback = "auto") {
    return Get().graphicsApi.value_or(std::move(fallback));
}

// Read at start only: a change takes effect the next time the game starts.
inline bool SetGraphicsApi(const std::string& value) {
    if (!IsSupportedGraphicsApi(value)) return false;
    Mutable().graphicsApi = value;
    return WriteSetting("video", "graphics_api", FormatString(value));
}

inline std::string DisplayMode(std::string fallback = "borderless") {
    return Get().displayMode.value_or(std::move(fallback));
}

inline bool NetworkEnabled(bool fallback = true) {
    return Get().networkEnabled.value_or(fallback);
}

// Discord Rich Presence (discord_presence.h). The key is the one the old presence used, so a player who turned that
// off keeps it off.
inline bool DiscordPresence(bool fallback = true) {
    return Get().discordPresence.value_or(fallback);
}

inline bool SetDiscordPresence(bool value) {
    Mutable().discordPresence = value;
    return WriteSetting("discord", "enabled", value ? "true" : "false");
}

inline std::string NandRoot(std::string fallback = "") {
    return Get().nandRoot.value_or(std::move(fallback));
}

inline std::string DvdRoot(std::string fallback = "") {
    return Get().dvdRoot.value_or(std::move(fallback));
}

// The one resolver for configured paths. A relative value means the same thing
// everywhere it can be configured: relative to the config file that named it,
// never to the process working directory (docs/WHEELWIZARD_CONTRACT.md).
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base,
                                               const std::string& value) {
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

inline std::filesystem::path ResolveRelativeToConfig(const std::string& value) {
    return ResolveRelativeTo(ResolveConfigPath().parent_path(), value);
}

// The extracted DATA directory. Empty when nothing is configured.
inline std::filesystem::path ResolvedDvdRoot() {
    const std::string configured = DvdRoot();
    return configured.empty() ? std::filesystem::path{} : ResolveRelativeToConfig(configured);
}

// A game folder for this run only, not saved (a "game" folder beside the program wins over the saved one).
inline void UseDvdRootForThisRun(const std::filesystem::path& path) {
    Mutable().dvdRoot = PathToUtf8(path);
}

// The game folder the start screen set up (game_folder.h).
inline bool SetDvdRoot(const std::filesystem::path& path) {
    const std::string value = PathToUtf8(path);
    Mutable().dvdRoot = value;
    return WriteSetting("paths", "dvd_root", FormatString(value));
}

inline const std::vector<std::string>& OverlayRoots() {
    return Get().overlayRoots;
}

inline void LogLoadedConfig() {
    static const bool logged = [] {
        const auto& config = Get();
        const auto configPath = ResolveConfigPath();
        std::cout << "[runtime-config] " << PathToUtf8(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::cout << " not found; using built-in defaults";
        } else {
            std::cout << " loaded";
            if (config.widescreen) {
                std::cout << " widescreen=" << (*config.widescreen ? "true" : "false");
            }
            if (config.windowWidth || config.windowHeight) {
                std::cout << " window=" << config.windowWidth.value_or(0) << "x"
                          << config.windowHeight.value_or(0);
            }
            if (config.resolutionMultiplier) {
                std::cout << " resolution_multiplier=" << *config.resolutionMultiplier;
            }
            if (config.dvdRoot) {
                std::cout << " dvd_root=" << *config.dvdRoot;
            }
            if (config.graphicsApi) {
                std::cout << " graphics_api=" << *config.graphicsApi;
            }
            if (config.frameInterpolationFps) {
                std::cout << " frame_interpolation_fps=" << *config.frameInterpolationFps;
            }
            if (config.skipUnreadyPipelines) {
                std::cout << " skip_unready_pipelines=" << (*config.skipUnreadyPipelines ? "true" : "false");
            }
            if (config.shaderCompilation) {
                std::cout << " shader_compilation=" << *config.shaderCompilation;
            }
            if (config.fastForwardSpeed) {
                std::cout << " fast_forward_speed=" << *config.fastForwardSpeed;
            }
            if (config.skipAnyCutscene) {
                std::cout << " skip_cutscenes=" << (*config.skipAnyCutscene ? "any" : "seen");
            }
            if (config.japaneseVoices) {
                std::cout << " voices=" << (*config.japaneseVoices ? "jp" : "en");
            }
            if (config.textLanguage) {
                std::cout << " text_language=" << GameLanguageCode(*config.textLanguage);
            }
            if (config.subtitleLanguage) {
                std::cout << " subtitles=" << GameLanguageCode(*config.subtitleLanguage);
            }
            if (config.disableCopyFilter) {
                std::cout << " disable_copy_filter=" << (*config.disableCopyFilter ? "true" : "false");
            }
            if (config.bloomMode) {
                std::cout << " bloom=" << kBloomModeNames[*config.bloomMode];
            }
            if (config.showFps) {
                std::cout << " show_fps=" << (*config.showFps ? "true" : "false");
            }
            if (config.textureReplacements) {
                std::cout << " texture_replacements=" << (*config.textureReplacements ? "true" : "false");
            }
            if (config.textureDumps) {
                std::cout << " texture_dumps=" << (*config.textureDumps ? "true" : "false");
            }
            if (config.audioVolume) {
                std::cout << " audio_volume=" << *config.audioVolume;
            }
            if (config.audioMuted) {
                std::cout << " audio_muted=" << (*config.audioMuted ? "true" : "false");
            }
            if (config.networkEnabled) {
                std::cout << " network_enabled=" << (*config.networkEnabled ? "true" : "false");
            }
            if (config.discordPresence) {
                std::cout << " discord_presence=" << (*config.discordPresence ? "true" : "false");
            }
            if (config.nandRoot) {
                std::cout << " nand_root=" << *config.nandRoot;
            }
        }
        std::cout << std::endl;
        return true;
    }();
    (void)logged;
}

} // namespace RuntimeConfigFile
