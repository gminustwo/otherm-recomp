#include "keybinds.h"

#include "hle_stubs.h"
#include "runtime_config.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>

namespace Keybinds {
namespace {

struct Default {
    const char* key;   // Config.toml [keyboard] key
    const char* name;  // shown in the Keybinds tab
    std::vector<SDL_Scancode> codes;
};

const std::array<Default, kActionCount>& Defaults() {
    static const std::array<Default, kActionCount> defaults = {{
        {"up", "Up", {SDL_SCANCODE_UP}},
        {"down", "Down", {SDL_SCANCODE_DOWN}},
        {"left", "Left", {SDL_SCANCODE_LEFT}},
        {"right", "Right", {SDL_SCANCODE_RIGHT}},
        {"jump", "Jump (2)", {SDL_SCANCODE_X, SDL_SCANCODE_SPACE}},
        {"run", "Fire / Bomb (1)", {SDL_SCANCODE_Z, SDL_SCANCODE_LSHIFT}},
        {"a", "Morph Ball (A)", {SDL_SCANCODE_A}},
        {"shake", "Shake", {SDL_SCANCODE_C, SDL_SCANCODE_LCTRL}},
        {"tilt_left", "Tilt Left", {SDL_SCANCODE_Q}},
        {"tilt_right", "Tilt Right", {SDL_SCANCODE_E}},
        {"plus", "Pause (+)", {SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE}},
        {"minus", "Skip (-)", {SDL_SCANCODE_MINUS, SDL_SCANCODE_TAB}},
    }};
    return defaults;
}

std::mutex g_mutex;
bool g_loaded = false;
std::array<std::vector<SDL_Scancode>, kActionCount> g_codes;
// Save / load state combos (Hotkey), and the keys of one completed, kept from Held() until each is let go.
constexpr const char* kHotkeyKeys[kHotkeyCount] = {"save_state", "load_state"};
const std::array<std::vector<SDL_Scancode>, kHotkeyCount> kHotkeyDefaults = {{{SDL_SCANCODE_F5}, {SDL_SCANCODE_F9}}};
std::array<std::vector<SDL_Scancode>, kHotkeyCount> g_hotkeys;
std::vector<SDL_Scancode> g_hotkeyHeld;
int g_capturing = -1;
std::vector<bool> g_heldAtCapture;  // keys already down when the capture began
bool g_suppress = false;            // a capture just ended: ignore the keyboard until all keys are up

// OTHERM_KEY_SCRIPT (keybinds.h ScriptedKeyEvents): keys by SDL name held between two presented-frame counts.
struct KeyStep {
    SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
    uint32_t from = 0, to = 0;
};
const std::vector<KeyStep>& KeyScript() {
    static const std::vector<KeyStep> steps = [] {
        std::vector<KeyStep> out;
        const char* script = std::getenv("OTHERM_KEY_SCRIPT");
        std::stringstream items(script != nullptr ? script : "");
        std::string item;
        while (std::getline(items, item, ',')) {
            const size_t c2 = item.rfind(':');
            const size_t c1 = c2 == std::string::npos || c2 == 0 ? std::string::npos : item.rfind(':', c2 - 1);
            if (c1 == std::string::npos) continue;
            const SDL_Scancode code = SDL_GetScancodeFromName(item.substr(0, c1).c_str());
            if (code == SDL_SCANCODE_UNKNOWN) continue;
            out.push_back({code, static_cast<uint32_t>(std::strtoul(item.c_str() + c1 + 1, nullptr, 10)),
                           static_cast<uint32_t>(std::strtoul(item.c_str() + c2 + 1, nullptr, 10))});
        }
        return out;
    }();
    return steps;
}

// SDL's keyboard state, with the scripted keys held (tests).
const bool* KeyboardState(int* count) {
    const bool* sdl = SDL_GetKeyboardState(count);
    if (KeyScript().empty()) return sdl;
    static std::array<bool, SDL_SCANCODE_COUNT> merged;
    merged.fill(false);
    for (int i = 0; sdl != nullptr && i < *count && i < SDL_SCANCODE_COUNT; ++i) merged[i] = sdl[i];
    const uint32_t frame = VI_HLE_PresentedFrames();
    for (const KeyStep& step : KeyScript())
        if (frame >= step.from && frame < step.to) merged[step.code] = true;
    *count = SDL_SCANCODE_COUNT;
    return merged.data();
}

std::vector<SDL_Scancode> Parse(const std::string& text) {
    std::vector<SDL_Scancode> codes;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string name = RuntimeConfigFile::Trim(
            std::string_view(text).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!name.empty()) {
            const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
            if (code != SDL_SCANCODE_UNKNOWN) {
                codes.push_back(code);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return codes;
}

std::string Format(const std::vector<SDL_Scancode>& codes, const char* separator) {
    std::string text;
    for (const SDL_Scancode code : codes) {
        if (!text.empty()) {
            text += separator;
        }
        text += SDL_GetScancodeName(code);
    }
    return text;
}

void LoadLocked() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;
    for (int i = 0; i < kActionCount; ++i) {
        const std::string saved = RuntimeConfigFile::KeyboardBinding(Defaults()[i].key);
        g_codes[i] = saved.empty() ? Defaults()[i].codes : Parse(saved);
    }
    for (int i = 0; i < kHotkeyCount; ++i) {
        const std::string saved = RuntimeConfigFile::KeyboardBinding(kHotkeyKeys[i]);
        g_hotkeys[i] = saved.empty() ? kHotkeyDefaults[i] : Parse(saved);  // "None" parses to no keys
    }
}

void SaveLocked(int action) {
    RuntimeConfigFile::SetKeyboardBinding(Defaults()[action].key, Format(g_codes[action], ","));
}

void SaveHotkeyLocked(int hotkey) {
    const std::string text = Format(g_hotkeys[hotkey], ",");
    RuntimeConfigFile::SetKeyboardBinding(kHotkeyKeys[hotkey], text.empty() ? "None" : text);
}

bool KeyDown(const bool* keys, int count, SDL_Scancode code) { return keys != nullptr && code < count && keys[code]; }

// Keys of a combo that is all held join g_hotkeyHeld; each leaves it once let go.
void UpdateHotkeyHeldLocked(const bool* keys, int count) {
    for (const auto& combo : g_hotkeys) {
        if (combo.empty() || !std::all_of(combo.begin(), combo.end(), [&](SDL_Scancode c) { return KeyDown(keys, count, c); }))
            continue;
        for (const SDL_Scancode code : combo)
            if (std::find(g_hotkeyHeld.begin(), g_hotkeyHeld.end(), code) == g_hotkeyHeld.end()) g_hotkeyHeld.push_back(code);
    }
    g_hotkeyHeld.erase(std::remove_if(g_hotkeyHeld.begin(), g_hotkeyHeld.end(),
                                      [&](SDL_Scancode c) { return !KeyDown(keys, count, c); }),
                       g_hotkeyHeld.end());
}

bool AnyKeyDown() {
    int count = 0;
    const bool* keys = KeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        if (keys[i]) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* ActionName(int action) {
    return action >= 0 && action < kActionCount ? Defaults()[action].name : "";
}

std::string KeyLabel(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (action < 0 || action >= kActionCount) {
        return {};
    }
    const std::string text = Format(g_codes[action], " / ");
    return text.empty() ? "None" : text;
}

bool Held(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (g_capturing >= 0 || g_suppress || action < 0 || action >= kActionCount) {
        return false;
    }
    int count = 0;
    const bool* keys = KeyboardState(&count);
    UpdateHotkeyHeldLocked(keys, count);
    for (const SDL_Scancode code : g_codes[action]) {
        if (KeyDown(keys, count, code) && std::find(g_hotkeyHeld.begin(), g_hotkeyHeld.end(), code) == g_hotkeyHeld.end()) {
            return true;
        }
    }
    return false;
}

void BeginCapture(int action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    int count = 0;
    const bool* keys = KeyboardState(&count);
    g_heldAtCapture.assign(static_cast<size_t>(count), false);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        g_heldAtCapture[i] = keys[i];
    }
    g_capturing = action;
}

int Capturing() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capturing;
}

bool PollCapture() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_capturing < 0) {
        return true;
    }
    int count = 0;
    const bool* keys = KeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        const bool before = i < static_cast<int>(g_heldAtCapture.size()) && g_heldAtCapture[i];
        if (!keys[i]) {
            if (before) {
                g_heldAtCapture[i] = false;  // released: a later press of it counts
            }
            continue;
        }
        if (before) {
            continue;
        }
        const auto code = static_cast<SDL_Scancode>(i);
        if (code != SDL_SCANCODE_ESCAPE) {
            g_codes[g_capturing] = {code};
            SaveLocked(g_capturing);
        }
        g_capturing = -1;
        g_suppress = true;
        return true;
    }
    return false;
}

