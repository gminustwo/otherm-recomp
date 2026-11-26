#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <unordered_map>
#include <string>
#include <cstdlib>
extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);
#include "game_layout.h"
#include "standalone_save_state.h"
#include "settings_overlay.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
#include "hud_theme.h"
#include "keybinds.h"
#include "game_graphics_options.h"
#include "runtime_config.h"
#include "game_folder.h"
#include "otherm_cheats.h"
#include "otherm_minimap.h"
#include "otherm_move.h"
#include "display_settings.h"
#include "dynamic_resolution.h"
#include "gpu_load.h"
#include <dolphin/gx/GXAurora.h>
#include "runtime_log.h"
#include "memory.h"
#include "wii_remote_input.h"
#include "virtual_remote.h"
#include "controller_gate.h"
#include "vulkan_probe.h"
#if defined(__ANDROID__)
#include "android_bridge.h"
#include <SDL3/SDL_system.h>
#endif

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <cfloat>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dbt.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include "updater.h"
#include "discord_presence.h"
#include "mod_install.h"
#include "hle/storage/riivolution.h"
#include "mod_runtime.h"
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_dialog.h>
#include "dolphin_import.h"
#include <filesystem>
#include <vector>
#include <fstream>
#include <cstring>
#include <mutex>
#include <optional>

extern "C" int g_gxFrameCount;
void VI_HLE_SetSpeed(uint32_t speed);
uint32_t VI_HLE_PresentedFrames() noexcept;  // hle_stubs.h: frames presented (OTHERM_KEY_SCRIPT counts by it)

// hle/otherm_cutscene.cpp: skip the movie or in-engine cutscene that is playing (the game's own skip for the latter).
void OthermRequestCutsceneSkip();
bool OthermCutscenePlaying();
bool OthermMovieUp();
// hle/otherm_language.cpp: a changed text or subtitle language's code taps; the game has started (its text language is
// set until the next start); the Japanese fonts loaded at that start.
void OthermLanguageApply();
bool OthermLanguageStarted();
bool OthermJapaneseFontLoaded();
uint32_t VI_HLE_RetraceCount() noexcept;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.

namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

struct DebugText { const char* text[5]; };   // en, de, fr, es, it
constexpr DebugText kDebugText[] = {
    {{"General", "Allgemein", "Général", "General", "Generali"}},
    {{"Audio", "Audio", "Audio", "Audio", "Audio"}},
    {{"Controls", "Steuerung", "Contrôles", "Controles", "Controlli"}},
    {{"Resolution", "Auflösung", "Résolution", "Resolución", "Risoluzione"}},
    {{"Down", "Unten", "Bas", "Abajo", "Giù"}},
    {{"Left", "Links", "Gauche", "Izquierda", "Sinistra"}},
    {{"Right", "Rechts", "Droite", "Derecha", "Destra"}},
    {{"Mute", "Stumm", "Muet", "Silenciar", "Muto"}},
    {{"Shader compilation", "Shader-Kompilierung", "Compilation des shaders", "Compilación de shaders", "Compilazione degli shader"}},
    {{"Ubershaders", "Ubershader", "Ubershaders", "Ubershaders", "Ubershader"}},
    {{"Skip draws", "Zeichnen überspringen", "Ignorer les dessins", "Omitir dibujos", "Salta i disegni"}},
    {{"Wait", "Warten", "Attendre", "Esperar", "Attendi"}},
    {{"Windowed", "Fenster", "Fenêtré", "Ventana", "Finestra"}},
    {{"Exclusive fullscreen", "Exklusiver Vollbildmodus", "Plein écran exclusif", "Pantalla completa exclusiva", "Schermo intero esclusivo"}},
    {{"Off", "Aus", "Désactivé", "Desactivado", "Disattivato"}},
    {{"Vibration", "Vibration", "Vibration", "Vibración", "Vibrazione"}},
    {{"Original 4:3", "Original 4:3", "Original 4:3", "Original 4:3", "Originale 4:3"}},
    {{"Cinematic 2.39:1", "Kinoformat 2.39:1", "Cinéma 2.39:1", "Cinemático 2.39:1", "Cinematografico 2.39:1"}},
    {{"Auto (screen size)", "Automatisch (Bildschirmgröße)", "Auto (taille de l'écran)", "Auto (tamaño de pantalla)", "Auto (dimensione schermo)"}},
};

// Debug menu text in the game's language (falls back to the English text itself).
const char* Tr(const char* en);

int g_debugLanguage = -1;
int DebugLanguageIndex();

const char* Tr(const char* en) {
    const int lang = DebugLanguageIndex();
    for (const auto& row : kDebugText)
        if (std::strcmp(row.text[0], en) == 0) return row.text[lang];
    return en;
}

int DebugLanguageIndex() {
    if (g_debugLanguage >= 0) return g_debugLanguage;
    switch (RuntimeConfigFile::Language()) {
    case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    case 5: return 4;
    default: return 0;
    }
}

// Every ImGui text call of this file goes through ImT, which looks the text up in the translation table
// (the English text itself when there is no entry), so a missing translation never hides anything.
namespace ImT {
template <class... A> bool Button(const char* l, A&&... a) { return ImGui::Button(Tr(l), std::forward<A>(a)...); }
template <class... A> bool SmallButton(const char* l, A&&... a) { return ImGui::SmallButton(Tr(l), std::forward<A>(a)...); }
template <class... A> bool Checkbox(const char* l, A&&... a) { return ImGui::Checkbox(Tr(l), std::forward<A>(a)...); }
template <class... A> bool RadioButton(const char* l, A&&... a) { return ImGui::RadioButton(Tr(l), std::forward<A>(a)...); }
template <class... A> bool SliderInt(const char* l, A&&... a) { return ImGui::SliderInt(Tr(l), std::forward<A>(a)...); }
template <class... A> bool SliderFloat(const char* l, A&&... a) { return ImGui::SliderFloat(Tr(l), std::forward<A>(a)...); }
template <class... A> bool Selectable(const char* l, A&&... a) { return ImGui::Selectable(Tr(l), std::forward<A>(a)...); }
template <class... A> bool MenuItem(const char* l, A&&... a) { return ImGui::MenuItem(Tr(l), std::forward<A>(a)...); }
template <class... A> bool InputInt(const char* l, A&&... a) { return ImGui::InputInt(Tr(l), std::forward<A>(a)...); }
template <class... A> bool InputText(const char* l, A&&... a) { return ImGui::InputText(Tr(l), std::forward<A>(a)...); }
inline bool InputTextWithHint(const char* l, const char* hint, char* buf, size_t size, ImGuiInputTextFlags f = 0) {
    return ImGui::InputTextWithHint(Tr(l), Tr(hint), buf, size, f);
}
inline bool BeginMenu(const char* l, bool enabled = true) { return ImGui::BeginMenu(Tr(l), enabled); }
inline bool BeginCombo(const char* l, const char* preview, ImGuiComboFlags f = 0) { return ImGui::BeginCombo(Tr(l), Tr(preview), f); }
inline void SeparatorText(const char* l) { ImGui::SeparatorText(Tr(l)); }
template <class... A> void Text(const char* f, A&&... a) { ImGui::Text(Tr(f), std::forward<A>(a)...); }
template <class... A> void TextDisabled(const char* f, A&&... a) { ImGui::TextDisabled(Tr(f), std::forward<A>(a)...); }
template <class... A> void TextWrapped(const char* f, A&&... a) { ImGui::TextWrapped(Tr(f), std::forward<A>(a)...); }
template <class... A> void BulletText(const char* f, A&&... a) { ImGui::BulletText(Tr(f), std::forward<A>(a)...); }
template <class... A> void SetTooltip(const char* f, A&&... a) { ImGui::SetTooltip(Tr(f), std::forward<A>(a)...); }
inline void TextUnformatted(const char* t, const char* end = nullptr) { ImGui::TextUnformatted(end ? t : Tr(t), end); }
inline bool Combo(const char* l, int* cur, const char* const items[], int n, int height = -1) {
    std::vector<const char*> translated;
    for (int i = 0; i < n; ++i) translated.push_back(Tr(items[i]));
    return ImGui::Combo(Tr(l), cur, translated.data(), n, height);
}
inline bool Combo(const char* l, int* cur, const char* zeroSeparated, int height = -1) { return ImGui::Combo(Tr(l), cur, zeroSeparated, height); }
}  // namespace ImT


const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

using Clock = std::chrono::steady_clock;
// Options menu and fast-forward state (the menu itself is drawn further down, DrawOptionsMenu).
enum : uint32_t {
    kOptLeft = 1, kOptRight = 2, kOptUp = 4, kOptDown = 8, kOptConfirm = 16, kOptBack = 32,
    kOptPagePrev = 64, kOptPageNext = 128,  // LB / RB (Q / E, Page Up / Page Down; - / + on a Wii Remote)
};
std::atomic<uint32_t> g_optEvents{0};
std::atomic<bool> g_optToggleRequest{false};
std::atomic<bool> g_optOpenRequest{false};  // RequestOptionsOpen (the tray icon's menu)
bool g_optOpen = false;
int g_optTab = 0, g_optRow = 0, g_optFirst = 0;  // g_optFirst: the first row shown (the list scrolls)
// A question over the menu (quit, import a save): `action` runs on the confirm button.
struct OptPrompt {
    std::string title, confirm, note;
    std::function<void()> action;
};
std::optional<OptPrompt> g_optPrompt;
int g_optPromptChoice = 1;        // 0 confirm, 1 Cancel
bool g_optMuteCapture = false;    // Options > Sound > Mute key: waiting for the key
std::string g_saveStatus;         // Options > Saves: what the last import or folder pick did
uint32_t g_optPrevHold = 0, g_optSuppress = 0;
Clock::time_point g_optLastToggle{}, g_optOpenedAt{};
std::array<Clock::time_point, 4> g_optRepeatAt{};

// The start screen before the game boots (vi.cpp VI_HLE_RunLauncher): Options in a launcher mode. Its first page,
// START, plays and sets the game up; the folder browser finds the folder with the player's disc image; LB / RB reach
// the Options pages (without what needs the game running). The game boots once Play is picked.
enum class LauncherScreen { Start, Browse, SettingUp };
struct LauncherState {
    bool active = false;
    LauncherScreen screen = LauncherScreen::Start;
    bool ready = false;                        // GameFolder::Ready(), read when START is shown
    std::string describe;                      // GameFolder::Describe(), then too
    std::vector<std::filesystem::path> beside; // disc images in the game's own folder (offered on START)
    std::string status;                        // the last set-up's result, shown on START
    bool statusOk = false;
    std::filesystem::path folder;              // the browser's folder; empty: the list of places
    bool listed = false;                       // `folders` and `contents` are of `folder`
    std::filesystem::path listedFolder;
    std::vector<std::filesystem::path> folders;
    GameFolder::FolderContents contents;
    std::vector<std::pair<std::string, std::filesystem::path>> places;
    bool pickFirst = false;                    // put the cursor on the page's first row that does something
};
LauncherState g_launcher;
bool g_launcherPlaysAfterInstall = false;  // up only for mod zips being unpacked: Play once they are

// Fast-forward (held or toggled) and faster menus / loading. g_ffSpeed 0 is Max: as fast as the PC runs the game
// (kFastForwardMax for the clock, which the game never reaches).
int g_ffSpeed = RuntimeConfigFile::FastForwardSpeed(3);
constexpr uint32_t kFastForwardMax = 64;
std::string FastForwardSpeedLabel() { return g_ffSpeed == 0 ? std::string("Max") : std::to_string(g_ffSpeed) + "x"; }
bool g_padStartAlone = false;  // the pad's Start went down without Back: its release skips a cutscene
bool g_padBackHeld = false;
bool g_ffOn = false;  // the player's fast-forward is running (UpdateGameSpeed), for the corner badge
bool g_ffToggleMode = RuntimeConfigFile::FastForwardToggle(false);
// Options > Controls captures.
int g_optPadCapture = -1;            // controller row waiting for a button, or -1
std::atomic<bool> g_ffKeyHeld{false}, g_ffPadHeld{false}, g_ffToggled{false};
uint32_t g_appliedSpeed = 1;
void RequestOptionsToggle();
int g_rumbleStrength = RuntimeConfigFile::RumbleStrength();  // percent, 0-200 (Options > Controls > Vibration)
// Options > Controls' table: the column picked on a table row (0 controller, 1 keyboard), the remote control a
// controller cell is waiting for a button for, and what the last controller binding did.
int g_optCol = 0;
int g_optPadBindControl = -1;
// Options > Controls, the Save state / Load state rows' controller cells: the chord being captured
// (VirtualRemote::PadHotkey), or -1. Inputs held when it began (the button that picked the cell) count once they are
// let go and pressed again. From the first input held, whatever is held kChordHold later is the chord, counted down on
// screen; letting everything go starts over, and 10 s with nothing held gives up.
int g_optChordCapture = -1;
bool g_optChordKeys = false;  // the keyboard column's (keys by SDL name), else the controller's (Dolphin input names)
std::vector<std::string> g_optChordIgnored, g_optChordHeld;
bool g_optChordCounting = false;
Clock::time_point g_optChordSince{}, g_optChordDeadline{};
constexpr auto kChordHold = std::chrono::seconds(5);
std::string g_controlsStatus;
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled();
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 90:
        return 1;
    case 120:
        return 2;
    case 144:
    case 180:  // the choices stop at 144 (the developer's call): older 180 / 240 settings come in as 144
    case 240:
        return 3;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("borderless");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
// What a draw does while its shader compiles: 0 ubershaders, 1 skip it, 2 wait (Config.toml [video] shader_compilation).
constexpr std::array<const char*, 3> kShaderCompilationNames{"ubershaders", "skip", "wait"};
int g_shaderCompilation = [] {
    const std::string value = RuntimeConfigFile::ShaderCompilation();
    return value == "wait" ? 2 : value == "skip" ? 1 : 0;
}();
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(false);  // off unless turned on
bool g_frameGraph = RuntimeConfigFile::FrameGraph(false);
std::atomic<bool> g_vsync{RuntimeConfigFile::VSync(true)};  // default on: no tearing
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
// Options > Display: FSR-style upscaling and SMAA-style anti-aliasing (aurora webgpu/post.cpp).
bool g_fsrUpscaling = RuntimeConfigFile::Upscaler("off") == "fsr";
int g_fsrSharpnessPercent = static_cast<int>(std::lround(RuntimeConfigFile::FsrSharpness(0.5f) * 100.0f));
bool g_smaa = RuntimeConfigFile::AntiAliasing("off") == "smaa";
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(10);  // the mouse cursor, after no mouse activity
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;
std::atomic_bool g_bootShadersReady = false;
bool g_bootShaderNotice = false;
Clock::time_point g_bootShaderWaitStart{};
constexpr uint32_t kBootShaderNoticeThreshold = 100;
constexpr auto kBootShaderWaitLimit = std::chrono::minutes(3);

// Render heights: 1x is 480 lines in every aspect (DisplaySettings), the width follows the aspect.
constexpr std::array<ResolutionItem, 12> kResolutions = {{
    {"Auto (screen size)", 0.0f}, {"0.5x (240p)", 0.5f}, {"0.75x (360p)", 0.75f}, {"1x (480p)", 1.0f},
    {"1.5x (720p)", 1.5f}, {"2x (960p)", 2.0f}, {"2.25x (1080p)", 2.25f}, {"3x (1440p)", 3.0f},
    {"4x (1920p)", 4.0f}, {"4.5x (4K)", 4.5f}, {"6x (2880p)", 6.0f}, {"8x (3840p)", 8.0f},
}};
// The highest scale Dynamic resolution may reach (Options > Display > Dynamic up to).
constexpr std::array<ResolutionItem, 6> kDynamicMax = {{
    {"1.5x (720p)", 1.5f}, {"2x (960p)", 2.0f}, {"2.25x (1080p)", 2.25f}, {"3x (1440p)", 3.0f}, {"4x (1920p)", 4.0f},
    {"4.5x (4K)", 4.5f},
}};

struct AspectItem {
    const char* label;
    int32_t mode;
};
constexpr std::array<AspectItem, 4> kAspects = {{
    {"Original 4:3", DisplaySettings::kAspect4x3},
    {"16:9", DisplaySettings::kAspect16x9},
    {"Fill screen", DisplaySettings::kAspectFill},
    {"Cinematic 2.39:1", DisplaySettings::kAspectCinematic},
}};

