#include "dolphin_import.h"

#include "nand_path.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "tray_icon.h"

#include <aurora/aurora.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include "android_bridge.h"
#endif

namespace DolphinImport {
namespace fs = std::filesystem;
namespace {

std::string Trim(std::string text) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

// [General] NANDRootPath and LoadPath of a Dolphin.ini (Dolphin writes them with forward slashes and a trailing one).
void ReadDolphinIni(const fs::path& ini, fs::path& nand, fs::path& load) {
    std::ifstream in(ini);
    std::string line, section;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        if (section != "General") continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equals)), value = Trim(line.substr(equals + 1));
        if (value.empty()) continue;
        if (key == "NANDRootPath") nand = RuntimeConfigFile::PathFromUtf8(value);
        if (key == "LoadPath") load = RuntimeConfigFile::PathFromUtf8(value);
    }
}

bool IsDirectory(const fs::path& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(path, ec);
}

// A Dolphin user folder (Config/Dolphin.ini beside Wii/ and Load/; on Linux the ini can live elsewhere).
void AddUser(std::vector<Install>& out, const fs::path& user, const fs::path& ini) {
    if (!IsDirectory(user)) return;
    Install install{user, {}, {}};
    ReadDolphinIni(ini, install.nand, install.load);
    if (install.nand.empty()) install.nand = user / "Wii";
    if (install.load.empty()) install.load = user / "Load";
    std::error_code ec;
    for (const Install& seen : out)
        if (fs::equivalent(seen.nand, install.nand, ec) && fs::equivalent(seen.load, install.load, ec)) return;
    out.push_back(std::move(install));
}

// A folder picked in Options: Dolphin's user folder, a portable install (its User folder), or a NAND itself.
void AddPicked(std::vector<Install>& out, const fs::path& picked) {
    if (IsDirectory(picked / "title")) {
        out.push_back({{}, picked, {}});
        return;
    }
    for (const fs::path& user : {picked, picked / "User"})
        if (IsDirectory(user / "Config") || IsDirectory(user / "Wii")) AddUser(out, user, user / "Config" / "Dolphin.ini");
}

std::vector<Install> Discover() {
    std::vector<Install> out;
    if (const std::string picked = RuntimeConfigFile::DolphinFolder(); !picked.empty())
        AddPicked(out, RuntimeConfigFile::PathFromUtf8(picked));
#if defined(_WIN32)
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Dolphin Emulator", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH] = {};
        DWORD size = sizeof(value) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"UserConfigPath", nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS &&
            type == REG_SZ && value[0] != 0) {
            const fs::path user(value);
            AddUser(out, user, user / "Config" / "Dolphin.ini");
        }
        RegCloseKey(key);
    }
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && appdata[0] != 0) {
        const fs::path user = RuntimeConfigFile::PathFromUtf8(appdata) / "Dolphin Emulator";
        AddUser(out, user, user / "Config" / "Dolphin.ini");
    }
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents != nullptr) {
        const fs::path user = fs::path(documents) / "Dolphin Emulator";
        AddUser(out, user, user / "Config" / "Dolphin.ini");
    }
    CoTaskMemFree(documents);
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        const fs::path user = fs::path(home) / "Library" / "Application Support" / "Dolphin";
        AddUser(out, user, user / "Config" / "Dolphin.ini");
    }
#else
    const char* home = std::getenv("HOME");
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const fs::path dataHome = data != nullptr && data[0] ? fs::path(data) : home ? fs::path(home) / ".local" / "share" : fs::path();
    const fs::path configHome = config != nullptr && config[0] ? fs::path(config) : home ? fs::path(home) / ".config" : fs::path();
    if (!dataHome.empty()) AddUser(out, dataHome / "dolphin-emu", configHome / "dolphin-emu" / "Dolphin.ini");
    if (home != nullptr) {
        AddUser(out, fs::path(home) / ".dolphin-emu", fs::path(home) / ".dolphin-emu" / "Config" / "Dolphin.ini");
        const fs::path flatpak = fs::path(home) / ".var" / "app" / "org.DolphinEmu.dolphin-emu";
        AddUser(out, flatpak / "data" / "dolphin-emu", flatpak / "config" / "dolphin-emu" / "Dolphin.ini");
    }
