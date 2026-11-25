#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The keyboard's sideways Wii Remote, one or two keys per control, changeable from the
// game's settings window (Keybinds tab) and saved to Config.toml's [keyboard] table as SDL
// scancode names ("jump = \"X,Space\"").
namespace Keybinds {

enum Action : int {
    kUp,
    kDown,
    kLeft,
    kRight,
    kJump,      // 2
    kRun,       // 1
    kA,
    kShake,
    kTiltLeft,
    kTiltRight,
    kPlus,
    kMinus,
    kActionCount,
};

const char* ActionName(int action);
// "X / Space", or "None".
std::string KeyLabel(int action);
// Any of the action's keys held (false while a key is being captured, or until it is let go).
bool Held(int action);

// Waits for the next key pressed and binds it to `action` alone; Escape cancels.
void BeginCapture(int action);
// The action waiting for a key, or -1.
int Capturing();
// Polls the keyboard during a capture; true once it has ended (bound or cancelled).
bool PollCapture();
// Keys held from a capture are ignored until they are released.
bool Suppressed();
// Puts the actions' keys and the save / load state keys back to the defaults.
void ResetDefaults();

// Save / load state on the keyboard: keys held together (Options > Controls, the keyboard column; Config.toml
// [keyboard] save_state / load_state, the keys' SDL names joined by commas, "Left Ctrl,S"; "None" for none). F5 and
// F9 unless changed. Once all of a combo's keys are held, Held() leaves them out until they are let go.
enum Hotkey : int { kSaveState, kLoadState, kHotkeyCount };
// "Left Ctrl + S", or "None".
std::string HotkeyLabel(int hotkey);
// Keys by SDL name, as held (no keys: none). False with `error` for one game key on its own or the other hotkey's
// combo.
bool SetHotkey(int hotkey, const std::vector<std::string>& keyNames, std::string& error);
// The hotkey a key going down completes (its other keys already held; the bigger combo if two), or -1.
int HotkeyCompletedBy(int scancode);
// Keys held now by SDL name, modifiers first (Escape, Backspace and Delete left out: they cancel or clear a capture).
std::vector<std::string> HeldKeyNames();
// Key names joined for display: "Left Ctrl + S".
std::string KeysLabel(const std::vector<std::string>& keyNames);

// Tests: OTHERM_KEY_SCRIPT="Left Ctrl:700:1100,S:705:1100" holds keys (SDL names) between two presented-frame counts,
// as OTHERM_PAD_SCRIPT holds pad inputs (a background test window gets no keyboard). This reads them as held; the
// key presses and releases due at `frame` (scancode, down) are for settings_overlay to post as SDL events.
std::vector<std::pair<int, bool>> ScriptedKeyEvents(uint32_t frame);

} // namespace Keybinds