// 60 (the game's own), 90, 120, 144: 90 and 144 put presents on an even grid between the game's frames (aurora
// frame_interpolation.cpp). aurora lowers a target the display cannot show (144 on a 120 Hz screen runs at 120).
constexpr std::array<uint32_t, 4> kFrameInterpolationTargetFps{0, 90, 120, 144};
constexpr std::array<const char*, 4> kFrameRateNames{"60 FPS (original)", "90 FPS", "120 FPS", "144 FPS"};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.renderScale = scale;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void SetAspectMode(int32_t mode) {
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.aspect = mode;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetAspectMode(mode);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void ApplyConfiguredMappings() {
    const ControllerGate gate;
    if (!gate) return;  // a device scan holds the controllers: the next frame applies them
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

// The last frames' times (the UI's frame is the presented one), for the graph.
constexpr int kFrameTimeCount = 180;
float g_frameTimes[kFrameTimeCount] = {};
int g_frameTimeNext = 0;

void DrawFrameGraph() {
    float last = g_frameTimes[(g_frameTimeNext + kFrameTimeCount - 1) % kFrameTimeCount];
    float peak = 0.0f, sum = 0.0f;
    int samples = 0;
    for (float t : g_frameTimes) {
        if (t <= 0.0f) continue;
        peak = std::max(peak, t);
        sum += t;
        ++samples;
    }
    ImT::Text("Frame: %.1f ms", last);
    ImT::TextDisabled("avg %.1f  max %.1f", samples > 0 ? sum / samples : 0.0f, peak);
    const float scale = ImGui::GetFontSize() / 13.0f;
    const ImVec2 size(kFrameTimeCount * scale, 50.0f * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* list = ImGui::GetWindowDrawList();
    // 0 to 33.3 ms (two 60 Hz frames), more when a frame took longer.
    const float top = std::max(100.0f / 3.0f, peak * 1.1f);
    const auto yOf = [&](float ms) { return origin.y + size.y - std::min(ms / top, 1.0f) * size.y; };
    list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 90));
    for (const float line : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
        if (line < top) {
            list->AddLine(ImVec2(origin.x, yOf(line)), ImVec2(origin.x + size.x, yOf(line)), IM_COL32(255, 255, 255, 60));
        }
    }
    const float w = size.x / kFrameTimeCount;
    for (int i = 0; i < kFrameTimeCount; ++i) {
        const float t = g_frameTimes[(g_frameTimeNext + i) % kFrameTimeCount];
        if (t <= 0.0f) continue;
        const ImU32 color = t < 1000.0f / 55.0f ? IM_COL32(90, 220, 90, 230)
                            : t < 1000.0f / 28.0f ? IM_COL32(240, 200, 60, 230)
                                                  : IM_COL32(240, 80, 70, 240);
        const float x = origin.x + i * w;
        list->AddRectFilled(ImVec2(x, yOf(t)), ImVec2(x + std::max(w, 1.0f), origin.y + size.y), color);
    }
}

// Is the GPU what holds the frame rate down? The GPU's load as Linux reports it (gpu_load.h: AMD GPUs, the Steam Deck's
// among them; elsewhere unknown and nothing is shown). A Deck at 4.5x (3840x2376, 9x its 1280x800 screen's pixels)
// kept its GPU 98% busy at its top clock and ran at 50-58 FPS; at Auto (1280x792) it held 60 at ~29%, at 3x ~68%
// (NOTES.md 2026-11-24). aurora's own waits do not show it reliably: the GPU's backlog surfaces in the swapchain's
// acquire and Dawn's device lock, or not at all in a window.
struct GpuLimitState {
    Clock::time_point lastCheck{};
    bool limited = false;
    Clock::time_point lastLimited{};  // when a check last found the GPU at its limit
    uint64_t limitedPixels = 0;       // the picture's size then (AuroraGetPictureSize): a smaller one may keep up
};
GpuLimitState g_gpuLimit;

// Each presented frame (AdvancePresentedFrame), at most once a second: at the limit from 90% busy (three seconds'
// mean), and shown for at least 5 s after that so the note does not blink.
void UpdateGpuLimit() {
    GpuLoad::Start();
    const Clock::time_point now = Clock::now();
    GpuLimitState& s = g_gpuLimit;
    if (now - s.lastCheck < std::chrono::seconds(1)) return;
    s.lastCheck = now;
    const int busy = GpuLoad::Latest().busy3s;
    const bool atLimit = busy >= 90;
    const bool wasLimited = s.limited;
    s.limited = atLimit || (s.limited && now - s.lastLimited < std::chrono::seconds(5));
    uint32_t width = 0, height = 0;
    if (atLimit || s.limited != wasLimited) AuroraGetPictureSize(&width, &height);
    if (atLimit) {
        s.lastLimited = now;
        s.limitedPixels = uint64_t(width) * height;
    }
    if (s.limited != wasLimited) {  // the log says so too, at most 20 times a session
        static int s_lines = 0;
        if (s_lines++ < 20)
            RT_LOGF(RT_TAG_RUNTIME, "[gpu] %s: %d%% busy, picture %ux%u\n",
                    s.limited ? "at its limit (FPS counter: GPU LIMITED)" : "below its limit again", busy, width, height);
    }
}

bool GpuLimited() { return g_gpuLimit.limited; }

// For Options (it pauses the game, so the GPU idles while it is open): limited within the last 30 s at a picture of
// this size or larger.
bool GpuLimitedRecentlyAt(uint32_t width, uint32_t height) {
    const GpuLimitState& s = g_gpuLimit;
    return s.lastLimited != Clock::time_point{} && Clock::now() - s.lastLimited <= std::chrono::seconds(30) &&
           uint64_t(width) * height >= s.limitedPixels;
}

// Top right, in the menus' style (hud_theme.cpp) and sized to the window: the frame rate (Options > Display > FPS
// counter) and, while fast-forward runs, its speed as set in Options > Game. Fast-forward is limited by how fast the
// CPU runs the game, not by the setting, so when the game falls short the speed it reaches is shown under it.
// Returns the y below the last panel.
float DrawCornerHud(const AuroraPresentTiming& timing, float top) {
    if (!g_showFps && !g_ffOn) return top;
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float px = std::clamp(display.y / 26.0f, 22.0f, 64.0f);
    const float pad = px * 0.45f, gap = px * 0.4f, labelPx = px * 0.48f, labelTrack = labelPx * 0.18f;
    const float right = display.x - px * 0.5f;
    top += px * 0.5f;
    const auto panel = [&](const char* label, const std::string& value, const std::string& note, ImU32 accent,
                           const char* widest) {
        const float labelW = HudTheme::Measure(labelPx, label, labelTrack).x;
        const float valueW = std::max(HudTheme::Measure(px, value.c_str(), px * 0.04f).x,
                                      HudTheme::Measure(px, widest, px * 0.04f).x);
        const float noteW = note.empty() ? 0.0f : HudTheme::Measure(labelPx, note.c_str(), labelTrack).x;
        const float w = pad * 2 + std::max(labelW + gap + valueW, noteW);
        const float h = px * 1.35f + (note.empty() ? 0.0f : labelPx * 1.3f);
        const ImVec2 a(right - w, top), b(right, top + h);
        HudTheme::Panel(list, a, b, px * 0.35f, HudTheme::kPanelTop, HudTheme::kPanelBottom, HudTheme::Alpha(accent, 210),
                        std::max(1.0f, px / 22.0f));
        list->AddRectFilled(ImVec2(a.x, a.y + px * 0.35f), ImVec2(a.x + std::max(2.0f, px / 10.0f), b.y), accent);
        const float midY = top + px * 0.68f;
        HudTheme::Text(list, ImVec2(a.x + pad, midY), labelPx, HudTheme::kTextDim, label, 0.0f, 0.5f, labelTrack);
        HudTheme::Text(list, ImVec2(b.x - pad, midY), px, HudTheme::kText, value.c_str(), 1.0f, 0.5f, px * 0.04f,
                       HudTheme::Alpha(accent, 80));
        if (!note.empty())
            HudTheme::Text(list, ImVec2(b.x - pad, top + px * 1.35f + labelPx * 0.45f), labelPx, accent, note.c_str(), 1.0f,
                           0.5f, labelTrack);
        top = b.y + px * 0.3f;
    };
    if (g_showFps) {
        const bool known = timing.sampleCount != 0;
        std::string note;
        // Frames that only repeat the last one (interpolation could not run) count as presented but carry no motion.
        if (known && timing.effectiveFramesPerSecond < timing.framesPerSecond * 0.95)
            note = "MOTION " + std::to_string(std::lround(timing.effectiveFramesPerSecond));
        // The GPU cannot draw a frame in time at this resolution (Options > Display > Resolution says so too).
        if (known && GpuLimited()) note = note.empty() ? "GPU LIMITED" : "GPU LIMITED, " + note;
        panel("FPS", known ? std::to_string(std::lround(timing.framesPerSecond)) : "--", note, HudTheme::kCyan, "000");
    }
    if (g_ffOn) {
        // The speed reached is the game's own clock against real time: its retraces, 59.94 a second at 1x (a 30 FPS
        // cutscene presents half as many frames as it gets retraces). Measured over half a second.
        static auto s_since = std::chrono::steady_clock::now();
        static uint32_t s_retracesSince = VI_HLE_RetraceCount();
        static double s_reached = 0.0;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - s_since).count();
        if (seconds >= 0.5) {
            const uint32_t retraces = VI_HLE_RetraceCount();
            s_reached = seconds < 2.0 ? (retraces - s_retracesSince) / seconds / 59.94 : 0.0;
            s_since = now;
            s_retracesSince = retraces;
        }
        char note[32] = "";
        if (s_reached > 0.0 && (g_ffSpeed == 0 || s_reached < g_ffSpeed * 0.9))
            std::snprintf(note, sizeof note, g_ffSpeed == 0 ? "AT %.1fX" : "REACHING %.1fX", s_reached);
        panel(">> FAST FORWARD", g_ffSpeed == 0 ? std::string("MAX") : std::to_string(g_ffSpeed) + "X", note,
              HudTheme::kOrange, "MAX");
    }
    return top;
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    {
        const float dt = ImGui::GetIO().DeltaTime;
        g_frameTimes[g_frameTimeNext] = dt > 0.0f && dt < 1.0f ? dt * 1000.0f : 0.0f;
        g_frameTimeNext = (g_frameTimeNext + 1) % kFrameTimeCount;
    }
    const float top = DrawCornerHud(presentTiming, 0.0f);
    if (!g_frameGraph) {
        return;
    }
    // The frame-time graph (Config.toml frame_graph) stays an ImGui window, under the counter.
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top + kMargin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) DrawFrameGraph();
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImT::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible() && !g_launcher.active) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Startup", nullptr, kFlags)) {
        // A visor-style cover over the boot while the shaders compile: the emblem in a slowly turning ring, scan
        // lines, the title and the shaders' progress. It lifts by itself (vi.cpp, first visible frame).
        using namespace HudTheme;
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 o = viewport->Pos, sz = viewport->Size;
        const ImVec2 end(o.x + sz.x, o.y + sz.y);
        const float u = std::min(sz.x / 16.0f, sz.y / 9.0f) / 60.0f;  // 1 at 960x540
        const float t = static_cast<float>(ImGui::GetTime());
        list->AddRectFilledMultiColor(o, end, IM_COL32(4, 12, 26, 255), IM_COL32(4, 12, 26, 255), IM_COL32(0, 2, 6, 255),
                                      IM_COL32(0, 2, 6, 255));
        // A faint grid and the visor's scan lines.
        const float grid = 48.0f * u;
        for (float x = o.x + std::fmod(sz.x * 0.5f, grid); x < end.x; x += grid)
            list->AddLine(ImVec2(x, o.y), ImVec2(x, end.y), IM_COL32(80, 220, 255, 10), 1.0f);
        for (float y = o.y + std::fmod(sz.y * 0.5f, grid); y < end.y; y += grid)
            list->AddLine(ImVec2(o.x, y), ImVec2(end.x, y), IM_COL32(80, 220, 255, 10), 1.0f);
        Scanlines(list, o, end, std::max(3.0f, 3.0f * u), IM_COL32(0, 0, 0, 60));

        if (g_launcher.active) {
            // The start screen draws its panel over this backdrop: only the emblem, faint and large, behind it.
            Emblem(list, ImVec2(o.x + sz.x * 0.5f, o.y + sz.y * 0.5f), std::min(sz.x, sz.y) * 0.95f, IM_COL32(80, 220, 255, 16));
        } else {
        const ImVec2 c(o.x + sz.x * 0.5f, o.y + sz.y * 0.40f);
        const float r = 92.0f * u;
        // The ring: a dim circle, two arcs turning opposite ways, tick marks.
        list->AddCircle(c, r * 1.30f, IM_COL32(80, 220, 255, 40), 96, 1.0f * u);
        list->PathArcTo(c, r * 1.30f, t * 0.6f, t * 0.6f + 1.9f, 48);
        list->PathStroke(IM_COL32(80, 220, 255, 200), ImDrawFlags_None, 2.5f * u);
        list->PathArcTo(c, r * 1.42f, -t * 0.4f + 3.4f, -t * 0.4f + 4.4f, 32);
        list->PathStroke(IM_COL32(255, 150, 40, 200), ImDrawFlags_None, 2.0f * u);
        for (int i = 0; i < 48; ++i) {
            const float a = i * 0.1309f;
            const float in = r * (i % 4 == 0 ? 1.12f : 1.17f);
            list->AddLine(ImVec2(c.x + std::cos(a) * in, c.y + std::sin(a) * in),
                          ImVec2(c.x + std::cos(a) * r * 1.22f, c.y + std::sin(a) * r * 1.22f), IM_COL32(80, 220, 255, 60),
                          1.0f);
        }
        const float pulse = 0.5f + 0.5f * std::sin(t * 2.0f);
        Emblem(list, c, r * 2.04f, Alpha(kCyan, int(40 + 30 * pulse)));  // a glow behind
        if (!Emblem(list, c, r * 1.9f, IM_COL32(225, 244, 255, 255))) list->AddCircle(c, r, kCyan, 64, 3.0f * u);

        float y = c.y + r * 1.62f;
        Text(list, ImVec2(c.x, y), 34.0f * u, kText, "METROID: OTHER M", 0.5f, 0.0f, 7.0f * u, IM_COL32(80, 220, 255, 70));
        y += Measure(34.0f * u, "M").y + 10.0f * u;
        list->AddLine(ImVec2(c.x - 170 * u, y), ImVec2(c.x + 170 * u, y), IM_COL32(80, 220, 255, 120), 1.0f);
        list->AddLine(ImVec2(c.x - 40 * u, y), ImVec2(c.x + 40 * u, y), kOrange, 2.0f * u);
        y += 12.0f * u;
        y += 6.0f * u;
        {
            const bool ready = g_bootShadersReady.load(std::memory_order_relaxed);
            const uint32_t queued = aurora_get_queued_pipeline_count();
            char shaderLine[96] = {};
            if (ready) std::snprintf(shaderLine, sizeof(shaderLine), "SHADERS READY");
            else if (queued > 0) std::snprintf(shaderLine, sizeof(shaderLine), "COMPILING SHADERS  -  %u TO GO", queued);
            else std::snprintf(shaderLine, sizeof(shaderLine), "PREPARING SHADERS");
            Text(list, ImVec2(c.x, y), 14.0f * u, ready ? kCyan : kText, shaderLine, 0.5f, 0.0f, 2.5f * u);
            y += Measure(14.0f * u, shaderLine).y + 10.0f * u;
            // A lit segment sliding along a dim track while compiling (aurora gives no total), full once ready.
            const float w = 200.0f * u, x0 = c.x - w * 0.5f;
            list->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w, y + 3.0f * u), IM_COL32(80, 220, 255, 40));
            const float p = std::fmod(t * 0.8f, 1.0f);
            const float s0 = ready ? x0 : x0 + std::max(0.0f, p * 1.3f - 0.3f) * w;
            const float s1 = ready ? x0 + w : x0 + std::min(1.0f, p * 1.3f) * w;
            list->AddRectFilled(ImVec2(s0, y), ImVec2(s1, y + 3.0f * u), kCyan);
        }
        }  // not the start screen
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void UpdateBootShaderState() {
    if (g_bootShadersReady.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now = Clock::now();
    if (g_bootShaderWaitStart == Clock::time_point{}) {
        g_bootShaderWaitStart = now;
    }
    const uint32_t queued = aurora_get_queued_pipeline_count();
    g_bootShaderNotice |= queued > kBootShaderNoticeThreshold;
    // Nothing is queued before the game draws, so the queue only counts once its first frames are up (the cover's
    // 60-frame tail after the first visible frame): their shaders compile behind the cover.
    const bool gameDrawing = g_strapInputAccepted.load(std::memory_order_acquire) &&
                             g_presentedFrame >= g_startupDismissFrame.load(std::memory_order_relaxed);
    // Also until the prewarm has saved the Vulkan pipeline cache: when the cache changed that save holds the device for
    // up to ~320 ms on a Steam Deck, which froze the first frames of play just after the cover lifted (NOTES.md
    // 2026-11-24). Behind the cover it is only a pause of its animation.
    const bool prewarmSettled = !aurora_pipeline_prewarm_pending();
    if ((queued == 0 && gameDrawing && prewarmSettled) || now - g_bootShaderWaitStart > kBootShaderWaitLimit) {
        g_bootShadersReady.store(true, std::memory_order_release);
        RT_LOGF(RT_TAG_RUNTIME, "[launcher] shaders ready after %.1f s: the startup cover lifts\n",
                std::chrono::duration<double>(now - g_bootShaderWaitStart).count());
    }
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide =
        Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F1 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active || g_optOpen || StartupScreenVisible();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void ApplyShaderCompilation() noexcept {
    // Ubershaders fall back to skipping, never to a stutter, for the few draws they do not cover.
    aurora_set_ubershader_mode(g_shaderCompilation == 0 ? AURORA_UBERSHADERS_HYBRID : AURORA_UBERSHADERS_OFF);
    aurora_set_skip_unready_pipelines(g_shaderCompilation != 2);
}

void InitializeRuntimeSettings() noexcept {
    Updater::Start();
    PAD_HLE_SetRumbleEnabled(g_rumbleStrength > 0);
    VirtualRemote::SetRumbleStrength(g_rumbleStrength);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
#if defined(__APPLE__)
    aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
#endif
    aurora_set_upscaler(g_fsrUpscaling ? AURORA_UPSCALER_FSR : AURORA_UPSCALER_OFF, g_fsrSharpnessPercent / 100.0f);
    aurora_set_anti_aliasing(g_smaa ? AURORA_ANTI_ALIASING_SMAA : AURORA_ANTI_ALIASING_OFF);
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    aurora_set_vsync(g_vsync.load());
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    ApplyShaderCompilation();
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    g_bootShadersReady.store(false, std::memory_order_relaxed);
    g_bootShaderNotice = false;
    g_bootShaderWaitStart = {};
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

// A gamepad input pressed while the rest of a save / load state chord it is in is held (Options > Controls): it
// belongs to the chord, so its own job here (LB skips a movie, the right stick fast-forwards) is left out.
bool CompletesHotkeyChord(const char* input) {
    std::vector<std::string> held;
    for (int h = 0; h < static_cast<int>(VirtualRemote::PadHotkey::Count); ++h) {
        const std::vector<std::string> chord = VirtualRemote::HotkeyInputs(static_cast<VirtualRemote::PadHotkey>(h));
        if (chord.size() < 2 || std::find(chord.begin(), chord.end(), input) == chord.end()) continue;
        if (held.empty()) held = VirtualRemote::HeldPadInputs();
        if (std::all_of(chord.begin(), chord.end(), [&held, input](const std::string& name) {
                return name == input || std::find(held.begin(), held.end(), name) != held.end();
            })) {
            RT_LOGF(RT_TAG_RUNTIME, "[hotkey] %s belongs to the %s chord here: its own action is left out\n", input,
                    h == 0 ? "save state" : "load state");
            return true;
        }
    }
    return false;
}

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        // Keys the port reacts to are left alone while the keyboard column of Save / Load state takes a combo.
        const bool keysTaken = g_rebind.active || (g_optChordCapture >= 0 && g_optChordKeys);
        // Save states (standalone_save_state.cpp): the keyboard's save / load state keys (Options > Controls; F5 / F9
        // unless changed). A combo fires as its last key goes down, and that key does nothing else then.
        if (!keysTaken && Keybinds::Capturing() < 0 && ev->sdl.type == SDL_EVENT_KEY_DOWN && !ev->sdl.key.repeat) {
            const int hotkey = Keybinds::HotkeyCompletedBy(ev->sdl.key.scancode);
            if (hotkey >= 0) {
                RT_LOGF(RT_TAG_RUNTIME, "[hotkey] keyboard %s: %s\n", Keybinds::HotkeyLabel(hotkey).c_str(),
                        hotkey == Keybinds::kSaveState ? "save state" : "load state");
                if (hotkey == Keybinds::kSaveState) StandaloneState::RequestSave();
                else StandaloneState::RequestLoad();
                continue;
            }
        }
        // F11 toggles between a window and borderless fullscreen (saved like the F1 bar's
        // Display mode).
        if (!keysTaken && IsToggleKey(ev->sdl, SDL_SCANCODE_F11)) {
            const AuroraDisplayMode mode = aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED
                                               ? AURORA_DISPLAY_MODE_BORDERLESS
                                               : AURORA_DISPLAY_MODE_WINDOWED;
            aurora_set_display_mode(mode);
            g_displayMode = static_cast<int>(aurora_get_display_mode());
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        }
        // Options: F1 on the keyboard.
        if (!keysTaken && IsToggleKey(ev->sdl, SDL_SCANCODE_F1)) {
            RequestOptionsToggle();
            ApplyInputBlockState();
        }
        // Fast-forward: F on the keyboard, a right stick click on a controller; the Guide button opens Options.
        if (!keysTaken && ev->sdl.type == SDL_EVENT_KEY_DOWN && ev->sdl.key.scancode == SDL_SCANCODE_F) {
            if (!ev->sdl.key.repeat) g_ffToggled = !g_ffToggled.load();
            g_ffKeyHeld = true;
        }
        if (ev->sdl.type == SDL_EVENT_KEY_UP && ev->sdl.key.scancode == SDL_SCANCODE_F) g_ffKeyHeld = false;
        // Other M: skip the movie or in-engine cutscene that is playing (hle/otherm_cutscene.cpp; an in-engine one through
        // the game's own skip, as − does): Backspace, a controller's left shoulder button (neither is bound in the
        // Metroid.ini profile), its A, or its Start let go without Back (Start + Back is HOME, the Options). The game
        // itself does nothing with + or 2 during a cutscene. Pressed with nothing playing, these do only their usual job.
        if (!keysTaken && ev->sdl.type == SDL_EVENT_KEY_DOWN && !ev->sdl.key.repeat &&
            ev->sdl.key.scancode == SDL_SCANCODE_BACKSPACE && !g_optOpen) {
            OthermRequestCutsceneSkip();
        }
        if (ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !g_optOpen && g_optPadCapture < 0) {
            const auto button = ev->sdl.gbutton.button;
            if (button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER && !CompletesHotkeyChord("Shoulder L")) {
                OthermRequestCutsceneSkip();
            } else if (button == SDL_GAMEPAD_BUTTON_SOUTH && (OthermCutscenePlaying() || OthermMovieUp()) &&
                       !CompletesHotkeyChord("Button A")) {
                OthermRequestCutsceneSkip();
            } else if (button == SDL_GAMEPAD_BUTTON_START) {
                g_padStartAlone = !g_padBackHeld;
            }
        }
        if (ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            g_padBackHeld = true;
            g_padStartAlone = false;
        }
        if (ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_UP && ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            g_padBackHeld = false;
        }
        if (ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_UP && ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
            if (g_padStartAlone && !g_optOpen && (OthermCutscenePlaying() || OthermMovieUp())) OthermRequestCutsceneSkip();
            g_padStartAlone = false;
        }
        // Options pages: a controller's shoulder buttons (LB / RB), Q / E or Page Up / Page Down on the keyboard (Q and
        // E only tilt the remote in the game, which Options does not use).
        if (g_optOpen && g_optPadCapture < 0 && Keybinds::Capturing() < 0 && !g_optMuteCapture && !g_rebind.active) {
            if (ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) g_optEvents |= kOptPagePrev;
                if (ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) g_optEvents |= kOptPageNext;
            } else if (ev->sdl.type == SDL_EVENT_KEY_DOWN && !ev->sdl.key.repeat) {
                const SDL_Scancode key = ev->sdl.key.scancode;
                if (key == SDL_SCANCODE_Q || key == SDL_SCANCODE_PAGEUP) g_optEvents |= kOptPagePrev;
                if (key == SDL_SCANCODE_E || key == SDL_SCANCODE_PAGEDOWN) g_optEvents |= kOptPageNext;
            }
        }
        if (ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            const bool down = ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            if (ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
                const bool chord = down && CompletesHotkeyChord("Thumb R");
                if (down && !chord) g_ffToggled = !g_ffToggled.load();
                g_ffPadHeld = down && !chord;
            } else if (down && ev->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                RequestOptionsToggle();
            }
        }
        if (!keysTaken && !g_optMuteCapture && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        // Escape is also the game's + (Keybinds); here it only backs out of Options.
        if (!keysTaken && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE) && g_optOpen) {
            g_optEvents |= kOptBack;
            ApplyInputBlockState();
        }
#if defined(__ANDROID__)
        // Android's back button or gesture (main.cpp keeps it from closing the app, SDL_HINT_ANDROID_TRAP_BACK_BUTTON):
        // backs out of Options, and in game opens them.
        if (!keysTaken && IsToggleKey(ev->sdl, SDL_SCANCODE_AC_BACK)) {
            if (g_optOpen) g_optEvents |= kOptBack;
            else RequestOptionsToggle();
            ApplyInputBlockState();
        }
#endif
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void SetShowFps(bool show) noexcept {
    g_showFps = show;
    RuntimeConfigFile::SetShowFps(show);
}

bool ShowFps() noexcept {
    return g_showFps;
}

void SetVSync(bool enabled) noexcept {
    g_vsync.store(enabled);
    aurora_set_vsync(enabled);
    RuntimeConfigFile::SetVSync(enabled);
}

bool VSync() noexcept {
    return g_vsync.load(std::memory_order_relaxed);
}

void SetFrameGraph(bool show) noexcept {
    g_frameGraph = show;
    RuntimeConfigFile::SetFrameGraph(show);
}

bool FrameGraph() noexcept {
    return g_frameGraph;
}

void SetRenderScale(float scale) noexcept {
    SetResolutionScale(scale);
}

float RenderScale() noexcept {
    return g_resolutionScale;
}

void SetAspect(int32_t mode) noexcept {
    SetAspectMode(mode);
}

int32_t Aspect() noexcept {
    return DisplaySettings::Latest().aspect;
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

// ---- Options: the player's settings menu, a visor-style panel (hud_theme.cpp) ----
// Opened with HOME (Start + Back together in Metroid.ini) / F1 / a controller's Guide button when Steam leaves it to
// the game. The game is held in VIWaitForRetrace while it is up (vi.cpp RunPausedFrames), which then presents and
// drives this menu. It is the only settings UI: the ImGui developer window is gone.
namespace {

void SetFrameInterpolationMode(int mode) {
    g_frameInterpolationMode = mode;
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(mode)];
    aurora_set_frame_interpolation_fps(targetFps);
    RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
    LimitResolutionForFrameRate();
    if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
}

void UpdateGameSpeed() {
    uint32_t speed = 1;
    g_ffOn = false;
    if (!g_optOpen && !StartupScreenVisible()) {
        static const bool testFf = std::getenv("OTHERM_TEST_FASTFORWARD") != nullptr;  // hidden test runs: always on
        const bool ff = testFf || (g_ffToggleMode ? g_ffToggled.load() : (g_ffKeyHeld.load() || g_ffPadHeld.load()));
        g_ffOn = ff;
        if (ff) {
            speed = g_ffSpeed == 0 ? kFastForwardMax : static_cast<uint32_t>(g_ffSpeed);
        }
    }
    if (speed == g_appliedSpeed) return;
    // Faster than the display: presents must not wait for it, and interpolated frames would only slow it down.
    const bool fast = speed > 1;
    if (fast != (g_appliedSpeed > 1)) {
        aurora_set_vsync(fast ? false : g_vsync.load());
        aurora_set_frame_interpolation_fps(fast ? 0 : kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)]);
    }
    VI_HLE_SetSpeed(speed);
    g_appliedSpeed = speed;
}

Clock::time_point g_optCaptureEnd{};

std::string PadBindingLabel(uint16_t target) {
    uint32_t count = 0;
    const auto* maps = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &count);
    for (uint32_t i = 0; maps && i < count; ++i)
        if (maps[i].padButton == target) return NativeButtonForValue(maps[i].nativeButton).label;
    return "Unmapped";
}

