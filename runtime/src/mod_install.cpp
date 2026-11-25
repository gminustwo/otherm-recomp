#include "mod_install.h"

#include "hle/storage/riivolution.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <cryptopp/files.h>
#include <cryptopp/filters.h>
#include <cryptopp/zinflate.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace ModInstall {
namespace {
namespace fs = std::filesystem;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

// The folder holding files/, textures/ or code/ (native code mods), a Riivolution pack or Other M Redux's files
// (riivolution.h FindPackRoot): `dir` itself, or the first one found up to three levels down.
fs::path FindRoot(const fs::path& dir, int depth = 3) {
    std::error_code ec;
    for (const char* sub : {"files", "textures", "code"})
        if (fs::is_directory(dir / sub, ec)) return dir;
    if (fs::path pack = RuntimeRiivolution::FindPackRoot(dir, 0); !pack.empty()) return pack;
    if (depth == 0) return {};
    std::vector<fs::path> subs;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_directory(ec)) subs.push_back(entry.path());
    std::sort(subs.begin(), subs.end());
    for (const fs::path& sub : subs)
        if (fs::path found = FindRoot(sub, depth - 1); !found.empty()) return found;
    return {};
}

// A folder name safe on every system.
std::string SafeName(std::string name) {
    for (char& c : name)
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    return name.empty() || name == ".." ? "mod" : name;
}

// ---- zip archives (store and deflate; zip64 for archives and files over 4 GB) ----
struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint64_t packed = 0, size = 0, local = 0;
};

bool ReadAt(std::ifstream& in, uint64_t at, void* out, size_t size) {
    in.clear();
    in.seekg(std::streamoff(at));
    in.read(static_cast<char*>(out), std::streamsize(size));
    return size_t(in.gcount()) == size;
}

// The central directory's entries.
bool ReadZipDirectory(std::ifstream& in, std::vector<ZipEntry>& entries, std::string& error) {
    in.seekg(0, std::ios::end);
    const uint64_t fileSize = uint64_t(in.tellg());
    // End of central directory: the last "PK\5\6" within the final 64 KB + 22 bytes.
    const uint64_t tailSize = std::min<uint64_t>(fileSize, 0x10000 + 22);
    std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
    if (tailSize < 22 || !ReadAt(in, fileSize - tailSize, tail.data(), tail.size())) {
        error = "not a zip file";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;)
        if (Le32(&tail[i]) == 0x06054B50u) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) {
        error = "not a zip file";
        return false;
    }
    uint64_t count = Le16(&tail[eocd + 10]), dirSize = Le32(&tail[eocd + 12]), dirAt = Le32(&tail[eocd + 16]);
    // Zip64: the locator just before the end record points at the zip64 end record with the real values.
    if (eocd >= 20 && Le32(&tail[eocd - 20]) == 0x07064B50u) {
        uint8_t record[56];
        const uint64_t recordAt = Le64(&tail[eocd - 20 + 8]);
        if (!ReadAt(in, recordAt, record, sizeof record) || Le32(record) != 0x06064B50u) {
            error = "damaged zip64 directory";
            return false;
        }
        count = Le64(record + 32);
        dirSize = Le64(record + 40);
        dirAt = Le64(record + 48);
    }
    if (dirAt + dirSize > fileSize || dirSize > (64ull << 20)) {
        error = "damaged zip";
        return false;
    }
    std::vector<uint8_t> dir(static_cast<size_t>(dirSize));
    if (!ReadAt(in, dirAt, dir.data(), dir.size())) {
        error = "damaged zip";
        return false;
    }
    size_t at = 0;
    for (uint64_t n = 0; n < count; ++n) {
        if (at + 46 > dir.size() || Le32(&dir[at]) != 0x02014B50u) {
            error = "damaged zip";
            return false;
        }
        ZipEntry entry;
        entry.method = Le16(&dir[at + 10]);
        entry.packed = Le32(&dir[at + 20]);
        entry.size = Le32(&dir[at + 24]);
        const uint16_t nameLen = Le16(&dir[at + 28]), extraLen = Le16(&dir[at + 30]), commentLen = Le16(&dir[at + 32]);
        entry.local = Le32(&dir[at + 42]);
        if (at + 46 + size_t(nameLen) + extraLen > dir.size()) {
            error = "damaged zip";
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(&dir[at + 46]), nameLen);
        // Zip64 extended information (0x0001): the values that were 0xFFFFFFFF, in this order.
        for (size_t x = at + 46 + nameLen, end = x + extraLen; x + 4 <= end;) {
            const uint16_t id = Le16(&dir[x]), len = Le16(&dir[x + 2]);
            size_t field = x + 4;
            if (id == 0x0001) {
                for (uint64_t* value : {&entry.size, &entry.packed, &entry.local})
                    if (*value == 0xFFFFFFFFu && field + 8 <= x + 4 + len) {
                        *value = Le64(&dir[field]);
                        field += 8;
                    }
            }
            x += 4 + size_t(len);
        }
        at += 46 + size_t(nameLen) + extraLen + commentLen;
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        entries.push_back(std::move(entry));
    }
    return true;
}