#endif
    return out;
}

std::mutex g_mutex;
std::vector<Install> g_installs;
std::chrono::steady_clock::time_point g_installsAt{};
bool g_installsValid = false;

// The newest write time and the number of files below a folder.
Save Describe(const fs::path& data) {
    Save save{data, {}, 0};
    std::error_code ec;
    for (fs::recursive_directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        ++save.files;
        save.newest = std::max(save.newest, it->last_write_time(fileError));
    }
    return save;
}

std::string Stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d_%H%M%S", &local);
    return text;
}

} // namespace

std::string GameId() {
    static std::string id;
    if (id.empty()) {
        std::ifstream boot(RuntimeConfigFile::ResolvedDvdRoot() / "sys" / "boot.bin", std::ios::binary);
        char text[6] = {};
        if (boot.read(text, 6)) {
            const std::string found(text, 6);
            if (std::all_of(found.begin(), found.end(), [](unsigned char c) { return std::isalnum(c); })) id = found;
        }
    }
    return id.empty() ? std::string("R3OE01") : id;
}

std::string TitleFolder() {
    const std::string id = GameId();
    char text[9];
    std::snprintf(text, sizeof text, "%02x%02x%02x%02x", static_cast<unsigned char>(id[0]), static_cast<unsigned char>(id[1]),
                  static_cast<unsigned char>(id[2]), static_cast<unsigned char>(id[3]));
    return text;
}

const std::vector<Install>& Installs() {
    std::lock_guard lock(g_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!g_installsValid || now - g_installsAt > std::chrono::seconds(2)) {
        g_installs = Discover();
        g_installsAt = now;
        g_installsValid = true;
    }
    return g_installs;
}

void Refresh() {
    std::lock_guard lock(g_mutex);
    g_installsValid = false;
}

std::optional<Save> FindSave() {
    // Described once per folder until Refresh: Options asks every frame.
    static std::map<fs::path, std::pair<std::chrono::steady_clock::time_point, Save>> cache;
    std::optional<Save> best;
    for (const Install& install : Installs()) {
        const fs::path data = install.nand / "title" / "00010000" / TitleFolder() / "data";
        if (!IsDirectory(data)) continue;
        auto& [at, save] = cache[data];
        if (save.data.empty() || std::chrono::steady_clock::now() - at > std::chrono::seconds(2)) {
            save = Describe(data);
            at = std::chrono::steady_clock::now();
        }
        if (save.files > 0 && (!best || save.newest > best->newest)) best = save;
    }
    return best;
}

Save PortSave() {
    static std::chrono::steady_clock::time_point at{};
    static Save save;
    if (save.data.empty() || std::chrono::steady_clock::now() - at > std::chrono::seconds(2)) {
        save = Describe(RuntimeNandPath::DiscoverNandRootPath() / "title" / "00010000" / TitleFolder() / "data");
        at = std::chrono::steady_clock::now();
    }
    return save;
}

fs::path TexturePack() {
    for (const Install& install : Installs()) {
        if (install.load.empty()) continue;
        const fs::path pack = install.load / "Textures" / GameId();
        if (IsDirectory(pack)) return pack;
    }
    return {};
}

size_t CountTextures(const fs::path& folder) {
    static std::map<fs::path, size_t> counted;
    if (const auto it = counted.find(folder); it != counted.end()) return it->second;
    size_t count = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        std::string ext = RuntimeConfigFile::PathToUtf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".dds" || ext == ".png") ++count;
    }
    counted[folder] = count;
    return count;
}

