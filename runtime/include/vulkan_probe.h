#pragma once
// Options > Display > Graphics API "Automatic" (main.cpp): Vulkan on a capable system, else the platform's default
// (Direct3D 12 on Windows). The check talks to the Vulkan loader directly, before aurora starts Dawn.
#include <string>

namespace VulkanProbe {

struct Result {
    bool preferVulkan = false;
    std::string reason;  // "NVIDIA GeForce RTX 5090, Vulkan 1.4, 32 GB video memory" or why not
};

// Vulkan when the loader and a discrete graphics card both offer Vulkan 1.3 or newer, the card has at least 4 GB of
// its own memory, and the computer has at least 8 GB of RAM and 4 CPU threads. Windows only (elsewhere: false).
Result Check();

// What the last Check() decided, for Options; empty before it ran.
const Result& Last();

}  // namespace VulkanProbe
