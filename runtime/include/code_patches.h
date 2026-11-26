#pragma once
// Rewrites of the game's own instructions that the port follows: a Gecko code writing an instruction, or a mod's
// game.rel with changed code. The translated game never reads its instructions, so each rewrite the port knows
// switches a code tap on (project/code_taps.txt, code_taps.h) with the value the rewritten code would give; a
// rewrite back to the game's own instruction switches it off again. Everything here runs on the game's thread.
//
// Known rewrites (Other M Redux's, NOTES.md 2026-11-25):
//  - 0x805713D0 nop: concentrate at any energy (rel_80571284 goes on as if rel_805C4628 said yes);
//  - "li rD, N" at 0x8054D998 / 0x8054D9AC / 0x8054D9CC / 0x8054D9F4 (r27, the shot type rel_8054D73C stores) and
//    at 0x8054E368 (r0, over rel_8054E304's load of the stored type): missiles in third person;
//  - Redux's own 200 bytes at 0x805F6040 (rel_805F602C): the cutscene suit model from the suit bits at 0x80CA4E20.
#include <cstdint>
#include <filesystem>
#include <string>

namespace CodePatches {
// Once game.rel is linked: reads the game's own instructions at the known sites from the disc's game.rel.
void Init(const std::filesystem::path& discRel);
// A write into the game's code (`size` bytes of `value` at `address`). True when the port follows it there.
// `apply` false: only recorded, for a batch (a mod's game.rel) that Commit() judges as a whole.
bool Write(uint32_t address, uint32_t size, uint32_t value, bool apply = true);
// True when a write at `address` would be followed.
bool Knows(uint32_t address);
// Makes the current instructions the state Reset() returns to (after a mod's game.rel was written in).
void Commit();
// Back to that state: the cheats that are on changed, and the codes write their instructions again.
void Reset();
// What is switched on, for the log ("" when nothing).
std::string Describe();
// A tap the port drives itself, not a mod's rewrite (hle/otherm_language.cpp): the tap at `address` gives `value`
// while `on`, else the game's own value. False when project/code_taps.txt has no such tap.
bool SetPortTap(uint32_t address, bool after, uint8_t gpr, bool on, uint32_t value);
// OTHERM_SELFTEST: decoding "li" and the switch on and off at a site.
bool SelfCheck();
}  // namespace CodePatches