bool Suppressed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_suppress && !AnyKeyDown()) {
        g_suppress = false;
    }
    return g_capturing >= 0 || g_suppress;
}

void ResetDefaults() {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    for (int i = 0; i < kActionCount; ++i) {
        g_codes[i] = Defaults()[i].codes;
        SaveLocked(i);
    }
    for (int i = 0; i < kHotkeyCount; ++i) {
        g_hotkeys[i] = kHotkeyDefaults[i];
        SaveHotkeyLocked(i);
    }
}

std::string HotkeyLabel(int hotkey) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (hotkey < 0 || hotkey >= kHotkeyCount) return {};
    const std::string text = Format(g_hotkeys[hotkey], " + ");
    return text.empty() ? "None" : text;
}

bool SetHotkey(int hotkey, const std::vector<std::string>& keyNames, std::string& error) {
    if (hotkey < 0 || hotkey >= kHotkeyCount) return false;
    std::vector<SDL_Scancode> codes;
    for (const std::string& name : keyNames) {
        const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
        if (code != SDL_SCANCODE_UNKNOWN && std::find(codes.begin(), codes.end(), code) == codes.end()) codes.push_back(code);
    }
    const auto sorted = [](std::vector<SDL_Scancode> c) {
        std::sort(c.begin(), c.end());
        return c;
    };
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    if (codes.size() == 1) {
        // On its own, a key the game uses would stop doing that (the combo keeps it from the game).
        for (int i = 0; i < kActionCount; ++i) {
            if (std::find(g_codes[i].begin(), g_codes[i].end(), codes[0]) != g_codes[i].end()) {
                error = std::string(SDL_GetScancodeName(codes[0])) + " is " + Defaults()[i].name +
                        " in the game: hold it together with another key.";
                return false;
            }
        }
    }
    const int other = hotkey == kSaveState ? kLoadState : kSaveState;
    if (!codes.empty() && sorted(codes) == sorted(g_hotkeys[other])) {
        error = std::string("Those keys are already ") + (other == kSaveState ? "Save state" : "Load state") + "'s.";
        return false;
    }
    g_hotkeys[hotkey] = codes;
    SaveHotkeyLocked(hotkey);
    return true;
}