std::string ImportSave(const Save& from, fs::path& backup) {
    std::error_code ec;
    const fs::path target = RuntimeNandPath::DiscoverNandRootPath() / "title" / "00010000" / TitleFolder() / "data";
    if (!IsDirectory(from.data)) return "Dolphin's save folder is gone.";
    if (fs::equivalent(from.data, target, ec)) return "That is this game's own save folder.";
    backup.clear();
    if (IsDirectory(target) && !fs::is_empty(target, ec)) {
        backup = RuntimeConfigFile::ApplicationDataDirectory() / "save_backups" / Stamp() / TitleFolder() / "data";
        fs::create_directories(backup.parent_path(), ec);
        if (!ec) fs::copy(target, backup, fs::copy_options::recursive, ec);
        if (ec) return "Couldn't back up the saves here (" + ec.message() + "); nothing was changed.";
    }
    // Copy beside the target first, so a failed copy leaves the old save in place.
    const fs::path staging = target.parent_path() / "data.importing";
    fs::remove_all(staging, ec);
    ec.clear();
    fs::create_directories(target.parent_path(), ec);
    if (!ec) fs::copy(from.data, staging, fs::copy_options::recursive, ec);
    // The copies keep Dolphin's write times, so Options shows when the save was made rather than when it was copied.
    for (fs::recursive_directory_iterator it(from.data, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        const fs::path copy = staging / it->path().lexically_relative(from.data);
        fs::last_write_time(copy, it->last_write_time(fileError), fileError);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        return "Couldn't copy Dolphin's save (" + ec.message() + "); nothing was changed.";
    }
    fs::remove_all(target, ec);
    if (!ec) fs::rename(staging, target, ec);
    if (ec) return "Couldn't put Dolphin's save in place (" + ec.message() + "). The old one is in save_backups.";
    RT_LOGF(RT_TAG_NAND, "imported the Dolphin save %s (%d files) into %s; backup %s\n",
            RuntimeConfigFile::PathToUtf8(from.data).c_str(), from.files, RuntimeConfigFile::PathToUtf8(target).c_str(),
            backup.empty() ? "(none: no save here)" : RuntimeConfigFile::PathToUtf8(backup).c_str());
    return {};
}

std::string FormatDate(fs::file_time_type time, bool withTime) {
    // file_clock to system_clock without C++20 clock_cast: both clocks tick now.
    const auto system = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(time - fs::file_time_type::clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%b %d, %Y", &local);
    if (text[4] == '0') std::memmove(text + 4, text + 5, std::strlen(text + 4));  // "May 05" -> "May 5"
    if (!withTime) return text;
    char clock[16];
    std::strftime(clock, sizeof clock, " %H:%M", &local);
    return std::string(text) + clock;
}

void ApplyTexturePacks() {
    const bool enabled = RuntimeConfigFile::TextureReplacements(true);
    const bool dumps = RuntimeConfigFile::TextureDumps(false);
    std::vector<std::string> folders;
    if (RuntimeConfigFile::DolphinTextures(false))
        if (const fs::path pack = TexturePack(); !pack.empty()) folders.push_back(RuntimeConfigFile::PathToUtf8(pack));
    std::vector<const char*> names;
    for (const std::string& folder : folders) names.push_back(folder.c_str());
    const bool prefetch = RuntimeConfigFile::PrefetchTextures(false);
    aurora_set_texture_prefetch(prefetch);
    aurora_set_texture_packs(enabled, dumps, names.data(), names.size());
    RT_LOGF(RT_TAG_RUNTIME, "texture packs: %s%s%s%s%s\n", enabled ? "on" : "off", dumps ? ", dumping" : "",
            prefetch ? ", prefetch" : "", folders.empty() ? "" : ", Dolphin's pack ",
            folders.empty() ? "" : folders[0].c_str());
}

void RestartGame() {
    TrayIcon::Remove();  // the new process adds its own
    // Back into the game, not the start screen (main.cpp LauncherWanted): the new process inherits this.
#if defined(_WIN32)
    SetEnvironmentVariableW(L"OTHERM_SKIP_LAUNCHER", L"1");
#else
    setenv("OTHERM_SKIP_LAUNCHER", "1", 1);
#endif
#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "restart: could not start the game again (error %lu)\n", GetLastError());
    }
    TerminateProcess(GetCurrentProcess(), 0);
#elif defined(__ANDROID__)
    // The environment doesn't reach a new process there: RestartActivity starts the game past the start screen itself.
    AndroidBridge::RestartApp();
#else
    char exe[4096] = {};
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (length > 0) execl(exe, exe, static_cast<char*>(nullptr));
#endif
    std::_Exit(0);
}

} // namespace DolphinImport
