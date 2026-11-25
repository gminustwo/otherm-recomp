#include "virtual_remote.h"

#include "button_icons.h"
#include "controller_gate.h"
#include "display_settings.h"
#include "hle_stubs.h"
#include "input_bindings.h"
#include "keybinds.h"
#include "otherm_move.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "save_state.h"
#include "wii_remote_input.h"
#include "wiimote_profile.h"

#include <aurora/aurora.h>

#include <dolphin/pad.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace VirtualRemote {
namespace {

using WiimoteProfile::kWpadA;
using WiimoteProfile::kWpadDown;
using WiimoteProfile::kWpadLeft;
using WiimoteProfile::kWpadMinus;
using WiimoteProfile::kWpadOne;
using WiimoteProfile::kWpadPlus;
using WiimoteProfile::kWpadRight;
using WiimoteProfile::kWpadTwo;
using WiimoteProfile::kWpadUp;

// The remote's D-pad by screen direction. Held sideways with the D-pad under the left thumb, the
// pad's "up" arrow points left, "right" points up, "down" points right and "left" points down.
constexpr uint32_t kScreenLeft = kWpadUp, kScreenRight = kWpadDown, kScreenUp = kWpadRight,
                   kScreenDown = kWpadLeft;

// A shake is one flick added to rest acc.y: a quick dip, a steep rise, back to rest (not tuned for Other M:
// its own shake check has not been looked at yet).
constexpr float kShakeFlickG[] = {-1.6f, -4.4f, -1.2f, 0.8f, 2.8f, 4.8f, 4.8f, 2.4f, 0.0f};
constexpr int kShakeFrames = static_cast<int>(std::size(kShakeFlickG));
constexpr float kMaxRollRadians = 0.7853982f;  // keyboard / touch tilt: 45 degrees
// Dolphin turns its emulated remote at up to 7 revolutions per second, so a full tilt takes two or three frames;
// moving half the remaining way each frame gets there in about as long.
constexpr float kAttitudeFollow = 0.5f;
constexpr int kStickThreshold = 48;            // of aurora's +-127 stick range (players 2-4)
constexpr float kDiagonalSlope = 0.4142136f;   // tan(22.5 degrees): eight-way stick sectors
constexpr int kTriggerThreshold = 128;         // of 255
// aurora's PADRead polls SDL; the game probes and reads all four channels each frame.
constexpr uint64_t kPadRefreshNs = 4'000'000;

struct Intent {
    uint32_t buttons = 0;  // WPAD bits, already in the sideways layout
    bool shake = false;
    float tilt = 0.0f;     // keyboard / touch roll: -1 counter-clockwise (left) .. 1 clockwise (right)
    bool attitude = false; // pitch / roll / ir below come from a Wii Remote profile
    float pitch = 0.0f;    // radians, + = pointer end up
    float roll = 0.0f;     // radians, + = right side down
    float ir[2] = {};      // profile aim, KPAD pos
    // The stick's direction for 360-degree movement (otherm_move.h), in the game's direction frame (rel_80572110):
    // x = the remote's D-pad Up minus Down, y = Right minus Left. (0, 0) for digital input.
    float move[2] = {};
};

struct Motion {
    int shakeFrame = kShakeFrames;  // the flick's next frame; kShakeFrames = at rest
    float pitch = 0.0f;
    float roll = 0.0f;
};

// Written by the Android UI thread (SetTouchState), read on the guest thread. The *SinceRead
// latches keep a press shorter than one 60 Hz input read (a quick tap) visible for one read.
std::atomic<uint32_t> g_touchButtons{0};
std::atomic<uint32_t> g_touchButtonsSinceRead{0};
std::atomic<uint32_t> g_lastButtons{0};  // player 1's WPAD buttons from the last Sample (LastButtons)
std::atomic<uint32_t> g_heldButtons{0};  // HoldButtonsUntil: buttons the port holds for the game
std::atomic<uint32_t> g_heldUntil{0};    // ... until this retrace count
std::atomic<float> g_moveX{0.0f}, g_moveY{0.0f};  // player 1's stick direction from the last Sample (MoveDirection)
std::atomic<bool> g_touchShake{false};
std::atomic<bool> g_touchShakeSinceRead{false};
std::atomic<float> g_touchTilt{0.0f};
std::atomic<bool> g_profileStale{true};

std::mutex g_mutex;
std::array<PADStatus, PAD_MAX_CONTROLLERS> g_pads{};
uint64_t g_padsReadNs = 0;
bool g_keyboardOnAPort = false;
std::array<Motion, PAD_MAX_CONTROLLERS> g_motion{};
std::array<bool, PAD_MAX_CONTROLLERS> g_announced{};
WiimoteProfile::Profile g_profile;
std::filesystem::path g_profilePath;  // the file LoadProfileLocked read; empty: the built-in copy
uint32_t g_profileGeneration = 0;  // counts profile loads

// Save / load state chords (PadHotkey; Options > Controls), read from Config.toml on first use. Guarded by g_mutex.
constexpr size_t kHotkeyCount = static_cast<size_t>(PadHotkey::Count);
constexpr const char* kHotkeyKeys[kHotkeyCount] = {"expr_save_state", "expr_load_state"};
std::array<std::vector<std::string>, kHotkeyCount> g_hotkeys;
bool g_hotkeysLoaded = false;
uint32_t g_hotkeysGeneration = 0;  // counts changes to g_hotkeys (FilterHomeChord)

// The inputs a chord expression names: each `quoted` name or bare word ("`Back` & `Shoulder R`", "Back & Start").
std::vector<std::string> ChordInputs(const std::string& text) {
    std::vector<std::string> out;
    const auto add = [&out](std::string name) {
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '`') {
            const size_t end = text.find('`', i + 1);
            if (end == std::string::npos) break;
            add(text.substr(i + 1, end - i - 1));
            i = end + 1;
        } else if (std::isalpha(static_cast<unsigned char>(text[i]))) {
            size_t end = i;
            while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end]))) ++end;
            add(text.substr(i, end - i));
            i = end;
        } else {
            ++i;
        }
    }
    return out;
}

void LoadHotkeysLocked() {
    if (g_hotkeysLoaded) return;
    g_hotkeysLoaded = true;
    ++g_hotkeysGeneration;
    for (size_t i = 0; i < kHotkeyCount; ++i) {
        const std::string text = RuntimeConfigFile::ControllerExpression(kHotkeyKeys[i]);
        g_hotkeys[i] = ChordInputs(text);
        if (!g_hotkeys[i].empty()) RT_LOGF(RT_TAG_CONFIG, "[hotkey] %s = %s\n", kHotkeyKeys[i], text.c_str());
    }
}

void RefreshPadsLocked() {
    const uint64_t now = SDL_GetTicksNS();
    if (g_padsReadNs != 0 && now - g_padsReadNs < kPadRefreshNs) {
        return;
    }
    g_padsReadNs = now;
    PADRead(g_pads.data());
    InputBindings::Apply(g_pads.data());
    g_keyboardOnAPort = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        uint32_t count = 0;
        if (PADGetKeyButtonBindings(port, &count) != nullptr) {
            g_keyboardOnAPort = true;
        }
    }
}

// Metroid.ini: $OTHERM_WIIMOTE_PROFILE, else next to the executable, else the built-in copy.
void LoadProfileLocked() {
    if (!g_profileStale.exchange(false)) {
        return;
    }
    ++g_profileGeneration;
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("OTHERM_WIIMOTE_PROFILE"); env != nullptr && *env != '\0') {
        candidates.push_back(RuntimeConfigFile::PathFromUtf8(env));
    }
    // Beside the .AppImage file in an AppImage (the program's own folder there is its read-only mount).
    if (const auto dir = RuntimeConfigFile::PlayerDirectory()) {
        candidates.push_back(*dir / "Metroid.ini");
    }
    for (const auto& path : candidates) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            continue;
        }
        std::ostringstream text;
        text << file.rdbuf();
        std::string error;
        std::vector<std::string> warnings;
        if (WiimoteProfile::Parse(text.str(), g_profile, error, &warnings)) {
            g_profilePath = path;
            RT_LOGF(RT_TAG_CONFIG, "[wiimote] profile %s: %d controls bound\n", RuntimeConfigFile::PathToUtf8(path).c_str(), g_profile.bound);
            for (const auto& warning : warnings) RT_LOGF(RT_TAG_CONFIG, "[wiimote]   %s\n", warning.c_str());
            return;
        }
        RT_LOGF(RT_TAG_CONFIG, "[wiimote] cannot use %s: %s\n", RuntimeConfigFile::PathToUtf8(path).c_str(), error.c_str());
    }
    std::string error;
    g_profilePath.clear();
    WiimoteProfile::Parse(WiimoteProfile::kDefaultOtherMProfile, g_profile, error);
    RT_LOGF(RT_TAG_CONFIG, "[wiimote] built-in Metroid.ini profile: %d controls bound\n", g_profile.bound);
}

