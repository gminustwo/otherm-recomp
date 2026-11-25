#pragma once

#include "wii_remote_input.h"

#include <cstdint>
#include <string>
#include <vector>

// Metroid: Other M reads nothing but a bare Wii Remote: held sideways for the third-person view (D-pad moves,
// 1 shoots, 2 jumps, A morphs) and pointed at the screen for the first-person view (IR pointer aims, A and B
// fire). A keyboard or an ordinary gamepad therefore has to become one.
//
// Gamepads follow a Dolphin Wii Remote profile (wiimote_profile.h): Metroid.ini next to the executable, else the
// built-in copy of it, so the controls are the ones the developer plays with in Dolphin:
//
//   Y = A   B = B   X = 1   A = 2   Back = -   Start = +
//   left stick / D-pad = the sideways remote's D-pad, and the pointer while aiming
//   hold RT = point at the screen (first-person view)      hold LT = remote upright (concentration with A)
//
// Every connected gamepad drives player 1 (a real Wii Remote is read by KPAD instead). The keyboard has a
// built-in sideways layout too (Keybinds; arrows, X / Space = 2, Z / Left Shift = 1, A = A, Enter = +, Tab = -),
// and holding the right mouse button points the remote at the screen with the mouse as the pointer (left click = A).
namespace SaveState { class Writer; class Reader; }

namespace VirtualRemote {

// True when `chan` should report a connected Wii Remote driven by a keyboard or gamepad.
bool Present(uint32_t chan);
// Fills one frame of the virtual remote on `chan`; false when Present(chan) is false.
bool Sample(uint32_t chan, WiiRemoteInput::KpadSample& sample);

// On-screen remote and phone motion for player 1 (the Android app's overlay, through JNI). Buttons
// use screen directions, like the keyboard; tilt is -1 (counter-clockwise) .. 1 (clockwise).
enum TouchButton : uint32_t {
    kTouchLeft = 0x001, kTouchRight = 0x002, kTouchUp = 0x004, kTouchDown = 0x008,
    kTouchOne = 0x010, kTouchTwo = 0x020, kTouchA = 0x040, kTouchPlus = 0x080, kTouchMinus = 0x100,
};
void SetTouchState(uint32_t touchButtons, bool shake, float tilt);
// Whether a minigame steered by tilting the remote (with the left stick) is up; Other M has none (always false).
bool InArcade();
// Player 1's Wii Remote buttons (WPAD bits) from keyboard, gamepads and touch, as last handed to the game.
uint32_t LastButtons();
// Holds these WPAD buttons on player 1's remote, besides the player's own, until that VI retrace count
// (otherm_cutscene.cpp presses − for the game's own cutscene skip).
void HoldButtonsUntil(uint32_t wpadButtons, uint32_t retrace);
// Player 1's stick direction from the last Sample, in the game's direction frame (x = the remote's D-pad Up minus
// Down, y = Right minus Left; up to about 1.4 on a diagonal); (0, 0) for digital input. otherm_move.cpp reads it.
void MoveDirection(float& x, float& y);
// Reloads Metroid.ini (or the built-in profile) on the next read.
void ReloadProfile();
// The text of the profile in use: Metroid.ini as read, else the built-in copy (button_icons.cpp draws from it).
std::string ProfileText();

// What the game's on-screen hints should call the Wii Remote motions (hle/otherm_messages.cpp rewrites them): pointing
// at the screen (first-person view; the input Metroid.ini's Tilt/Forward lets go of, "RT") and holding the remote
// vertically (Tilt/Backward's input, "LT"), named for the gamepad in use ("R2" on PlayStation, "ZR" on Nintendo), or the
// mouse when no gamepad is connected. A name is empty when the binding is not one input this can name. `wiiRemote`:
// a real Wii Remote is player 1, so the game's own text is right.
struct MotionHintNames {
    std::string point;
    std::string vertical;
    bool wiiRemote = false;
};
MotionHintNames HintNames();

// Options > Controls, the controller column: the remote controls a gamepad input can be bound to in Metroid.ini.
enum class PadControl : int { A, B, One, Two, Plus, Minus, Point, Upright, Count };
// The one gamepad input (a Dolphin name, "Button Y") that drives `control`, or empty when its binding is not one
// this can name (an edited Metroid.ini).
std::string PadInput(PadControl control);
// That input as the pad in use labels it ("Y", "Triangle", "LT"), "None" when unbound, "(custom)" when not one input.
std::string PadLabel(PadControl control);
// Binds `control` to `input` in Metroid.ini (written next to the executable, then reloaded). A control that had
// `input` takes `control`'s old one, so no two share a button. B and 1 keep their swap in the game's menus ($menu);
// + and - keep out of the HOME chord (Start & Back). False with `error` when the file cannot be written.
bool BindPad(PadControl control, const std::string& input, std::string& error);
// Puts the bindings PadControl covers (and Tilt/Angle) back to the built-in Metroid.ini's.
bool ResetPadBindings(std::string& error);
// Waits for the next gamepad input pressed (a button, a trigger or a stick direction past half way).
void BeginPadCapture();
bool PadCapturing();
// The Dolphin name of the input pressed since BeginPadCapture, once; empty while waiting.
std::string PollPadCapture();
void CancelPadCapture();
// Every input player 1's gamepads hold past half way (Dolphin names, in a fixed order), the scripted pad's included.
std::vector<std::string> HeldPadInputs();
// Inputs as the pad in use labels them, joined: "View + RB".
std::string PadInputsLabel(const std::vector<std::string>& inputs);

// Options > Controls, the Save state and Load state rows: a chord of gamepad inputs held together (Config.toml
// [controller] expr_save_state / expr_load_state, written as a Dolphin expression: "`Back` & `Shoulder R`").
enum class PadHotkey : int { SaveState, LoadState, Count };
std::vector<std::string> HotkeyInputs(PadHotkey hotkey);
// The chord as the pad in use labels it, "None" when there is none.
std::string HotkeyLabel(PadHotkey hotkey);
// Sets the chord (no inputs: none). False with `error` for one that cannot work: one game button on its own, the
// Options chord (HOME), or the other hotkey's chord.
bool SetHotkey(PadHotkey hotkey, const std::vector<std::string>& inputs, std::string& error);
// Once a frame: the hotkey whose chord has just been completed (every input held, the last one just now), else
// Count. `allowed` false (Options is up) only keeps count, so a chord still held when Options closes does not fire.
// A completed chord's inputs stay away from the game until each is let go (ProfileIntent), so View + RB does not
// also press - .
PadHotkey PollHotkeys(bool allowed);

// Rumble (hle/input/wpad.cpp WPADControlMotor): the game switches the Wii Remote's motor on and off; while it is on,
// player 1's gamepads (or the real Wii Remote on that channel) rumble at `percent` of a middling strength (0-200).
void SetMotor(uint32_t chan, bool on);
void SetRumbleStrength(int percent);
int RumbleStrength();
// Options pauses the game: the motors stop until the game runs again.
void PauseRumble(bool paused);
// A quarter-second rumble at the current strength (Options, when the strength changes), so it can be felt.
void PreviewRumble();

// Save states (save_state.h).
void SaveState(::SaveState::Writer& out);
void LoadState(::SaveState::Reader& in);
} // namespace VirtualRemote
