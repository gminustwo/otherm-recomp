#include "game_folder.h"

#include "disc_image.h"
#include "runtime_config.h"
#include "runtime_log.h"
#if defined(__ANDROID__)
#include "android_bridge.h"
#include "nod_disc.h"
#endif

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace GameFolder {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kGameId = "R3OE01";
// Disc/DATA holds about 7.1 GiB of the disc's files; a little more is asked for.
constexpr uintmax_t kSpaceNeeded = 7'800'000'000ull;

bool IsExtractedDisc(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path / "files", ec) && fs::is_regular_file(path / "sys" / "fst.bin", ec);
}

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The folder the program is in, and the download's top folder (the program sits in its engine/ folder).
fs::path EngineDirectory() {
    const char* base = SDL_GetBasePath();
    fs::path dir = base ? fs::u8path(base) : fs::current_path();
    if (!dir.has_filename()) dir = dir.parent_path();
    return dir;
}

// Remembers the game at `root`, relative to UserData/Config.toml (one level below the game folder) so the folder can move.
void SaveDvdRoot(const fs::path& root) {
    std::error_code ec;
    const fs::path rel = fs::relative(root, RuntimeConfigFile::ResolveConfigPath().parent_path(), ec);
    RuntimeConfigFile::SetDvdRoot(ec || rel.empty() ? root : rel);
}

std::string GigabytesText(uintmax_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / 1e9);
    return text;
}

// ---- nodtool (encounter/nod, MIT / Apache-2.0): Dolphin's compressed images (.rvz, .wia, .gcz) ----

bool IsNodImage(const fs::path& p) {
    const std::string ext = Lower(RuntimeConfigFile::PathToUtf8(p.extension()));
    return ext == ".rvz" || ext == ".wia" || ext == ".gcz";
}

fs::path NodTool() {
#if defined(_WIN32)
    return EngineDirectory() / "nodtool.exe";
#else
    return EngineDirectory() / "nodtool";
#endif
}

// Runs `exe` with `args` (UTF-8) and no window; `output` gets what it writes (stdout and stderr). Its exit code, or
// -1 when it could not start.
int RunTool(const fs::path& exe, const std::vector<std::string>& args, std::string& output) {
#if defined(_WIN32)
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    for (const std::string& arg : args) command += L" \"" + RuntimeConfigFile::PathFromUtf8(arg).wstring() + L"\"";
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe.wstring().c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return -1;
    }
    char buffer[4096];
    DWORD n = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &n, nullptr) && n > 0) output.append(buffer, n);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
#else
    const auto quote = [](const std::string& s) {
        std::string q = "'";
        for (const char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
    };
    std::string command = quote(RuntimeConfigFile::PathToUtf8(exe));
    for (const std::string& arg : args) command += " " + quote(arg);
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) return -1;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0;) output.append(buffer, n);
    const int status = pclose(pipe);
    return status == -1 ? -1 : WEXITSTATUS(status);
#endif
}

// The rest of the line after `prefix` in `text` (the first line that starts with it), or "".
std::string LineAfter(const std::string& text, const std::string& prefix) {
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        at = end + 1;
    }
    return {};
}

bool NodToolReady(std::string& error) {
    std::error_code ec;
    if (fs::is_regular_file(NodTool(), ec)) return true;
    error = "this build reads .rvz, .wia and .gcz images with nodtool, which is missing beside the program. Convert "
            "the image to .iso with Dolphin (right-click the game > Convert File), or download the game again.";
    return false;
}