// The key or controller button being picked for a Controls row: polled every frame until it is pressed.
std::array<bool, SDL_SCANCODE_COUNT> g_optMuteKeys{};  // the keys already down when the Mute key capture began
Clock::time_point g_optMuteDeadline{};

void BeginMuteCapture() {
    g_optMuteCapture = true;
    g_optMuteDeadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    g_optMuteKeys.fill(false);
    std::copy_n(keys, std::min(count, static_cast<int>(g_optMuteKeys.size())), g_optMuteKeys.begin());
}

// What the column being captured holds now, and how it is named.
std::vector<std::string> ChordHeldNow() { return g_optChordKeys ? Keybinds::HeldKeyNames() : VirtualRemote::HeldPadInputs(); }
std::string ChordLabel(const std::vector<std::string>& held) {
    return g_optChordKeys ? Keybinds::KeysLabel(held) : VirtualRemote::PadInputsLabel(held);
}

// The keyboard column's combo: a key the port uses on its own (Options, fullscreen, fast-forward, mute) is refused
// alone, as Keybinds refuses the game's keys.
bool SetKeyChord(int hotkey, const std::vector<std::string>& keys, std::string& error) {
    if (keys.size() == 1) {
        const SDL_Scancode code = SDL_GetScancodeFromName(keys[0].c_str());
        if (code == SDL_SCANCODE_F1 || code == SDL_SCANCODE_F11 || code == SDL_SCANCODE_F ||
            (g_muteHotkey != PAD_KEY_INVALID && code == g_muteHotkey)) {
            error = keys[0] + " already does something here: hold it together with another key.";
            return false;
        }
    }
    return Keybinds::SetHotkey(hotkey, keys, error);
}

void BeginChordCapture(int hotkey, bool keys) {
    g_optChordCapture = hotkey;
    g_optChordKeys = keys;
    g_optChordIgnored = ChordHeldNow();
    g_optChordHeld.clear();
    g_optChordCounting = false;
    g_optChordDeadline = Clock::now() + std::chrono::seconds(10);
    RT_LOGF(RT_TAG_RUNTIME, "options: %s %s capture (%zu held at the start)\n", hotkey == 0 ? "save state" : "load state",
            keys ? "keyboard" : "controller", g_optChordIgnored.size());
}

void EndChordCapture() {
    g_optChordCapture = -1;
    g_optChordHeld.clear();
    g_optChordCounting = false;
    g_optCaptureEnd = Clock::now();
}

// 5, 4, 3, 2, 1 while the chord's buttons are held.
int ChordSecondsLeft() {
    const auto left = kChordHold - (Clock::now() - g_optChordSince);
    return std::clamp(static_cast<int>(std::chrono::ceil<std::chrono::seconds>(left).count()), 1,
                      static_cast<int>(kChordHold.count()));
}

const char* HotkeyName(int hotkey) { return hotkey == 0 ? "Save state" : "Load state"; }

void PollChordCapture() {
    const int hotkey = g_optChordCapture;
    const auto set = [hotkey](const std::vector<std::string>& held, std::string& error) {
        return g_optChordKeys ? SetKeyChord(hotkey, held, error)
                              : VirtualRemote::SetHotkey(static_cast<VirtualRemote::PadHotkey>(hotkey), held, error);
    };
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    const auto key = [keys, count](SDL_Scancode code) { return keys != nullptr && code < count && keys[code]; };
    if (key(SDL_SCANCODE_ESCAPE)) {  // the old chord stays
        EndChordCapture();
        return;
    }
    if (key(SDL_SCANCODE_BACKSPACE) || key(SDL_SCANCODE_DELETE)) {
        std::string error;
        g_controlsStatus = set({}, error) ? std::string(HotkeyName(hotkey)) + (g_optChordKeys ? ": no keys." : ": no controller buttons.")
                                          : error;
        EndChordCapture();
        return;
    }
    std::vector<std::string> held = ChordHeldNow();
    const auto in = [](const std::vector<std::string>& list, const std::string& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    g_optChordIgnored.erase(std::remove_if(g_optChordIgnored.begin(), g_optChordIgnored.end(),
                                           [&](const std::string& name) { return !in(held, name); }),
                            g_optChordIgnored.end());
    held.erase(std::remove_if(held.begin(), held.end(), [&](const std::string& name) { return in(g_optChordIgnored, name); }),
               held.end());
    const auto now = Clock::now();
    if (held.empty()) {
        if (g_optChordCounting) RT_LOGF(RT_TAG_RUNTIME, "options: chord let go, waiting again\n");
        g_optChordCounting = false;
        if (now > g_optChordDeadline) {
            RT_LOGF(RT_TAG_RUNTIME, "options: chord capture gave up (nothing held)\n");
            EndChordCapture();
            return;
        }
    } else {
        g_optChordDeadline = now + std::chrono::seconds(10);
        if (!g_optChordCounting) {
            g_optChordCounting = true;
            g_optChordSince = now;
        } else if (now - g_optChordSince >= kChordHold) {
            std::string error;
            const std::string label = ChordLabel(held);
            const bool done = set(held, error);
            g_controlsStatus = done ? std::string(HotkeyName(hotkey)) + ": " + label + "." : error;
            RT_LOGF(RT_TAG_RUNTIME, "options: %s %s %s%s%s\n", HotkeyName(hotkey), g_optChordKeys ? "keys" : "chord", label.c_str(),
                    done ? "" : ": ", error.c_str());
            EndChordCapture();
            return;
        }
    }
    if (held != g_optChordHeld && !held.empty())
        RT_LOGF(RT_TAG_RUNTIME, "options: chord held %s, %d s left\n", ChordLabel(held).c_str(), ChordSecondsLeft());
    g_optChordHeld = std::move(held);
}

void PollOptionsCapture() {
    const ControllerGate gate;
    if (!gate) return;  // a device scan holds the controllers: capture goes on next frame
    if (g_optChordCapture >= 0) PollChordCapture();
    if (Keybinds::Capturing() >= 0 && Keybinds::PollCapture()) g_optCaptureEnd = Clock::now();
    if (g_optMuteCapture) {
        // Escape keeps the old key, Backspace or Delete leaves Mute without one; F1 stays the Options key.
        int count = 0;
        const bool* keys = SDL_GetKeyboardState(&count);
        for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_optMuteCapture; ++i) {
            if (keys[i] && !g_optMuteKeys[i] && i != SDL_SCANCODE_F1) {
                if (i != SDL_SCANCODE_ESCAPE) {
                    g_muteHotkey = (i == SDL_SCANCODE_BACKSPACE || i == SDL_SCANCODE_DELETE) ? PAD_KEY_INVALID : i;
                    RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
                }
                g_optMuteCapture = false;
                g_optCaptureEnd = Clock::now();
            }
            g_optMuteKeys[i] = keys[i];
        }
        if (Clock::now() > g_optMuteDeadline) g_optMuteCapture = false;
    }
    // Options > Controls, a controller cell: the next gamepad input goes to Metroid.ini (Escape keeps the old one).
    if (g_optPadBindControl >= 0) {
        int count = 0;
        const bool* keys = SDL_GetKeyboardState(&count);
        if (SDL_SCANCODE_ESCAPE < count && keys[SDL_SCANCODE_ESCAPE]) VirtualRemote::CancelPadCapture();
        const std::string input = VirtualRemote::PollPadCapture();
        if (!input.empty()) {
            std::string error;
            const auto control = static_cast<VirtualRemote::PadControl>(g_optPadBindControl);
            g_controlsStatus = VirtualRemote::BindPad(control, input, error) ? std::string() : error;
            RT_LOGF(RT_TAG_RUNTIME, "options: controller input %s for control %d%s%s\n", input.c_str(), g_optPadBindControl,
                    error.empty() ? "" : ": ", error.c_str());
        }
        if (!VirtualRemote::PadCapturing()) {
            g_optPadBindControl = -1;
            g_optCaptureEnd = Clock::now();
        }
    }
    if (g_optPadCapture < 0) return;
    if (!g_rebind.active) {
        g_optPadCapture = -1;
        g_optCaptureEnd = Clock::now();
        return;
    }
    auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
    if (!pad || Clock::now() > g_rebind.deadline) {
        g_rebind.active = false;
        return;
    }
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
        const bool down = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
        if (down && !g_rebind.buttons[i]) CompleteRebind(static_cast<uint32_t>(i));
        g_rebind.buttons[i] = down;
    }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
        const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
        if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
        if (g_rebind.axesReady[i] && std::abs(value) >= 16384) CompleteRebind(PADEncodeAxisButton(i, value < 0));
    }
}

bool OptionsCapturing() {
    return Keybinds::Capturing() >= 0 || g_optPadCapture >= 0 || g_optMuteCapture || g_optPadBindControl >= 0 ||
           g_optChordCapture >= 0 || Clock::now() - g_optCaptureEnd < std::chrono::milliseconds(400);
}

void SetOptionsOpen(bool open) {
    if (open == g_optOpen) return;
    g_optOpen = open;
    g_optLastToggle = Clock::now();
    g_optPrompt.reset();
    g_optMuteCapture = false;
    if (g_optPadBindControl >= 0) VirtualRemote::CancelPadCapture();
    g_optPadBindControl = -1;
    g_optChordCapture = -1;
    g_optChordHeld.clear();
    g_optChordCounting = false;
    VirtualRemote::PauseRumble(open);  // the game stands still while Options is up, its rumble too
    if (open) {
        g_optOpenedAt = Clock::now();
        g_optRow = 0;
        g_optCol = 0;
        g_optFirst = 0;
        g_controlsStatus.clear();
        g_ffToggled = false;
        DolphinImport::Refresh();
    }
}

void RequestOptionsToggle() {
    if (StartupScreenVisible() || Clock::now() - g_optLastToggle < std::chrono::milliseconds(250)) return;
    g_optToggleRequest = true;
}

} // namespace

void RequestOptionsOpen() noexcept { g_optOpenRequest = true; }

