#include "wiimote_profile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace WiimoteProfile {
namespace {

constexpr const char* kNames[kControlCount] = {
    "Buttons/A",    "Buttons/B",     "Buttons/1",     "Buttons/2",      "Buttons/-",     "Buttons/+",
    "Buttons/Home", "D-Pad/Up",      "D-Pad/Down",    "D-Pad/Left",     "D-Pad/Right",   "IR/Up",
    "IR/Down",      "IR/Left",       "IR/Right",      "Tilt/Forward",   "Tilt/Backward", "Tilt/Left",
    "Tilt/Right",   "Tilt/Modifier", "Shake/X",       "Shake/Y",        "Shake/Z",
};

constexpr uint32_t kButtonBits[] = {kWpadA, kWpadB, kWpadOne, kWpadTwo, kWpadMinus, kWpadPlus, kWpadHome,
                                    kWpadUp, kWpadDown, kWpadLeft, kWpadRight};

constexpr double kPi = 3.14159265358979323846;

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

double Value(const Profile& profile, int control, const InputExpr::InputSource& source) {
    const InputExpr::Expression& expr = profile.controls[control];
    if (expr.Empty()) {
        return 0.0;
    }
    const double v = expr.Evaluate(source);
    return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

} // namespace

const char* DolphinName(int control) {
    return control >= 0 && control < kControlCount ? kNames[control] : "";
}

bool Parse(const std::string& iniText, Profile& out, std::string& error, std::vector<std::string>* warnings) {
    out = Profile{};
    // Collect the [Profile] section, else the first [Wiimote1].
    std::vector<std::pair<std::string, std::string>> profileKeys;
    std::vector<std::pair<std::string, std::string>> wiimoteKeys;
    std::vector<std::pair<std::string, std::string>>* current = nullptr;
    std::istringstream in(iniText);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            const std::string section = line.substr(1, line.size() - 2);
            current = section == "Profile" ? &profileKeys : section == "Wiimote1" ? &wiimoteKeys : nullptr;
            continue;
        }
        const size_t eq = line.find('=');
        if (current == nullptr || eq == std::string::npos) {
            continue;
        }
        current->emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    const auto& keys = profileKeys.empty() ? wiimoteKeys : profileKeys;
    if (keys.empty()) {
        error = "no [Profile] or [Wiimote1] section";
        return false;
    }
    for (const auto& [key, value] : keys) {
        if (key == "Device") {
            out.device = value;
            continue;
        }
        if (key == "Tilt/Angle") {
            out.tiltMaxDegrees = std::clamp(std::atof(value.c_str()), 0.0, 180.0);
            continue;
        }
        if (key == "Tilt/Modifier/Range") {
            out.tiltModifierRange = std::clamp(std::atof(value.c_str()) / 100.0, 0.0, 1.0);
            continue;
        }
        int control = -1;
        for (int i = 0; i < kControlCount; ++i) {
            if (key == kNames[i]) {
                control = i;
                break;
            }
        }
        if (control < 0) {
            if (warnings != nullptr) warnings->push_back("not modelled: " + key);
            continue;
        }
        std::string parseError;
        InputExpr::Expression expr;
        if (!InputExpr::Expression::Parse(value, expr, parseError)) {
            if (warnings != nullptr) warnings->push_back(key + ": " + parseError);
            continue;
        }
        if (!expr.Empty()) {
            ++out.bound;
        }
        out.controls[control] = std::move(expr);
        out.text[control] = value;
    }
    if (out.bound == 0) {
        error = "no Wii Remote control has a binding";
        return false;
    }
    return true;
}

uint32_t ButtonsSharingHomeInputs(const Profile& profile) {
    const std::vector<std::string> home = profile.controls[kButtonHome].ReferencedInputs();
    uint32_t bits = 0;
    for (int control = kButtonA; control <= kDpadRight; ++control) {
        if (control == kButtonHome) continue;
        for (const std::string& input : profile.controls[control].ReferencedInputs())
            if (!input.empty() && input[0] != '$' && std::find(home.begin(), home.end(), input) != home.end())
                bits |= kButtonBits[control];
    }
    return bits;
}