// The built-in layout for port 1, used while no port has aurora's keyboard switched on.
Intent KeyboardIntent() {
    Intent intent;
    if (SDL_GetKeyboardFocus() == nullptr) {
        return intent;
    }
    // The keys come from the game's Keybinds tab (Config.toml [keyboard]); none count while one
    // is being picked there, or until the key picked is let go.
    if (Keybinds::Suppressed()) {
        return intent;
    }
    using namespace Keybinds;
    if (Held(kLeft)) intent.buttons |= kScreenLeft;
    if (Held(kRight)) intent.buttons |= kScreenRight;
    if (Held(kUp)) intent.buttons |= kScreenUp;
    if (Held(kDown)) intent.buttons |= kScreenDown;
    if (Held(kJump)) intent.buttons |= kWpadTwo;
    if (Held(kRun)) intent.buttons |= kWpadOne;
    if (Held(Keybinds::kA)) intent.buttons |= kWpadA;
    if (Held(kPlus)) intent.buttons |= kWpadPlus;
    if (Held(kMinus)) intent.buttons |= kWpadMinus;
    intent.shake = Held(kShake);
    intent.tilt = (Held(kTiltRight) ? 1.0f : 0.0f) - (Held(kTiltLeft) ? 1.0f : 0.0f);
    return intent;
}

// OTHERM_PAD_SCRIPT="Button A:3700:3730,Button B:4400:4430" (tests: the test window ignores real controllers): a
// scripted Xbox pad, plugged in from the start, holds each named input (named as Metroid.ini names them) between two
// presented-frame counts. It goes through the profile as a real pad does, so $menu and the icon set follow it too.
struct PadStep { std::string input; uint32_t from = 0, to = 0; double value = 1.0; };
const std::vector<PadStep>& PadScript() {
    static const std::vector<PadStep> steps = [] {
        std::vector<PadStep> out;
        const char* script = std::getenv("OTHERM_PAD_SCRIPT");
        std::stringstream items(script != nullptr ? script : "");
        std::string item;
        while (std::getline(items, item, ',')) {
            const size_t c2 = item.rfind(':');
            const size_t c1 = c2 == std::string::npos || c2 == 0 ? std::string::npos : item.rfind(':', c2 - 1);
            if (c1 == std::string::npos) continue;
            // "Left X+=0.6:100:200" holds an axis part way.
            std::string input = item.substr(0, c1);
            double value = 1.0;
            if (const size_t eq = input.find('='); eq != std::string::npos) {
                value = std::strtod(input.c_str() + eq + 1, nullptr);
                input.resize(eq);
            }
            out.push_back({input, static_cast<uint32_t>(std::strtoul(item.c_str() + c1 + 1, nullptr, 10)),
                           static_cast<uint32_t>(std::strtoul(item.c_str() + c2 + 1, nullptr, 10)), value});
        }
        if (!out.empty()) RT_LOGF(RT_TAG_CONFIG, "[script] scripted pad: %zu press(es)\n", out.size());
        return out;
    }();
    return steps;
}

double ScriptedPadInput(const std::string& name) {
    const uint32_t frame = VI_HLE_PresentedFrames();
    for (const PadStep& step : PadScript())
        if (step.input == name && frame >= step.from && frame < step.to) return step.value;
    return 0.0;
}

// HOME as a chord (Metroid.ini: Start & Back opens Options): the buttons that share its inputs (+ on Start, - on Back)
// wait up to kChordWindow before reaching the game, so the half of the chord pressed first does not open the game's
// pause screen in the moment before the other half arrives. A press let go sooner still reaches the game, for one
// read. Once HOME is down they stay away from the game until HOME and they are all let go (the profile already
// reports + released while Back joins Start, so "released" alone would let the held-back + out as a tap).
//
// Save / load state chords (PadHotkey): a held-back button whose own input is in one (- with View + RB) waits
// kHotkeyWindow instead, as the chord's other button tends to follow the modifier a moment later. `swallowed`: the
// buttons whose input a completed chord has taken (ProfileIntent); they are dropped, not let out as a tap.
std::string PadInputLocked(PadControl control);  // below: the one gamepad input of + or - ("Back")
uint32_t FilterHomeChord(uint32_t buttons, uint32_t swallowed) {
    constexpr auto kChordWindow = std::chrono::milliseconds(100);
    constexpr auto kHotkeyWindow = std::chrono::milliseconds(300);
    static uint32_t s_generation = ~0u, s_hotkeyGeneration = ~0u, s_shared = 0, s_long = 0;
    if (s_generation != g_profileGeneration || s_hotkeyGeneration != g_hotkeysGeneration) {
        s_generation = g_profileGeneration;
        s_hotkeyGeneration = g_hotkeysGeneration;
        s_shared = WiimoteProfile::ButtonsSharingHomeInputs(g_profile);
        const auto inChord = [](const std::string& input) {
            return !input.empty() && std::any_of(g_hotkeys.begin(), g_hotkeys.end(), [&input](const std::vector<std::string>& c) {
                return std::find(c.begin(), c.end(), input) != c.end();
            });
        };
        s_long = s_shared & ((inChord(PadInputLocked(PadControl::Minus)) ? kWpadMinus : 0u) |
                             (inChord(PadInputLocked(PadControl::Plus)) ? kWpadPlus : 0u));
    }
    static bool s_chord = false;
    static uint32_t s_waiting = 0, s_passed = 0;
    static std::array<std::chrono::steady_clock::time_point, 16> s_since{};
    if (buttons & WiimoteProfile::kWpadHome) {
        s_chord = true;
    } else if (!(buttons & s_shared)) {
        s_chord = false;
    }
    if (s_chord) {
        s_waiting = s_passed = 0;
        return buttons & ~s_shared;
    }
    const auto now = std::chrono::steady_clock::now();
    uint32_t taps = 0;
    for (int bit = 0; bit < 16; ++bit) {
        const uint32_t b = 1u << bit;
        if (!(s_shared & b)) continue;
        if (!(buttons & b)) {  // let go: one read for a press that never got through (not one a chord took)
            if ((s_waiting & b) && !(swallowed & b)) taps |= b;
            s_waiting &= ~b;
            s_passed &= ~b;
        } else if (!(s_waiting & b) && !(s_passed & b)) {
            s_waiting |= b;
            s_since[bit] = now;
        } else if ((s_waiting & b) && now - s_since[bit] >= ((s_long & b) ? kHotkeyWindow : kChordWindow)) {
            s_waiting &= ~b;
            s_passed |= b;
        }
    }
    return (buttons & ~s_shared) | (buttons & s_passed) | taps;
}