// `nodtool info`: "Game ID: R3OE01", "Title: ...", "Disc 1, Revision 1" (the lines this file reads).
bool NodProbe(const fs::path& image, DiscImage::Info& info, std::string& error) {
    if (!NodToolReady(error)) return false;
    std::string text;
    const int code = RunTool(NodTool(), {"info", RuntimeConfigFile::PathToUtf8(image)}, text);
    info.id = LineAfter(text, "Game ID: ");
    info.title = LineAfter(text, "Title: ");
    if (code != 0 || info.id.empty()) {
        error = "nodtool could not read it (exit " + std::to_string(code) + "): " + text.substr(0, 400);
        return false;
    }
    info.revision = 0;
    for (size_t at = text.find("Disc "); at != std::string::npos; at = text.find("Disc ", at + 1)) {
        const size_t rev = text.find(", Revision ", at);
        if (rev != std::string::npos && rev - at < 12) {
            info.revision = std::atoi(text.c_str() + rev + 11);
            break;
        }
    }
    return true;
}

// `nodtool extract` writes the data partition's sys/ and files/ into `out`, as DiscImage::Extract does.
bool NodExtract(const fs::path& image, const fs::path& out, std::string& error) {
    if (!NodToolReady(error)) return false;
    std::string text;
    const int code = RunTool(NodTool(), {"extract", RuntimeConfigFile::PathToUtf8(image), RuntimeConfigFile::PathToUtf8(out), "-q"},
                             text);
    if (code != 0 || !IsExtractedDisc(out)) {
        error = "nodtool extract failed (exit " + std::to_string(code) + "): " + text.substr(0, 400);
        return false;
    }
    return true;
}

// The bytes of every file the disc's file table (sys/fst.bin) lists: what Disc/DATA/files holds when it is done.
uintmax_t FstFileBytes(const fs::path& fst) {
    std::ifstream in(fst, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    const auto be = [&](size_t o) {
        return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3]
                                 : 0u;
    };
    const size_t entries = be(8);
    if (d.size() < 12 || entries * 12 > d.size()) return 0;
    uintmax_t total = 0;
    for (size_t i = 1; i < entries; ++i)
        if (d[i * 12] == 0) total += be(i * 12 + 8);  // a file entry: flags 0, offset, size
    return total;
}

uintmax_t FolderBytes(const fs::path& folder) {
    uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

// ---- the set-up on its worker thread ----

std::mutex g_mutex;
SetUpStatus g_status;            // guarded by g_mutex
fs::path g_finishedRoot;         // the game folder to save, once finished ok (saved on the start screen's thread)
fs::path g_staging;              // nodtool's output while it runs (its progress is measured from what it wrote)
std::atomic<float> g_fraction{-1.0f};
Clock::time_point g_started;
Clock::time_point g_nextMeasure;
uintmax_t g_expectedBytes = 0;

void Finish(bool ok, const std::string& message, const fs::path& root) {
    std::lock_guard lock(g_mutex);
    g_status.running = false;
    g_status.finished = true;
    g_status.ok = ok;
    g_status.message = message;
    g_status.seconds = std::chrono::duration<double>(Clock::now() - g_started).count();
    g_finishedRoot = ok ? root : fs::path();
    g_staging.clear();
    RT_LOGF(RT_TAG_RUNTIME, "[launcher] set-up %s after %.0f s: %s\n", ok ? "done" : "failed", g_status.seconds, message.c_str());
}

void ShowProgress(int done, int total) {
    if (total > 0) g_fraction = float(done) / float(total);
}

// Unpacks a disc image into <game folder>/Disc/DATA. "" on success, else why not. `probe` and `extract` read the image
// (`name` in messages, `from` in the log): a file through DiscImage or nodtool, or on Android the document the system's
// picker handed over, through nod.
std::string SetUpFrom(const std::string& name, const std::string& from,
                      const std::function<bool(DiscImage::Info&, std::string&)>& probe,
                      const std::function<bool(const fs::path&, std::string&)>& extract) {
    DiscImage::Info info;
    std::string error;
    if (!probe(info, error)) return name + ": " + error;
    if (info.id != kGameId)
        return "That is " + info.id + " (" + info.title + "), not Metroid: Other M for the USA (R3OE01).";
    if (info.revision != 1)
        return "That is Metroid: Other M USA revision " + std::to_string(info.revision) + "; only revision 1 works.";
    const fs::path disc = GameRoot() / "Disc";
    const fs::path target = disc / "DATA";
    const fs::path staging = disc / "DATA.new";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(disc, ec);
    if (const fs::space_info space = fs::space(disc, ec); !ec && space.available < kSpaceNeeded)
        return "Setting up needs about " + GigabytesText(kSpaceNeeded) + " free in " + RuntimeConfigFile::PathToUtf8(disc) +
               "; there is " + GigabytesText(space.available) + ".";
    RT_LOGF(RT_TAG_RUNTIME, "[launcher] setting up %s revision %d from %s into %s\n", info.id.c_str(), info.revision,
            from.c_str(), RuntimeConfigFile::PathToUtf8(target).c_str());
    if (!extract(staging, error)) {
        fs::remove_all(staging, ec);
        return "The disc could not be set up: " + error;
    }
    fs::remove_all(target, ec);
    fs::rename(staging, target, ec);
    if (ec) return "The disc could not be set up: " + ec.message();
    return {};
}

std::string SetUpFromImage(const fs::path& image) {
    const bool nod = IsNodImage(image);
    return SetUpFrom(
        RuntimeConfigFile::PathToUtf8(image.filename()), RuntimeConfigFile::PathToUtf8(image),
        [&](DiscImage::Info& info, std::string& error) {
            return nod ? NodProbe(image, info, error) : DiscImage::Probe(image, info, error);
        },
        [&](const fs::path& staging, std::string& error) {
            if (!nod) return DiscImage::Extract(image, staging, &ShowProgress, error);
            {
                std::lock_guard lock(g_mutex);
                g_staging = staging;
            }
            return NodExtract(image, staging, error);
        });
}

} // namespace