namespace {

// A cell of Options > Controls' table: what is bound, and what picking it does (none: fixed, it only shows).
struct OptCell {
    std::string text;
    std::function<void()> pick;
};
struct OptRow {
    std::string label;
    std::string value;             // empty: an action row
    std::function<void(int)> step; // -1 / +1 for a value, 0 for confirm; none: a row that only shows something
    std::string help;
    // A percentage drawn as a bar from 0 to sliderMax (step still takes -1 / +1); -1: not a slider. setSlider takes the
    // value under the mouse.
    int slider = -1;
    int sliderMax = 100;
    std::function<void(int)> setSlider;
    // Options > Controls: a table row, the controller column then the keyboard column (left / right picks one).
    std::vector<OptCell> cells;
    // A value that A picks (step(0)), with no left / right arrows: the start screen's Game files, the folder browser.
    bool button = false;
    // The label's colour when not selected (0: the usual one): the start screen's notes and results.
    ImU32 color = 0;
    // After the help, in amber: what does not take effect until the next start.
    std::string warn;
};

// Options > Display > Graphics API, Automatic: what it picks on this computer and why (vulkan_probe.h; checked here
// once when the game started on a fixed choice).
std::string AutomaticApiHelp() {
#if defined(_WIN32)
    if (VulkanProbe::Last().reason.empty()) VulkanProbe::Check();
    const VulkanProbe::Result& probe = VulkanProbe::Last();
    return std::string("Vulkan with a graphics card that has Vulkan 1.3+ and 4 GB, else Direct3D 12. ") +
           (probe.preferVulkan ? "Vulkan here: " : "Direct3D 12 here: ") + probe.reason + ".";
#else
    return "Takes effect the next time the game starts.";
#endif
}

std::string OnOff(bool on) { return on ? "On" : "Off"; }
std::string Percent(int v) { return std::to_string(v) + "%"; }
int Cycle(int v, int dir, int count) { return ((v + dir) % count + count) % count; }
std::string Thousands(size_t n) {
    std::string digits = std::to_string(n);
    for (int at = int(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<size_t>(at), ",");
    return digits;
}
// A path as Windows writes it (Dolphin.ini's paths use forward slashes).
std::string DisplayPath(std::filesystem::path path) {
    return RuntimeConfigFile::PathToUtf8(path.make_preferred());
}
// Shows a folder or file in the system's file manager or editor. Android opens no app's own files for another app:
// there the player is told where it is, to reach from a PC over USB.
void OpenPath(const std::filesystem::path& path) {
#if defined(__ANDROID__)
    std::string text = RuntimeConfigFile::PathToUtf8(path);
    if (const size_t shared = text.find("Android/data/"); shared != std::string::npos) text = text.substr(shared);
    SDL_ShowAndroidToast(("From a PC over USB: " + text).c_str(), 1, -1, 0, 0);
#else
    SDL_OpenURL(("file:///" + RuntimeConfigFile::PathToUtf8(path)).c_str());
#endif
}

void OpenFolder(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    OpenPath(folder);
}

void StepVolume(int& percent, int dir, void (*apply)(float), bool (*save)(float)) {
    percent = std::clamp(percent + (dir == 0 ? 10 : dir * 10), 0, 100);
    if (dir == 0 && percent == 100) percent = 0;
    apply(percent / 100.0f);
    save(percent / 100.0f);
}

// A volume as a slider: left / right step by 10%, the mouse sets it where it is clicked.
OptRow VolumeRow(const char* label, int& percent, void (*apply)(float), bool (*save)(float)) {
    OptRow row{label, Percent(percent), [&percent, apply, save](int d) { StepVolume(percent, d, apply, save); }, {}};
    row.slider = percent;
    row.setSlider = [&percent, apply, save](int v) {
        percent = std::clamp(v, 0, 100);
        apply(percent / 100.0f);
        save(percent / 100.0f);
    };
    return row;
}

// Options > Controls > Vibration: 0 (off) to 200%; each change gives a short rumble at the new strength.
void SetRumbleStrength(int percent) {
    g_rumbleStrength = std::clamp(percent, 0, 200);
    VirtualRemote::SetRumbleStrength(g_rumbleStrength);
    PAD_HLE_SetRumbleEnabled(g_rumbleStrength > 0);
    RuntimeConfigFile::SetRumbleStrength(g_rumbleStrength);
    if (g_rumbleStrength > 0) VirtualRemote::PreviewRumble();
}

// Options > Saves > Pick Dolphin's folder: SDL's folder dialog answers on a thread of its own.
std::mutex g_pickedFolderMutex;
std::optional<std::string> g_pickedFolder;

void PickDolphinFolder() {
    static const SDL_DialogFileCallback done = [](void*, const char* const* files, int) {
        std::lock_guard lock(g_pickedFolderMutex);
        g_pickedFolder = files != nullptr && files[0] != nullptr ? std::string(files[0]) : std::string();
    };
    const std::string start = RuntimeConfigFile::DolphinFolder();
    SDL_ShowOpenFolderDialog(done, nullptr, nullptr, start.empty() ? nullptr : start.c_str(), false);
}

void TakePickedFolder() {
    std::optional<std::string> picked;
    {
        std::lock_guard lock(g_pickedFolderMutex);
        picked.swap(g_pickedFolder);
    }
    if (!picked.has_value() || picked->empty()) return;  // cancelled
    const std::u8string generic = RuntimeConfigFile::PathFromUtf8(*picked).generic_u8string();
    RuntimeConfigFile::SetDolphinFolder(std::string(generic.begin(), generic.end()));
    DolphinImport::Refresh();
    g_saveStatus = DolphinImport::FindSave() ? "Found a save in the picked folder." : "No Other M save in the picked folder.";
    DolphinImport::ApplyTexturePacks();  // its texture pack, if that is switched on
}

enum OptTab { kOptGame, kOptDisplay, kOptSound, kOptControls, kOptTextures, kOptSaves, kOptCheats, kOptMods, kOptTabCount };

constexpr const char* kOptTabs[kOptTabCount] = {"Game", "Display", "Sound", "Controls", "Textures", "Saves", "Cheats", "Mods"};

// Options > Display > Steam Deck preset (shown on a Deck): 60 FPS first. Dynamic resolution up to 2.25x (1080 lines on
// the 800-line screen: sharper than the screen when there is room, and a cap that leaves the APU's shared power budget
// to the CPU, which runs the game at ~11.4 of its 16.7 ms), no inserted frames (they cost the GPU frames of its own),
// FSR for when Dynamic drops below the screen, no SMAA, borderless fullscreen. NOTES.md 2026-11-24.
constexpr float kSteamDeckDynamicMax = 2.25f;

bool SteamDeckPresetInUse() {
    return DynamicResolution::Enabled() && std::fabs(DynamicResolution::Max() - kSteamDeckDynamicMax) < 0.001f &&
           g_frameInterpolationMode == 0 && !g_smaa && g_fsrUpscaling;
}

void ApplySteamDeckPreset() {
    RT_LOGF(RT_TAG_RUNTIME, "[options] Steam Deck preset: Dynamic up to %gx, 60 FPS, FSR, no SMAA\n", kSteamDeckDynamicMax);
    DynamicResolution::SetMax(kSteamDeckDynamicMax);
    DynamicResolution::SetEnabled(true, g_resolutionScale);
    if (g_frameInterpolationMode != 0) SetFrameInterpolationMode(0);
    g_smaa = false;
    aurora_set_anti_aliasing(AURORA_ANTI_ALIASING_OFF);
    RuntimeConfigFile::SetAntiAliasing("off");
    g_fsrUpscaling = true;
    aurora_set_upscaler(AURORA_UPSCALER_FSR, g_fsrSharpnessPercent / 100.0f);
    RuntimeConfigFile::SetUpscaler("fsr");
    if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED) {
        aurora_set_display_mode(AURORA_DISPLAY_MODE_BORDERLESS);
        g_displayMode = static_cast<int>(aurora_get_display_mode());
        RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
    }
}

std::vector<OptRow> OptionsRows(int tab) {
    std::vector<OptRow> rows;
    if (tab == kOptGame) {
        // Save states: the quick slot F5 / F9 use (standalone_save_state.cpp). The game stands still while Options is
        // up, so the menu closes and the save or load happens as the game runs again, at once.
        std::error_code stateError;
        const std::filesystem::path quick = StandaloneState::QuickSlotPath();
        const bool haveState = std::filesystem::exists(quick, stateError);
        const std::string savedAt =
            haveState ? DolphinImport::FormatDate(std::filesystem::last_write_time(quick, stateError), true) : std::string();
        // " (F5 or View + RB)": the keys and the controller's buttons Options > Controls has for it, if any.
        const auto keys = [](int hotkey) {
            const std::string key = Keybinds::HotkeyLabel(hotkey);
            const std::vector<std::string> pad = VirtualRemote::HotkeyInputs(static_cast<VirtualRemote::PadHotkey>(hotkey));
            std::string text = key == "None" ? std::string() : key;
            if (!pad.empty()) text += (text.empty() ? "" : " or ") + VirtualRemote::PadInputsLabel(pad);
            return text.empty() ? text : " (" + text + ")";
        };
        const std::string saveKeys = keys(Keybinds::kSaveState);
        const std::string loadKeys = keys(Keybinds::kLoadState);
        if (!g_launcher.active) {  // before the game boots there is no game to save or load
        rows.push_back({"Save state", "", [](int d) {
            if (d != 0) return;
            StandaloneState::RequestSave();
            SetOptionsOpen(false);
        }, haveState ? "Saves the game as it is right now" + saveKeys + ", over the state from " + savedAt + "."
                     : "Saves the game as it is right now" + saveKeys + ". Load state brings it back."});
        rows.push_back({"Load state", "", [haveState, savedAt](int d) {
            if (d != 0 || !haveState) return;
            g_optPrompt = OptPrompt{"LOAD STATE?", "LOAD", "Back to the state from " + savedAt + ". Play since then is lost.", [] {
                StandaloneState::RequestLoad();
                SetOptionsOpen(false);
            }};
            g_optPromptChoice = 1;
        }, haveState ? "Goes back to the state saved " + savedAt + loadKeys + "." : std::string("No state saved yet.")});
        }
        rows.push_back({"Fast-forward speed", FastForwardSpeedLabel(), [](int d) {
            // 2x .. 8x, then Max (0); Max sits after 8x.
            int step = g_ffSpeed == 0 ? 9 : g_ffSpeed;
            step = d == 0 ? (step >= 9 ? 2 : step + 1) : std::clamp(step + d, 2, 9);
            g_ffSpeed = step == 9 ? 0 : step;
            RuntimeConfigFile::SetFastForwardSpeed(g_ffSpeed);
        }, "How fast the game runs while fast-forward is on. All of it speeds up together, sound and movies too; when "
           "the PC cannot keep up it all runs slower together. Max: as fast as the PC goes."});
        rows.push_back({"Skip cutscenes", RuntimeConfigFile::SkipAnyCutscene(true) ? "Any" : "Seen ones", [](int) {
            RuntimeConfigFile::SetSkipAnyCutscene(!RuntimeConfigFile::SkipAnyCutscene(true));
        }, "Start, A, LB or Backspace (or - as in the game) skip a cutscene the way the game itself skips one. Any: "
           "every cutscene. Seen ones: only those the game allows, seen before or once the game is cleared."});
        // Voices, text and subtitles each in their own language (hle/otherm_language.cpp). The game's language index:
        // 0 JP, 1 US, 2 DE, 3 FR, 4 ES, 5 IT, 6 US French, 7 US Spanish; -1 the game's own setting, -2 subtitles off.
        static constexpr int32_t kTextChoices[] = {-1, 1, 0, 6, 7, 3, 4, 2, 5};
        static constexpr int32_t kSubtitleChoices[] = {-1, -2, 1, 0, 6, 7, 3, 4, 2, 5};
        const auto languageName = [](int32_t index) -> std::string {
            static constexpr const char* kNames[] = {"Japanese", "English", "German", "French (Europe)",
                                                     "Spanish (Europe)", "Italian", "French (Canada)",
                                                     "Spanish (Latin America)"};
            return index >= 0 && index < 8 ? kNames[index] : index == -2 ? "Off" : "Game setting";
        };
        // A / Enter steps on and wraps; left and right step either way.
        const auto stepChoice = [](const int32_t* choices, int count, int32_t current, int d) {
            int at = 0;
            while (at < count && choices[at] != current) ++at;
            return choices[((at == count ? 0 : at) + (d == 0 ? 1 : d) % count + count) % count];
        };
        rows.push_back({"Voices", RuntimeConfigFile::JapaneseVoices() ? "Japanese" : "English", [](int) {
            RuntimeConfigFile::SetJapaneseVoices(!RuntimeConfigFile::JapaneseVoices());
        }, "The language the characters speak in movies and cutscenes: the disc has the English and the Japanese voice "
           "acting, no other. Applies from the next line spoken."});
        OptRow text{"Text language", languageName(RuntimeConfigFile::TextLanguage()), [stepChoice](int d) {
            RuntimeConfigFile::SetTextLanguage(stepChoice(kTextChoices, 9, RuntimeConfigFile::TextLanguage(), d));
            OthermLanguageApply();
        }, "Menus, messages and the HUD, over the game's own Language option."};
        if (OthermLanguageStarted()) text.warn = "Applies at the next start.";
        rows.push_back(text);
        OptRow subtitles{"Subtitles", languageName(RuntimeConfigFile::SubtitleLanguage()), [stepChoice](int d) {
            RuntimeConfigFile::SetSubtitleLanguage(stepChoice(kSubtitleChoices, 10, RuntimeConfigFile::SubtitleLanguage(), d));
            OthermLanguageApply();
        }, "Movies and cutscenes. Game setting: the game's own Subtitles option and text language."};
        if (OthermLanguageStarted() && !OthermJapaneseFontLoaded()) subtitles.warn = "Japanese needs a restart.";
        rows.push_back(subtitles);
        rows.push_back({"Fast-forward button", g_ffToggleMode ? "Press to toggle" : "Hold", [](int) {
            g_ffToggleMode = !g_ffToggleMode;
            RuntimeConfigFile::SetFastForwardToggle(g_ffToggleMode);
        }, "Keyboard: F.  Controller: click the right stick."});
        if (!Updater::Current().empty()) {
            using S = Updater::State;
            const S st = Updater::GetState();
            const std::string value = st == S::Available ? Updater::Latest() + " - install"
                                    : st == S::Checking ? "Checking..."
                                    : st == S::Downloading ? "Downloading..."
                                    : st == S::UpToDate ? "Up to date (" + Updater::Current() + ")"
                                    : st == S::Failed ? Updater::Current() + ", couldn't check - retry"
                                                      : Updater::Current();  // the version stays visible
            rows.push_back({"Update", value, [](int d) {
                if (d == 0) Updater::Install();
            }, st == S::Available ? "Downloads the new version, then restarts the game into it. Saves and settings are kept."
                                  : "New versions are checked for when the game starts."});
            rows.push_back({"Check for updates", OnOff(Updater::CheckEnabled()), [](int) {
                Updater::SetCheckEnabled(!Updater::CheckEnabled());
            }, "Look for a new version each time the game starts."});
        }
#if !defined(__ANDROID__)
        rows.push_back({"Discord status", OnOff(DiscordPresence::Enabled()), [](int) {
            DiscordPresence::SetEnabled(!DiscordPresence::Enabled());
        }, "Shows Other M, the area you are in and the time played on your Discord profile while the Discord app "
           "runs on this PC."});
#endif
        if (!g_launcher.active) {  // the start screen has this row on its START page
            rows.push_back({"Start screen", OnOff(RuntimeConfigFile::ShowLauncher(true)), [](int) {
                RuntimeConfigFile::SetShowLauncher(!RuntimeConfigFile::ShowLauncher(true));
            }, "On: the start screen comes up before the game boots. Off: the game boots straight away."});
        }
        if (!g_launcher.active)
        rows.push_back({"Reset game", "", [](int d) {
            if (d != 0) return;
            g_optPrompt = OptPrompt{"RESET THE GAME?", "RESET", "Back to the title screen. Play since your last save is lost.", [] {
                // Tests: OTHERM_TEST_NO_RESTART keeps the process (a restart would start a window outside the test).
                if (std::getenv("OTHERM_TEST_NO_RESTART") != nullptr) {
                    RT_LOGF(RT_TAG_RUNTIME, "reset: requested (OTHERM_TEST_NO_RESTART: not restarting)\n");
                    return;
                }
                RT_LOGF(RT_TAG_RUNTIME, "reset: restarting the game from Options\n");
                DolphinImport::RestartGame();
            }};
            g_optPromptChoice = 1;
        }, "Starts the game over from its title screen, like the console's Reset button."});
        rows.push_back({"Quit to desktop", "", [](int d) {
            if (d != 0) return;
            g_optPrompt = OptPrompt{"QUIT TO DESKTOP?", "QUIT", "", [] { ExitForAuroraWindowClose(); }};
            g_optPromptChoice = 1;
        }, "Close the game. Anything not saved in-game is lost."});
    } else if (tab == kOptDisplay) {
        // First on a Deck: the one choice most players there want.
        if (GpuLoad::IsSteamDeck()) {
            const bool inUse = SteamDeckPresetInUse();
            rows.push_back({"Steam Deck preset", inUse ? "In use" : "Apply", [inUse](int) {
                if (!inUse) ApplySteamDeckPreset();
            }, "Locked 60 FPS: Dynamic up to 1080p, no inserted frames, FSR, no SMAA. Also set 60 Hz in Quick Access."});
        }
        static constexpr const char* kModes[] = {"Windowed", "Fullscreen", "Exclusive fullscreen"};
        g_displayMode = static_cast<int>(aurora_get_display_mode());
        rows.push_back({"Window", kModes[std::clamp(g_displayMode, 0, 2)], [](int d) {
            const int mode = d == 0 ? Cycle(g_displayMode, 1, 2) : Cycle(g_displayMode, d, 3);
            aurora_set_display_mode(static_cast<AuroraDisplayMode>(mode));
            g_displayMode = static_cast<int>(aurora_get_display_mode());
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        }, g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE
               ? "Exclusive fullscreen asks the screen for the refresh rate closest to the frame rate."
               : "F11 also switches between a window and fullscreen."});
        // The list: Dynamic (dynamic_resolution.cpp), then the fixed scales (kResolutions).
        const bool dynamic = DynamicResolution::Enabled();
        int res = 0;
        for (int i = 0; i < int(kResolutions.size()); ++i)
            if (std::fabs(kResolutions[i].scale - g_resolutionScale) < 0.001f) res = i;
        const int choice = dynamic ? 0 : res + 1;
        const int choices = int(kResolutions.size()) + 1;
        uint32_t pictureWidth = 0, pictureHeight = 0;
        AuroraGetPictureSize(&pictureWidth, &pictureHeight);
        const std::string now = "now " + std::to_string(pictureWidth) + "x" + std::to_string(pictureHeight);
        std::string label = dynamic ? "Dynamic" : kResolutions[res].label;
        if (dynamic && DynamicResolution::CurrentScale() > 0.0f) {
            char scale[32];
            std::snprintf(scale, sizeof scale, " (%gx now)", DynamicResolution::CurrentScale());
            label += scale;
        }
        // One line each (the help line shrinks longer text to fit).
        std::string help;
        if (dynamic && DynamicResolution::Measurable())
            help = "Keeps 60 FPS: as sharp as your GPU can draw in time, changing with the scene (" + now + ").";
        else if (dynamic)
            help = "Needs the GPU's load, which only Linux with an AMD GPU reports: Auto here (" + now + ").";
        else if (GpuLimitedRecentlyAt(pictureWidth, pictureHeight))
            help = "How sharp the picture is (" + now + "). Your GPU could not keep up: pick lower, or Dynamic.";
        else
            help = "How sharp the picture is (" + now + "). Auto matches your screen; Dynamic keeps 60 FPS.";
        rows.push_back({"Resolution", label, [choice, choices](int d) {
            int next = choice;
            for (int tries = 0; tries < choices; ++tries) {
                next = Cycle(next, d == 0 ? 1 : d, choices);
                if (next == 0 || !(IsHighFrameRateMode() && IsHighResolutionScale(kResolutions[next - 1].scale))) break;
            }
            if (next == 0) {
                DynamicResolution::SetEnabled(true, g_resolutionScale);
            } else {
                DynamicResolution::SetEnabled(false, kResolutions[next - 1].scale);
                SetResolutionScale(kResolutions[next - 1].scale);
            }
        }, help});
        if (dynamic) {
            int top = 0;
            for (int i = 0; i < int(kDynamicMax.size()); ++i)
                if (kDynamicMax[i].scale <= DynamicResolution::Max() + 0.001f) top = i;
            rows.push_back({"Dynamic up to", kDynamicMax[top].label, [top](int d) {
                DynamicResolution::SetMax(kDynamicMax[Cycle(top, d == 0 ? 1 : d, int(kDynamicMax.size()))].scale);
            }, "The sharpest Dynamic may go. Lower keeps the GPU cooler and the battery fuller."});
        }
        // Other M draws 4:3 and 16:9 itself, and Fill screen's wider view too (otherm_widescreen.cpp: the game's own
        // camera, so its glow and screen effects stay in place; NOTES.md 2026-11-19). Cinematic is not offered; an
        // older setting still shows by its name until it is changed.
        const int aspectMode = DisplaySettings::Latest().aspect;
        int asp = 0;
        for (int i = 0; i < int(kAspects.size()); ++i)
            if (kAspects[i].mode == aspectMode) asp = i;
        rows.push_back({"Aspect ratio", kAspects[asp].label, [asp](int d) {
            constexpr int kOffered = 3;  // Original 4:3, 16:9, Fill screen
            SetAspectMode(kAspects[Cycle(std::min(asp, kOffered - 1), d == 0 ? 1 : d, kOffered)].mode);
        }, (aspectMode != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen()
               ? "Switching between 4:3 and widescreen takes effect the next time the game starts."
           : aspectMode == DisplaySettings::kAspectFill && DisplaySettings::SurfaceWide()
               ? "Fills your wide screen: the game shows more to the sides, and the HUD moves to the edges."
           : aspectMode == DisplaySettings::kAspectFill
               ? "On screens wider than 16:9 (21:9, 32:9) the game shows more to the sides. This screen shows 16:9."
               : "The game's own 4:3 or 16:9 picture. Fill screen shows more to the sides on ultrawide screens."});
        // What runs can be less than the choice: aurora lowers a rate the screen cannot show.
        const uint32_t chosen = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        const uint32_t running = aurora_get_frame_interpolation_fps();
        std::string rate = kFrameRateNames[static_cast<size_t>(g_frameInterpolationMode)];
        if (chosen != 0 && running != chosen && g_appliedSpeed <= 1)
            rate += running == 0 ? " (60 here)" : " (" + std::to_string(running) + " here)";
        rows.push_back({"Frame rate", rate, [](int d) {
            SetFrameInterpolationMode(Cycle(g_frameInterpolationMode, d == 0 ? 1 : d, int(kFrameInterpolationTargetFps.size())));
        }, chosen != 0 && running != chosen && g_appliedSpeed <= 1
               ? "This screen refreshes too slowly to show that many frames; it runs at the most it can show."
               : "Above 60, extra frames are made between the game's own for smoother motion. 90 and 144 suit screens "
                 "of that refresh rate."});
        rows.push_back({"V-Sync", OnOff(g_vsync.load()), [](int) { SetVSync(!g_vsync.load()); },
                        "No screen tearing. Turn off only if your screen has its own sync (G-Sync, FreeSync)."});
        rows.push_back({"FPS counter", OnOff(g_showFps), [](int) { SetShowFps(!g_showFps); }, "Frames per second in the corner."});
        // No Bloom row: its filter was written for another game's renderer and does nothing in Other M.
        rows.push_back({"Copy filter", OnOff(!g_disableCopyFilter), [](int) {
            g_disableCopyFilter = !g_disableCopyFilter;
            aurora_set_disable_copy_filter(g_disableCopyFilter);
            RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
        }, "The console's slight blur on the picture it copies each frame. Off is sharper."});
        rows.push_back({"Anti-aliasing", g_smaa ? "SMAA" : "Off", [](int) {
            g_smaa = !g_smaa;
            aurora_set_anti_aliasing(g_smaa ? AURORA_ANTI_ALIASING_SMAA : AURORA_ANTI_ALIASING_OFF);
            RuntimeConfigFile::SetAntiAliasing(g_smaa ? "smaa" : "off");
        }, "Smooths jagged edges on the finished picture."});
        {
            uint32_t picW = 0, picH = 0;
            AuroraGetPictureSize(&picW, &picH);
            rows.push_back({"Upscaling", g_fsrUpscaling ? "FSR" : "Off", [](int) {
                g_fsrUpscaling = !g_fsrUpscaling;
                aurora_set_upscaler(g_fsrUpscaling ? AURORA_UPSCALER_FSR : AURORA_UPSCALER_OFF, g_fsrSharpnessPercent / 100.0f);
                RuntimeConfigFile::SetUpscaler(g_fsrUpscaling ? "fsr" : "off");
            }, "FSR enlarges the picture to your screen with sharp edges. It acts when Resolution is below your "
               "screen's (now " + std::to_string(picW) + "x" + std::to_string(picH) + ")."});
        }
        if (g_fsrUpscaling) {
            rows.push_back({"FSR sharpness", std::to_string(g_fsrSharpnessPercent) + "%", [](int d) {
                g_fsrSharpnessPercent = std::clamp(g_fsrSharpnessPercent + (d < 0 ? -10 : 10), 0, 100);
                if (d == 0 && g_fsrSharpnessPercent == 100) g_fsrSharpnessPercent = 0;
                aurora_set_upscaler(AURORA_UPSCALER_FSR, g_fsrSharpnessPercent / 100.0f);
                RuntimeConfigFile::SetFsrSharpness(g_fsrSharpnessPercent / 100.0f);
            }, "How much the upscaled picture is sharpened."});
        }
        {
            static constexpr std::array<const char*, 3> kModes{"Ubershaders", "Skip draws", "Wait"};
            static constexpr std::array<const char*, 3> kNotes{
                "An effect the game shows for the first time is drawn by a generic shader while its own compiles: "
                "no stutter and nothing missing.",
                "An effect the game shows for the first time is missing for a moment while its shader compiles.",
                "The game pauses briefly the first time it shows an effect, while its shader compiles."};
            rows.push_back({"Shader compilation", kModes[static_cast<size_t>(g_shaderCompilation)], [](int d) {
                g_shaderCompilation = (g_shaderCompilation + (d < 0 ? 2 : 1)) % 3;
                ApplyShaderCompilation();
                RuntimeConfigFile::SetShaderCompilation(kShaderCompilationNames[static_cast<size_t>(g_shaderCompilation)]);
            }, kNotes[static_cast<size_t>(g_shaderCompilation)]});
        }
        {
            // Config names as main.cpp's backend table and RuntimeConfigFile::IsSupportedGraphicsApi take them.
#if defined(__APPLE__)
            static constexpr std::array<const char*, 2> kApis{"auto", "metal"};
            static constexpr std::array<const char*, 2> kApiNames{"Automatic", "Metal"};
#elif defined(_WIN32)
            static constexpr std::array<const char*, 3> kApis{"auto", "d3d12", "vulkan"};
            static constexpr std::array<const char*, 3> kApiNames{"Automatic", "Direct3D 12", "Vulkan"};
#else
            static constexpr std::array<const char*, 2> kApis{"auto", "vulkan"};
            static constexpr std::array<const char*, 2> kApiNames{"Automatic", "Vulkan"};
#endif
            const std::string chosen = RuntimeConfigFile::GraphicsApi("auto");
            int api = 0;
            for (int i = 0; i < int(kApis.size()); ++i)
                if (chosen == kApis[i]) api = i;
            const std::string running = GraphicsApiDisplayName();
            std::string value = kApiNames[api];
            if (api == 0) value += " (" + running + ")";
            else if (running != kApiNames[api]) value += " - after a restart";
            rows.push_back({"Graphics API", value, [api](int d) {
                RuntimeConfigFile::SetGraphicsApi(kApis[Cycle(api, d == 0 ? 1 : d, int(kApis.size()))]);
            }, api != 0 && running != kApiNames[api] ? "Takes effect the next time the game starts (running " + running + " now)."
               : api == 0 ? AutomaticApiHelp()
                          : "Direct3D 12 or Vulkan. Takes effect the next time the game starts."});
        }
    } else if (tab == kOptSound) {
        rows.push_back(VolumeRow("Master volume", g_audioVolumePercent, [](float v) { AudioBackend::Instance().SetMasterVolume(v); },
                                 [](float v) { return RuntimeConfigFile::SetAudioVolume(v); }));
        rows.push_back({"Mute", OnOff(g_audioMuted), [](int) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }, std::string("The ") + KeyBindingName(g_muteHotkey) + " key mutes and unmutes at any time."});
        rows.push_back({"Mute key", g_optMuteCapture ? "Press a key..." : KeyBindingName(g_muteHotkey), [](int d) {
            if (d == 0) BeginMuteCapture();
        }, "Select it, then press the new key. Escape keeps the old one; Backspace leaves none."});
    } else if (tab == kOptTextures) {
        // Dolphin-style packs (aurora texture_replacement.cpp): the port's texture_replacements folder, the mods' textures,
        // and, when it is switched on, Dolphin's own pack for the game read from Dolphin's folder.
        size_t indexed = 0, loaded = 0;
        aurora_get_texture_pack_counts(&indexed, &loaded);
        const bool on = RuntimeConfigFile::TextureReplacements(true);
        const std::string counts = aurora_texture_packs_pending() ? "Takes effect when you return to the game."
                                   : !on                            ? "Off: the game's own textures everywhere."
                                   : Thousands(indexed) + " replacement textures, " + Thousands(loaded) + " loaded so far.";
        rows.push_back({"Texture packs", OnOff(on), [](int) {
            RuntimeConfigFile::SetTextureReplacements(!RuntimeConfigFile::TextureReplacements(true));
            DolphinImport::ApplyTexturePacks();
        }, counts});
        if (const std::filesystem::path pack = DolphinImport::TexturePack(); !pack.empty()) {
            rows.push_back({"Dolphin's texture pack", OnOff(RuntimeConfigFile::DolphinTextures(false)), [](int) {
                RuntimeConfigFile::SetDolphinTextures(!RuntimeConfigFile::DolphinTextures(false));
                DolphinImport::ApplyTexturePacks();
            }, Thousands(DolphinImport::CountTextures(pack)) + " texture files, used from " + DisplayPath(pack) +
                   " (nothing is copied)."});
        } else {
            rows.push_back({"Dolphin's texture pack", "Not found", {},
                            "Dolphin keeps a pack in its Load/Textures/" + DolphinImport::GameId() +
                                " folder. Options > Saves can point to a portable Dolphin."});
        }
        // Dolphin's Prefetch Custom Textures (Graphics > Advanced): every pack file into memory, here in the background.
        {
            size_t done = 0, total = 0;
            uint64_t bytes = 0, budget = 0;
            bool full = false;
            aurora_get_texture_prefetch(&done, &total, &bytes, &budget, &full);
            const bool prefetch = RuntimeConfigFile::PrefetchTextures(false);
            const auto gigabytes = [](uint64_t value) {
                char text[32];
                std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(value) / 1e9);
                return std::string(text);
            };
            std::string help;
            if (!prefetch && total != 0) {
                help = "Frees its " + gigabytes(bytes) + " of memory when you return to the game.";
            } else if (!prefetch) {
                help = "Off: a texture is read from its pack the first time it is drawn; the game's own shows meanwhile.";
            } else if (!on) {
                help = "Waits for the texture packs to be on.";
            } else if (total == 0) {
                help = "Starts when you return to the game.";
            } else if (full) {
                help = Thousands(done) + " of " + Thousands(total) + " textures in memory (" + gigabytes(bytes) +
                       "): stopped at half this PC's memory. The rest load as they are drawn.";
            } else if (done < total) {
                help = "Loading into memory: " + Thousands(done) + " of " + Thousands(total) + " textures (" +
                       gigabytes(bytes) + ").";
            } else {
                help = "All " + Thousands(total) + " textures in memory (" + gigabytes(bytes) + " of up to " +
                       gigabytes(budget) + ").";
            }
            rows.push_back({"Prefetch custom textures", OnOff(prefetch), [](int) {
                RuntimeConfigFile::SetPrefetchTextures(!RuntimeConfigFile::PrefetchTextures(false));
                aurora_set_texture_prefetch(RuntimeConfigFile::PrefetchTextures(false));
            }, help});
        }
        // Each folder in texture_replacements is a pack; a file named "disabled" in it switches it off.
        const std::filesystem::path packs = RuntimeConfigFile::ApplicationDataDirectory() / "texture_replacements";
        std::vector<std::filesystem::path> folders;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(packs, ec))
            if (entry.is_directory(ec)) folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end());
        for (const std::filesystem::path& folder : folders) {
            const bool disabled = std::filesystem::exists(folder / "disabled", ec);
            rows.push_back({"Pack: " + RuntimeConfigFile::PathToUtf8(folder.filename()), OnOff(!disabled), [folder, disabled](int) {
                std::error_code error;
                if (disabled) std::filesystem::remove(folder / "disabled", error);
                else std::ofstream(folder / "disabled") << "Switched off in Options > Textures.\n";
                DolphinImport::ApplyTexturePacks();
            }, Thousands(DolphinImport::CountTextures(folder)) + " texture files in texture_replacements."});
        }
        // Dolphin's own words (Graphics > Advanced > Utility).
        rows.push_back({"Dump textures", OnOff(RuntimeConfigFile::TextureDumps(false)), [](int) {
            RuntimeConfigFile::SetTextureDumps(!RuntimeConfigFile::TextureDumps(false));
            DolphinImport::ApplyTexturePacks();
        }, "Dumps decoded game textures to Cache/texture_dumps, named the way a texture pack needs them (as Dolphin does)."});
        rows.push_back({"Read the packs again", "", [](int d) {
            if (d == 0) DolphinImport::ApplyTexturePacks();
        }, "After adding or changing files in a pack's folder."});
        rows.push_back({"Open the texture packs folder", "", [packs](int d) {
            if (d == 0) OpenFolder(packs);
        }, "Put a pack's folder here (Dolphin packs work as they are), then pick Read the packs again."});
    } else if (tab == kOptSaves) {
        TakePickedFolder();
        const std::optional<DolphinImport::Save> dolphin = DolphinImport::FindSave();
        const DolphinImport::Save mine = DolphinImport::PortSave();
        rows.push_back({"Dolphin's save", dolphin ? "Last written " + DolphinImport::FormatDate(dolphin->newest) : "Not found", {},
                        dolphin ? DisplayPath(dolphin->data)
                                : std::string("Looked in Dolphin's folders on this computer (and the one picked below).")});
        rows.push_back({"The save here", mine.files > 0 ? "Last written " + DolphinImport::FormatDate(mine.newest) : "None yet", {},
                        DisplayPath(mine.data)});
        const bool older = dolphin && mine.files > 0 && dolphin->newest < mine.newest;
        rows.push_back({"Import Dolphin's save", "", [dolphin, older](int d) {
            if (d != 0 || !dolphin) return;
            g_optPrompt = OptPrompt{"IMPORT DOLPHIN'S SAVE?", "IMPORT",
                                    std::string(older ? "Dolphin's save is OLDER than the one here. " : "") +
                                        "The save here is backed up first, then the game restarts.",
                                    [save = *dolphin] {
                std::filesystem::path backup;
                g_saveStatus = DolphinImport::ImportSave(save, backup);
                if (!g_saveStatus.empty()) return;
                // Tests: OTHERM_TEST_NO_RESTART keeps the process (a restart would start a window outside the test).
                if (std::getenv("OTHERM_TEST_NO_RESTART") != nullptr) {
                    g_saveStatus = "Imported. Restart the game to load it.";
                    return;
                }
                DolphinImport::RestartGame();
            }};
            g_optPromptChoice = 1;
        }, !g_saveStatus.empty() ? g_saveStatus
           : dolphin             ? std::string("Replaces this game's saves (all three slots) with Dolphin's, then restarts the game.")
                                 : std::string("No Dolphin save of Other M was found.")});
        const std::string picked = RuntimeConfigFile::DolphinFolder();
        rows.push_back({"Pick Dolphin's folder", "", [](int d) {
            if (d == 0) PickDolphinFolder();
        }, picked.empty() ? "For a portable Dolphin: its User folder, or the folder Dolphin.exe is in."
                          : "Picked: " + DisplayPath(RuntimeConfigFile::PathFromUtf8(picked))});
        rows.push_back({"Open the save folder", "", [data = mine.data](int d) {
            if (d == 0) OpenFolder(data);
        }, "This game's save files (share/save0.dat to save2.dat, system.bin)."});
        rows.push_back({"Open the save backups folder", "", [](int d) {
            if (d == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "save_backups");
        }, "Each import keeps the saves it replaced here, by date."});
    } else if (tab == kOptCheats) {
        // The built-in cheats (otherm_cheats.h), then the Gecko codes (Dolphin .ini files in the cheats folder or a mod).
        rows.push_back({"Infinite energy", OnOff(OthermCheats::InfiniteEnergy()), [](int) {
            OthermCheats::SetInfiniteEnergy(!OthermCheats::InfiniteEnergy());
        }, "Samus takes no damage and her energy stays full."});
        rows.push_back({"Infinite missiles", OnOff(OthermCheats::InfiniteMissiles()), [](int) {
            OthermCheats::SetInfiniteMissiles(!OthermCheats::InfiniteMissiles());
        }, "Missiles refill as soon as they are fired (once Adam has authorized them)."});
        rows.push_back({"360-degree movement", OnOff(OthermMove::Enabled()), [](int) {
            OthermMove::SetEnabled(!OthermMove::Enabled());
        }, "Samus runs the way the stick points, not just the D-pad's eight directions."});
        rows.push_back({"Minimap faces north", OnOff(OthermMinimap::NorthUp()), [](int) {
            OthermMinimap::SetNorthUp(!OthermMinimap::NorthUp());
        }, "The map in the HUD stays north up, like the pause screen's map, instead of turning with the view."});
        rows.push_back({"Power bombs", "No ammo to refill", {},
                        "Other M gives Power Bombs no ammo count: once authorized, you charge one in Morph Ball and "
                        "release it."});
        static std::vector<ModRuntime::CheatCode> codes;
        codes = ModRuntime::CheatCodes();
        for (const auto& code : codes) {
            const std::string name = code.name;
            const bool on = code.enabled;
            rows.push_back({"Code: " + name, code.supported ? OnOff(on) : "Can't run", [name, on](int) {
                ModRuntime::SetCheatEnabled(name, !on);
            }, code.note});
        }
        rows.push_back({"Open the cheats folder", "", [](int d) {
            if (d == 0) OpenPath(RuntimeConfigFile::PathFromUtf8(ModRuntime::CheatFolder()));
        }, "Put Gecko codes here as a Dolphin game .ini file (R3OE01.ini); they show up after a restart."});
    } else if (tab == kOptMods) {
        // Before the game boots (the start screen) the mods are not read yet, so a change applies at Play.
        const std::string applies = g_launcher.active ? "Applies when you press Play." : "Applies the next time the game starts.";
        // A zip dropped in the mods folder being unpacked (mod_install.h): its row is where the mod appears when done.
        if (const ModInstall::InstallStatus install = ModInstall::Installing(); install.busy) {
            OptRow bar{"Installing " + install.name,
                       install.fraction < 0.0f ? std::string("Reading") : Percent(int(install.fraction * 100.0f)), nullptr,
                       "Unpacked once into its own folder in the mods folder; then it is listed here with its options." +
                           std::string(install.queued > 0 ? " More zips follow." : "")};
            bar.slider = install.fraction < 0.0f ? 0 : int(install.fraction * 100.0f);
            bar.sliderMax = 100;
            rows.push_back(bar);
        }
        for (const ModInstall::Mod& mod : ModInstall::List()) {
            const std::string name = mod.name;
            const bool on = mod.enabled;
            rows.push_back({name, OnOff(on), [name, on](int) { ModInstall::SetEnabled(name, !on); }, applies});
            if (!on) continue;
            // A Riivolution pack's own options (Other M Redux: its models, music, movies...), remembered in the mod's
            // folder the way Dolphin does.
            const std::filesystem::path folder = ModInstall::Folder() / RuntimeConfigFile::PathFromUtf8(name);
            for (const RuntimeRiivolution::PackOption& option : RuntimeRiivolution::PackOptions(folder)) {
                const uint32_t count = static_cast<uint32_t>(option.choices.size());
                const std::string value = option.selected == 0 || option.selected > count
                                              ? std::string("Off")
                                              : count == 1 ? std::string("On") : option.choices[option.selected - 1];
                rows.push_back({"   " + option.name, value, [folder, id = option.id, selected = option.selected, count](int d) {
                    // Off, then each choice; A / Enter steps on and wraps.
                    const uint32_t states = count + 1;
                    const int step = d == 0 ? 1 : d;
                    const uint32_t next = static_cast<uint32_t>((static_cast<int>(selected) + step + static_cast<int>(states)) %
                                                                static_cast<int>(states));
                    RuntimeRiivolution::SetPackOption(folder, id, next);
                }, option.section + ": one of this mod's own options. " + applies});
            }
        }
        rows.push_back({"Open the mods folder", "", [](int d) {
            if (d == 0) OpenFolder(ModInstall::Folder());
        }, "Put a mod's .zip (or its folder) here; it is installed the next time the game starts."});
    } else {
        // The controls as a table: what each does, the controller's button (Metroid.ini, virtual_remote.cpp) and the
        // keyboard's key (Config.toml [keyboard], keybinds.cpp). Pick a cell and press the new button or key; a cell
        // with no arrows is fixed.
        using VirtualRemote::PadControl;
        using VirtualRemote::PadHotkey;
        const int keyCapture = Keybinds::Capturing();
        const int style = std::max(0, aurora_button_icon_style());
        const auto padName = [style](const char* xbox, const char* playstation, const char* nintendo) {
            return std::string(style == 1 ? playstation : style == 2 ? nintendo : xbox);
        };
        const auto padCell = [](PadControl control) {
            const bool waiting = g_optPadBindControl == static_cast<int>(control);
            return OptCell{waiting ? "Press a button..." : VirtualRemote::PadLabel(control), [control] {
                g_controlsStatus.clear();
                g_optPadBindControl = static_cast<int>(control);
                VirtualRemote::BeginPadCapture();
            }};
        };
        const auto keyCell = [keyCapture](int action) {
            return OptCell{keyCapture == action ? "Press a key..." : Keybinds::KeyLabel(action), [action] {
                g_controlsStatus.clear();
                Keybinds::BeginCapture(action);
            }};
        };
        const auto fixed = [](std::string text) { return OptCell{std::move(text), {}}; };
        const std::string pickHelp = !g_controlsStatus.empty() ? g_controlsStatus
                                     : g_optPadBindControl >= 0 || keyCapture >= 0
                                         ? std::string("Press the new button or key. Escape keeps the old one.")
                                         : std::string("Pick a controller or keyboard cell (left / right), then press the new button or key.");
        const auto table = [&rows, &pickHelp](std::string label, OptCell pad, OptCell key, std::string help = {}) {
            OptRow row{std::move(label), "", {}, help.empty() ? pickHelp : std::move(help)};
            row.cells = {std::move(pad), std::move(key)};
            rows.push_back(std::move(row));
        };
        const std::string moveHelp = "The left stick or D-pad moves Samus and aims; it is not changed here.";
        table("Move left", fixed("Stick / D-pad left"), keyCell(Keybinds::kLeft), moveHelp);
        table("Move right", fixed("Stick / D-pad right"), keyCell(Keybinds::kRight), moveHelp);
        table("Up", fixed("Stick / D-pad up"), keyCell(Keybinds::kUp), moveHelp);
        table("Down", fixed("Stick / D-pad down"), keyCell(Keybinds::kDown), moveHelp);
        table("Jump (2)", padCell(PadControl::Two), keyCell(Keybinds::kJump));
        table("Fire / Bomb (1)", padCell(PadControl::One), keyCell(Keybinds::kRun));
        table("Morph Ball / Missile (A)", padCell(PadControl::A), keyCell(Keybinds::kA));
        table("Lock on (B)", padCell(PadControl::B), fixed("None"));
        table("First-person view (point)", padCell(PadControl::Point),
              fixed(RuntimeConfigFile::MousePointer(true) ? "Right mouse button" : "None"));
        table("Concentrate (remote upright)", padCell(PadControl::Upright), fixed("None"),
              "Hold it with A (" + VirtualRemote::PadLabel(PadControl::A) + ") to refill missiles, and energy when it runs low.");
        table("Pause (+)", padCell(PadControl::Plus), keyCell(Keybinds::kPlus));
        table("Skip (-)", padCell(PadControl::Minus), keyCell(Keybinds::kMinus));
        table("Shake", fixed("None"), keyCell(Keybinds::kShake));
        table("Tilt left", fixed("None"), keyCell(Keybinds::kTiltLeft));
        table("Tilt right", fixed("None"), keyCell(Keybinds::kTiltRight));
        table("Options", fixed(padName("View + Menu", "Create + Options", "- and +")), fixed("F1"));
        table("Fast-forward", fixed(padName("RS (click)", "R3", "RS (click)")), fixed("F"));
        table("Skip a cutscene", fixed(padName("Start, A or LB", "Options, Cross or L1", "+, B or L")), fixed("Backspace"));
        // Save / load state: in either column, whatever is held when a count of 5 ends (PollChordCapture;
        // DrawOptionsMenu counts it down). Controller: a chord (VirtualRemote::PadHotkey); keyboard: keys (Keybinds).
        const auto chordCell = [](PadHotkey hotkey, bool keys) {
            const int index = static_cast<int>(hotkey);
            std::string text = keys ? Keybinds::HotkeyLabel(index) : VirtualRemote::HotkeyLabel(hotkey);
            if (g_optChordCapture == index && g_optChordKeys == keys)
                text = g_optChordCounting ? ChordLabel(g_optChordHeld) + "  " + std::to_string(ChordSecondsLeft())
                                          : std::string(keys ? "Hold keys..." : "Hold buttons...");
            return OptCell{text, [index, keys] {
                g_controlsStatus.clear();
                BeginChordCapture(index, keys);
            }};
        };
        const std::string chordHelp =
            !g_controlsStatus.empty() ? g_controlsStatus
            : g_optChordCapture >= 0
                ? std::string(g_optChordKeys ? "Hold the keys" : "Hold the buttons") +
                      " together: whatever is held when the count of 5 ends is the shortcut."
                : "Pick a cell, then hold the buttons or keys together for 5 seconds (e.g. " +
                      padName("View + RB", "Create + R1", "- + R") + ", or Left Ctrl + S).";
        table("Save state", chordCell(PadHotkey::SaveState, false), chordCell(PadHotkey::SaveState, true), chordHelp);
        table("Load state", chordCell(PadHotkey::LoadState, false), chordCell(PadHotkey::LoadState, true), chordHelp);

        OptRow vibration{"Vibration", g_rumbleStrength == 0 ? std::string("Off") : Percent(g_rumbleStrength), [](int d) {
            SetRumbleStrength(d == 0 ? (g_rumbleStrength >= 200 ? 0 : g_rumbleStrength + 10) : g_rumbleStrength + d * 10);
        }, "How hard the controller rumbles when the game shakes the Wii Remote: 100% is the usual, 0% is off."};
        vibration.slider = g_rumbleStrength;
        vibration.sliderMax = 200;
        vibration.setSlider = [](int v) { SetRumbleStrength(v); };
        rows.push_back(std::move(vibration));
        rows.push_back({"Mouse aims the Wii pointer", OnOff(RuntimeConfigFile::MousePointer(true)), [](int) {
            RuntimeConfigFile::SetMousePointer(!RuntimeConfigFile::MousePointer(true));
        }, "Hold the right mouse button to point at the screen; a left click is A."});
        rows.push_back({"Controller: default buttons", "", [](int d) {
            if (d != 0) return;
            std::string error;
            g_controlsStatus = VirtualRemote::ResetPadBindings(error) ? std::string("Controller buttons back to the defaults.") : error;
        }, "Puts the controller's buttons in the table back to the defaults (none for save / load state)."});
        rows.push_back({"Keyboard: default keys", "", [](int d) { if (d == 0) Keybinds::ResetDefaults(); },
                        "Puts the keyboard's keys in the table back to the defaults."});
        // The controller is read through the Dolphin-style profile beside the game (virtual_remote.cpp ProfileIntent).
        rows.push_back({"Controller: open Metroid.ini", "", [](int d) {
            if (d != 0) return;
            if (const auto dir = RuntimeConfigFile::PlayerDirectory()) OpenPath(*dir / "Metroid.ini");
        }, "The controller's buttons and sticks, in Dolphin's profile format. Save it, then pick Reload."});
        rows.push_back({"Controller: reload Metroid.ini", "", [](int d) {
            if (d == 0) VirtualRemote::ReloadProfile();
        }, "Reads the profile again; the on-screen button pictures follow at the next build."});
        rows.push_back({"Wii Remotes", OnOff(g_wiiRemotesEnabled), [](int) {
            g_wiiRemotesEnabled = !g_wiiRemotesEnabled;
            RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
        }, "Real Wii Remotes over Bluetooth (pair with 1+2 in Windows' Bluetooth settings). Takes effect the next time "
           "the game starts; turn it off with a Mayflash DolphinBar."});
        rows.push_back({"Keep looking for Wii Remotes", OnOff(g_wiiContinuousScan), [](int) {
            g_wiiContinuousScan = !g_wiiContinuousScan;
            RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
        }, "While none is connected, check Bluetooth every 2 seconds, like Dolphin's Continuous Scanning."});
        rows.push_back({"Look for Wii Remotes now", "", [](int d) {
            if (d == 0 && g_wiiRemotesEnabled) WiiRemoteInput::RescanNow();
        }, !g_wiiRemotesEnabled ? std::string("Wii Remotes are off.")
           : WiiRemoteInput::IsScanning() ? "Looking... (" + std::to_string(WiiRemoteInput::ScanCount()) + " so far): press 1+2 on the remote."
                                          : std::string("Then press 1+2 (or the red SYNC button) on the remote.")});
    }
    return rows;
}