int StyleFor(SDL_Gamepad* gamepad) {
    if (gamepad == nullptr) return 0;  // the scripted pad
    switch (SDL_GetGamepadType(gamepad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return 1;  // PlayStation glyphs
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return 2;  // Nintendo
    default:
        return 0;  // Xbox
    }
}

// Gamepads through the Wii Remote profile. Every connected pad that is not a real Wii Remote drives player 1: an
// input name reads as its strongest value across them, so "!`Trigger R`" is 0 while any pad holds RT. With no pad
// at all every input reads 0 and the profile still decides the remote's resting attitude.
// 360-degree movement on: the D-pad's bits come from the stick's direction as a whole, so a direction counts once the
// stick is past half way in it (a round zone, not each axis past half way: 45 degrees at 0.6 deflection moves too) and
// is the nearest of eight 45-degree sectors. The game's own direction index (rel_80572110) follows these bits;
// otherm_move.cpp puts the exact direction (intent.move) next to it.
void RoundDpad(Intent& intent) {
    const float x = intent.move[0], y = intent.move[1];
    intent.buttons &= ~(kWpadUp | kWpadDown | kWpadLeft | kWpadRight);
    if (std::hypot(x, y) < static_cast<float>(InputExpr::kConditionThreshold)) {
        intent.move[0] = intent.move[1] = 0.0f;
        return;
    }
    constexpr float kTan22_5 = 0.41421356f;
    if (std::fabs(x) >= kTan22_5 * std::fabs(y)) intent.buttons |= x > 0.0f ? kWpadUp : kWpadDown;
    if (std::fabs(y) >= kTan22_5 * std::fabs(x)) intent.buttons |= y > 0.0f ? kWpadRight : kWpadLeft;
}

Intent ProfileIntent() {
    std::vector<SDL_Gamepad*> pads;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad* gamepad = SDL_GetGamepadFromID(ids[i]);
        if (gamepad == nullptr) gamepad = SDL_OpenGamepad(ids[i]);  // not opened by Aurora's GAMEPAD_ADDED handler (yet)
        if (gamepad == nullptr) {
            static SDL_JoystickID s_failed = 0;
            if (s_failed != ids[i]) { s_failed = ids[i]; RT_LOGF(RT_TAG_CONFIG, "[gamepad] cannot open pad %u: %s\n", unsigned(ids[i]), SDL_GetError()); }
            continue;
        }
        const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForName(SDL_GetGamepadName(gamepad));
        if (kind != WiiRemoteInput::Kind::NotWii && kind != WiiRemoteInput::Kind::WiiUPro) {
            continue;  // a real Wii Remote: the game reads it through KPAD
        }
        static std::vector<SDL_JoystickID> s_seen;
        if (std::find(s_seen.begin(), s_seen.end(), ids[i]) == s_seen.end()) {
            s_seen.push_back(ids[i]);
            RT_LOGF(RT_TAG_CONFIG, "[gamepad] using '%s' (type %d) for player 1 through the Wii Remote profile\n",
                    SDL_GetGamepadName(gamepad), int(SDL_GetGamepadType(gamepad)));
        }
        pads.push_back(gamepad);
    }
    SDL_free(ids);
    if (!PadScript().empty()) pads.push_back(nullptr);  // the scripted pad

    // $menu, for the profile: one of the game's menus with a 1 / B legend is up. Its badges come from
    // button_icons/<style>/marker7 (button_icons.cpp), and aurora reports them drawn in the last frames;
    // they appear only in the menu layouts (option, save, save_dialog, theater, gallery, story). With no pad there are
    // no replaced badges and $menu stays 0. The message icons switch to their menu set with it.
    const bool menu = !pads.empty() && aurora_button_icon_marker(7);
    static bool s_wasMenu = false;
    if (menu != s_wasMenu) {
        s_wasMenu = menu;
        RT_LOGF(RT_TAG_CONFIG, "[gamepad] frame %u: $menu = %d\n", unsigned(VI_HLE_PresentedFrames()), int(menu));
    }
    aurora_set_button_icon_menu(menu);
    const auto read = [menu](SDL_Gamepad* pad, const std::string& name) {
        if (!name.empty() && name[0] == '$') return name == "$menu" && menu ? 1.0 : 0.0;
        return pad == nullptr ? ScriptedPadInput(name) : InputBindings::ReadDolphinInput(pad, name);
    };
    const auto strongest = [&pads, &read](const std::string& name) {
        double value = 0.0;
        for (SDL_Gamepad* pad : pads) value = std::max(value, read(pad, name));
        return value;
    };
    // Save / load state chords (PollHotkeys): from the moment every input of one is held, the game reads those inputs
    // as let go until each really is, so the chord does nothing in the game (View + RB does not also press -; before
    // RB arrives, FilterHomeChord keeps - from the game for the first 300 ms of View).
    LoadHotkeysLocked();
    static std::vector<std::string> s_hotkeyHeld;
    for (const std::vector<std::string>& chord : g_hotkeys) {
        if (chord.empty() ||
            !std::all_of(chord.begin(), chord.end(), [&strongest](const std::string& name) { return strongest(name) >= 0.5; }))
            continue;
        for (const std::string& name : chord)
            if (std::find(s_hotkeyHeld.begin(), s_hotkeyHeld.end(), name) == s_hotkeyHeld.end()) s_hotkeyHeld.push_back(name);
    }
    s_hotkeyHeld.erase(std::remove_if(s_hotkeyHeld.begin(), s_hotkeyHeld.end(),
                                      [&strongest](const std::string& name) { return strongest(name) < 0.5; }),
                       s_hotkeyHeld.end());
    const auto source = [&strongest](const std::string& name) {
        if (std::find(s_hotkeyHeld.begin(), s_hotkeyHeld.end(), name) != s_hotkeyHeld.end()) return 0.0;
        return strongest(name);
    };
    const WiimoteProfile::State state = WiimoteProfile::Evaluate(g_profile, source);
    uint32_t swallowed = 0;  // + / - whose input a completed chord holds
    if (!s_hotkeyHeld.empty()) {
        const auto taken = [](PadControl control) {
            const std::string input = PadInputLocked(control);
            return !input.empty() && std::find(s_hotkeyHeld.begin(), s_hotkeyHeld.end(), input) != s_hotkeyHeld.end();
        };
        if (taken(PadControl::Minus)) swallowed |= kWpadMinus;
        if (taken(PadControl::Plus)) swallowed |= kWpadPlus;
    }
    Intent intent;
    intent.buttons = FilterHomeChord(state.buttons, swallowed);
    intent.shake = state.shake;
    intent.attitude = true;
    intent.pitch = state.pitch;
    intent.roll = state.roll;
    intent.ir[0] = state.ir[0];
    intent.ir[1] = state.ir[1];
    intent.move[0] = state.dpad[0] - state.dpad[1];
    intent.move[1] = state.dpad[3] - state.dpad[2];
    if (OthermMove::Enabled()) RoundDpad(intent);

    // On-screen button prompts follow the pad last pressed (else the first one); none = the game's own icons.
    static int s_lastPressedStyle = -1;
    for (SDL_Gamepad* pad : pads) {
        const auto one = [pad, &read](const std::string& name) { return read(pad, name); };
        if (WiimoteProfile::Evaluate(g_profile, one).buttons != 0) s_lastPressedStyle = StyleFor(pad);
    }
    if (pads.empty()) s_lastPressedStyle = -1;
    aurora_set_button_icon_style(s_lastPressedStyle >= 0 ? s_lastPressedStyle : pads.empty() ? -1 : StyleFor(pads[0]));
    return intent;
}