fs::path Find(const fs::path& path) {
    if (path.empty()) {
        return {};
    }
    if (IsExtractedDisc(path)) {
        return path;
    }
    if (IsExtractedDisc(path / "DATA")) {
        return path / "DATA";
    }
    return {};
}

std::string Problem(const fs::path& root) {
    if (!IsExtractedDisc(root)) {
        return "That folder isn't an extracted game: it needs the files and sys folders Dolphin's Extract "
               "Entire Disc makes.";
    }
    std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
    char header[8] = {};
    if (!boot.read(header, sizeof(header))) {
        return "That folder has no sys/boot.bin, so the game can't be identified.";
    }
    // The disc header carries the revision; the DATA partition's own sys/boot.bin says 0 on this disc, so only the
    // game is checked here (recomp.yml pins main.dol's hash, which is what really matters).
    const std::string id(header, 6);
    if (id != kGameId) {
        return "That is " + id + ", not Metroid: Other M " + kGameId + ". This engine runs only that disc.";
    }
    return {};
}

fs::path GameRoot() {
#if defined(__ANDROID__) || defined(__APPLE__)
    // The app data folder, where its saves and settings are too: on Android the app's own folder on shared storage
    // (Android/data/<app>/files); on macOS ~/Library/Application Support/OtherMRecompiled, not the .app, which can
    // run from a read-only copy (App Translocation) and which an update replaces whole.
    return RuntimeConfigFile::ApplicationDataDirectory();
#else
    // An AppImage's program runs from its read-only mount: the game folder is the one holding the .AppImage file.
    if (const auto& appImage = RuntimeConfigFile::AppImageDirectory()) return *appImage;
    const fs::path engine = EngineDirectory();
    return engine.filename() == "engine" ? engine.parent_path() : engine;
#endif
}

bool Ready() {
#if defined(_WIN32)
    // A "game" folder next to the program needs no questions, and wins over the saved folder.
    if (const fs::path beside = Find(EngineDirectory() / "game"); !beside.empty() && Problem(beside).empty()) {
        if (RuntimeConfigFile::DvdRoot().empty()) {
            RuntimeConfigFile::SetDvdRoot(beside);
        } else {
            RuntimeConfigFile::UseDvdRootForThisRun(beside);
        }
        return true;
    }
#endif
    fs::path configured = Find(RuntimeConfigFile::ResolvedDvdRoot());
    if (configured.empty()) {
        // The download's own Disc/DATA (set up earlier).
        if (const fs::path own = Find(GameRoot() / "Disc" / "DATA"); !own.empty() && Problem(own).empty()) {
            SaveDvdRoot(own);
            configured = own;
        }
    }
    return !configured.empty() && Problem(configured).empty();
}

