#pragma once

// The program's icon in the notification area (the system tray) while it runs, on Windows: the app icon with the
// program's name and version as its tooltip. A click on it brings the game window forward; its menu has Show the game,
// Options and Quit. It runs on its own thread, so the game never waits while the menu is open. Elsewhere these do
// nothing.
namespace TrayIcon {
// Adds the icon for the game window, once aurora has made it. Once; later calls do nothing.
void Start();
// Takes the icon away (any thread; more than once is fine). Every way out of the process calls it first: Windows
// leaves the icon of a process that ended without removing it in the tray until the mouse passes over it.
void Remove();
} // namespace TrayIcon
