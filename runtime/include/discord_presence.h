#pragma once
// Discord Rich Presence: "Playing Metroid Other M Recomp" with where the player is and the time played, in the profile
// of the Discord app running on this PC (Options > Game > Discord status, Config.toml [discord] enabled).
// discord_presence.cpp's worker thread talks to that app through its local socket (discord-ipc-N); the game only hands
// it text, and Publish never waits. Not on Android, where Discord has no local socket.
#include <string>

namespace DiscordPresence {

// The Discord application the port shows as (its name and icon are set in Discord's developer portal).
inline constexpr char kApplicationId[] = "1575365757578252388";

#if defined(__ANDROID__)
inline void Start() {}
inline bool Enabled() { return false; }
inline void SetEnabled(bool) {}
inline void Publish(const std::string&, const std::string&) {}
#else
// Once, at start (main.cpp), after the settings are read: the session's start time is the one Discord counts from.
void Start();
bool Enabled();
// Options: saved to Config.toml. Off clears the status and closes the connection.
void SetEnabled(bool on);
// What Discord shows under the game's name: `details` (where: the area) and `state` (what: a cutscene, a movie).
// Sent when it changes, at most once every 4 seconds (Discord takes 5 updates in 20 seconds).
void Publish(const std::string& details, const std::string& state);
#endif

}  // namespace DiscordPresence