// Unattended bring-up: OTHERM_SCRIPT="A:1500:1530,2:1900:1930" holds a button (A, B, 1, 2, +, -, Home, Up, Down,
// Left, Right) between two presented-frame counts (hle_stubs.h VI_HLE_PresentedFrames). "Point" instead holds the
// remote level at the screen (what RT does through the profile: Other M's first-person view), aimed at its centre,
// or "Point/x/y" at KPAD pos x, y (-1..1, +y down).
Intent ScriptIntent() {
    struct Step { uint32_t button; uint32_t from; uint32_t to; bool point; float aim[2]; };
    static std::vector<Step> s_steps;
    static bool s_parsed = false;
    if (!s_parsed) {
        s_parsed = true;
        if (const char* script = std::getenv("OTHERM_SCRIPT")) {
            std::string text(script);
            size_t start = 0;
            while (start < text.size()) {
                const size_t comma = text.find(',', start);
                const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const size_t c1 = item.find(':');
                const size_t c2 = c1 == std::string::npos ? std::string::npos : item.find(':', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos) {
                    const std::string name = item.substr(0, c1);
                    const uint32_t button = name == "A" ? kWpadA : name == "B" ? WiimoteProfile::kWpadB : name == "1" ? kWpadOne
                                          : name == "2" ? kWpadTwo : name == "+" ? kWpadPlus : name == "-" ? kWpadMinus
                                          : name == "Home" ? WiimoteProfile::kWpadHome : name == "Up" ? kWpadUp : name == "Down" ? kWpadDown
                                          : name == "Left" ? kWpadLeft : name == "Right" ? kWpadRight : 0u;
                    Step step{button, static_cast<uint32_t>(std::strtoul(item.c_str() + c1 + 1, nullptr, 10)),
                              static_cast<uint32_t>(std::strtoul(item.c_str() + c2 + 1, nullptr, 10)),
                              name.rfind("Point", 0) == 0, {0.0f, 0.0f}};
                    if (step.point && name.size() > 6 && name[5] == '/') {
                        char* end = nullptr;
                        step.aim[0] = std::strtof(name.c_str() + 6, &end);
                        if (end != nullptr && *end == '/') step.aim[1] = std::strtof(end + 1, nullptr);
                    }
                    s_steps.push_back(step);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            RT_LOGF(RT_TAG_CONFIG, "[script] %zu scripted press(es)\n", s_steps.size());
        }
    }
    Intent intent;
    const uint32_t frame = VI_HLE_PresentedFrames();
    for (const Step& step : s_steps) {
        if (frame < step.from || frame >= step.to) continue;
        intent.buttons |= step.button;
        if (!step.point) continue;
        intent.attitude = true;  // pitch and roll stay 0: level
        intent.ir[0] = step.aim[0];
        intent.ir[1] = step.aim[1];
    }
    return intent;
}

// The on-screen remote and phone motion set through SetTouchState.
Intent TouchIntent() {
    Intent intent;
    const uint32_t touch = g_touchButtons.load(std::memory_order_relaxed) |
                           g_touchButtonsSinceRead.exchange(0, std::memory_order_relaxed);
    if (touch & kTouchLeft) intent.buttons |= kScreenLeft;
    if (touch & kTouchRight) intent.buttons |= kScreenRight;
    if (touch & kTouchUp) intent.buttons |= kScreenUp;
    if (touch & kTouchDown) intent.buttons |= kScreenDown;
    if (touch & kTouchOne) intent.buttons |= kWpadOne;
    if (touch & kTouchTwo) intent.buttons |= kWpadTwo;
    if (touch & kTouchA) intent.buttons |= kWpadA;
    if (touch & kTouchPlus) intent.buttons |= kWpadPlus;
    if (touch & kTouchMinus) intent.buttons |= kWpadMinus;
    intent.shake = g_touchShake.load(std::memory_order_relaxed) ||
                   g_touchShakeSinceRead.exchange(false, std::memory_order_relaxed);
    intent.tilt = std::clamp(g_touchTilt.load(std::memory_order_relaxed), -1.0f, 1.0f);
    return intent;
}

// A GameCube-style pad state (after the user's remapping) as the sideways remote: players 2-4 only.
Intent PadIntent(const PADStatus& pad) {
    Intent intent;
    const uint32_t b = pad.button;
    if (b & PAD_BUTTON_LEFT) intent.buttons |= kScreenLeft;
    if (b & PAD_BUTTON_RIGHT) intent.buttons |= kScreenRight;
    if (b & PAD_BUTTON_UP) intent.buttons |= kScreenUp;
    if (b & PAD_BUTTON_DOWN) intent.buttons |= kScreenDown;
    const int sx = pad.stickX;
    const int sy = pad.stickY;  // +y up
    if (sx * sx + sy * sy >= kStickThreshold * kStickThreshold) {
        if (std::abs(sx) >= kDiagonalSlope * std::abs(sy)) intent.buttons |= sx < 0 ? kScreenLeft : kScreenRight;
        if (std::abs(sy) >= kDiagonalSlope * std::abs(sx)) intent.buttons |= sy < 0 ? kScreenDown : kScreenUp;
    }
    if (b & PAD_BUTTON_A) intent.buttons |= kWpadTwo;
    if (b & (PAD_BUTTON_B | PAD_BUTTON_X)) intent.buttons |= kWpadOne;
    if (b & PAD_BUTTON_Y) intent.buttons |= kWpadA;
    if (b & PAD_BUTTON_START) intent.buttons |= kWpadPlus;
    if ((pad.extButton & PAD_BUTTON_BACK) != 0 || (b & PAD_TRIGGER_L) != 0 || pad.triggerLeft >= kTriggerThreshold) {
        intent.buttons |= kWpadMinus;
    }
    intent.shake = (b & (PAD_TRIGGER_Z | PAD_TRIGGER_R)) != 0 || pad.triggerRight >= kTriggerThreshold;
    return intent;
}

// Accelerometer for the remote's attitude plus any shake flick (KPAD frame, rest flat: y = -1).
void ApplyMotion(Motion& motion, const Intent& intent, WiiRemoteInput::KpadSample& sample) {
    if (intent.shake && motion.shakeFrame >= kShakeFrames) {
        motion.shakeFrame = 0;
    }
    const float targetPitch = intent.attitude ? intent.pitch : 0.0f;
    const float targetRoll = intent.tilt != 0.0f ? intent.tilt * kMaxRollRadians : intent.attitude ? intent.roll : 0.0f;
    motion.pitch += (targetPitch - motion.pitch) * kAttitudeFollow;
    motion.roll += (targetRoll - motion.roll) * kAttitudeFollow;
    if (std::fabs(motion.pitch - targetPitch) < 0.001f) motion.pitch = targetPitch;
    if (std::fabs(motion.roll - targetRoll) < 0.001f) motion.roll = targetRoll;
    WiimoteProfile::AccelerationFor(motion.pitch, motion.roll, sample.acc);
    if (motion.shakeFrame < kShakeFrames) {
        sample.acc[1] += kShakeFlickG[motion.shakeFrame++];
    }
}

void AnnounceLocked(uint32_t chan, bool pad) {
    if (g_announced[chan]) {
        return;
    }
    g_announced[chan] = true;
    const char* source = "keyboard and every connected gamepad";
    if (pad) {
        const s32 index = PADGetIndexForPort(chan);
        SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
        const char* name = gamepad != nullptr ? SDL_GetGamepadName(gamepad) : nullptr;
        source = name != nullptr ? name : g_keyboardOnAPort ? "keyboard (settings bindings)" : "controller";
    }
    RT_LOGF(RT_TAG_RUNTIME, "player %u: %s as a sideways Wii Remote\n", chan + 1, source);
}

// The mouse over the game window, in KPAD pos units (the game's 16:9 or 4:3 view inside the presented picture).
bool MousePosition(float pos[2], SDL_MouseButtonFlags& buttons) {
    SDL_Window* window = RuntimeConfigFile::MousePointer(true) ? SDL_GetMouseFocus() : nullptr;
    if (window == nullptr) {
        return false;
    }
    float mx = 0.0f, my = 0.0f;
    buttons = SDL_GetMouseState(&mx, &my);
    int w = 0, h = 0;
    if (!SDL_GetWindowSize(window, &w, &h) || w <= 0 || h <= 0) {
        return false;
    }
    const auto settings = DisplaySettings::Current();
    const float windowAspect = static_cast<float>(w) / static_cast<float>(h);
    const float pictureAspect = settings.aspect == DisplaySettings::kAspect4x3        ? 4.0f / 3.0f
                                : settings.aspect == DisplaySettings::kAspect16x9      ? 16.0f / 9.0f
                                : settings.aspect == DisplaySettings::kAspectCinematic ? DisplaySettings::kRatioCinematic
                                                                                       : windowAspect;
    const float pictureH = pictureAspect >= windowAspect ? static_cast<float>(w) / pictureAspect : static_cast<float>(h);
    const float pictureW = pictureH * pictureAspect;
    const float viewW = std::min(pictureW, pictureH * (DisplaySettings::GameWidescreen() ? 16.0f / 9.0f : 4.0f / 3.0f));
    pos[0] = std::clamp((mx - w * 0.5f) / (viewW * 0.5f), -1.0f, 1.0f);
    pos[1] = std::clamp((my - h * 0.5f) / (pictureH * 0.5f), -1.0f, 1.0f);
    return true;
}

} // namespace

bool InArcade() { return false; }

void ReloadProfile() { g_profileStale.store(true); }

void SetTouchState(uint32_t touchButtons, bool shake, float tilt) {
    g_touchButtons.store(touchButtons, std::memory_order_relaxed);
    g_touchButtonsSinceRead.fetch_or(touchButtons, std::memory_order_relaxed);
    g_touchShake.store(shake, std::memory_order_relaxed);
    if (shake) {
        g_touchShakeSinceRead.store(true, std::memory_order_relaxed);
    }
    g_touchTilt.store(std::isfinite(tilt) ? tilt : 0.0f, std::memory_order_relaxed);
}

namespace {
// The one input of a binding like "`Trigger R`" or "!Back" (negated: the motion happens while it is NOT held). Empty
// for anything else (several inputs, functions).
std::string SingleInput(const std::string& text, bool negated) {
    size_t i = text.find_first_not_of(" \t");
    if (i == std::string::npos) return {};
    if ((text[i] == '!') != negated) return {};
    if (negated) i = text.find_first_not_of(" \t", i + 1);
    if (i == std::string::npos) return {};
    std::string name;
    size_t end = 0;
    if (text[i] == '`') {
        end = text.find('`', i + 1);
        if (end == std::string::npos) return {};
        name = text.substr(i + 1, end - i - 1);
        ++end;
    } else {
        end = i;
        while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end]))) ++end;
        name = text.substr(i, end - i);
    }
    return text.find_first_not_of(" \t", end) == std::string::npos ? name : std::string();
}

// The input that levels the remote at the screen: the one Tilt/Forward's resting tilt is negated on that
// Tilt/Backward does not use ("!`Trigger R`" in "(!`Trigger R` & !`Trigger L`) * 0.28"; LT is Backward's, it
// turns the remote upright). Empty when there is not exactly one.
std::string PointInput(const WiimoteProfile::Profile& profile) {
    const std::string& text = profile.text[WiimoteProfile::kTiltForward];
    const std::vector<std::string> upright = profile.controls[WiimoteProfile::kTiltBackward].ReferencedInputs();
    std::string found;
    for (size_t bang = text.find('!'); bang != std::string::npos; bang = text.find('!', bang + 1)) {
        size_t i = text.find_first_not_of(" \t", bang + 1);
        if (i == std::string::npos) break;
        std::string name;
        if (text[i] == '`') {
            const size_t end = text.find('`', i + 1);
            if (end == std::string::npos) break;
            name = text.substr(i + 1, end - i - 1);
        } else {
            size_t end = i;
            while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end]))) ++end;
            name = text.substr(i, end - i);
        }
        if (name.empty() || std::find(upright.begin(), upright.end(), name) != upright.end()) continue;
        if (!found.empty() && found != name) return {};
        found = name;
    }
    return found;
}

