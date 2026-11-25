#include "vulkan_probe.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace VulkanProbe {
namespace {

Result g_last;

#if defined(_WIN32)
// The few Vulkan 1.0 declarations the check needs (the build has no Vulkan headers; this ABI never changes). The
// layouts match vulkan_core.h: VkApplicationInfo, VkInstanceCreateInfo, the head of VkPhysicalDeviceProperties and
// all of VkPhysicalDeviceMemoryProperties.
using VkInstance = struct VkInstance_T*;
using VkPhysicalDevice = struct VkPhysicalDevice_T*;
using VkResult = int32_t;
constexpr VkResult kVkSuccess = 0, kVkIncomplete = 5;
struct VkApplicationInfo {
    int32_t sType;  // VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
    int32_t sType;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
    const void* pNext;
    uint32_t flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};
// VkPhysicalDeviceProperties is 824 bytes; only its head is read: apiVersion, driverVersion, vendorID, deviceID,
// deviceType, deviceName[256].
struct alignas(8) PropertiesBuffer {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int32_t deviceType;  // 1 integrated, 2 discrete, 3 virtual, 4 CPU
    char deviceName[256];
    unsigned char rest[2048];
};
struct VkMemoryType { uint32_t propertyFlags, heapIndex; };
struct VkMemoryHeap { uint64_t size; uint32_t flags; };  // flags bit 0: VK_MEMORY_HEAP_DEVICE_LOCAL_BIT
struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
};
using PFN_vkVoidFunction = void(__stdcall*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(__stdcall*)(VkInstance, const char*);
using PFN_vkEnumerateInstanceVersion = VkResult(__stdcall*)(uint32_t*);
using PFN_vkCreateInstance = VkResult(__stdcall*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(__stdcall*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult(__stdcall*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties = void(__stdcall*)(VkPhysicalDevice, PropertiesBuffer*);
using PFN_vkGetPhysicalDeviceMemoryProperties = void(__stdcall*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);

constexpr uint32_t kVulkan13 = (1u << 22) | (3u << 12);
constexpr uint64_t kMinVideoMemory = 3584ull << 20;  // "4 GB" cards report a little less than 4096 MB
constexpr uint64_t kMinSystemMemory = 7ull << 30;    // "8 GB" systems report a little less too
constexpr unsigned kMinThreads = 4;

std::string VersionText(uint32_t v) {
    return std::to_string((v >> 22) & 0x7Fu) + "." + std::to_string((v >> 12) & 0x3FFu);
}

std::string GigabytesText(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f GB", double(bytes) / double(1ull << 30));
    return text;
}

Result CheckWindows() {
    Result result;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys < kMinSystemMemory) {
        result.reason = "less than 8 GB of RAM (" + GigabytesText(memory.ullTotalPhys) + ")";
        return result;
    }
    if (system.dwNumberOfProcessors < kMinThreads) {
        result.reason = "fewer than 4 CPU threads";
        return result;
    }
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (loader == nullptr) {
        result.reason = "no Vulkan driver installed";
        return result;
    }
    const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
    const auto global = [getProc](const char* name) { return getProc ? getProc(nullptr, name) : nullptr; };
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(global("vkEnumerateInstanceVersion"));
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(global("vkCreateInstance"));
    uint32_t loaderVersion = 1u << 22;  // a loader without vkEnumerateInstanceVersion is Vulkan 1.0
    if (enumerateVersion != nullptr && enumerateVersion(&loaderVersion) != kVkSuccess) loaderVersion = 1u << 22;
    if (createInstance == nullptr || loaderVersion < kVulkan13) {
        result.reason = "the Vulkan loader is " + VersionText(loaderVersion) + ", older than 1.3";
        FreeLibrary(loader);
        return result;
    }
    VkApplicationInfo app{0, nullptr, "Metroid: Other M", 1, "Other M Recompiled", 1, kVulkan13};
    VkInstanceCreateInfo info{1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkInstance instance = nullptr;
    if (createInstance(&info, nullptr, &instance) != kVkSuccess || instance == nullptr) {
        result.reason = "Vulkan could not start";
        FreeLibrary(loader);
        return result;
    }
    const auto proc = [getProc, instance](const char* name) { return getProc(instance, name); };
    const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(proc("vkDestroyInstance"));
    const auto enumerateDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(proc("vkEnumeratePhysicalDevices"));
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(proc("vkGetPhysicalDeviceProperties"));
    const auto getMemory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(proc("vkGetPhysicalDeviceMemoryProperties"));
    std::vector<VkPhysicalDevice> devices;
    if (enumerateDevices != nullptr && getProperties != nullptr && getMemory != nullptr) {
        uint32_t count = 0;
        if (enumerateDevices(instance, &count, nullptr) == kVkSuccess && count > 0) {
            devices.resize(count);
            const VkResult got = enumerateDevices(instance, &count, devices.data());
            devices.resize(got == kVkSuccess || got == kVkIncomplete ? count : 0);
        }
    }
    // The best card: a discrete one with Vulkan 1.3 and the most video memory. Otherwise remember why the best
    // candidate fell short.
    std::string bestShort;
    uint64_t bestMemory = 0;
    for (VkPhysicalDevice device : devices) {
        PropertiesBuffer props{};
        getProperties(device, &props);
        props.deviceName[sizeof(props.deviceName) - 1] = '\0';
        VkPhysicalDeviceMemoryProperties memoryProps{};
        getMemory(device, &memoryProps);
        uint64_t local = 0;
        for (uint32_t i = 0; i < memoryProps.memoryHeapCount && i < 16; ++i)
            if ((memoryProps.memoryHeaps[i].flags & 1u) != 0) local = std::max(local, memoryProps.memoryHeaps[i].size);
        const std::string name = props.deviceName;
        const std::string summary = name + ", Vulkan " + VersionText(props.apiVersion) + ", " + GigabytesText(local) +
                                    " video memory";
        if (props.deviceType != 2) {
            if (bestShort.empty()) bestShort = name + " is not a separate graphics card";
            continue;
        }
        if (props.apiVersion < kVulkan13) {
            bestShort = name + " has Vulkan " + VersionText(props.apiVersion) + ", older than 1.3";
            continue;
        }
        if (local < kMinVideoMemory) {
            bestShort = name + " has less than 4 GB of video memory";
            continue;
        }
        if (local > bestMemory) {
            bestMemory = local;
            result.preferVulkan = true;
            result.reason = summary;
        }
    }
    if (!result.preferVulkan) result.reason = devices.empty() ? "no Vulkan graphics card" : bestShort;
    if (destroyInstance != nullptr) destroyInstance(instance, nullptr);
    FreeLibrary(loader);
    return result;
}
#endif

}  // namespace

Result Check() {
#if defined(_WIN32)
    g_last = CheckWindows();
#else
    g_last = Result{false, "not checked on this system"};
#endif
    return g_last;
}

const Result& Last() { return g_last; }

}  // namespace VulkanProbe
