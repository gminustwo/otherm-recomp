// The GPU's load from Linux's amdgpu sysfs files, sampled ten times a second on a thread of its own: a read of
// gpu_busy_percent takes ~35 us on the Steam Deck but once took 2.6 ms, too long for the game thread
// (NOTES.md 2026-11-24). The Deck's one-second means swing (99%, 78%, 99% at 4.5x), so the FPS counter's note uses
// the three-second mean; dynamic resolution acts on the one-second mean with the clock it ran at.
#include "gpu_load.h"

#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace GpuLoad {
namespace {

std::atomic<int> g_busy1s{-1};
std::atomic<int> g_busy3s{-1};
std::atomic<int> g_clockPermille{1000};  // clockFraction * 1000

#if defined(__linux__)
struct Device {
    std::string busyFile;
    std::string clockFile;  // pp_dpm_sclk, or empty
};

// pp_dpm_sclk lists the clock levels ("0: 200Mhz", "1: 1100Mhz *", ...); the current one is marked with '*' and may
// be a value between the levels ("1: 1040Mhz *"). Returns the current and the highest listed, in MHz.
bool ReadClock(const std::string& file, int& current, int& highest) {
    std::FILE* f = std::fopen(file.c_str(), "r");
    if (f == nullptr) return false;
    char line[128];
    current = -1;
    highest = -1;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        const char* colon = std::strchr(line, ':');
        if (colon == nullptr) continue;
        const int mhz = std::atoi(colon + 1);
        if (mhz <= 0) continue;
        highest = std::max(highest, mhz);
        if (std::strchr(colon, '*') != nullptr) current = mhz;
    }
    std::fclose(f);
    return current > 0 && highest > 0;
}

int ReadBusy(const std::string& file) {
    std::FILE* f = std::fopen(file.c_str(), "r");
    if (f == nullptr) return -1;
    int value = -1;
    if (std::fscanf(f, "%d", &value) != 1) value = -1;
    std::fclose(f);
    return value;
}

std::vector<Device> FindDevices() {
    std::vector<Device> found;
    std::vector<std::string> seen;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;  // not card0-eDP-1
        std::error_code fileEc;
        const auto dir = entry.path() / "device";
        const std::string canonical = std::filesystem::canonical(dir, fileEc).string();
        if (fileEc || !std::filesystem::exists(dir / "gpu_busy_percent", fileEc)) continue;
        if (std::find(seen.begin(), seen.end(), canonical) != seen.end()) continue;
        seen.push_back(canonical);
        Device device{(dir / "gpu_busy_percent").string(), std::string()};
        if (std::filesystem::exists(dir / "pp_dpm_sclk", fileEc)) device.clockFile = (dir / "pp_dpm_sclk").string();
        found.push_back(std::move(device));
    }
    return found;
}

void Sample(std::vector<Device> devices) {
    // Ten samples a second; with several GPUs the busiest one counts (the one the game draws on).
    std::array<int, 30> busyRing{};
    std::array<int, 10> clockRing{};
    size_t next = 0, filled = 0;
    for (;;) {
        int busiest = -1, clockPermille = 1000;
        for (const auto& device : devices) {
            const int busy = ReadBusy(device.busyFile);
            if (busy < busiest) continue;
            busiest = busy;
            int current = 0, highest = 0;
            clockPermille = !device.clockFile.empty() && ReadClock(device.clockFile, current, highest)
                                ? std::clamp(current * 1000 / highest, 1, 1000)
                                : 1000;
        }
        if (busiest >= 0) {
            busyRing[next % busyRing.size()] = busiest;
            clockRing[next % clockRing.size()] = clockPermille;
            ++next;
            filled = std::min(filled + 1, busyRing.size());
            if (next % 10 == 0) {
                int sum1 = 0, sum3 = 0, clocks = 0;
                for (size_t i = 0; i < filled; ++i) sum3 += busyRing[i];
                for (size_t i = 0; i < 10; ++i) {
                    sum1 += busyRing[(next - 1 - i) % busyRing.size()];
                    clocks += clockRing[(next - 1 - i) % clockRing.size()];
                }
                g_busy1s.store(sum1 / 10, std::memory_order_relaxed);
                g_busy3s.store(sum3 / static_cast<int>(filled), std::memory_order_relaxed);
                g_clockPermille.store(clocks / 10, std::memory_order_relaxed);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
#endif

}  // namespace

void Start() {
#if defined(__linux__)
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        std::vector<Device> devices = FindDevices();
        if (devices.empty()) {
            RT_LOGF(RT_TAG_RUNTIME, "[gpu] no GPU load reported here: no GPU LIMITED note, dynamic resolution stays at Auto\n");
            return;
        }
        RT_LOGF(RT_TAG_RUNTIME, "[gpu] load from %s%s%s\n", devices.front().busyFile.c_str(),
                devices.front().clockFile.empty() ? "" : " (with its clock)",
                devices.size() > 1 ? " and other GPUs: the busiest" : "");
        std::thread(Sample, std::move(devices)).detach();
    });
#endif
}

bool Available() { return g_busy1s.load(std::memory_order_relaxed) >= 0; }

Reading Latest() {
    Reading reading;
    reading.busy1s = g_busy1s.load(std::memory_order_relaxed);
    reading.busy3s = g_busy3s.load(std::memory_order_relaxed);
    reading.clockFraction = g_clockPermille.load(std::memory_order_relaxed) / 1000.0;
    reading.valid = reading.busy1s >= 0;
    return reading;
}

bool IsSteamDeck() {
#if defined(__linux__)
    static const bool deck = [] {
        const auto read = [](const char* path) {
            std::string text;
            if (std::FILE* f = std::fopen(path, "r")) {
                char buffer[64] = {};
                if (std::fgets(buffer, sizeof buffer, f) != nullptr) text = buffer;
                std::fclose(f);
            }
            while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
            return text;
        };
        const std::string vendor = read("/sys/class/dmi/id/sys_vendor");
        const std::string product = read("/sys/class/dmi/id/product_name");
        return vendor == "Valve" && (product == "Jupiter" || product == "Galileo");
    }();
    return deck;
#else
    return false;
#endif
}

}  // namespace GpuLoad