// A Dolphin XInput input name as the controller in use labels it (0 Xbox, 1 PlayStation, 2 Nintendo: SDL reads
// buttons by position, so a Nintendo pad's B sits where Xbox has A).
std::string PadInputLabel(const std::string& input, int style) {
    struct Label { const char* input; const char* xbox; const char* playstation; const char* nintendo; };
    static constexpr Label kLabels[] = {
        {"Trigger L", "LT", "L2", "ZL"},       {"Trigger R", "RT", "R2", "ZR"},
        {"Shoulder L", "LB", "L1", "L"},       {"Shoulder R", "RB", "R1", "R"},
        {"Button A", "A", "Cross", "B"},       {"Button B", "B", "Circle", "A"},
        {"Button X", "X", "Square", "Y"},      {"Button Y", "Y", "Triangle", "X"},
        {"Back", "View", "Create", "-"},       {"Start", "Menu", "Options", "+"},
        {"Thumb L", "the left stick button", "L3", "the left stick button"},
        {"Thumb R", "the right stick button", "R3", "the right stick button"},
    };
    for (const Label& label : kLabels)
        if (input == label.input) return style == 1 ? label.playstation : style == 2 ? label.nintendo : label.xbox;
    return {};
}
} // namespace

MotionHintNames HintNames() {
    MotionHintNames names;
    using WiiRemoteInput::Kind;
    const Kind kind = WiiRemoteInput::KindForPort(0);
    if (kind == Kind::Remote || kind == Kind::RemoteWithNunchuk || kind == Kind::RemoteWithClassic) {
        names.wiiRemote = true;
        return names;
    }
    // The pad in use (its button pictures); before the first remote read (the game reads its messages at boot) the
    // first gamepad connected. -1 with none.
    int style = aurora_button_icon_style();
    if (style < 0 && !PadScript().empty()) style = StyleFor(nullptr);
    if (style < 0) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids != nullptr && i < count && style < 0; ++i) {
            SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
            if (pad == nullptr) pad = SDL_OpenGamepad(ids[i]);
            if (pad == nullptr) continue;
            const Kind padKind = WiiRemoteInput::KindForName(SDL_GetGamepadName(pad));
            if (padKind == Kind::NotWii || padKind == Kind::WiiUPro) style = StyleFor(pad);
        }
        SDL_free(ids);
    }
    if (style < 0) {
        // Keyboard and mouse: the right mouse button points the remote (MousePosition); nothing holds it vertically.
        if (RuntimeConfigFile::MousePointer(true)) names.point = "the right mouse button";
        return names;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadProfileLocked();
    names.point = PadInputLabel(PointInput(g_profile), style);
    names.vertical = PadInputLabel(SingleInput(g_profile.text[WiimoteProfile::kTiltBackward], false), style);
    return names;
}

// ---- Options > Controls: the controller column (Metroid.ini) ----
namespace {

// The inputs of a binding in order, with whether each is negated ("`Button B` & !$menu | `Button X` & $menu" gives
// Button B, !$menu, Button X, $menu). Operators, brackets and numbers are skipped.
struct Term { std::string name; bool negated = false; };
std::vector<Term> Terms(const std::string& text) {
    std::vector<Term> out;
    bool negate = false;
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '!') { negate = true; ++i; continue; }
        if (c == '`') {
            const size_t end = text.find('`', i + 1);
            if (end == std::string::npos) break;
            out.push_back({text.substr(i + 1, end - i - 1), negate});
            negate = false;
            i = end + 1;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '$') {
            size_t end = i + 1;
            while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
            out.push_back({text.substr(i, end - i), negate});
            negate = false;
            i = end;
            continue;
        }
        ++i;
    }
    return out;
}

using WiimoteProfile::Control;

std::vector<std::string> HomeInputsLocked() { return g_profile.controls[WiimoteProfile::kButtonHome].ReferencedInputs(); }

std::string PadInputLocked(PadControl control) {
    const auto& text = g_profile.text;
    switch (control) {
    case PadControl::A: return SingleInput(text[WiimoteProfile::kButtonA], false);
    case PadControl::Two: return SingleInput(text[WiimoteProfile::kButton2], false);
    case PadControl::B:
    case PadControl::One: {
        // One input, or the menu swap Metroid.ini uses: "X & !$menu | Y & $menu" (X outside the game's menus).
        const std::string& t = text[control == PadControl::B ? WiimoteProfile::kButtonB : WiimoteProfile::kButton1];
        if (std::string one = SingleInput(t, false); !one.empty()) return one;
        const std::vector<Term> terms = Terms(t);
        if (terms.size() == 4 && !terms[0].negated && terms[1].name == "$menu" && terms[1].negated && !terms[2].negated &&
            terms[3].name == "$menu" && !terms[3].negated)
            return terms[0].name;
        return {};
    }
    case PadControl::Plus:
    case PadControl::Minus: {
        // One input, or one kept out of the HOME chord: "Start & !Back".
        const std::string& t = text[control == PadControl::Plus ? WiimoteProfile::kButtonPlus : WiimoteProfile::kButtonMinus];
        const std::vector<Term> terms = Terms(t);
        if (terms.empty() || terms[0].negated || terms[0].name[0] == '$') return {};
        for (size_t i = 1; i < terms.size(); ++i) {
            const std::vector<std::string> home = HomeInputsLocked();
            if (!terms[i].negated || std::find(home.begin(), home.end(), terms[i].name) == home.end()) return {};
        }
        return terms[0].name;
    }
    case PadControl::Point: return PointInput(g_profile);
    case PadControl::Upright: return SingleInput(text[WiimoteProfile::kTiltBackward], false);
    default: return {};
    }
}

// Short names for the table ("Y", "LT", "D-pad up"); the hint names above are written for sentences.
std::string ShortPadLabel(const std::string& input, int style) {
    struct Label { const char* input; const char* xbox; const char* playstation; const char* nintendo; };
    static constexpr Label kShort[] = {
        {"Thumb L", "LS", "L3", "LS"},           {"Thumb R", "RS", "R3", "RS"},
        {"Pad N", "D-pad up", "D-pad up", "D-pad up"},       {"Pad S", "D-pad down", "D-pad down", "D-pad down"},
        {"Pad W", "D-pad left", "D-pad left", "D-pad left"}, {"Pad E", "D-pad right", "D-pad right", "D-pad right"},
        {"Left X-", "Left stick left", "Left stick left", "Left stick left"},
        {"Left X+", "Left stick right", "Left stick right", "Left stick right"},
        {"Left Y+", "Left stick up", "Left stick up", "Left stick up"},
        {"Left Y-", "Left stick down", "Left stick down", "Left stick down"},
        {"Right X-", "Right stick left", "Right stick left", "Right stick left"},
        {"Right X+", "Right stick right", "Right stick right", "Right stick right"},
        {"Right Y+", "Right stick up", "Right stick up", "Right stick up"},
        {"Right Y-", "Right stick down", "Right stick down", "Right stick down"},
    };
    for (const Label& label : kShort)
        if (input == label.input) return style == 1 ? label.playstation : style == 2 ? label.nintendo : label.xbox;
    const std::string label = PadInputLabel(input, style);
    return label.empty() ? input : label;
}

std::string Quote(const std::string& input) { return input.empty() ? std::string() : "`" + input + "`"; }

// Metroid.ini's text with `key = value` set (the line replaced, or added at the end of [Profile]); keeps CRLF files CRLF.
std::string SetProfileKey(std::string text, const std::string& key, const std::string& value) {
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    size_t at = 0, profileEnd = std::string::npos;
    bool inProfile = false;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '[') {
            if (inProfile) profileEnd = at;
            inProfile = line.compare(first, 9, "[Profile]") == 0;
        } else if (first != std::string::npos) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string name = line.substr(first, eq - first);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                if (inProfile && name == key) {
                    const size_t lineEnd = end < text.size() && end > at && text[end - 1] == '\r' ? end - 1 : end;
                    text.replace(at, lineEnd - at, key + " = " + value);
                    return text;
                }
            }
        }
        if (end >= text.size()) break;
        at = end + 1;
    }
    const std::string line = key + " = " + value + eol;
    if (profileEnd != std::string::npos) return text.insert(profileEnd, line);
    if (!text.empty() && text.back() != '\n') text += eol;
    return text + line;
}

// Every input a player-1 gamepad holds past half way, by Dolphin name, in the order a chord's label names them
// (PadInputsLabel: "View + RB").
const std::vector<std::string>& CaptureInputs() {
    static const std::vector<std::string> inputs = {
        "Back", "Start", "Shoulder L", "Shoulder R", "Trigger L", "Trigger R", "Thumb L", "Thumb R", "Button A", "Button B",
        "Button X", "Button Y", "Pad N", "Pad S", "Pad W", "Pad E", "Left X-", "Left X+", "Left Y-", "Left Y+",
        "Right X-", "Right X+", "Right Y-", "Right Y+"};
    return inputs;
}
std::vector<SDL_Gamepad*> PlayerOnePads() {
    std::vector<SDL_Gamepad*> pads;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) continue;
        const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForName(SDL_GetGamepadName(pad));
        if (kind == WiiRemoteInput::Kind::NotWii || kind == WiiRemoteInput::Kind::WiiUPro) pads.push_back(pad);
    }
    SDL_free(ids);
    return pads;
}
} // namespace