std::string Describe() {
    const fs::path configured = Find(RuntimeConfigFile::ResolvedDvdRoot());
    if (!configured.empty()) {
        const std::string problem = Problem(configured);
        if (problem.empty()) return "Metroid: Other M (R3OE01) in " + RuntimeConfigFile::PathToUtf8(configured) + ".";
        return RuntimeConfigFile::PathToUtf8(configured) + ": " + problem;
    }
#if defined(__ANDROID__)
    return "Not set up yet: choose your disc image (USA, R3OE01, revision 1: .rvz, .iso, .wia, .gcz, .ciso or .wbfs). "
           "It is set up once; you can delete the image afterwards.";
#else
    return "Not set up yet: choose the folder that holds your disc image (USA, R3OE01, revision 1: .rvz, .iso, .wia, "
           ".gcz, .ciso or .wbfs). It is set up once; you can delete the image afterwards.";
#endif
}

bool IsImage(const fs::path& p) {
    const std::string ext = Lower(RuntimeConfigFile::PathToUtf8(p.extension()));
    return ext == ".iso" || ext == ".ciso" || ext == ".wbfs" || IsNodImage(p);
}

FolderContents Scan(const fs::path& folder) {
    FolderContents out;
    out.extracted = Find(folder);
    std::error_code ec;
    std::vector<std::pair<uintmax_t, fs::path>> images;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_regular_file(entryEc) && IsImage(it->path())) images.emplace_back(it->file_size(entryEc), it->path());
    }
    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [size, path] : images) out.images.push_back(std::move(path));
    return out;
}

void StartSetUp(const fs::path& source) {
    {
        std::lock_guard lock(g_mutex);
        if (g_status.running) return;
        g_status = SetUpStatus{};
        g_status.running = true;
        g_status.source = RuntimeConfigFile::PathToUtf8(source.filename().empty() ? source.parent_path().filename()
                                                                                    : source.filename());
        g_finishedRoot.clear();
        g_staging.clear();
        g_fraction = -1.0f;
        g_expectedBytes = 0;
        g_started = Clock::now();
        g_nextMeasure = g_started;
    }
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        // An extracted game: used where it is.
        const fs::path found = Find(source);
        const std::string problem = found.empty() ? Problem(source) : Problem(found);
        if (problem.empty()) Finish(true, "The game's files in " + RuntimeConfigFile::PathToUtf8(found) + " are used.", found);
        else Finish(false, problem, {});
        return;
    }
    std::thread([source] {
        const std::string problem = SetUpFromImage(source);
        if (problem.empty()) {
            Finish(true, "Set up from " + RuntimeConfigFile::PathToUtf8(source.filename()) + ". You can delete the image now.",
                   GameRoot() / "Disc" / "DATA");
        } else {
            Finish(false, problem, {});
        }
    }).detach();
}

#if defined(__ANDROID__)
void StartSetUpFromDocument(const std::string& uri, const std::string& name) {
    const std::string shown = name.empty() ? std::string("the picked file") : name;
    {
        std::lock_guard lock(g_mutex);
        if (g_status.running) return;
        g_status = SetUpStatus{};
        g_status.running = true;
        g_status.source = shown;
        g_finishedRoot.clear();
        g_staging.clear();
        g_fraction = -1.0f;
        g_expectedBytes = 0;
        g_started = Clock::now();
        g_nextMeasure = g_started;
    }
    // Opened here, on the start screen's thread (Java's), and read on the worker.
    const int fd = AndroidBridge::OpenDocument(uri);
    if (fd < 0) {
        Finish(false, shown + " could not be opened.", {});
        return;
    }
    std::thread([fd, shown] {
        const std::string problem = SetUpFrom(
            shown, "the picked file " + shown,
            [fd](DiscImage::Info& info, std::string& error) { return NodDisc::Probe(fd, info, error); },
            [fd](const fs::path& staging, std::string& error) { return NodDisc::Extract(fd, staging, &ShowProgress, error); });
        close(fd);
        if (problem.empty()) Finish(true, "Set up from " + shown + ". You can delete the image now.", GameRoot() / "Disc" / "DATA");
        else Finish(false, problem, {});
    }).detach();
}
#endif