int HotkeyCompletedBy(int scancode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadLocked();
    int count = 0;
    const bool* keys = KeyboardState(&count);
    int found = -1;
    for (int h = 0; h < kHotkeyCount; ++h) {
        const auto& combo = g_hotkeys[h];
        if (std::find(combo.begin(), combo.end(), static_cast<SDL_Scancode>(scancode)) == combo.end()) continue;
        const bool rest = std::all_of(combo.begin(), combo.end(),
                                      [&](SDL_Scancode c) { return c == scancode || KeyDown(keys, count, c); });
        if (rest && (found < 0 || combo.size() > g_hotkeys[found].size())) found = h;
    }
    return found;
}

std::vector<std::string> HeldKeyNames() {
    int count = 0;
    const bool* keys = KeyboardState(&count);
    std::vector<std::string> names;
    const auto add = [&](int i) {
        if (!KeyDown(keys, count, static_cast<SDL_Scancode>(i)) || i == SDL_SCANCODE_ESCAPE || i == SDL_SCANCODE_BACKSPACE ||
            i == SDL_SCANCODE_DELETE)
            return;
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(i));
        if (name != nullptr && *name != '\0') names.push_back(name);
    };
    for (int i = SDL_SCANCODE_LCTRL; i <= SDL_SCANCODE_RGUI; ++i) add(i);  // modifiers first: "Left Ctrl + S"
    for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)); ++i)
        if (i < SDL_SCANCODE_LCTRL || i > SDL_SCANCODE_RGUI) add(i);
    return names;
}

std::string KeysLabel(const std::vector<std::string>& keyNames) {
    std::string label;
    for (const std::string& name : keyNames) label += (label.empty() ? "" : " + ") + name;
    return label;
}

} // namespace Keybinds

namespace Keybinds {
std::vector<std::pair<int, bool>> ScriptedKeyEvents(uint32_t frame) {
    std::vector<std::pair<int, bool>> out;
    for (const KeyStep& step : KeyScript()) {
        if (frame == step.from) out.push_back({static_cast<int>(step.code), true});
        if (frame == step.to) out.push_back({static_cast<int>(step.code), false});
    }
    return out;
}
} // namespace Keybinds