std::vector<std::string> HeldPadInputs() {
    std::vector<std::string> held;
    const std::vector<SDL_Gamepad*> pads = PlayerOnePads();
    for (const std::string& name : CaptureInputs()) {
        double value = ScriptedPadInput(name);
        for (SDL_Gamepad* pad : pads) value = std::max(value, InputBindings::ReadDolphinInput(pad, name));
        if (value >= 0.5) held.push_back(name);
    }
    return held;
}

namespace {
std::mutex g_captureMutex;
bool g_capturing = false;
std::vector<std::string> g_captureHeld;  // held when the capture began (the button that picked the cell)
uint64_t g_captureDeadlineNs = 0;

// Writes `lines` into Metroid.ini (the file read, else a new one next to the executable from the built-in copy) and
// reloads it. Called with g_mutex held.
bool WriteProfileLinesLocked(const std::vector<std::pair<std::string, std::string>>& lines, std::string& error) {
    std::filesystem::path path = g_profilePath;
    std::string text = WiimoteProfile::kDefaultOtherMProfile;
    if (path.empty()) {
        const auto dir = RuntimeConfigFile::PlayerDirectory();
        if (!dir) {
            error = "No folder to write Metroid.ini to.";
            return false;
        }
        path = *dir / "Metroid.ini";
    } else {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream all;
        all << file.rdbuf();
        if (file) text = all.str();
    }
    for (const auto& [key, value] : lines) text = SetProfileKey(text, key, value);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            error = "Couldn't write " + RuntimeConfigFile::PathToUtf8(path) + ".";
            return false;
        }
    }
    for (const auto& [key, value] : lines)
        RT_LOGF(RT_TAG_CONFIG, "[wiimote] %s = %s (Options > Controls)\n", key.c_str(), value.c_str());
    g_profileStale.store(true);
    ButtonIcons::Refresh();  // the prompts follow the new bindings (a worker; it reads the profile after this lock)
    return true;
}

} // namespace

std::string PadInput(PadControl control) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadProfileLocked();
    return PadInputLocked(control);
}