// ---- The start screen (launcher): START, the folder browser, setting up ----

std::mutex g_launcherPickMutex;
std::optional<std::string> g_launcherPicked;  // what the system's folder picker returned ("" cancelled)

// Steam Deck's Game Mode (gamescope) shows no desktop windows: the system's folder picker would never come up there.
bool InGamescope() {
#if defined(_WIN32)
    return false;
#else
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr ||
           (desktop != nullptr && std::strstr(desktop, "gamescope") != nullptr);
#endif
}

std::string FileName(const std::filesystem::path& path) {
    return RuntimeConfigFile::PathToUtf8(path.filename().empty() ? path : path.filename());
}

std::string SizeText(const std::filesystem::path& file) {
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::file_size(file, ec);
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", ec ? 0.0 : double(bytes) / 1e9);
    return text;
}

std::string DurationText(double seconds) {
    const int s = std::max(0, int(seconds));
    return s < 60 ? std::to_string(s) + " s" : std::to_string(s / 60) + " min " + std::to_string(s % 60) + " s";
}

void LauncherGoTo(LauncherScreen screen) {
    g_launcher.screen = screen;
    g_optTab = 0;
    g_optRow = 0;
    g_optFirst = 0;
    g_optPrompt.reset();
    g_launcher.pickFirst = true;
}