State Evaluate(const Profile& profile, const InputExpr::InputSource& source) {
    State state;
    for (int i = kButtonA; i <= kDpadRight; ++i) {
        if (Value(profile, i, source) > InputExpr::kConditionThreshold) {
            state.buttons |= kButtonBits[i];
        }
    }
    for (int i = 0; i < 4; ++i)
        state.dpad[i] = static_cast<float>(std::clamp(Value(profile, kDpadUp + i, source), 0.0, 1.0));
    // Dolphin's IR cursor (absolute input, its default): the stick position is the aim.
    state.ir[0] = static_cast<float>(std::clamp(Value(profile, kIrRight, source) - Value(profile, kIrLeft, source), -1.0, 1.0));
    state.ir[1] = static_cast<float>(std::clamp(Value(profile, kIrDown, source) - Value(profile, kIrUp, source), -1.0, 1.0));
    // Dolphin's Tilt: y = Forward - Backward, x = Right - Left, scaled to the maximum angle.
    double forward = std::clamp(Value(profile, kTiltForward, source) - Value(profile, kTiltBackward, source), -1.0, 1.0);
    double right = std::clamp(Value(profile, kTiltRight, source) - Value(profile, kTiltLeft, source), -1.0, 1.0);
    if (Value(profile, kTiltModifier, source) > InputExpr::kConditionThreshold) {
        forward *= profile.tiltModifierRange;
        right *= profile.tiltModifierRange;
    }
    const double maxRadians = profile.tiltMaxDegrees * kPi / 180.0;
    state.pitch = static_cast<float>(-forward * maxRadians);
    state.roll = static_cast<float>(right * maxRadians);
    state.shake = Value(profile, kShakeX, source) > InputExpr::kConditionThreshold ||
                  Value(profile, kShakeY, source) > InputExpr::kConditionThreshold ||
                  Value(profile, kShakeZ, source) > InputExpr::kConditionThreshold;
    return state;
}

void AccelerationFor(float pitch, float roll, float acc[3]) {
    // Gravity in the remote's frame: x right, y out of the button face, z toward the pointer end. Raising the
    // pointer end by `pitch` moves gravity onto -z; lowering the right side by `roll` onto +x. The z sign is the
    // game's: Other M's "held vertically" input (rel_805747AC, its concentration) wants z < -0.8 with |y| < 0.3,
    // and its tutorial shows the remote turned upright pointer end up; with +z there, missiles refilled only with
    // the pointer end held down (NOTES.md 2026-11-08, runs conc2 / conc3). The x sign is not checked.
    acc[0] = std::sin(roll) * std::cos(pitch);
    acc[1] = -std::cos(roll) * std::cos(pitch);
    acc[2] = -std::sin(pitch);
}

// The developer's Dolphin profile for Metroid: Other M, verbatim (project/controls/Metroid.ini). The
// remote is held sideways: the D-pad bindings are already turned a quarter (stick left = the remote's Up), the
// IR pointer follows the left stick or D-pad, and the remote is tilted forward (pointer below the screen, so no IR:
// third-person view) unless the right trigger is held, which levels it at the screen for the first-person view.
// The left trigger turns it upright, pointer end up (concentration: LT + A refills missiles), with or without the
// right trigger. The resting tilt is 0.28 of Tilt/Angle = 90, about 25 degrees: just past the IR cut-off
// (kIrVisiblePitchRadians, ~20 degrees) and still level enough for Other M's "held level" input (rel_80574ADC),
// without which it never gives control back after a first-person view; upright needs more than ~73 degrees
// (rel_805747AC: |y| < 0.3), so the angle is 90, not Dolphin's default 85. The forward term drops out while
// either trigger is held, so Forward - Backward never cancels out (Dolphin's Tilt subtracts them too).
// HOME (the port's Options menu, settings_overlay FilterMenuInput) is Start + Back pressed together (Steam keeps the
// Guide button for itself); alone they are + and -. In the game's menus ($menu, virtual_remote.cpp) X and B swap:
// Wii 1, the menus' BACK, is on B there.
const char* const kDefaultOtherMProfile =
    "[Profile]\n"
    "Device = XInput/0/Gamepad\n"
    "Buttons/A = `Button Y`\n"
    "Buttons/B = `Button B` & !$menu | `Button X` & $menu\n"
    "Buttons/1 = `Button X` & !$menu | `Button B` & $menu\n"
    "Buttons/2 = `Button A`\n"
    "Buttons/- = Back & !Start\n"
    "Buttons/+ = Start & !Back\n"
    "Buttons/Home = Start & Back\n"
    "IR/Up = `Left Y+` | `Pad N`\n"
    "IR/Down = `Left Y-` | `Pad S`\n"
    "IR/Left = `Left X-` | `Pad W`\n"
    "IR/Right = `Left X+` | `Pad E`\n"
    "Tilt/Circle Stick = 55.000000000000007\n"
    "Tilt/Angle = 90\n"
    "Tilt/Forward = (!`Trigger R` & !`Trigger L`) * 0.28\n"
    "Tilt/Backward = `Trigger L`\n"
    "D-Pad/Up = `Pad W` | `Left X-`\n"
    "D-Pad/Down = `Pad E` | `Left X+`\n"
    "D-Pad/Left = `Pad S` | `Left Y-`\n"
    "D-Pad/Right = `Pad N` | `Left Y+`\n";

} // namespace WiimoteProfile