SetUpStatus Progress() {
    std::lock_guard lock(g_mutex);
    if (g_status.finished && !g_finishedRoot.empty()) {
        SaveDvdRoot(g_finishedRoot);
        g_finishedRoot.clear();
    }
    if (g_status.running) {
        g_status.seconds = std::chrono::duration<double>(Clock::now() - g_started).count();
        // nodtool says nothing while it works: what it wrote so far against what the disc's file table lists,
        // measured twice a second.
        if (!g_staging.empty() && Clock::now() >= g_nextMeasure) {
            g_nextMeasure = Clock::now() + std::chrono::milliseconds(500);
            if (g_expectedBytes == 0) g_expectedBytes = FstFileBytes(g_staging / "sys" / "fst.bin");
            if (g_expectedBytes > 0)
                g_fraction = std::min(1.0f, float(double(FolderBytes(g_staging / "files")) / double(g_expectedBytes)));
        }
        g_status.fraction = g_fraction.load();
    }
    return g_status;
}

void TakeFinished() {
    std::lock_guard lock(g_mutex);
    g_status.finished = false;
}

std::vector<std::pair<std::string, fs::path>> Places() {
    std::vector<std::pair<std::string, fs::path>> places;
    std::error_code ec;
    const auto add = [&](const std::string& name, const fs::path& path) {
        if (!path.empty() && fs::is_directory(path, ec)) places.emplace_back(name, path);
    };
    add("This game's folder", GameRoot());
    // The system's own user folders (moved ones too: OneDrive's Desktop on Windows, XDG's on Linux).
    const auto user = [&](const char* name, SDL_Folder folder) {
        if (const char* path = SDL_GetUserFolder(folder); path != nullptr && *path) {
            fs::path dir = RuntimeConfigFile::PathFromUtf8(path);
            if (!dir.has_filename()) dir = dir.parent_path();
            add(name, dir);
        }
    };
#if !defined(_WIN32)
    user("Home", SDL_FOLDER_HOME);
#endif
    user("Downloads", SDL_FOLDER_DOWNLOADS);
    user("Desktop", SDL_FOLDER_DESKTOP);
    user("Documents", SDL_FOLDER_DOCUMENTS);
#if defined(_WIN32)
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1u << i)) == 0) continue;
        const std::string root = std::string(1, char('A' + i)) + ":\\";
        add("Drive " + root, fs::path(root));
    }
#else
    if (const char* homeText = std::getenv("HOME"); homeText != nullptr && *homeText)
        add("Emulation: Wii games (EmuDeck)", fs::path(homeText) / "Emulation" / "roms" / "wii");
    // SD cards and USB drives: /run/media/<card> (older SteamOS), /run/media/<user>/<card> (newer and desktops).
    for (const char* mounts : {"/run/media", "/media"}) {
        const char* user = std::getenv("USER");
        for (fs::directory_iterator it(mounts, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            if (user != nullptr && it->path().filename() == user) {
                std::error_code inner;
                for (fs::directory_iterator card(it->path(), inner), cardEnd; !inner && card != cardEnd; card.increment(inner))
                    if (card->is_directory(inner)) add("Drive: " + RuntimeConfigFile::PathToUtf8(card->path().filename()), card->path());
            } else {
                add("Drive: " + RuntimeConfigFile::PathToUtf8(it->path().filename()), it->path());
            }
        }
        ec.clear();
    }
    add("The whole system (/)", fs::path("/"));
#endif
    return places;
}

} // namespace GameFolder