// Whether the zip holds textures/ or code/: those load before the window opens, so the zip is unpacked first.
bool NeedsEarlyInstall(const fs::path& zip) {
    std::ifstream in(zip, std::ios::binary);
    std::vector<ZipEntry> entries;
    std::string error;
    if (!ReadZipDirectory(in, entries, error)) return true;  // reported by the install itself
    for (const ZipEntry& entry : entries) {
        std::string name = entry.name;
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name.rfind("textures/", 0) == 0 || name.rfind("code/", 0) == 0 || name.find("/textures/") != std::string::npos ||
            name.find("/code/") != std::string::npos)
            return true;
    }
    return false;
}

// ---- the worker that unpacks big zips while the start screen is up ----
std::mutex g_installMutex;
std::vector<fs::path> g_installQueue;
std::atomic<bool> g_installBusy{false};
std::string g_installName;
std::atomic<uint64_t> g_installDone{0}, g_installTotal{0};
std::vector<std::string> g_installed;  // this start's installs (mod names)

// Unpacks one dropped zip into its own mod folder.
void InstallZip(const fs::path& zip) {
    std::error_code ec;
    const fs::path mods = Folder();
    const fs::path temp = mods / ".installing";
    fs::remove_all(temp, ec);
    std::string error;
    fs::path modRoot;
    const auto started = std::chrono::steady_clock::now();
    if (ExtractZip(zip, temp, error, [](uint64_t done, uint64_t total) {
            g_installDone.store(done, std::memory_order_relaxed);
            g_installTotal.store(total, std::memory_order_relaxed);
        })) {
        modRoot = FindRoot(temp);
        if (modRoot.empty()) error = "no files/, textures/, code/ folder, Riivolution pack or Other M Redux files inside";
    }
    if (modRoot.empty()) {
        RT_LOGF(RT_TAG_RUNTIME, "mods: %s not installed (%s)\n", RuntimeConfigFile::PathToUtf8(zip.filename()).c_str(), error.c_str());
        fs::rename(zip, fs::path(zip).concat(".not-a-mod"), ec);  // not retried every start
        fs::remove_all(temp, ec);
        return;
    }
    const std::string name = SafeName(RuntimeConfigFile::PathToUtf8(modRoot == temp ? zip.stem() : modRoot.filename()));
    const fs::path target = mods / RuntimeConfigFile::PathFromUtf8(name);
    fs::remove_all(target, ec);  // a newer zip of the same mod replaces it
    fs::rename(modRoot, target, ec);
    fs::remove_all(temp, ec);
    if (ec) {
        RT_LOGF(RT_TAG_RUNTIME, "mods: %s not installed (%s)\n", name.c_str(), ec.message().c_str());
        return;
    }
    fs::remove(zip, ec);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    RT_LOGF(RT_TAG_RUNTIME, "mods: installed %s (%.1f s)\n", name.c_str(), seconds);
    std::lock_guard<std::mutex> lock(g_installMutex);
    g_installed.push_back(name);
}
} // namespace

fs::path Folder() { return RuntimeConfigFile::ApplicationDataDirectory() / "mods"; }