std::string ProfileText() {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadProfileLocked();
    if (!g_profilePath.empty()) {
        std::ifstream file(g_profilePath, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        if (file) return text.str();
    }
    return WiimoteProfile::kDefaultOtherMProfile;
}

std::string PadLabel(PadControl control) {
    int style = aurora_button_icon_style();
    if (style < 0) style = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadProfileLocked();
    const std::string input = PadInputLocked(control);
    if (!input.empty()) return ShortPadLabel(input, style);
    static constexpr Control kText[] = {WiimoteProfile::kButtonA, WiimoteProfile::kButtonB, WiimoteProfile::kButton1,
                                        WiimoteProfile::kButton2, WiimoteProfile::kButtonPlus, WiimoteProfile::kButtonMinus,
                                        WiimoteProfile::kTiltForward, WiimoteProfile::kTiltBackward};
    const std::string& text = g_profile.text[kText[static_cast<int>(control)]];
    return text.find_first_not_of(" \t") == std::string::npos ? "None" : "(custom)";
}

bool BindPad(PadControl control, const std::string& input, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadProfileLocked();
    // The stick and the D-pad move Samus and aim: they are not offered.
    for (int c = WiimoteProfile::kDpadUp; c <= WiimoteProfile::kIrRight; ++c) {
        const std::vector<std::string> used = g_profile.controls[c].ReferencedInputs();
        if (std::find(used.begin(), used.end(), input) != used.end()) {
            error = "That one moves Samus and aims.";
            return false;
        }
    }
    constexpr int kCount = static_cast<int>(PadControl::Count);
    std::array<std::string, kCount> now;
    for (int i = 0; i < kCount; ++i) now[static_cast<size_t>(i)] = PadInputLocked(static_cast<PadControl>(i));
    const size_t index = static_cast<size_t>(control);
    std::array<bool, kCount> changed{};
    for (size_t i = 0; i < now.size(); ++i) {
        if (i != index && now[i] == input) {
            now[i] = now[index];  // the swap
            changed[i] = true;
        }
    }
    now[index] = input;
    changed[index] = true;
    const auto at = [&now](PadControl c) -> const std::string& { return now[static_cast<size_t>(c)]; };
    const auto was = [&changed](PadControl c) { return changed[static_cast<size_t>(c)]; };
    std::vector<std::pair<std::string, std::string>> lines;
    if (was(PadControl::A)) lines.push_back({"Buttons/A", Quote(at(PadControl::A))});
    if (was(PadControl::Two)) lines.push_back({"Buttons/2", Quote(at(PadControl::Two))});
    if (was(PadControl::B) || was(PadControl::One)) {
        // In the game's menus B and 1 trade inputs, so the pad's "back" button stays Wii 1 (BACK) there.
        const std::string& b = at(PadControl::B);
        const std::string& one = at(PadControl::One);
        if (!b.empty() && !one.empty()) {
            lines.push_back({"Buttons/B", Quote(b) + " & !$menu | " + Quote(one) + " & $menu"});
            lines.push_back({"Buttons/1", Quote(one) + " & !$menu | " + Quote(b) + " & $menu"});
        } else {
            if (was(PadControl::B)) lines.push_back({"Buttons/B", Quote(b)});
            if (was(PadControl::One)) lines.push_back({"Buttons/1", Quote(one)});
        }
    }
    const std::vector<std::string> home = HomeInputsLocked();
    const auto homeGuard = [&home](const std::string& in) {
        // An input of the HOME chord counts for + / - only while the chord's other inputs are up.
        std::string text = Quote(in);
        if (std::find(home.begin(), home.end(), in) == home.end()) return text;
        for (const std::string& other : home)
            if (other != in) text += " & !" + Quote(other);
        return text;
    };
    if (was(PadControl::Plus)) lines.push_back({"Buttons/+", homeGuard(at(PadControl::Plus))});
    if (was(PadControl::Minus)) lines.push_back({"Buttons/-", homeGuard(at(PadControl::Minus))});
    if (was(PadControl::Point) || was(PadControl::Upright)) {
        // The resting forward tilt drops out while either is held (Tilt/Forward minus Tilt/Backward, as in Dolphin);
        // its factor (0.28 of Tilt/Angle in the default profile) is kept.
        const std::string& forward = g_profile.text[WiimoteProfile::kTiltForward];
        std::string factor;
        if (const size_t star = forward.rfind('*'); star != std::string::npos) {
            factor = forward.substr(star + 1);
            factor.erase(0, factor.find_first_not_of(" \t"));
        }
        const std::string& point = at(PadControl::Point);
        const std::string& upright = at(PadControl::Upright);
        std::string rest = !point.empty() && !upright.empty() ? "(!" + Quote(point) + " & !" + Quote(upright) + ")"
                           : !point.empty()                   ? "!" + Quote(point)
                           : !upright.empty()                 ? "!" + Quote(upright)
                                                              : std::string("1");
        if (!factor.empty()) rest += " * " + factor;
        lines.push_back({"Tilt/Forward", rest});
        lines.push_back({"Tilt/Backward", Quote(upright)});
    }
    return WriteProfileLinesLocked(lines, error);
}

bool ResetPadBindings(std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    WiimoteProfile::Profile defaults;
    std::string parseError;
    WiimoteProfile::Parse(WiimoteProfile::kDefaultOtherMProfile, defaults, parseError);
    std::vector<std::pair<std::string, std::string>> lines;
    for (const int c : {WiimoteProfile::kButtonA, WiimoteProfile::kButtonB, WiimoteProfile::kButton1, WiimoteProfile::kButton2,
                        WiimoteProfile::kButtonMinus, WiimoteProfile::kButtonPlus, WiimoteProfile::kTiltForward,
                        WiimoteProfile::kTiltBackward})
        lines.push_back({WiimoteProfile::DolphinName(c), defaults.text[static_cast<size_t>(c)]});
    char angle[32];
    std::snprintf(angle, sizeof(angle), "%g", defaults.tiltMaxDegrees);
    lines.push_back({"Tilt/Angle", angle});
    // The save / load state chords have no default: they are cleared.
    LoadHotkeysLocked();
    for (size_t i = 0; i < kHotkeyCount; ++i) {
        if (g_hotkeys[i].empty()) continue;
        g_hotkeys[i].clear();
        ++g_hotkeysGeneration;
        RuntimeConfigFile::SetControllerExpression(kHotkeyKeys[i], "");
        RT_LOGF(RT_TAG_CONFIG, "[hotkey] %s cleared (Options > Controls)\n", kHotkeyKeys[i]);
    }
    return WriteProfileLinesLocked(lines, error);
}

std::string PadInputsLabel(const std::vector<std::string>& inputs) {
    const int style = std::max(0, aurora_button_icon_style());
    const std::vector<std::string>& order = CaptureInputs();
    const auto rank = [&order](const std::string& name) { return std::find(order.begin(), order.end(), name) - order.begin(); };
    std::vector<std::string> sorted = inputs;  // as held or as written in Config.toml: named in CaptureInputs' order
    std::stable_sort(sorted.begin(), sorted.end(), [&rank](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    std::string label;
    for (const std::string& input : sorted) label += (label.empty() ? "" : " + ") + ShortPadLabel(input, style);
    return label;
}

std::vector<std::string> HotkeyInputs(PadHotkey hotkey) {
    const size_t index = static_cast<size_t>(hotkey);
    if (index >= kHotkeyCount) return {};
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadHotkeysLocked();
    return g_hotkeys[index];
}

std::string HotkeyLabel(PadHotkey hotkey) {
    const std::vector<std::string> inputs = HotkeyInputs(hotkey);
    return inputs.empty() ? std::string("None") : PadInputsLabel(inputs);
}

bool SetHotkey(PadHotkey hotkey, const std::vector<std::string>& inputs, std::string& error) {
    const size_t index = static_cast<size_t>(hotkey);
    if (index >= kHotkeyCount) return false;
    const auto sorted = [](std::vector<std::string> names) {
        std::sort(names.begin(), names.end());
        return names;
    };
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadProfileLocked();
    LoadHotkeysLocked();
    if (inputs.size() == 1) {
        // On its own, an input that already does something would stop doing it (the chord keeps it from the game).
        const std::string& input = inputs[0];
        const std::string label = ShortPadLabel(input, std::max(0, aurora_button_icon_style()));
        bool used = input == "Shoulder L" || input == "Thumb R";  // the port's: skip a movie, fast-forward
        for (int c = 0; c < WiimoteProfile::kControlCount && !used; ++c) {
            const std::vector<std::string> names = g_profile.controls[static_cast<size_t>(c)].ReferencedInputs();
            used = std::find(names.begin(), names.end(), input) != names.end();
        }
        if (used) {
            error = label + " already does something in the game: hold it together with another button.";
            return false;
        }
    }
    if (!inputs.empty() && sorted(inputs) == sorted(HomeInputsLocked())) {
        error = "Those buttons open Options.";
        return false;
    }
    for (size_t i = 0; i < kHotkeyCount; ++i) {
        if (i != index && !inputs.empty() && sorted(inputs) == sorted(g_hotkeys[i])) {
            error = std::string("Those buttons are already ") + (i == 0 ? "Save state" : "Load state") + "'s.";
            return false;
        }
    }
    g_hotkeys[index] = inputs;
    ++g_hotkeysGeneration;
    std::string text;
    for (const std::string& input : inputs) text += (text.empty() ? "" : " & ") + Quote(input);
    if (!RuntimeConfigFile::SetControllerExpression(kHotkeyKeys[index], text)) {
        error = "Couldn't save the setting (Config.toml).";
        return false;
    }
    RT_LOGF(RT_TAG_CONFIG, "[hotkey] %s = %s (Options > Controls)\n", kHotkeyKeys[index], text.c_str());
    return true;
}

PadHotkey PollHotkeys(bool allowed) {
    std::array<std::vector<std::string>, kHotkeyCount> chords;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadHotkeysLocked();
        chords = g_hotkeys;
    }
    static std::array<bool, kHotkeyCount> s_complete{};
    if (std::all_of(chords.begin(), chords.end(), [](const std::vector<std::string>& c) { return c.empty(); })) {
        s_complete.fill(false);
        return PadHotkey::Count;
    }
    // While a device scan holds the controllers nothing fires, and what was complete stays so (no second firing after).
    const ControllerGate gate;
    if (!gate) return PadHotkey::Count;
    const std::vector<std::string> held = HeldPadInputs();
    size_t fired = kHotkeyCount;
    for (size_t i = 0; i < kHotkeyCount; ++i) {
        const bool complete = !chords[i].empty() && std::all_of(chords[i].begin(), chords[i].end(), [&held](const std::string& n) {
            return std::find(held.begin(), held.end(), n) != held.end();
        });
        // Two completed at once (one chord inside the other): the one with more buttons.
        if (complete && !s_complete[i] && allowed && (fired == kHotkeyCount || chords[i].size() > chords[fired].size()))
            fired = i;
        s_complete[i] = complete;
    }
    if (fired == kHotkeyCount) return PadHotkey::Count;
    RT_LOGF(RT_TAG_RUNTIME, "[hotkey] frame %u: %s (%s); the game's buttons 0x%04X\n", unsigned(VI_HLE_PresentedFrames()),
            fired == 0 ? "save state" : "load state", PadInputsLabel(chords[fired]).c_str(),
            unsigned(g_lastButtons.load(std::memory_order_relaxed)));
    return static_cast<PadHotkey>(fired);
}

void BeginPadCapture() {
    std::vector<std::string> held = HeldPadInputs();
    std::lock_guard<std::mutex> lock(g_captureMutex);
    g_capturing = true;
    g_captureHeld = std::move(held);
    g_captureDeadlineNs = SDL_GetTicksNS() + 10'000'000'000ull;
}

bool PadCapturing() {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    return g_capturing;
}

std::string PollPadCapture() {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (!g_capturing) return {};
    const std::vector<std::string> held = HeldPadInputs();
    for (const std::string& name : held) {
        if (std::find(g_captureHeld.begin(), g_captureHeld.end(), name) == g_captureHeld.end()) {
            g_capturing = false;
            return name;
        }
    }
    // An input let go since the capture began counts when it is pressed again.
    g_captureHeld.erase(std::remove_if(g_captureHeld.begin(), g_captureHeld.end(), [&held](const std::string& name) {
        return std::find(held.begin(), held.end(), name) == held.end();
    }), g_captureHeld.end());
    if (SDL_GetTicksNS() > g_captureDeadlineNs) g_capturing = false;
    return {};
}

void CancelPadCapture() {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    g_capturing = false;
}

// ---- Rumble ----
namespace {
std::mutex g_rumbleMutex;
std::array<bool, PAD_MAX_CONTROLLERS> g_motorOn{};
std::array<uint64_t, PAD_MAX_CONTROLLERS> g_motorSentNs{};
// A change of the motor that could not reach SDL during a device scan (ControllerGate): RenewRumble sends it.
std::array<bool, PAD_MAX_CONTROLLERS> g_motorUnsent{};
std::atomic<int> g_rumblePercent{100};
bool g_rumblePaused = false;
int g_rumbleLogs = 0;

// The pads that rumble for `chan`: a real Wii Remote on it, and for player 1 every gamepad read through Metroid.ini.
std::vector<SDL_Gamepad*> RumblePads(uint32_t chan) {
    std::vector<SDL_Gamepad*> pads;
    if (SDL_Gamepad* remote = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan))) {
        using WiiRemoteInput::Kind;
        const Kind kind = WiiRemoteInput::KindForName(SDL_GetGamepadName(remote));
        if (kind == Kind::Remote || kind == Kind::RemoteWithNunchuk || kind == Kind::RemoteWithClassic) pads.push_back(remote);
    }
    if (chan == 0) {
        for (SDL_Gamepad* pad : PlayerOnePads())
            if (std::find(pads.begin(), pads.end(), pad) == pads.end()) pads.push_back(pad);
    }
    return pads;
}

// Tests: OTHERM_TEST_RUMBLE_LOG_ONLY=1 logs what would be sent without shaking the developer's controller.
void SendMotorLocked(uint32_t chan, bool on, bool log) {
    static const bool logOnly = std::getenv("OTHERM_TEST_RUMBLE_LOG_ONLY") != nullptr;
    const ControllerGate gate;
    if (!gate) {
        g_motorUnsent[chan] = true;
        return;
    }
    g_motorUnsent[chan] = false;
    const int percent = g_rumblePaused ? 0 : g_rumblePercent.load(std::memory_order_relaxed);
    // A Wii Remote's motor is either on or off; 100% gives a pad's motors half their power, 200% all of it.
    const uint16_t strength = on ? static_cast<uint16_t>(std::clamp(32767 * percent / 100, 0, 65535)) : 0;
    const std::vector<SDL_Gamepad*> pads = RumblePads(chan);
    for (SDL_Gamepad* pad : pads) {
        const bool can = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        bool sent = false;
        if (!logOnly) sent = SDL_RumbleGamepad(pad, strength, strength, strength != 0 ? 1000 : 0);
        if (log) {
            RT_LOGF(RT_TAG_RUNTIME, "[rumble] '%s' (rumble %s): %u%s\n", SDL_GetGamepadName(pad), can ? "yes" : "no",
                    unsigned(strength), logOnly ? " (log only)" : sent ? "" : " - refused");
        }
    }
    if (log && pads.empty()) RT_LOGF(RT_TAG_RUNTIME, "[rumble] no pad to rumble on channel %u\n", chan);
    g_motorSentNs[chan] = SDL_GetTicksNS();
}

// While the motor stays on, SDL's rumble (sent for a second at a time) is renewed.
void RenewRumble() {
    std::lock_guard<std::mutex> lock(g_rumbleMutex);
    const uint64_t now = SDL_GetTicksNS();
    for (uint32_t chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan) {
        if (g_motorUnsent[chan]) {
            SendMotorLocked(chan, g_motorOn[chan] && !g_rumblePaused, false);
        } else if (g_motorOn[chan] && now - g_motorSentNs[chan] > 500'000'000ull) {
            SendMotorLocked(chan, true, false);
        }
    }
}
} // namespace