void LauncherShowStart() {
    LauncherGoTo(LauncherScreen::Start);
    g_launcher.ready = GameFolder::Ready();
    g_launcher.describe = GameFolder::Describe();
    g_launcher.beside = g_launcher.ready ? std::vector<std::filesystem::path>{} : GameFolder::Scan(GameFolder::GameRoot()).images;
}

void LauncherBrowse(const std::filesystem::path& folder) {
    LauncherGoTo(LauncherScreen::Browse);
    g_launcher.folder = folder;
    g_launcher.listed = false;
}

// Where the game's files come from: the folder browser, or on Android the system's file picker (an app reads shared
// storage only through what the player picks there; LauncherUpdate takes the answer).
void LauncherChooseGame() {
#if defined(__ANDROID__)
    RT_LOGF(RT_TAG_RUNTIME, "[launcher] the system's file picker\n");
    AndroidBridge::PickDisc();
#else
    LauncherBrowse(GameFolder::GameRoot());
#endif
}

void LauncherSetUp(const std::filesystem::path& source) {
    RT_LOGF(RT_TAG_RUNTIME, "[launcher] set up from %s\n", RuntimeConfigFile::PathToUtf8(source).c_str());
    GameFolder::StartSetUp(source);
    LauncherGoTo(LauncherScreen::SettingUp);
}

void LauncherPlay() {
    RT_LOGF(RT_TAG_RUNTIME, "[launcher] play\n");
    g_launcher.active = false;
    SetOptionsOpen(false);
}

// B on the start screen: up one folder in the browser (from the places back to START), from an Options page back to
// START. Nothing while setting up, or on START itself.
void LauncherBack() {
    if (g_launcher.screen == LauncherScreen::Browse) {
        if (g_launcher.folder.empty()) {
            LauncherShowStart();
            return;
        }
        const std::filesystem::path parent = g_launcher.folder.parent_path();
        LauncherBrowse(parent == g_launcher.folder || parent.empty() ? std::filesystem::path() : parent);
    } else if (g_launcher.screen == LauncherScreen::Start && g_optTab != 0) {
        LauncherShowStart();
    }
}

// Reads the browser's folder when it changed: its subfolders (no hidden or system ones), its disc images.
void LauncherList() {
    if (g_launcher.listed && g_launcher.listedFolder == g_launcher.folder) return;
    g_launcher.listed = true;
    g_launcher.listedFolder = g_launcher.folder;
    g_launcher.folders.clear();
    g_launcher.contents = {};
    if (g_launcher.folder.empty()) {
        g_launcher.places = GameFolder::Places();
        return;
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator it(g_launcher.folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        const std::string name = FileName(it->path());
        if (name.empty() || name[0] == '.' || name[0] == '$' || name == "System Volume Information") continue;
        g_launcher.folders.push_back(it->path());
    }
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::sort(g_launcher.folders.begin(), g_launcher.folders.end(),
              [&](const auto& a, const auto& b) { return lower(FileName(a)) < lower(FileName(b)); });
    g_launcher.contents = GameFolder::Scan(g_launcher.folder);
    RT_LOGF(RT_TAG_RUNTIME, "[launcher] folder %s: %zu folders, %zu disc images%s\n",
            RuntimeConfigFile::PathToUtf8(g_launcher.folder).c_str(), g_launcher.folders.size(), g_launcher.contents.images.size(),
            g_launcher.contents.extracted.empty() ? "" : ", an extracted game");
}

// A row that only says something (no step): the folder shown, what a folder holds, a set-up's result.
OptRow NoteRow(std::string label, std::string help, ImU32 color) {
    OptRow row{std::move(label), "", nullptr, std::move(help)};
    row.color = color;
    return row;
}

OptRow ButtonRow(std::string label, std::string value, std::function<void(int)> pick, std::string help) {
    OptRow row{std::move(label), std::move(value), [pick = std::move(pick)](int d) {
                   if (d == 0) pick(0);
               }, std::move(help)};
    row.button = true;
    return row;
}

#if defined(__ANDROID__)
constexpr const char* kChooseGameHelp = "The game is not set up yet: choose your disc image.";
constexpr const char* kAnotherGameHelp = " A: another disc image.";
#else
constexpr const char* kChooseGameHelp = "The game is not set up yet: choose the folder that holds your disc image.";
constexpr const char* kAnotherGameHelp = " A: another folder.";
#endif

std::vector<OptRow> LauncherStartRows() {
    std::vector<OptRow> rows;
    const bool ready = g_launcher.ready;
    const std::filesystem::path beside = g_launcher.beside.empty() ? std::filesystem::path() : g_launcher.beside.front();
    // A mod zip dropped in the mods folder is unpacked while this is up (its progress is on the Mods page); the game
    // reads the mods once it is done, so Play waits.
    const ModInstall::InstallStatus install = ModInstall::Installing();
    const std::string installing =
        install.fraction < 0.0f ? std::string("Installing mods") : "Installing mods " + Percent(int(install.fraction * 100.0f));
    rows.push_back(ButtonRow("Play", install.busy ? installing : "", [ready, beside](int) {
        if (ModInstall::Installing().busy) return;
        if (ready) LauncherPlay();
        else if (!beside.empty()) LauncherSetUp(beside);
        else LauncherChooseGame();
    }, install.busy ? "Waits until " + install.name + " is installed (Mods shows its progress)." +
                          (g_launcherPlaysAfterInstall ? " The game starts when it is done." : "")
       : ready ? std::string("Starts the game.")
       : !beside.empty() ? "Sets the game up from " + FileName(beside) + " first (once, a few minutes), then Play starts it."
                         : std::string(kChooseGameHelp)));
    rows.push_back(ButtonRow("Game files", ready ? "Ready" : "Not set up", [](int) { LauncherChooseGame(); },
                             ready ? g_launcher.describe + kAnotherGameHelp : g_launcher.describe));
    if (!ready && !beside.empty()) {
        rows.push_back(ButtonRow("Set up from " + FileName(beside), SizeText(beside), [beside](int) { LauncherSetUp(beside); },
                                 "Found beside the game: unpacks it into the game's folder, once (a few minutes)."));
    }
    if (!g_launcher.status.empty())
        rows.push_back(NoteRow(g_launcher.status, g_launcher.statusOk ? "" : "Game files picks another folder or image.",
                               g_launcher.statusOk ? HudTheme::kCyan : HudTheme::kOrange));
    rows.push_back({"Start screen", OnOff(RuntimeConfigFile::ShowLauncher(true)), [](int) {
        RuntimeConfigFile::SetShowLauncher(!RuntimeConfigFile::ShowLauncher(true));
    }, "On: this screen comes up before the game boots. Off: the game boots straight away (this screen still comes up "
       "while the game is not set up)."});
    rows.push_back(ButtonRow("Quit", "", [](int) { ExitForAuroraWindowClose(); }, "Closes the program."));
    return rows;
}

std::vector<OptRow> LauncherBrowseRows() {
    LauncherList();
    std::vector<OptRow> rows;
    if (g_launcher.folder.empty()) {
        rows.push_back(NoteRow("Where is your disc image?", "Pick a place, then the folder that holds the image.", HudTheme::kTextDim));
        for (const auto& [name, path] : g_launcher.places)
            rows.push_back(ButtonRow(name, "", [path](int) { LauncherBrowse(path); }, RuntimeConfigFile::PathToUtf8(path)));
        if (!InGamescope()) {
            rows.push_back(ButtonRow("The system's folder picker", "", [](int) {
                static const SDL_DialogFileCallback done = [](void*, const char* const* files, int) {
                    std::lock_guard lock(g_launcherPickMutex);
                    g_launcherPicked = files != nullptr && files[0] != nullptr ? std::string(files[0]) : std::string();
                };
                const std::string start = RuntimeConfigFile::PathToUtf8(GameFolder::GameRoot());
                SDL_ShowOpenFolderDialog(done, nullptr, nullptr, start.c_str(), false);
            }, "Opens the system's own window to pick the folder."));
        }
        return rows;
    }
    const GameFolder::FolderContents& found = g_launcher.contents;
    rows.push_back(NoteRow(RuntimeConfigFile::PathToUtf8(g_launcher.folder), "The folder shown. B goes up one folder.",
                           HudTheme::kCyan));
    if (!found.extracted.empty()) {
        const std::filesystem::path extracted = found.extracted;
        rows.push_back(ButtonRow("Use this folder", "Extracted game", [extracted](int) { LauncherSetUp(extracted); },
                                 "The game's files, extracted with Dolphin: used where they are."));
    } else if (!found.images.empty()) {
        const std::filesystem::path image = found.images.front();
        rows.push_back(ButtonRow("Use this folder", FileName(image), [image](int) { LauncherSetUp(image); },
                                 "Sets the game up from " + FileName(image) + " (" + SizeText(image) +
                                     "), once: a few minutes. You can delete the image afterwards."));
    } else {
        rows.push_back(NoteRow("No disc image in this folder",
                               "Disc images: .rvz, .iso, .wia, .gcz, .ciso or .wbfs. Or a folder Dolphin's Extract Entire Disc made.",
                               HudTheme::kTextDim));
    }
    if (found.images.size() > 1) {
        for (const std::filesystem::path& image : found.images)
            rows.push_back(ButtonRow(FileName(image), SizeText(image), [image](int) { LauncherSetUp(image); },
                                     "Sets the game up from this image, once: a few minutes."));
    }
    rows.push_back(ButtonRow("Up one folder", "", [](int) { LauncherBack(); },
                             RuntimeConfigFile::PathToUtf8(g_launcher.folder.parent_path())));
    rows.push_back(ButtonRow("Other places", "", [](int) { LauncherBrowse({}); }, "Home, downloads, drives and SD cards."));
    for (const std::filesystem::path& folder : g_launcher.folders)
        rows.push_back(ButtonRow(FileName(folder) + "/", "", [folder](int) { LauncherBrowse(folder); }, ""));
    return rows;
}

std::vector<OptRow> LauncherSetUpRows() {
    const GameFolder::SetUpStatus st = GameFolder::Progress();
    const std::string help = "Once only. Keep the game open: this takes a few minutes.";
    std::vector<OptRow> rows;
    rows.push_back(NoteRow("Setting up from " + st.source, help, HudTheme::kText));
    OptRow bar{"Unpacking", st.fraction < 0.0f ? std::string("Reading") : Percent(int(st.fraction * 100.0f)), nullptr, help};
    bar.slider = st.fraction < 0.0f ? 0 : int(st.fraction * 100.0f);
    bar.sliderMax = 100;
    rows.push_back(bar);
    rows.push_back({"Time", DurationText(st.seconds), nullptr, help});
    return rows;
}

std::vector<OptRow> PageRows(int page);

// Each start-screen frame: a finished set-up goes back to START with its result; a folder the system's picker
// returned is shown in the browser.
void LauncherUpdate() {
    if (g_launcher.pickFirst) {
        g_launcher.pickFirst = false;
        const std::vector<OptRow> rows = PageRows(g_optTab);
        for (int i = 0; i < int(rows.size()); ++i)
            if (rows[static_cast<size_t>(i)].step) {
                g_optRow = i;
                break;
            }
    }
    if (g_launcher.screen == LauncherScreen::SettingUp) {
        const GameFolder::SetUpStatus st = GameFolder::Progress();
        if (st.finished) {
            GameFolder::TakeFinished();
            g_launcher.status = st.message;
            g_launcher.statusOk = st.ok;
            LauncherShowStart();
        }
    }
    if (g_launcherPlaysAfterInstall && !ModInstall::Installing().busy) {
        g_launcherPlaysAfterInstall = false;
        RT_LOGF(RT_TAG_RUNTIME, "[launcher] mods installed; playing (the start screen is off)\n");
        LauncherPlay();
        return;
    }
    std::optional<std::string> picked;
    {
        std::lock_guard lock(g_launcherPickMutex);
        picked.swap(g_launcherPicked);
    }
    if (picked && !picked->empty()) LauncherBrowse(RuntimeConfigFile::PathFromUtf8(*picked));
#if defined(__ANDROID__)
    if (std::string uri, name; g_launcher.screen != LauncherScreen::SettingUp && AndroidBridge::TakePickedDisc(uri, name)) {
        if (uri.empty()) {
            RT_LOGF(RT_TAG_RUNTIME, "[launcher] the file picker was closed without a choice\n");
        } else {
            RT_LOGF(RT_TAG_RUNTIME, "[launcher] set up from the picked file %s\n", name.c_str());
            GameFolder::StartSetUpFromDocument(uri, name);
            LauncherGoTo(LauncherScreen::SettingUp);
        }
    }
#endif
}

// The pages Options shows: its own, or the start screen's (START and the Options pages; the browser and the set-up
// are one page each).
int PageCount() {
    if (!g_launcher.active) return kOptTabCount;
    return g_launcher.screen == LauncherScreen::Start ? 1 + kOptTabCount : 1;
}

std::string PageName(int page) {
    if (!g_launcher.active) return kOptTabs[std::clamp(page, 0, kOptTabCount - 1)];
    switch (g_launcher.screen) {
    case LauncherScreen::Browse: return "Game files";
    case LauncherScreen::SettingUp: return "Setting up";
    case LauncherScreen::Start: break;
    }
    return page == 0 ? std::string("Start") : std::string(kOptTabs[std::clamp(page - 1, 0, kOptTabCount - 1)]);
}

std::vector<OptRow> PageRows(int page) {
    if (!g_launcher.active) return OptionsRows(page);
    switch (g_launcher.screen) {
    case LauncherScreen::Browse: return LauncherBrowseRows();
    case LauncherScreen::SettingUp: return LauncherSetUpRows();
    case LauncherScreen::Start: break;
    }
    return page == 0 ? LauncherStartRows() : OptionsRows(page - 1);
}

// Save states: what the last F5 / F9 did, for a few seconds at the bottom left.
void DrawStateNotice() {
    const std::string notice = StandaloneState::Notice();
    if (notice.empty()) return;
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float px = std::clamp(display.y / 45.0f, 15.0f, 40.0f);
    const ImVec2 size = HudTheme::Measure(px, notice.c_str());
    const ImVec2 at(px, display.y - size.y - px * 1.2f);
    const ImVec2 a(at.x - px * 0.6f, at.y - px * 0.35f), b(at.x + size.x + px * 0.6f, at.y + size.y + px * 0.35f);
    HudTheme::Panel(list, a, b, px * 0.5f, HudTheme::kPanelTop, HudTheme::kPanelBottom, HudTheme::Alpha(HudTheme::kCyan, 200),
                    std::max(1.0f, px / 16.0f));
    list->AddRectFilled(a, ImVec2(a.x + std::max(2.0f, px / 8.0f), b.y), HudTheme::kOrange);
    HudTheme::Text(list, at, px, HudTheme::kText, notice.c_str(), 0.0f, 0.0f);
}

// Save / load state from the controller (Options > Controls): each chord fires once as it is completed, as F5 / F9 do.
void PollStateHotkeys() {
    switch (VirtualRemote::PollHotkeys(!g_optOpen && !StartupScreenVisible())) {
    case VirtualRemote::PadHotkey::SaveState: StandaloneState::RequestSave(); break;
    case VirtualRemote::PadHotkey::LoadState: StandaloneState::RequestLoad(); break;
    default: break;
    }
}

// OTHERM_TEST_REBIND=1 (tests): a virtual SDL gamepad is plugged in, Options > Controls > "Controller: Jump (2)" is
// selected, the pad's West button is pressed, and the new binding (shown and saved) is checked.
void RebindSelfTest() {
    static const bool on = std::getenv("OTHERM_TEST_REBIND") != nullptr;
    static int stage = 0, frames = 0;
    static SDL_JoystickID padId = 0;
    static SDL_Joystick* pad = nullptr;
    if (!on || stage < 0) return;
    ++frames;
    if (stage == 0) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = 0xFFFFFFFFu;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        desc.name = "Rebind test pad";
        padId = SDL_AttachVirtualJoystick(&desc);
        pad = padId ? SDL_OpenJoystick(padId) : nullptr;
        RT_LOGF(RT_TAG_RUNTIME, "rebind test: virtual pad %s\n", pad ? "attached" : SDL_GetError());
        stage = pad ? 1 : -1;
        frames = 0;
    } else if (stage == 1) {  // the game picks the pad up as a controller, then the row starts its capture
        int port = -1;  // a real controller may already be player 1: rebind the virtual pad's own port
        for (uint32_t p = 0; p < PAD_MAX_CONTROLLERS && port < 0; ++p) {
            const int index = PADGetIndexForPort(p);
            if (index >= 0 && PADGetSDLGamepadForIndex(static_cast<uint32_t>(index)) == SDL_GetGamepadFromID(padId)) port = int(p);
        }
        if (port >= 0) {
            g_controllerPort = port;
            for (const OptRow& row : OptionsRows(3))
                if (row.label == "Controller: Jump (2)") row.step(0);
            RT_LOGF(RT_TAG_RUNTIME, "rebind test: was %s, capturing %d\n", PadBindingLabel(PAD_BUTTON_A).c_str(), g_optPadCapture);
            stage = g_optPadCapture >= 0 ? 2 : -1;
            frames = 0;
        } else if (frames > 600) {
            RT_LOGF(RT_TAG_RUNTIME, "rebind test: FAILED (the pad never reached a controller port)\n");
            stage = -1;
        }
    } else if (stage == 2) {
        PollOptionsCapture();
        if (frames == 10) SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_WEST, true);
        if (g_optPadCapture < 0 || frames > 300) {
            SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_WEST, false);
            const std::string now = PadBindingLabel(PAD_BUTTON_A);
            std::ifstream cfg(RuntimeConfigFile::ApplicationDataDirectory() / "Config.toml");
            const std::string text((std::istreambuf_iterator<char>(cfg)), {});
            const bool saved = text.find("west") != std::string::npos;  // [controller] jump = "west"
            RT_LOGF(RT_TAG_RUNTIME, "rebind test: %s (Jump is now %s, saved %s)\n",
                    now.rfind("West", 0) == 0 && saved ? "PASSED" : "FAILED", now.c_str(), saved ? "yes" : "no");
            stage = -1;
        }
    }
}

// Options with the mouse: what DrawOptionsMenu drew where (screen pixels), hit-tested on the next frame. Moving the
// mouse over a row selects it (only moving it: a cursor left resting does not undo controller navigation), a click
// acts like A on it, on an arrow like left / right, on a tab or page button turns to it; the wheel moves up and down,
// a right click backs out like B.
// Cell: a Controls table cell (index row * 2 + column); Slider: a slider row's bar (index: the row).
enum class OptHit { Tab, PagePrev, PageNext, Row, RowLeft, RowRight, Choice, Cell, Slider };
struct OptHitBox {
    ImVec2 a, b;
    OptHit what;
    int index;
};
std::vector<OptHitBox> g_optHits;
ImVec2 g_optLastMouse{-1.0f, -1.0f};

uint32_t OptionsMouseEvents() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    if (!ImGui::IsMousePosValid(&mouse)) return 0;
    const bool moved = std::fabs(mouse.x - g_optLastMouse.x) + std::fabs(mouse.y - g_optLastMouse.y) > 1.0f;
    g_optLastMouse = mouse;
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    uint32_t ev = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? kOptBack : 0;
    const OptHitBox* hit = nullptr;
    for (auto it = g_optHits.rbegin(); it != g_optHits.rend() && hit == nullptr; ++it)  // drawn last = on top
        if (mouse.x >= it->a.x && mouse.x <= it->b.x && mouse.y >= it->a.y && mouse.y <= it->b.y) hit = &*it;
    if (g_optPrompt) {
        if (hit != nullptr && hit->what == OptHit::Choice) {
            if (moved || click) g_optPromptChoice = hit->index;
            if (click) ev |= kOptConfirm;
        }
        return ev;
    }
    if (io.MouseWheel > 0.0f) ev |= kOptUp;
    if (io.MouseWheel < 0.0f) ev |= kOptDown;
    if (hit == nullptr) return ev;
    if (click) {
        static constexpr const char* kNames[] = {"tab", "previous page", "next page", "row", "row's left arrow", "row's right arrow",
                                                 "prompt button", "table cell", "slider"};
        RT_LOGF(RT_TAG_RUNTIME, "options: mouse click on %s %d\n", kNames[static_cast<int>(hit->what)], hit->index);
    }
    switch (hit->what) {
    case OptHit::Row:
    case OptHit::RowLeft:
    case OptHit::RowRight:
        if (moved || click) g_optRow = hit->index;
        if (click) ev |= hit->what == OptHit::Row ? kOptConfirm : hit->what == OptHit::RowLeft ? kOptLeft : kOptRight;
        break;
    case OptHit::Tab:
        if (click && hit->index != g_optTab) {
            g_optTab = hit->index;
            g_optRow = 0;
            g_optFirst = 0;
        }
        break;
    case OptHit::PagePrev:
        if (click) ev |= kOptPagePrev;
        break;
    case OptHit::PageNext:
        if (click) ev |= kOptPageNext;
        break;
    case OptHit::Choice:
        break;
    case OptHit::Cell:
        if (moved || click) {
            g_optRow = hit->index / 2;
            g_optCol = hit->index % 2;
        }
        if (click) ev |= kOptConfirm;
        break;
    case OptHit::Slider:
        if (moved || click) g_optRow = hit->index;
        // Pressed or dragged on the bar: the value under the pointer, in steps of 10.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && hit->b.x > hit->a.x) {
            const std::vector<OptRow> rows = PageRows(g_optTab);
            if (hit->index < int(rows.size())) {
                const OptRow& row = rows[static_cast<size_t>(hit->index)];
                const float f = std::clamp((mouse.x - hit->a.x) / (hit->b.x - hit->a.x), 0.0f, 1.0f);
                const int value = static_cast<int>(std::lround(f * row.sliderMax / 10.0f)) * 10;
                if (row.setSlider && value != row.slider) row.setSlider(value);
            }
        }
        break;
    }
    return ev;
}

