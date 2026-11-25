#pragma once

// Dolphin Wii Remote profiles applied to an ordinary gamepad.
//
// A Dolphin profile (User/Config/Profiles/Wiimote/<name>.ini, or a [WiimoteN] section of WiimoteNew.ini) binds
// each control of the emulated Wii Remote to an input expression ("`Button Y`", "`Left Y+` | `Pad N`",
// "!`Trigger R`"). This evaluates those expressions with the runtime's Dolphin-compatible expression engine
// (input_expr.h) and turns the result into what the game reads from KPAD: buttons, the IR pointer and the tilt
// that shows up on the accelerometer, the way Dolphin's emulated remote would produce them. Metroid: Other M's
// default controls are the developer's own profile, Metroid.ini (kDefaultOtherMProfile, and the copy next to the
// executable, which wins).
//
// Self-contained (no SDL, no runtime state) so tests/wiimote_profile_tests.cpp can check it on its own.

#include "input_expr.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace WiimoteProfile {

enum Control : int {
    kButtonA,
    kButtonB,
    kButton1,
    kButton2,
    kButtonMinus,
    kButtonPlus,
    kButtonHome,
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
    kIrUp,
    kIrDown,
    kIrLeft,
    kIrRight,
    kTiltForward,
    kTiltBackward,
    kTiltLeft,
    kTiltRight,
    kTiltModifier,
    kShakeX,
    kShakeY,
    kShakeZ,
    kControlCount,
};

// Dolphin's key for the control: "Buttons/A", "D-Pad/Up", "IR/Up", "Tilt/Forward", "Shake/X".
const char* DolphinName(int control);

// WPAD_BUTTON_* bits, as the game reads them from KPADStatus.hold.
inline constexpr uint32_t kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                          kWpadPlus = 0x0010, kWpadTwo = 0x0100, kWpadOne = 0x0200, kWpadB = 0x0400,
                          kWpadA = 0x0800, kWpadMinus = 0x1000, kWpadHome = 0x8000;

struct Profile {
    std::array<InputExpr::Expression, kControlCount> controls;
    std::array<std::string, kControlCount> text;
    std::string device;            // "XInput/0/Gamepad"; informational: every connected pad is read
    double tiltMaxDegrees = 85.0;  // Dolphin "Tilt/Angle" (85 is Dolphin's default)
    double tiltModifierRange = 0.5;  // Dolphin "Tilt/Modifier/Range" (50 %)
    int bound = 0;                 // controls with an expression
};

// Reads the [Profile] section (a profile file), else [Wiimote1] (WiimoteNew.ini). Keys this does not model
// (extensions, options, calibration) are skipped and listed in `warnings`. False with `error` set when nothing
// parses.
bool Parse(const std::string& iniText, Profile& out, std::string& error,
           std::vector<std::string>* warnings = nullptr);

// One frame of the remote the profile describes, before motion smoothing.
struct State {
    uint32_t buttons = 0;  // WPAD bits; the D-pad in the remote's own directions, as the profile binds them
    float pitch = 0.0f;    // radians; + = pointer end up (Dolphin Tilt "Backward"), - = down ("Forward")
    float roll = 0.0f;     // radians; + = right side down (Dolphin Tilt "Right")
    float ir[2] = {};      // where the remote aims: -1..1, +x right, +y down (KPAD pos)
    // The D-pad bindings' own values (0..1; a stick's deflection where one is bound), in the remote's directions:
    // up, down, left, right. Other M's 360-degree movement cheat reads the stick's direction from them.
    float dpad[4] = {};
    bool shake = false;
};

State Evaluate(const Profile& profile, const InputExpr::InputSource& source);

// WPAD bits of the buttons (other than HOME) whose binding reads an input HOME's binding also reads: with
// "Buttons/Home = `Start` & `Back`", + (Start) and - (Back). A pad presses those a moment before the chord is
// complete, so the runtime holds them back briefly (virtual_remote.cpp).
uint32_t ButtonsSharingHomeInputs(const Profile& profile);

// Dolphin's emulated IR camera only sees the sensor bar while the remote is roughly level; tilted further the
// game gets no pointer (Other M: back to the third-person view).
inline constexpr float kIrVisiblePitchRadians = 0.35f;  // ~20 degrees

// Accelerometer for a remote at rest in the given attitude, in KPAD's frame (g; flat and level: y = -1;
// pointer end up: z = -1, as Other M reads it).
void AccelerationFor(float pitch, float roll, float acc[3]);

// Metroid.ini as the developer uses it in Dolphin (project/controls/Metroid.ini).
extern const char* const kDefaultOtherMProfile;

} // namespace WiimoteProfile
