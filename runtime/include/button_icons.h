#pragma once

// The per-controller button prompts, drawn by the program itself: <UserData>/button_icons/<xbox|playstation|nintendo>
// (aurora's texture_replacement.cpp shows the set of the controller in use). Every picture of the game that names a
// Wii Remote button the profile binds (Metroid.ini) is redrawn with that controller button, from the player's own
// disc: the message-text icons, the menu badges, the pause frame's tabs and the title's "PLEASE PRESS (2)". The same
// pictures the former tools/make-button-icons.py drew at build time on Windows only, so an AppImage or any fresh
// folder has them too.
namespace ButtonIcons {
// Draws the sets on a worker thread when they are missing or were drawn for other bindings (`force`: always), then
// has aurora read them again. Needs the game's files (RuntimeConfigFile::ResolvedDvdRoot). Any thread.
void Refresh(bool force = false);
} // namespace ButtonIcons