void HandleOptionsInput() {
    PollOptionsCapture();
    uint32_t ev = g_optEvents.exchange(0);
    if (OptionsCapturing()) return;  // the key or button pressed belongs to the capture
    ev |= OptionsMouseEvents();
    if (g_optPrompt) {
        if (ev & (kOptLeft | kOptRight | kOptUp | kOptDown)) g_optPromptChoice ^= 1;
        if (ev & kOptBack) g_optPrompt.reset();
        if (ev & kOptConfirm) {
            const OptPrompt prompt = std::move(*g_optPrompt);
            g_optPrompt.reset();
            if (g_optPromptChoice == 0 && prompt.action) prompt.action();
        }
        return;
    }
    if (ev & kOptBack) {
        // The start screen never closes: B goes back within it.
        if (g_launcher.active) LauncherBack();
        else SetOptionsOpen(false);
        return;
    }
    // LB / RB turn the page; up and down stay within it.
    const bool pagePrev = (ev & kOptPagePrev) != 0, pageNext = (ev & kOptPageNext) != 0;
    if (pagePrev != pageNext && PageCount() > 1) {
        g_optTab = Cycle(g_optTab, pageNext ? 1 : -1, PageCount());
        g_optRow = 0;
        g_optFirst = 0;
        RT_LOGF(RT_TAG_RUNTIME, "options: page %s\n", PageName(g_optTab).c_str());
    }
    const std::vector<OptRow> rows = PageRows(g_optTab);
    const int count = static_cast<int>(rows.size());
    if (count == 0) return;
    if (ev & kOptUp) g_optRow = g_optRow <= 0 ? count - 1 : g_optRow - 1;
    if (ev & kOptDown) g_optRow = g_optRow >= count - 1 ? 0 : g_optRow + 1;
    g_optRow = std::clamp(g_optRow, 0, count - 1);
    const int dir = (ev & kOptLeft) ? -1 : (ev & kOptRight) ? 1 : 0;
    const OptRow& row = rows[static_cast<size_t>(g_optRow)];
    if (!row.cells.empty()) {
        // A table row: left / right pick the column, A picks the cell.
        if (dir != 0) g_optCol = dir;
        g_optCol = std::clamp(g_optCol < 0 ? 0 : g_optCol, 0, static_cast<int>(row.cells.size()) - 1);
        const OptCell& cell = row.cells[static_cast<size_t>(g_optCol)];
        if ((ev & kOptConfirm) && cell.pick) cell.pick();
        return;
    }
    if (!row.step) return;
    if (dir != 0 && !row.value.empty() && !row.button) row.step(dir);
    if (ev & kOptConfirm) row.step(0);
}

// The buttons the footer and the page badges name, for the controller in use (the button_icons set it shows).
struct OptHints { const char* prev; const char* next; const char* select; const char* close; };
// A real Wii Remote is player 1 (not a gamepad read through Metroid.ini): it turns pages with - and +.
bool RealRemoteIsPlayerOne() {
    using WiiRemoteInput::Kind;
    const Kind kind = WiiRemoteInput::KindForPort(0);
    return kind == Kind::Remote || kind == Kind::RemoteWithNunchuk || kind == Kind::RemoteWithClassic;
}

OptHints CurrentHints() {
    if (RealRemoteIsPlayerOne()) return {"-", "+", "2", "1"};
    switch (aurora_button_icon_style()) {
    case 0: return {"LB", "RB", "A", "B"};
    case 1: return {"L1", "R1", "CROSS", "CIRCLE"};
    case 2: return {"L", "R", "B", "A"};  // SDL reads pads by position: the Nintendo B sits where the Xbox A does
    default: return {"Q", "E", "X", "ESC"};
    }
}

void DrawOptionsMenu() {
    if (!g_optOpen) return;
    using namespace HudTheme;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const GameLayout::View view = GameLayout::GameView();
    const float k = view.scale;
    const float t = std::chrono::duration<float>(Clock::now() - g_optOpenedAt).count();
    const float pop = std::min(1.0f, t * 6.0f);  // the panel opens out from its middle in a sixth of a second
    // Layout units (y up) around the game picture's centre, as the game's own menus are placed.
    auto S = [&](float x, float y) { return ImVec2(view.centerX + x * k, view.centerY - y * k * pop); };
    auto Txt = [&](const std::string& s, float x, float y, float size, ImU32 col, float ax = 0.0f, float tracking = 0.0f,
                   ImU32 glow = 0) {
        HudTheme::Text(list, S(x, y), size * k, Alpha(col, int(((col >> IM_COL32_A_SHIFT) & 0xFF) * pop)), s.c_str(), ax,
                       0.5f, tracking * k, glow ? Alpha(glow, int(((glow >> IM_COL32_A_SHIFT) & 0xFF) * pop)) : 0);
    };
    // Text no wider than `maxW` layout units: the size shrinks to fit.
    auto Fit = [&](const std::string& s, float size, float maxW, float tracking = 0.0f) {
        const float w = Measure(size * k, s.c_str(), tracking * k).x / k;
        return w > maxW ? size * maxW / w : size;
    };
    // A slanted plate (tabs, buttons): `slant` layout units of lean.
    auto Plate = [&](float x0, float y0, float x1, float y1, float slant, ImU32 fill, ImU32 border, float thick) {
        const ImVec2 pts[4] = {S(x0 + slant, y0), S(x1, y0), S(x1 - slant, y1), S(x0, y1)};
        list->AddConvexPolyFilled(pts, 4, fill);
        if (border) list->AddPolyline(pts, 4, border, ImDrawFlags_Closed, thick * k);
    };
    auto Chevron = [&](float x, float y, float size, bool right, ImU32 col, float thick) {
        const float d = right ? size * 0.5f : -size * 0.5f;
        const ImVec2 pts[3] = {S(x - d, y + size * 0.6f), S(x + d, y), S(x - d, y - size * 0.6f)};
        list->AddPolyline(pts, 3, col, ImDrawFlags_None, thick * k);
    };
    // A box the mouse can click (OptionsMouseEvents), from two corners in layout units.
    g_optHits.clear();
    auto Hit = [&](float x0, float y0, float x1, float y1, OptHit what, int index) {
        const ImVec2 p = S(x0, y0), q = S(x1, y1);
        g_optHits.push_back({ImVec2(std::min(p.x, q.x), std::min(p.y, q.y)), ImVec2(std::max(p.x, q.x), std::max(p.y, q.y)), what,
                             index});
    };

    // The game behind, dimmed through the visor's scan lines.
    const ImVec2 vp0 = viewport->Pos, vp1(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y);
    list->AddRectFilled(vp0, vp1, IM_COL32(1, 6, 16, 170));
    Scanlines(list, vp0, vp1, std::max(3.0f, 1.6f * k), IM_COL32(0, 0, 0, 45));

    const float hw = 380.0f, top = 204.0f, bottom = -214.0f;
    const ImVec2 pa = S(-hw, top), pb = S(hw, bottom);
    Panel(list, pa, pb, 26.0f * k, kPanelTop, kPanelBottom, Alpha(kCyan, 210), 1.6f * k);
    Scanlines(list, ImVec2(pa.x + 2, pa.y + 2), ImVec2(pb.x - 2, pb.y - 2), 3.0f * k, IM_COL32(80, 220, 255, 10));
    CornerBrackets(list, pa, pb, 30.0f * k, kOrange, 2.2f * k);
    // A sweep line drifting down the panel.
    {
        const float span = top - bottom;
        const float sy = top - std::fmod(t * 70.0f, span + 60.0f) + 30.0f;
        if (sy < top - 4.0f && sy > bottom + 4.0f) {
            list->AddRectFilledMultiColor(S(-hw + 2, sy + 10), S(hw - 2, sy), IM_COL32(80, 220, 255, 0),
                                          IM_COL32(80, 220, 255, 0), IM_COL32(80, 220, 255, 22), IM_COL32(80, 220, 255, 22));
            list->AddLine(S(-hw + 2, sy), S(hw - 2, sy), IM_COL32(80, 220, 255, 40), 1.0f);
        }
    }

    // Header: the emblem, the title, the game's name.
    const float headY = top - 32.0f;
    Emblem(list, S(-hw + 52, headY), 46.0f * k, Alpha(kCyan, int(235 * pop)));
    // The start screen names its page (START, GAME FILES, SETTING UP) and the port's version.
    const char* title = !g_launcher.active ? "OPTIONS"
                        : g_launcher.screen == LauncherScreen::Browse    ? "GAME FILES"
                        : g_launcher.screen == LauncherScreen::SettingUp ? "SETTING UP"
                        : g_optTab == 0                                  ? "START"
                                                                         : "OPTIONS";
    Txt(title, -hw + 88, headY + 1, 32.0f, kText, 0.0f, 6.0f, IM_COL32(80, 220, 255, 70));
    Txt("METROID: OTHER M", hw - 28, headY + 8, 13.0f, kCyan, 1.0f, 3.0f);
    const std::string subtitle = !g_launcher.active ? std::string("SYSTEM CONFIGURATION")
                                 : Updater::Current().empty() ? std::string("RECOMPILED")
                                                              : "RECOMPILED  " + Updater::Current();
    Txt(subtitle, hw - 28, headY - 10, 11.0f, kTextDim, 1.0f, 2.5f);
    // Divider with tick marks; brighter under the title.
    const float divY = top - 64.0f;
    list->AddLine(S(-hw + 18, divY), S(hw - 18, divY), Alpha(kCyan, 90), 1.0f * k);
    list->AddLine(S(-hw + 18, divY), S(-hw + 250, divY), Alpha(kCyan, 230), 2.0f * k);
    for (float x = -hw + 270; x < hw - 20; x += 22.0f) list->AddLine(S(x, divY), S(x, divY - 5), Alpha(kCyan, 70), 1.0f);

    // Tabs: slanted plates between the two buttons that turn the page (LB / RB on an Xbox pad).
    const OptHints hints = CurrentHints();
    const float badgeW = 44.0f, tabY = top - 92.0f;
    const float stripX0 = -hw + 24 + badgeW + 8, stripX1 = hw - 24 - badgeW - 8;
    const int pages = PageCount();
    const float tabW = (stripX1 - stripX0) / pages;
    auto Upper = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return text;
    };
    auto Badge = [&](float cx, const char* label) {
        const ImVec2 a = S(cx - badgeW * 0.5f, tabY + 13), b = S(cx + badgeW * 0.5f, tabY - 13);
        list->AddRectFilled(a, b, IM_COL32(14, 40, 66, 230), 7.0f * k);
        list->AddRect(a, b, Alpha(kCyan, 170), 7.0f * k, 0, 1.4f * k);
        Txt(label, cx, tabY, Fit(label, 13.0f, badgeW - 12.0f, 1.0f), kText, 0.5f, 1.0f);
    };
    if (pages > 1) {  // the start screen's browser and set-up are one page each: nothing to turn
        Badge(-hw + 24 + badgeW * 0.5f, hints.prev);
        Badge(hw - 24 - badgeW * 0.5f, hints.next);
        Hit(-hw + 24, tabY + 16, -hw + 24 + badgeW, tabY - 16, OptHit::PagePrev, 0);
        Hit(hw - 24 - badgeW, tabY + 16, hw - 24, tabY - 16, OptHit::PageNext, 0);
    }
    float tabSize = 17.0f;  // one size for every tab: the longest name decides
    for (int i = 0; i < pages; ++i) tabSize = std::min(tabSize, Fit(Upper(PageName(i)), 17.0f, tabW - 20.0f, 1.5f));
    for (int i = 0; i < pages; ++i) {
        const float x0 = stripX0 + i * tabW;
        const bool sel = i == g_optTab;
        Plate(x0 + 2, tabY + 16, x0 + tabW - 2, tabY - 16, 7.0f, sel ? IM_COL32(80, 220, 255, 70) : IM_COL32(14, 40, 66, 200),
              sel ? kOrange : Alpha(kCyan, 80), sel ? 2.2f : 1.2f);
        Txt(Upper(PageName(i)), x0 + tabW * 0.5f, tabY, tabSize, sel ? kText : kTextDim, 0.5f, 1.5f,
            sel ? IM_COL32(80, 220, 255, 60) : 0);
        if (sel) list->AddRectFilled(S(x0 + 14, tabY - 20), S(x0 + tabW - 14, tabY - 22.5f), kOrange);
        Hit(x0 + 2, tabY + 16, x0 + tabW - 2, tabY - 16, OptHit::Tab, i);
    }

    const std::vector<OptRow> rows = PageRows(g_optTab);
    // A page with a table (Controls) names its columns under the tabs; its rows start a little lower.
    const bool hasTable = std::any_of(rows.begin(), rows.end(), [](const OptRow& r) { return !r.cells.empty(); });
    const float rowH = 32.0f, rowsTop = tabY - (hasTable ? 62.0f : 44.0f), rowsBottom = bottom + 74.0f;
    // Table columns, in layout units: the controller cell, then the keyboard cell.
    constexpr float kCellX[2][2] = {{-26.0f, 136.0f}, {146.0f, 352.0f}};
    if (hasTable) {
        const float headY = tabY - 37.0f;
        Txt("ACTION", -hw + 56, headY, 11.5f, kTextDim, 0.0f, 2.0f);
        Txt("CONTROLLER", (kCellX[0][0] + kCellX[0][1]) * 0.5f, headY, 11.5f, kCyan, 0.5f, 2.0f);
        Txt("KEYBOARD", (kCellX[1][0] + kCellX[1][1]) * 0.5f, headY, 11.5f, kCyan, 0.5f, 2.0f);
    }
    const int visible = std::max(1, int((rowsTop - rowsBottom) / rowH) + 1);
    int& first = g_optFirst;  // the first row shown: the list scrolls to keep the selected row in view
    if (g_optRow < first) first = g_optRow;
    if (g_optRow >= first + visible) first = g_optRow - visible + 1;
    first = std::clamp(first, 0, std::max(0, int(rows.size()) - visible));
    float y = rowsTop;
    for (int i = first; i < int(rows.size()) && i < first + visible; ++i, y -= rowH) {
        const OptRow& row = rows[static_cast<size_t>(i)];
        const bool sel = i == g_optRow;
        const bool tableRow = !row.cells.empty();
        const bool action = row.value.empty() && !tableRow;
        const bool info = !row.step;  // only shows something: no arrows
        Hit(-hw + 20, y + 15, hw - 20, y - 15, OptHit::Row, i);
        if (!info && !action && !tableRow && !row.button) {
            Hit(hw - 272, y + 15, hw - 228, y - 15, OptHit::RowLeft, i);
            Hit(hw - 52, y + 15, hw - 12, y - 15, OptHit::RowRight, i);
        }
        if (sel) {
            // A bar of light fading to the right, an orange edge and a chevron cursor.
            list->AddRectFilledMultiColor(S(-hw + 20, y + 14), S(hw - 20, y - 14), IM_COL32(80, 220, 255, 75),
                                          IM_COL32(80, 220, 255, 8), IM_COL32(80, 220, 255, 8), IM_COL32(80, 220, 255, 75));
            list->AddLine(S(-hw + 20, y - 14), S(hw - 20, y - 14), Alpha(kCyan, 120), 1.0f);
            list->AddRectFilled(S(-hw + 20, y + 14), S(-hw + 24, y - 14), kOrange);
            const float bob = std::sin(t * 7.0f) * 2.5f;
            Chevron(-hw + 38 + bob, y, 12.0f, true, kOrange, 2.4f);
        } else {
            list->AddLine(S(-hw + 30, y - 15), S(hw - 30, y - 15), IM_COL32(80, 220, 255, 22), 1.0f);
        }
        const float labelW = tableRow ? kCellX[0][0] - (-hw + 56) - 10.0f : action ? 2 * hw - 90.0f : 2 * hw - 330.0f;
        const ImU32 labelCol = sel ? IM_COL32(255, 255, 255, 255) : row.color ? row.color : action ? kAmber : kText;
        Txt(row.label, -hw + 56, y, Fit(row.label, 19.0f, labelW), labelCol, 0.0f, 0.0f, sel ? IM_COL32(80, 220, 255, 50) : 0);
        if (tableRow) {
            // Two cells; the picked one (left / right) is a lit plate. A fixed cell is dim and has no plate.
            for (size_t c = 0; c < row.cells.size() && c < 2; ++c) {
                const OptCell& cell = row.cells[c];
                const float x0 = kCellX[c][0], x1 = kCellX[c][1];
                const bool picked = sel && g_optCol == int(c);
                if (picked)
                    Plate(x0, y + 13, x1, y - 13, 6.0f, cell.pick ? IM_COL32(80, 220, 255, 70) : IM_COL32(80, 220, 255, 25),
                          cell.pick ? kOrange : Alpha(kCyan, 90), cell.pick ? 2.0f : 1.0f);
                else if (cell.pick)
                    Plate(x0, y + 13, x1, y - 13, 6.0f, IM_COL32(14, 40, 66, sel ? 170 : 110), Alpha(kCyan, sel ? 90 : 40), 1.0f);
                Txt(cell.text, (x0 + x1) * 0.5f, y, Fit(cell.text, 17.0f, x1 - x0 - 16.0f),
                    !cell.pick ? kTextDim : picked ? kText : Alpha(kCyan, 220), 0.5f);
                Hit(x0, y + 15, x1, y - 15, OptHit::Cell, i * 2 + int(c));
            }
        } else if (row.slider >= 0 && row.sliderMax > 0) {
            // A bar from 0 to the maximum, its fill and a knob; the value beside it. 200% bars mark 100%.
            const float bx0 = hw - 236.0f, bx1 = hw - 112.0f;
            const float f = std::clamp(float(row.slider) / float(row.sliderMax), 0.0f, 1.0f);
            const float fx = bx0 + (bx1 - bx0) * f;
            list->AddRectFilled(S(bx0, y + 3), S(bx1, y - 3), IM_COL32(14, 40, 66, 230), 3.0f * k);
            list->AddRect(S(bx0, y + 3), S(bx1, y - 3), Alpha(kCyan, sel ? 120 : 60), 3.0f * k, 0, 1.0f);
            if (f > 0.0f) list->AddRectFilled(S(bx0, y + 3), S(fx, y - 3), sel ? kCyan : Alpha(kCyan, 170), 3.0f * k);
            if (row.sliderMax == 200) list->AddLine(S((bx0 + bx1) * 0.5f, y + 7), S((bx0 + bx1) * 0.5f, y - 7), Alpha(kText, 120), 1.0f);
            list->AddCircleFilled(S(fx, y), 6.5f * k, sel ? kOrange : Alpha(kCyan, 230));
            Txt(row.value, hw - 72, y, Fit(row.value, 17.0f, 62.0f), sel ? kCyan : Alpha(kCyan, 200), 0.5f);
            if (!info) {  // a bar that only shows (the start screen's set-up) has no arrows and no mouse handle
                Hit(bx0, y + 12, bx1, y - 12, OptHit::Slider, i);
                const ImU32 arrow = sel ? kCyan : Alpha(kCyan, 70);
                Chevron(hw - 250, y, 11.0f, false, arrow, 2.0f);
                Chevron(hw - 30, y, 11.0f, true, arrow, 2.0f);
            }
        } else if (!action) {
            Txt(row.value, hw - 140, y, Fit(row.value, 19.0f, info || row.button ? 236.0f : 176.0f),
                info ? kTextDim : sel ? kCyan : Alpha(kCyan, 200), 0.5f, 0.0f, sel && !info ? IM_COL32(80, 220, 255, 60) : 0);
            if (!info && !row.button) {
                const ImU32 arrow = sel ? kCyan : Alpha(kCyan, 70);
                Chevron(hw - 250, y, 11.0f, false, arrow, 2.0f);
                Chevron(hw - 30, y, 11.0f, true, arrow, 2.0f);
            }
        }
    }
    // More rows above or below.
    const ImU32 more = Alpha(kCyan, 150);
    if (first > 0) list->AddTriangleFilled(S(-7, rowsTop + 17), S(7, rowsTop + 17), S(0, rowsTop + 24), more);
    if (first + visible < int(rows.size()))
        list->AddTriangleFilled(S(-8, y + rowH - 22), S(8, y + rowH - 22), S(0, y + rowH - 31), more);

    // What the selected row does, and the buttons.
    const float footY = bottom + 58.0f;
    list->AddLine(S(-hw + 18, footY + 10), S(hw - 18, footY + 10), Alpha(kCyan, 60), 1.0f);
    const OptRow* selRow = g_optRow >= 0 && g_optRow < int(rows.size()) ? &rows[static_cast<size_t>(g_optRow)] : nullptr;
    const std::string help = selRow ? selRow->help : "";
    if (!selRow || selRow->warn.empty()) {
        Txt(help, 0, footY - 8, Fit(help, 15.0f, 2 * hw - 48.0f), kText, 0.5f);
    } else {
        const std::string lead = help.empty() ? std::string() : help + " ";
        const float size = Fit(lead + selRow->warn, 15.0f, 2 * hw - 48.0f);
        const float leadW = Measure(size * k, lead.c_str()).x / k;
        const float left = -(leadW + Measure(size * k, selRow->warn.c_str()).x / k) * 0.5f;
        Txt(lead, left, footY - 8, size, kText);
        Txt(selRow->warn, left + leadW, footY - 8, size, kAmber);
    }
    std::string hint = std::string(hints.prev) + "/" + hints.next + "  PAGE     UP/DOWN  CHOOSE     LEFT/RIGHT  CHANGE     " +
                       hints.select + "  SELECT     " + hints.close + "  CLOSE";
    if (g_launcher.active) {
        switch (g_launcher.screen) {
        case LauncherScreen::Start:
            hint = g_optTab == 0 ? std::string(hints.prev) + "/" + hints.next + "  OPTIONS     UP/DOWN  CHOOSE     " + hints.select + "  SELECT"
                                 : std::string(hints.prev) + "/" + hints.next + "  PAGE     UP/DOWN  CHOOSE     LEFT/RIGHT  CHANGE     " +
                                       hints.select + "  SELECT     " + hints.close + "  START";
            break;
        case LauncherScreen::Browse:
            hint = std::string("UP/DOWN  CHOOSE     ") + hints.select + "  OPEN     " + hints.close +
                   (g_launcher.folder.empty() ? "  START" : "  UP ONE FOLDER");
            break;
        case LauncherScreen::SettingUp: hint = "PLEASE WAIT"; break;
        }
    }
    Txt(hint, 0, bottom + 22.0f, Fit(hint, 11.5f, 2 * hw - 60.0f, 1.5f), kTextDim, 0.5f, 1.5f);

    if (g_optPrompt) {
        // Opaque: ImGui blends in linear light here, so even a few percent of the rows behind would show through.
        list->AddRectFilled(vp0, vp1, IM_COL32(0, 0, 0, 120));
        const bool note = !g_optPrompt->note.empty();
        const float pw = note ? 270.0f : 215.0f;
        const ImVec2 qa = S(-pw, 78), qb = S(pw, -78);
        Panel(list, qa, qb, 18.0f * k, IM_COL32(14, 22, 40, 255), IM_COL32(6, 10, 22, 255), kOrange, 1.8f * k);
        list->AddRectFilled(S(-pw + 10, 70), S(pw - 10, 64), IM_COL32(255, 150, 40, 160));  // a warning stripe
        Txt(g_optPrompt->title, 0, note ? 40 : 34, Fit(g_optPrompt->title, 24.0f, 2 * pw - 40.0f, 4.0f), kText, 0.5f, 4.0f,
            IM_COL32(255, 150, 40, 60));
        if (note) Txt(g_optPrompt->note, 0, 12, Fit(g_optPrompt->note, 14.0f, 2 * pw - 40.0f), kTextDim, 0.5f);
        for (int i = 0; i < 2; ++i) {
            const float cx = i == 0 ? -95.0f : 95.0f;
            const bool sel = g_optPromptChoice == i;
            Plate(cx - 75, -18, cx + 75, -54, 8.0f, sel ? IM_COL32(255, 150, 40, 90) : IM_COL32(14, 40, 66, 200),
                  sel ? kOrange : Alpha(kCyan, 90), sel ? 2.2f : 1.2f);
            Txt(i == 0 ? g_optPrompt->confirm : std::string("CANCEL"), cx, -36, 18.0f, sel ? kText : kTextDim, 0.5f, 3.0f);
            Hit(cx - 75, -18, cx + 75, -54, OptHit::Choice, i);
        }
    }

    if (g_optChordCapture >= 0) {
        // Save / load state buttons or keys (PollChordCapture): what is held and the seconds left, 5 to 1, with a bar
        // that fills as they run out.
        list->AddRectFilled(vp0, vp1, IM_COL32(0, 0, 0, 120));
        const float pw = 260.0f;
        Panel(list, S(-pw, 96), S(pw, -96), 18.0f * k, IM_COL32(14, 22, 40, 255), IM_COL32(6, 10, 22, 255), kCyan, 1.8f * k);
        list->AddRectFilled(S(-pw + 10, 88), S(pw - 10, 82), Alpha(kCyan, 160));
        const std::string title = std::string(g_optChordCapture == 0 ? "SAVE STATE" : "LOAD STATE") +
                                  (g_optChordKeys ? "  -  KEYBOARD" : "  -  CONTROLLER");
        Txt(title, 0, 62, Fit(title, 22.0f, 2 * pw - 40.0f, 4.0f), kText, 0.5f, 4.0f, IM_COL32(80, 220, 255, 60));
        if (g_optChordCounting) {
            const std::string held = ChordLabel(g_optChordHeld);
            Txt(held, 0, 32, Fit(held, 20.0f, 2 * pw - 40.0f), kCyan, 0.5f);
            const std::string seconds = std::to_string(ChordSecondsLeft());
            Txt(seconds, 0, -12, 54.0f, kText, 0.5f, 0.0f, IM_COL32(255, 150, 40, 110));
            const float f = std::clamp(std::chrono::duration<float>(Clock::now() - g_optChordSince).count() /
                                           std::chrono::duration<float>(kChordHold).count(),
                                       0.0f, 1.0f);
            const float bx0 = -pw + 50.0f, bx1 = pw - 50.0f, by = -50.0f;
            list->AddRectFilled(S(bx0, by + 3), S(bx1, by - 3), IM_COL32(14, 40, 66, 230), 3.0f * k);
            if (f > 0.0f) list->AddRectFilled(S(bx0, by + 3), S(bx0 + (bx1 - bx0) * f, by - 3), kOrange, 3.0f * k);
        } else {
            Txt(g_optChordKeys ? "Press and hold the keys together." : "Press and hold the buttons together.", 0, 22, 17.0f, kCyan,
                0.5f);
            Txt("Whatever is held when the count of 5 ends is the shortcut.", 0, -6,
                Fit("Whatever is held when the count of 5 ends is the shortcut.", 14.0f, 2 * pw - 40.0f), kTextDim, 0.5f);
        }
        const std::string foot = "LET GO TO START OVER     ESC  KEEP THE OLD ONES     BACKSPACE  NONE";
        Txt(foot, 0, -76, Fit(foot, 11.0f, 2 * pw - 40.0f, 1.5f), kTextDim, 0.5f, 1.5f);
    }
}
} // namespace