bool ExtractZip(const fs::path& zip, const fs::path& dest, std::string& error,
                const std::function<void(uint64_t done, uint64_t total)>& progress) {
    std::ifstream in(zip, std::ios::binary);
    std::vector<ZipEntry> entries;
    if (!in || !ReadZipDirectory(in, entries, error)) {
        if (error.empty()) error = "can't read the zip";
        return false;
    }
    uint64_t total = 0, done = 0;
    for (const ZipEntry& entry : entries) total += entry.size;
    const fs::path root = fs::absolute(dest).lexically_normal();
    std::vector<char> chunk(1 << 20);
    try {
        for (const ZipEntry& entry : entries) {
            if (entry.name.empty()) continue;
            const fs::path target = (root / RuntimeConfigFile::PathFromUtf8(entry.name)).lexically_normal();
            const auto rel = target.lexically_relative(root);
            if (rel.empty() || *rel.begin() == ".." || fs::path(RuntimeConfigFile::PathFromUtf8(entry.name)).has_root_path())
                throw std::runtime_error("the zip holds a file outside its own folder: " + entry.name);
            if (entry.name.back() == '/') {
                fs::create_directories(target);
                continue;
            }
            uint8_t local[30];
            if (!ReadAt(in, entry.local, local, sizeof local) || Le32(local) != 0x04034B50u)
                throw std::runtime_error("damaged zip entry " + entry.name);
            const uint64_t start = entry.local + 30 + Le16(local + 26) + Le16(local + 28);
            if (entry.method != 0 && entry.method != 8) throw std::runtime_error("unsupported compression in " + entry.name);
            fs::create_directories(target.parent_path());
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("can't write " + RuntimeConfigFile::PathToUtf8(target));
            std::unique_ptr<CryptoPP::Inflator> inflator;
            if (entry.method == 8) inflator = std::make_unique<CryptoPP::Inflator>(new CryptoPP::FileSink(file));
            in.clear();
            in.seekg(std::streamoff(start));
            for (uint64_t left = entry.packed; left > 0;) {
                const size_t n = size_t(std::min<uint64_t>(left, chunk.size()));
                in.read(chunk.data(), std::streamsize(n));
                if (size_t(in.gcount()) != n) throw std::runtime_error("damaged zip entry " + entry.name);
                if (inflator) inflator->Put(reinterpret_cast<const CryptoPP::byte*>(chunk.data()), n);
                else file.write(chunk.data(), std::streamsize(n));
                left -= n;
                if (progress) progress(done + (entry.packed ? (entry.size * (entry.packed - left)) / entry.packed : 0), total);
            }
            if (inflator) inflator->MessageEnd();
            inflator.reset();
            file.close();
            if (!file || fs::file_size(target) != entry.size) throw std::runtime_error("damaged zip entry " + entry.name);
            done += entry.size;
            if (progress) progress(done, total);
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

bool SelfCheck() {
    // A stored zip "My Mod/files/a.txt" installs as mods-like folder content; "../evil.txt" is refused.
    auto zipOf = [](const std::string& name, const std::string& body) {
        std::string z, cd;
        auto u16 = [](std::string& o, uint16_t v) { o += char(v & 255); o += char(v >> 8); };
        auto u32 = [&](std::string& o, uint32_t v) { u16(o, uint16_t(v & 0xFFFF)); u16(o, uint16_t(v >> 16)); };
        u32(z, 0x04034B50u); u16(z, 20); u16(z, 0); u16(z, 0); u16(z, 0); u16(z, 0); u32(z, 0);
        u32(z, uint32_t(body.size())); u32(z, uint32_t(body.size())); u16(z, uint16_t(name.size())); u16(z, 0);
        z += name + body;
        u32(cd, 0x02014B50u); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0);
        u32(cd, uint32_t(body.size())); u32(cd, uint32_t(body.size())); u16(cd, uint16_t(name.size()));
        u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, 0);
        cd += name;
        const uint32_t cdAt = uint32_t(z.size());
        z += cd;
        u32(z, 0x06054B50u); u16(z, 0); u16(z, 0); u16(z, 1); u16(z, 1); u32(z, uint32_t(cd.size())); u32(z, cdAt); u16(z, 0);
        return z;
    };
    const fs::path work = fs::temp_directory_path() / "otherm-modcheck";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    std::ofstream(work / "good.zip", std::ios::binary) << zipOf("My Mod/files/a.txt", "hello");
    std::ofstream(work / "bad.zip", std::ios::binary) << zipOf("../evil.txt", "x");
    std::ofstream(work / "redux.zip", std::ios::binary) << zipOf("redux.rel", "rel");
    std::string error;
    const bool good = ExtractZip(work / "good.zip", work / "out", error) && FindRoot(work / "out") == work / "out" / "My Mod" &&
                      fs::file_size(work / "out" / "My Mod" / "files" / "a.txt", ec) == 5;
    const bool bad = !ExtractZip(work / "bad.zip", work / "out2", error) && !fs::exists(work / "evil.txt", ec);
    // Other M Redux's release zip holds its files at the top, without an XML: the zip's folder is the mod.
    const bool redux = ExtractZip(work / "redux.zip", work / "out3", error) && FindRoot(work / "out3") == work / "out3";
    fs::remove_all(work, ec);
    return good && bad && redux;
}

void StartInstallingDroppedZips() {
    std::error_code ec;
    std::vector<fs::path> zips;
    for (const auto& entry : fs::directory_iterator(Folder(), ec)) {
        std::string ext = RuntimeConfigFile::PathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (entry.is_regular_file(ec) && ext == ".zip") zips.push_back(entry.path());
    }
    std::sort(zips.begin(), zips.end());
    std::vector<fs::path> later;
    for (const fs::path& zip : zips) {
        if (NeedsEarlyInstall(zip)) InstallZip(zip);
        else later.push_back(zip);
    }
    if (later.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        g_installQueue = later;
    }
    g_installBusy.store(true);
    RT_LOGF(RT_TAG_RUNTIME, "mods: installing %zu zip(s) while the start screen is up\n", later.size());
    // Detached: quitting during an install must not end in a joinable thread at exit; the zip stays until it is
    // unpacked whole, so the next start installs it again.
    std::thread([] {
        for (;;) {
            fs::path zip;
            {
                std::lock_guard<std::mutex> lock(g_installMutex);
                if (g_installQueue.empty()) break;
                zip = g_installQueue.front();
                g_installQueue.erase(g_installQueue.begin());
                g_installName = RuntimeConfigFile::PathToUtf8(zip.stem());
            }
            g_installDone.store(0);
            g_installTotal.store(0);
            InstallZip(zip);
        }
        g_installBusy.store(false);
    }).detach();
}

InstallStatus Installing() {
    InstallStatus status;
    status.busy = g_installBusy.load();
    if (!status.busy) return status;
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        status.name = g_installName;
        status.queued = int(g_installQueue.size());
    }
    const uint64_t total = g_installTotal.load(std::memory_order_relaxed);
    status.fraction = total ? float(double(g_installDone.load(std::memory_order_relaxed)) / double(total)) : -1.0f;
    return status;
}

std::vector<std::string> WaitForInstalls() {
    if (g_installBusy.load()) RT_LOGF(RT_TAG_RUNTIME, "mods: the game waits for the zips being installed\n");
    while (g_installBusy.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard<std::mutex> lock(g_installMutex);
    return g_installed;
}

std::vector<Mod> List() {
    std::vector<Mod> mods;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(Folder(), ec)) {
        const std::string name = RuntimeConfigFile::PathToUtf8(entry.path().filename());
        if (entry.is_directory(ec) && !name.empty() && name[0] != '.')
            mods.push_back({name, !fs::exists(entry.path() / "disabled", ec)});
    }
    std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
    return mods;
}

void SetEnabled(const std::string& name, bool enabled) {
    const fs::path marker = Folder() / RuntimeConfigFile::PathFromUtf8(name) / "disabled";
    std::error_code ec;
    if (enabled) fs::remove(marker, ec);
    else std::ofstream(marker).put('\n');
}
} // namespace ModInstall
