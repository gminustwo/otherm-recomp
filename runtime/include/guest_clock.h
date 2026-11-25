#pragma once

#include <cstdint>

// Fast-forward the game cannot tell from a faster Wii (like Dolphin's unthrottled emulation; NOTES.md 2026-11-23).
//
// Normally the guest time base is the host's steady clock (ppc_helpers.cpp). While fast-forward runs it is a virtual
// clock instead, moved on retrace by retrace by vi.cpp:
//  * from the instant of the last retrace it runs `speed` times faster than real time, but never reaches the next
//    retrace before vi.cpp delivers it, so every retrace interval is exactly one retrace interval of guest time;
//  * vi.cpp delivers the next retrace as soon as the game waits for it (the scheduler's idle loop, the present
//    path), and only then: a game still busy with its frame gets the retrace at the normal 1x moment, as it would on
//    the console. So the game never sees a dropped frame that fast-forward caused, and when the PC cannot reach the
//    chosen speed the whole game (picture, its timers, sleeps, alarms, audio streams, Sofdec movies) runs slower
//    together instead of the picture falling behind its sound.
// The audio clock (audio.cpp) and the OS sleep timers (os_sleep.cpp) read the same time base.
namespace GuestClock {

bool FastForwarding();
// Starts the virtual clock at the current time base; the next retrace is one interval away.
void BeginFastForward(uint32_t speed, uint64_t retraceIntervalTicks);
void SetFastForwardSpeed(uint32_t speed);
// vi.cpp delivered a retrace: the time base goes on from that retrace's instant and stops short of the next one.
void FastForwardRetrace(uint64_t retraceIntervalTicks);
// Back to the host clock, continuing from the virtual time base.
void EndFastForward();

} // namespace GuestClock
