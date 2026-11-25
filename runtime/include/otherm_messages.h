#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// Metroid: Other M's on-screen hints (files/message/message_all.dat) describe Wii Remote motions: "Point the Wii
// Remote at the screen", "Hold the Wii Remote vertically and press and hold (A)". The DVD read serves this file
// rewritten so they name what the player holds instead ("Hold RT for first-person view"); hle/otherm_messages.cpp.
namespace OthermMessages {

// True for the game's message file; DVDReadPrio then serves it through Read.
bool Handles(const std::filesystem::path& hostPath);

// Bytes [offset, offset + length) of the rewritten file (fewer at its end). The file is rewritten for the controls in
// use at every read that starts at offset 0 (the game reads the header before each section). False when the host file
// cannot be read.
bool Read(const std::filesystem::path& hostPath, uint32_t offset, uint32_t length, std::vector<uint8_t>& out);

} // namespace OthermMessages
