// Checks the Dolphin Wii Remote profile reader against Metroid.ini, the developer's Dolphin profile for
// Metroid: Other M: every binding parses, and each gamepad input reaches the Wii Remote control Dolphin gives it.
// Usage: wiimote_profile_tests [path to Metroid.ini]   (no path: the built-in copy)

#include "wiimote_profile.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

static int g_failures = 0;
static std::map<std::string, double> g_inputs;

static void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

static WiimoteProfile::State Run(const WiimoteProfile::Profile& profile, std::map<std::string, double> inputs) {
    g_inputs = std::move(inputs);
    return WiimoteProfile::Evaluate(profile, [](const std::string& name) {
        const auto it = g_inputs.find(name);
        return it == g_inputs.end() ? 0.0 : it->second;
    });
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main(int argc, char** argv) {
    using namespace WiimoteProfile;
    std::string text = kDefaultOtherMProfile;
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) {
            std::printf("cannot read %s\n", argv[1]);
            return 2;
        }
        std::ostringstream all;
        all << file.rdbuf();
        text = all.str();
        Check(text.find("Buttons/A = `Button Y`") != std::string::npos, "the file is Metroid.ini");
    }
    Profile profile;
    std::string error;
    std::vector<std::string> warnings;
    Check(Parse(text, profile, error, &warnings), "parse: " + error);
    Check(profile.bound == 17, "17 bound controls (got " + std::to_string(profile.bound) + ")");
    Check(profile.device == "XInput/0/Gamepad", "device");
    Check(warnings.size() == 1 && warnings[0] == "not modelled: Tilt/Circle Stick", "only Tilt/Circle Stick skipped");

    Check(profile.tiltMaxDegrees == 90.0, "Tilt/Angle = 90");
    const float kPi = 3.14159265358979323846f;
    const float restTilt = 0.28f * kPi / 2.0f;  // Tilt/Forward's 0.28 of 90 degrees

    // Nothing held: the remote is tilted forward (Tilt/Forward's resting term), pointer below the screen: no IR.
    State idle = Run(profile, {});
    Check(idle.buttons == 0, "idle: no buttons");
    Check(Near(idle.pitch, -restTilt), "idle: tilted forward about 25 degrees");
    Check(std::fabs(idle.pitch) > kIrVisiblePitchRadians, "idle: IR not visible (third person)");
    // ...and still about level for Other M's "held level" input (rel_80574ADC: |x| < 0.5, y < -0.8, |z| < 0.5),
    // without which the game never gives control back after the first-person view.
    float idleAcc[3];
    AccelerationFor(idle.pitch, idle.roll, idleAcc);
    Check(std::fabs(idleAcc[0]) < 0.5f && idleAcc[1] < -0.8f && std::fabs(idleAcc[2]) < 0.5f, "idle: held level for Other M");

    // Face buttons, Xbox names.
    Check(Run(profile, {{"Button Y", 1}}).buttons == kWpadA, "Y -> A");
    Check(Run(profile, {{"Button B", 1}}).buttons == kWpadB, "B -> B");
    Check(Run(profile, {{"Button X", 1}}).buttons == kWpadOne, "X -> 1");
    Check(Run(profile, {{"Button A", 1}}).buttons == kWpadTwo, "A -> 2");
    Check(Run(profile, {{"Back", 1}}).buttons == kWpadMinus, "Back -> -");
    Check(Run(profile, {{"Start", 1}}).buttons == kWpadPlus, "Start -> +");
    Check(Run(profile, {{"Start", 1}, {"Back", 1}}).buttons == kWpadHome, "Start + Back -> HOME (Options) only");
    Check(Run(profile, {{"Shoulder R", 1}}).buttons == 0, "RB: unbound");
    Check(ButtonsSharingHomeInputs(profile) == (kWpadPlus | kWpadMinus), "+ and - wait for the HOME chord");
    // In the game's menus ($menu = 1, set by the runtime) X and B swap: B is Wii 1, the menus' BACK.
    Check(Run(profile, {{"$menu", 1}, {"Button B", 1}}).buttons == kWpadOne, "menu: B -> 1 (back)");
    Check(Run(profile, {{"$menu", 1}, {"Button X", 1}}).buttons == kWpadB, "menu: X -> B");
    Check(Run(profile, {{"$menu", 1}, {"Button A", 1}}).buttons == kWpadTwo, "menu: A -> 2 (unchanged)");
    Check(Run(profile, {{"$menu", 1}}).buttons == 0, "menu: nothing held, no buttons");

    // Sideways D-pad: stick or pad left is the remote's Up, right its Down, down its Left, up its Right.
    Check(Run(profile, {{"Left X-", 1}}).buttons == kWpadUp, "stick left -> D-pad Up");
    Check(Run(profile, {{"Pad W", 1}}).buttons == kWpadUp, "pad left -> D-pad Up");
    Check(Run(profile, {{"Left X+", 1}}).buttons == kWpadDown, "stick right -> D-pad Down");
    Check(Run(profile, {{"Pad S", 1}}).buttons == kWpadLeft, "pad down -> D-pad Left");
    Check(Run(profile, {{"Left Y+", 1}}).buttons == kWpadRight, "stick up -> D-pad Right");
    Check(Run(profile, {{"Left Y+", 0.3}}).buttons == 0, "stick barely up: below the press threshold");

    // The same stick aims the pointer.
    State aimLeft = Run(profile, {{"Left X-", 1}});
    Check(Near(aimLeft.ir[0], -1.0f) && Near(aimLeft.ir[1], 0.0f), "stick left aims left");
    State aimUp = Run(profile, {{"Pad N", 1}});
    Check(Near(aimUp.ir[0], 0.0f) && Near(aimUp.ir[1], -1.0f), "pad up aims up (KPAD +y is down)");
    State aimHalf = Run(profile, {{"Left X+", 0.5}, {"Left Y-", 0.25}});
    Check(Near(aimHalf.ir[0], 0.5f) && Near(aimHalf.ir[1], 0.25f), "analog aim");

    // Right trigger levels the remote at the screen: IR visible, first-person view.
    State pointing = Run(profile, {{"Trigger R", 1}});
    Check(Near(pointing.pitch, 0.0f), "R: level");
    Check(std::fabs(pointing.pitch) < kIrVisiblePitchRadians, "R: IR visible");
    Check(Near(Run(profile, {{"Trigger R", 0.5}}).pitch, -restTilt * 0.5f), "R half way: half tilted");
    // Left trigger turns the remote upright, pointer end up, alone or with R: Other M's concentration (LT + A). Its
    // "held vertically" input (rel_805747AC) wants z < -0.8 and |y| < 0.3.
    for (const State& upright : {Run(profile, {{"Trigger L", 1}}), Run(profile, {{"Trigger L", 1}, {"Trigger R", 1}})}) {
        Check(Near(upright.pitch, kPi / 2.0f), "L: pointer end straight up");
        float acc[3];
        AccelerationFor(upright.pitch, upright.roll, acc);
        Check(acc[2] < -0.8f && std::fabs(acc[1]) < 0.3f, "L: held vertically for Other M");
    }

    // Accelerometer, KPAD frame.
    float acc[3];
    AccelerationFor(0.0f, 0.0f, acc);
    Check(Near(acc[0], 0.0f) && Near(acc[1], -1.0f) && Near(acc[2], 0.0f), "level: y = -1");
    AccelerationFor(1.5707963f, 0.0f, acc);
    Check(Near(acc[1], 0.0f) && Near(acc[2], -1.0f), "pointer end up: z = -1");
    AccelerationFor(-1.5707963f, 0.0f, acc);
    Check(Near(acc[2], 1.0f), "pointer end down: z = +1");

    if (g_failures == 0) {
        std::printf("wiimote_profile_tests: all checks passed\n");
        return 0;
    }
    std::printf("wiimote_profile_tests: %d failure(s)\n", g_failures);
    return 1;
}
