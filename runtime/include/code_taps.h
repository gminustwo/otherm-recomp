#pragma once
// Code taps (project/code_taps.txt; the translator's Lifting/CodeTaps.cs): at a tapped instruction the translated code
// takes one register's value through OTHERM_TAP. That is the value itself unless the runtime switched the tap on
// (code_patches.cpp, when a mod rewrote that instruction): one byte load and a branch that stays untaken otherwise.
#include <cstdint>

constexpr uint32_t kOthermCodeTapCapacity = 64u;  // RuntimeConfig::kCodeTapCount must fit (code_patches.cpp checks)

extern "C" uint8_t g_othermCodeTapOn[kOthermCodeTapCapacity];
extern "C" uint32_t OthermCodeTapValue(uint32_t id, uint32_t value);

#define OTHERM_TAP(id, value) \
    (g_othermCodeTapOn[(id)] ? OthermCodeTapValue((id), static_cast<uint32_t>(value)) : static_cast<uint32_t>(value))