void FilterMenuInput(uint32_t chan, WiiRemoteInput::KpadSample& sample) noexcept {
    if (chan != 0) return;
    constexpr uint32_t kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kPlus = 0x0010, kTwo = 0x0100,
                       kOne = 0x0200, kB = 0x0400, kA = 0x0800, kMinus = 0x1000, kHome = 0x8000;
    const uint32_t hold = sample.hold;
    const uint32_t pressed = hold & ~g_optPrevHold;
    g_optPrevHold = hold;
    // Buttons that closed the menu stay away from the game until they are let go.
    g_optSuppress &= hold;
    // The game's own HOME Menu never opens: Options takes its place. Not while a save / load state chord is being held
    // in Options > Controls: View + Menu there are buttons of the chord.
    if ((pressed & kHome) && g_optChordCapture < 0) RequestOptionsToggle();
    if (pressed & kHome) g_optSuppress |= kHome;
    if (g_optOpen) {
        const bool upright = sample.hasNunchuk;
        const uint32_t dirs[4] = {upright ? kLeft : kUp, upright ? kRight : kDown, upright ? kUp : kRight,
                                  upright ? kDown : kLeft};
        static constexpr uint32_t kEv[4] = {kOptLeft, kOptRight, kOptUp, kOptDown};
        uint32_t ev = 0;
        const auto now = Clock::now();
        for (int i = 0; i < 4; ++i) {
            if (pressed & dirs[i]) {
                ev |= kEv[i];
                g_optRepeatAt[i] = now + std::chrono::milliseconds(400);
            } else if ((hold & dirs[i]) && now >= g_optRepeatAt[i]) {
                ev |= kEv[i];
                g_optRepeatAt[i] = now + std::chrono::milliseconds(90);
            }
        }
        if (pressed & (kTwo | kA)) ev |= kOptConfirm;
        if (pressed & (kOne | kB)) ev |= kOptBack;
        // - and + turn the page on a real remote; a gamepad has LB / RB (its Back and Start are - and + here too).
        if (RealRemoteIsPlayerOne()) {
            if (pressed & kMinus) ev |= kOptPagePrev;
            if (pressed & kPlus) ev |= kOptPageNext;
        }
        g_optEvents |= ev;
        g_optSuppress |= hold;
    }
    sample.hold &= ~g_optSuppress;
}

// Tests: OTHERM_TEST_PAD_EVENTS="<frame>:<lb|rb|a|start|q|e|f1|f+|f-|dc>,..." posts a controller button or a key press (down,
// then up) at that presented frame, for the Options page buttons. Posted, because SDL drops a background window's
// controller events (run-otherm.ps1 keeps the test window in the background), so a virtual pad would not get through.
void PostTestEvents() {
    // OTHERM_KEY_SCRIPT (keybinds.h): the scripted keys' presses and releases, on the frame count Keybinds reads them by.
    {
        static uint32_t s_lastKeyFrame = 0;
        const uint32_t now = VI_HLE_PresentedFrames();
        for (uint32_t frame = s_lastKeyFrame + 1; frame <= now && s_lastKeyFrame != 0; ++frame) {
            for (const auto& [code, down] : Keybinds::ScriptedKeyEvents(frame)) {
                SDL_Event event{};
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.scancode = static_cast<SDL_Scancode>(code);
                event.key.down = down;
                SDL_PushEvent(&event);
                RT_LOGF(RT_TAG_RUNTIME, "test: key %s %s at frame %u\n", SDL_GetScancodeName(static_cast<SDL_Scancode>(code)),
                        down ? "down" : "up", frame);
            }
        }
        s_lastKeyFrame = std::max(now, 1u);
    }
    struct Press { uint32_t frame; std::string what; };
    static const std::vector<Press> presses = [] {
        std::vector<Press> out;
        const char* spec = std::getenv("OTHERM_TEST_PAD_EVENTS");
        for (std::string_view text = spec ? spec : ""; !text.empty();) {
            const size_t comma = text.find(',');
            const std::string_view item = text.substr(0, comma);
            text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
            const size_t colon = item.find(':');
            if (colon == std::string_view::npos) continue;
            uint32_t frame = 0;
            std::from_chars(item.data(), item.data() + colon, frame);
            out.push_back({frame, std::string(item.substr(colon + 1))});
        }
        return out;
    }();
    for (const Press& press : presses) {
        if (press.frame != g_presentedFrame) continue;
        // "mv:x:y" moves the mouse to window pixel x, y; "lc:x:y" moves there and left-clicks; "rc" right-clicks; "wu" /
        // "wd" turn the wheel up / down. ImGui drops mouse events of other windows, so they carry the game window's ID.
        if (press.what.rfind("mv:", 0) == 0 || press.what.rfind("lc:", 0) == 0 || press.what == "rc" || press.what == "wu" ||
            press.what == "wd") {
            int windows = 0;
            SDL_Window** list = SDL_GetWindows(&windows);
            const SDL_WindowID window = list != nullptr && windows > 0 ? SDL_GetWindowID(list[0]) : 0;
            SDL_free(list);
            float x = g_optLastMouse.x, y = g_optLastMouse.y;
            if (press.what.size() > 3 && press.what[2] == ':') std::sscanf(press.what.c_str() + 3, "%f:%f", &x, &y);
            SDL_Event event{};
            if (press.what == "wu" || press.what == "wd") {
                event.type = SDL_EVENT_MOUSE_WHEEL;
                event.wheel.windowID = window;
                event.wheel.y = press.what == "wu" ? 1.0f : -1.0f;
                event.wheel.mouse_x = x;
                event.wheel.mouse_y = y;
                SDL_PushEvent(&event);
            } else {
                event.type = SDL_EVENT_MOUSE_MOTION;
                event.motion.windowID = window;
                event.motion.x = x;
                event.motion.y = y;
                SDL_PushEvent(&event);
                if (press.what != "mv" && press.what.rfind("mv:", 0) != 0) {
                    for (const bool down : {true, false}) {
                        SDL_Event button{};
                        button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                        button.button.windowID = window;
                        button.button.button = press.what == "rc" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                        button.button.down = down;
                        button.button.clicks = 1;
                        button.button.x = x;
                        button.button.y = y;
                        SDL_PushEvent(&button);
                    }
                }
            }
            RT_LOGF(RT_TAG_RUNTIME, "test: posted %s at frame %u (window %u)\n", press.what.c_str(), press.frame, unsigned(window));
            continue;
        }
        // "dc": a device interface arrival as Windows announces one, sent to SDL's hidden device windows (its joystick
        // thread's and HIDAPI's), so a test can start the controller re-scan a real device change starts.
        if (press.what == "dc") {
#if defined(_WIN32)
            static const wchar_t kPath[] = L"\\\\?\\HID#VID_0000&PID_0000#test#{4d1e55b2-f16f-11cf-88cb-001111000030}";
            alignas(DEV_BROADCAST_DEVICEINTERFACE_W) unsigned char buffer[sizeof(DEV_BROADCAST_DEVICEINTERFACE_W) + sizeof(kPath)] = {};
            auto* arrival = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(buffer);
            arrival->dbcc_size = sizeof(buffer);
            arrival->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            arrival->dbcc_classguid = {0x4D1E55B2L, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
            std::memcpy(arrival->dbcc_name, kPath, sizeof(kPath));
            int sent = 0;
            for (const wchar_t* className : {L"Message", L"SDL_HIDAPI_DEVICE_DETECTION"}) {
                for (HWND window = FindWindowExW(HWND_MESSAGE, nullptr, className, nullptr); window != nullptr;
                     window = FindWindowExW(HWND_MESSAGE, window, className, nullptr)) {
                    DWORD process = 0;
                    GetWindowThreadProcessId(window, &process);
                    if (process != GetCurrentProcessId()) continue;
                    DWORD_PTR result = 0;
                    SendMessageTimeoutW(window, WM_DEVICECHANGE, DBT_DEVICEARRIVAL, reinterpret_cast<LPARAM>(arrival),
                                        SMTO_ABORTIFHUNG, 1000, &result);
                    ++sent;
                }
            }
            RT_LOGF(RT_TAG_RUNTIME, "test: device arrival sent to %d SDL window(s) at frame %u\n", sent, press.frame);
#endif
            continue;
        }
        // "f+" / "f-": the fast-forward key only pressed / only let go (it is held in between).
        if (press.what == "f+" || press.what == "f-") {
            SDL_Event event{};
            event.type = press.what == "f+" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.scancode = SDL_SCANCODE_F;
            event.key.down = press.what == "f+";
            SDL_PushEvent(&event);
            RT_LOGF(RT_TAG_RUNTIME, "test: posted %s at frame %u\n", press.what.c_str(), press.frame);
            continue;
        }
        for (const bool down : {true, false}) {
            SDL_Event event{};
            if (press.what == "lb" || press.what == "rb" || press.what == "a" || press.what == "start") {
                // "a" / "start": the controller's A / Start (the cutscene skip; settings_overlay HandleEvents).
                event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
                event.gbutton.button = press.what == "lb"   ? SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
                                       : press.what == "rb" ? SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
                                       : press.what == "a"  ? SDL_GAMEPAD_BUTTON_SOUTH
                                                            : SDL_GAMEPAD_BUTTON_START;
                event.gbutton.down = down;
            } else if (press.what == "q" || press.what == "e" || press.what == "f1") {
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.scancode = press.what == "q" ? SDL_SCANCODE_Q : press.what == "e" ? SDL_SCANCODE_E : SDL_SCANCODE_F1;
                event.key.down = down;
            } else {
                continue;
            }
            SDL_PushEvent(&event);
        }
        RT_LOGF(RT_TAG_RUNTIME, "test: posted %s at frame %u\n", press.what.c_str(), press.frame);
    }
}

// Tests: OTHERM_TEST_FIND_TEXT="<frame>:<text>" logs where guest memory holds that text at that presented frame (MEM1 and
// MEM2), e.g. to see a rewritten message (otherm_messages.cpp) loaded.
void FindTestText() {
    static const char* spec = std::getenv("OTHERM_TEST_FIND_TEXT");
    if (spec == nullptr) return;
    static const uint32_t frame = static_cast<uint32_t>(std::strtoul(spec, nullptr, 10));
    static bool done = false;
    if (done || g_presentedFrame < frame) return;
    done = true;
    const char* colon = std::strchr(spec, ':');
    const std::string text = colon != nullptr ? colon + 1 : "";
    int found = 0;
    for (const auto& [base, size] : {std::pair<uint32_t, uint32_t>{0x80000000u, 0x01800000u}, {0x90000000u, 0x04000000u}}) {
        const uint8_t* mem = nullptr;
        try {
            mem = static_cast<const uint8_t*>(Memory::GetPointer(base, size));
        } catch (const Memory::AccessViolation&) {
            mem = nullptr;
        }
        if (mem == nullptr || text.empty()) continue;
        const std::string_view view(reinterpret_cast<const char*>(mem), size);
        for (size_t at = view.find(text); at != std::string_view::npos && found < 8; at = view.find(text, at + 1), ++found)
            RT_LOGF(RT_TAG_RUNTIME, "test: \"%s\" at 0x%08X\n", text.c_str(), base + static_cast<uint32_t>(at));
    }
    RT_LOGF(RT_TAG_RUNTIME, "test: \"%s\" found %d time(s) at frame %u\n", text.c_str(), found, frame);
}

void Draw() noexcept {
    PostTestEvents();
    FindTestText();
    static const bool themed = (HudTheme::ApplyImGuiStyle(), true);
    (void)themed;
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    RebindSelfTest();
    if (g_optToggleRequest.exchange(false) && !g_launcher.active) SetOptionsOpen(!g_optOpen);
    if (g_optOpenRequest.exchange(false) && !g_launcher.active && !StartupScreenVisible()) SetOptionsOpen(true);
    if (g_launcher.active) LauncherUpdate();
    // Test runs: OTHERM_TEST_OPTIONS=<page>[,<row>][,quit] opens Options on that page (0 Game, 1 Display, 2 Sound,
    // 3 Controls, 4 Textures, 5 Saves, 6 Cheats, 7 Mods) and row at presented frame 600, with the quit prompt up for
    // "quit": screenshots of the menu without scripted presses.
    static const char* testOptions = std::getenv("OTHERM_TEST_OPTIONS");
    if (testOptions != nullptr && g_presentedFrame == 600) {
        SetOptionsOpen(true);
        g_optTab = std::clamp(std::atoi(testOptions), 0, kOptTabCount - 1);
        if (const char* comma = std::strchr(testOptions, ','); comma != nullptr && std::isdigit(static_cast<unsigned char>(comma[1])))
            g_optRow = std::atoi(comma + 1);
        if (std::strstr(testOptions, "quit") != nullptr) g_optPrompt = OptPrompt{"QUIT TO DESKTOP?", "QUIT", "", {}};
    }
    PollStateHotkeys();
    if (g_optOpen) HandleOptionsInput();
    UpdateGameSpeed();
    DrawFpsOverlay();
    controller_mapping_wizard::Draw();
    ApplyInputBlockState();
    DrawOptionsMenu();
    DrawStateNotice();
    DrawStartupScreen();
}

bool GamePauseRequested() noexcept { return g_optOpen && !StartupScreenVisible(); }

void BeginLauncher() noexcept {
    g_launcher = LauncherState{};
    g_launcher.active = true;
    SetOptionsOpen(true);
    LauncherShowStart();
    RT_LOGF(RT_TAG_RUNTIME, "[launcher] start screen up; game files %s\n", g_launcher.ready ? "ready" : "not set up");
}

bool LauncherActive() noexcept { return g_launcher.active; }

bool LauncherSettingUp() noexcept { return g_launcher.active && g_launcher.screen == LauncherScreen::SettingUp; }

void PlayWhenModsInstalled() noexcept { g_launcherPlaysAfterInstall = true; }

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           !g_bootShadersReady.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept {
    ++g_presentedFrame;
    UpdateGpuLimit();
    // Options, the start screen and fast-forward (or faster menus) say nothing about the load of normal play.
    DynamicResolution::OnPresentedFrame(g_optOpen || StartupScreenVisible() || g_appliedSpeed > 1);
}
} // namespace settings_overlay