void SetMotor(uint32_t chan, bool on) {
    if (chan >= PAD_MAX_CONTROLLERS) return;
    std::lock_guard<std::mutex> lock(g_rumbleMutex);
    if (on == g_motorOn[chan]) return;
    g_motorOn[chan] = on;
    const bool log = g_rumbleLogs < 40;
    if (log) {
        ++g_rumbleLogs;
        RT_LOGF(RT_TAG_RUNTIME, "[rumble] channel %u motor %s at frame %u (strength %d%%)\n", chan, on ? "on" : "off",
                unsigned(VI_HLE_PresentedFrames()), g_rumblePercent.load(std::memory_order_relaxed));
    }
    SendMotorLocked(chan, on, log);
}

void SetRumbleStrength(int percent) {
    g_rumblePercent.store(std::clamp(percent, 0, 200), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_rumbleMutex);
    for (uint32_t chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan)
        if (g_motorOn[chan]) SendMotorLocked(chan, true, false);
}

int RumbleStrength() { return g_rumblePercent.load(std::memory_order_relaxed); }

void PreviewRumble() {
    static const bool logOnly = std::getenv("OTHERM_TEST_RUMBLE_LOG_ONLY") != nullptr;
    const ControllerGate gate;
    if (!gate) return;
    const int percent = g_rumblePercent.load(std::memory_order_relaxed);
    const uint16_t strength = static_cast<uint16_t>(std::clamp(32767 * percent / 100, 0, 65535));
    for (SDL_Gamepad* pad : RumblePads(0)) {
        const bool sent = !logOnly && SDL_RumbleGamepad(pad, strength, strength, 250);
        RT_LOGF(RT_TAG_RUNTIME, "[rumble] preview on '%s': %u%s\n", SDL_GetGamepadName(pad), unsigned(strength),
                logOnly ? " (log only)" : sent ? "" : " - refused");
    }
}

void PauseRumble(bool paused) {
    std::lock_guard<std::mutex> lock(g_rumbleMutex);
    if (paused == g_rumblePaused) return;
    g_rumblePaused = paused;
    for (uint32_t chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan)
        if (g_motorOn[chan]) SendMotorLocked(chan, !paused, false);
}

bool Present(uint32_t chan) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const ControllerGate gate; gate) RefreshPadsLocked();  // else the last reading
    return chan == 0 || g_pads[chan].err == PAD_ERR_NONE;
}

bool Sample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    // The gamepads' part of the sample (the pads, the profile, rumble) only with the controllers to itself: during a
    // device scan it is the last frame's again, while keyboard, mouse and touch stay live.
    const ControllerGate gate;
    if (gate) RefreshPadsLocked();
    // Player 1 reads every gamepad straight from SDL through the profile; the pad route serves players 2-4.
    const bool pad = chan != 0 && g_pads[chan].err == PAD_ERR_NONE;
    if (chan != 0 && !pad) {
        return false;
    }
    AnnounceLocked(chan, pad);
    Intent intent;
    if (pad) {
        intent = PadIntent(g_pads[chan]);
    }
    const auto merge = [&intent](const Intent& other) {
        intent.buttons |= other.buttons;
        intent.shake = intent.shake || other.shake;
        if (std::hypot(other.move[0], other.move[1]) > std::hypot(intent.move[0], intent.move[1])) {
            intent.move[0] = other.move[0];
            intent.move[1] = other.move[1];
        }
        if (other.tilt != 0.0f) intent.tilt = other.tilt;
        if (other.attitude) {
            intent.attitude = true;
            intent.pitch = other.pitch;
            intent.roll = other.roll;
            intent.ir[0] = other.ir[0];
            intent.ir[1] = other.ir[1];
        }
    };
    bool mousePointing = false;
    float mousePos[2] = {};
    if (chan == 0) {
        LoadProfileLocked();
        if (!g_keyboardOnAPort) {
            merge(KeyboardIntent());
        }
        static Intent s_profile;
        if (gate) s_profile = ProfileIntent();
        merge(s_profile);
        merge(ScriptIntent());  // after the profile: a scripted Point overrides its resting attitude
        merge(TouchIntent());
        // Right mouse button held: the remote points at the screen, aimed by the mouse; left click is A.
        SDL_MouseButtonFlags mouseButtons = 0;
        // Holding the right button is explicit enough to count while a gamepad is in use too.
        if (MousePosition(mousePos, mouseButtons) && (mouseButtons & SDL_BUTTON_RMASK)) {
            mousePointing = true;
            intent.attitude = true;
            intent.pitch = 0.0f;
            intent.roll = 0.0f;
            if (mouseButtons & SDL_BUTTON_LMASK) intent.buttons |= kWpadA;
        }
    }
    if (chan == 0 && g_heldButtons.load(std::memory_order_relaxed) != 0) {
        if (VI_HLE_RetraceCount() < g_heldUntil.load(std::memory_order_relaxed)) {
            intent.buttons |= g_heldButtons.load(std::memory_order_relaxed);
        } else {
            g_heldButtons.store(0, std::memory_order_relaxed);
        }
    }
    // No InputBlocked() check here: frame_input.cpp / kpad.cpp blank the game's copy after that check.
    sample = {};
    sample.hold = intent.buttons;
    if (chan == 0) {
        // OTHERM_LOG_BUTTONS=1 (tests): player 1's buttons as the game gets them, each time they change.
        static const bool logButtons = std::getenv("OTHERM_LOG_BUTTONS") != nullptr;
        if (logButtons && intent.buttons != g_lastButtons.load(std::memory_order_relaxed))
            RT_LOGF(RT_TAG_CONFIG, "[buttons] frame %u: 0x%04X\n", unsigned(VI_HLE_PresentedFrames()), unsigned(intent.buttons));
        g_lastButtons.store(intent.buttons, std::memory_order_relaxed);
        g_moveX.store(intent.move[0], std::memory_order_relaxed);
        g_moveY.store(intent.move[1], std::memory_order_relaxed);
    }
    if (chan == 0 && gate) RenewRumble();
    Motion& motion = g_motion[chan];
    ApplyMotion(motion, intent, sample);
    // The IR camera sees the sensor bar only while the remote is about level: that is what puts Other M in its
    // first-person view. Aim from the mouse while it points, else from the profile's IR bindings. Only while it is
    // held level, not while it swings through level (upright back to the resting tilt when LT is let go), which
    // would flash the first-person view.
    const float heldPitch = intent.attitude ? intent.pitch : 0.0f;
    if (chan == 0 && std::fabs(motion.pitch) < WiimoteProfile::kIrVisiblePitchRadians &&
        std::fabs(heldPitch) < WiimoteProfile::kIrVisiblePitchRadians && std::fabs(motion.roll) < 1.0f) {
        sample.pointerValid = true;
        sample.pointer[0] = mousePointing ? mousePos[0] : intent.ir[0];
        sample.pointer[1] = mousePointing ? mousePos[1] : intent.ir[1];
    }
    return true;
}

uint32_t LastButtons() { return g_lastButtons.load(std::memory_order_relaxed); }

void HoldButtonsUntil(uint32_t wpadButtons, uint32_t retrace) {
    g_heldUntil.store(retrace, std::memory_order_relaxed);
    g_heldButtons.store(wpadButtons, std::memory_order_relaxed);
}

void MoveDirection(float& x, float& y) {
    x = g_moveX.load(std::memory_order_relaxed);
    y = g_moveY.load(std::memory_order_relaxed);
}

// Save states: the per-port shake and attitude (the rest is read live from the pads each frame).
void SaveState(::SaveState::Writer& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.Pod(g_motion);
}
void LoadState(::SaveState::Reader& in) {
    std::lock_guard<std::mutex> lock(g_mutex);
    in.Pod(g_motion);
}
} // namespace VirtualRemote
