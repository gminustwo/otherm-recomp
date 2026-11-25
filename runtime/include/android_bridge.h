#pragma once

#include <string>

// What the game asks of the Android app (android/java/io/github/gminustwo/otherm/OtherMActivity.java), through JNI.
// Android builds only (runtime/src/android/android_bridge.cpp).
namespace AndroidBridge {

// Shows the system's file picker for the disc image (an app reads shared storage only through what the player picks
// there); the choice arrives through TakePickedDisc.
void PickDisc();
// The document picked since the last call: its URI and display name. False while there is none; true with an empty
// URI when the player canceled.
bool TakePickedDisc(std::string& uri, std::string& name);
// Opens a picked document for reading: a file descriptor the caller closes, or -1.
int OpenDocument(const std::string& uri);

// Ends this process and starts the game again in a fresh one (RestartActivity), past the start screen. Does not return.
[[noreturn]] void RestartApp();
// Quit: closes the app's task and ends the process. Does not return.
[[noreturn]] void Quit();

// The updater (updater.cpp), through Java's HTTPS: a GET's HTTP status (-1 when the server could not be reached) and
// body; a download into `path`; handing a downloaded APK to Android's package installer, which asks the player first.
int HttpGet(const std::string& url, const std::string& accept, std::string& body);
bool Download(const std::string& url, const std::string& path);
bool InstallPackage(const std::string& path);

// The device, for the log: maker and model, the chip, the Android version.
std::string DeviceDescription();

} // namespace AndroidBridge
