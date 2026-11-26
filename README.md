[![release](https://img.shields.io/github/v/release/gminustwo/otherm-recomp?filter=v%2A&sort=semver&display_name=tag&label=release&color=orange)](https://github.com/gminustwo/otherm-recomp/releases/latest)
[![downloads](https://img.shields.io/github/downloads/gminustwo/otherm-recomp/total?label=downloads&color=brightgreen)](https://github.com/gminustwo/otherm-recomp/releases)
[![Discord](https://img.shields.io/discord/1574161125094850682?label=Discord&logo=discord&logoColor=white&color=5865F2)](https://discord.gg/ggvx7hnPzY)
[![Patreon](https://img.shields.io/badge/Patreon-support-F96854?logo=patreon&logoColor=white)](https://www.patreon.com/rengar1)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=kofi&logoColor=white)](https://ko-fi.com/rengar12707)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-support-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/rengar)

# Other M Recompiled

A native PC port of **Metroid: Other M** (Wii, 2010), made by statically recompiling the game's PowerPC code
(`main.dol` and `game.rel`) to C++ with its own recompiler and runtime. There is no emulator, interpreter or JIT at
runtime: the game's own code runs natively, with the Wii's graphics, sound and controllers replaced by PC ones.

**No game data is included**: you need your own disc image of the USA release (`R3OE01`, revision 1). The downloads
contain the game's code in recompiled form, none of its data; the game's files are read from your disc image.

Downloads for Windows (a zip), Linux / the Steam Deck (an AppImage), Android (an APK) and Macs with Apple
silicon (an app) are on the
[Releases](https://github.com/gminustwo/otherm-recomp/releases) page; the game offers new versions itself and installs
them in place.

## What it adds

- **Options** (Start + Back together, F1, or the Guide button): pages for Game, Display, Sound, Controls, Textures,
  Saves, Cheats and Mods. LB / RB turn the pages; the mouse works too. The game pauses while it is open.
- **Display**: internal resolution, any window size and aspect ratio (Fill screen draws a wider 3D view for 16:10,
  21:9 and 32:9, with the HUD moved to the screen's edges), frame interpolation to 90, 120, 144 or 240 FPS, V-Sync.
- **Save states** (F5 / F9, or a held controller chord set in Options > Controls).
- **Skip cutscenes**: Start, A or LB (Backspace on the keyboard) skips a movie or an in-engine cutscene. In-engine
  cutscenes end through the game's own skip, the one it offers after you have seen a cutscene, so the game carries
  on exactly as after watching it. Options > Game > Skip cutscenes: **Any** (the default) or **Seen ones** (the
  game's own rule).
- **Fast-forward** (hold F or click the right stick; 2x to 8x or Max in Options > Game): the whole game speeds up
  together, movies, cutscenes, sound and timers alike, as in Dolphin. When the PC can't reach the chosen speed,
  everything slows down together instead of the sound running ahead of the picture.
- **Controls**: any controller through a Dolphin Wii Remote profile (`Metroid.ini`), rebinding from Options, rumble,
  the mouse as the Wii pointer, real Wii Remotes over Bluetooth.
- **Textures**: Dolphin-style texture packs, with optional prefetch.
- **Saves**: imports your Dolphin save.
- **Cheats**: infinite energy and missiles, 360-degree movement, a minimap that faces north, and Gecko codes.
- **Mods**: Riivolution packs and [Other M Redux](#mods), dropped into the mods folder as they are downloaded.
- **Discord status**: while the Discord app runs on the same PC, your profile shows Other M, the area you are in
  (Main Sector, Sector 1 and so on), whether a cutscene or movie is playing, and the time played. Options > Game >
  Discord status turns it off.

## Mods

Put a mod in `UserData/mods` (Options > Mods > **Open the mods folder**): its zip as you downloaded it, or its
folder. A zip is unpacked once, at the next start, with its progress on the start screen. Mods made for Dolphin's
Riivolution work as they are, and their options (the ones Dolphin's Riivolution menu shows) are listed under the mod
in Options > Mods.

**[Other M Redux](https://github.com/ShadowOne333/Metroid-Other-M-Redux)**: download `Other.M.Redux.zip` from its
Releases page and put it in the mods folder as it is (or unpack it into a folder of its own there). Its models,
sound, movies, text and subtitle timings replace the game's, and the port does what Redux's patched program does:
Hard Mode from the start, concentration at any energy (it takes twice as long), missiles in third person (hold B and
press 1; with the default controls, B and X on the controller), the stuck Sector 3 door opened, and cutscenes showing
the suit Samus has. Redux's optional Maxximum Edition soundtrack is off until you turn it on in Options > Mods.
Redux's Classic Controller and Nunchuk support isn't needed here, as the port has its own controller support.

## Installing on Windows

1. Download `OtherMRecompiled-<version>-windows-x64.zip` from
   [Releases](https://github.com/gminustwo/otherm-recomp/releases) and unpack it into a folder of its own, for
   example `C:\Games\OtherM` (the zip holds one folder, `OtherMRecompiled`).
2. Start `MetroidOtherM.exe`. Pick **Play** (or **Game files**) and point it at the folder that holds your disc image:
   `.rvz`, `.iso`, `.wia`, `.gcz`, `.ciso` or `.wbfs`, any name. **Use this folder** sets the game up from the image
   into `Disc\DATA` in the game's folder, once (a few minutes, with a progress bar; about 7.5 GB). Afterwards you can
   delete the image.
3. To play from Steam (for Steam Input), add `MetroidOtherM.exe` as a Non-Steam Game.

Everything the game writes stays in its folder: `UserData\` (saves, settings, save states, logs, mods) and
`Metroid.ini` (your controller layout, once you change it in Options > Controls). An update keeps all of it.

## Installing on the Steam Deck (or another Linux PC)

1. Download `OtherMRecompiled-<version>-x86_64.AppImage` from
   [Releases](https://github.com/gminustwo/otherm-recomp/releases) and put it in a folder of its own, for example
   `~/Games/OtherM`.
2. Make the AppImage executable: right-click it in Dolphin (the file manager) > Properties > Permissions >
   "Is executable", or `chmod +x OtherMRecompiled-*.AppImage`.
3. Add it to Steam in Desktop Mode: Steam > Games > Add a Non-Steam Game to My Library > Browse, and pick the
   AppImage. Then it starts from Game Mode like any other game.
4. Start it. The start screen comes up first. Pick **Play** (or **Game files**) and point it at the folder that holds
   your disc image: `.rvz`, `.iso`, `.wia`, `.gcz`, `.ciso` or `.wbfs`, any name, on the internal drive or an SD
   card. **Use this folder** sets the game up from the image into `Disc/DATA` beside the AppImage, once (a few
   minutes, with a progress bar; about 7.5 GB). Afterwards you can delete the image. A disc image in the AppImage's
   own folder is offered right away.

Everything the game writes stays in that folder: `UserData/` (saves, settings, save states, logs) and `Metroid.ini`
(your controller layout, once you change it in Options > Controls). Moving the folder keeps all of it.

## Installing on Android

It needs a 64-bit ARM phone, tablet or handheld with Vulkan and Android 11 or newer, and a controller (Bluetooth or
USB): there are no touch controls.

1. Download `OtherMRecompiled-<version>-android-arm64.apk` from
   [Releases](https://github.com/gminustwo/otherm-recomp/releases) and open it. The first time, Android asks you to
   allow installing apps from your browser or file manager.
2. Start Other M Recompiled. Pick **Play** (or **Game files**) and choose your disc image in Android's file picker:
   `.rvz`, `.iso`, `.wia`, `.gcz`, `.ciso` or `.wbfs`, on the device, an SD card or a USB drive. The game sets itself
   up from it, once (a few minutes, with a progress bar; about 7.5 GB free). Afterwards you can delete the image.

Everything the game writes is in its folder on the device's storage, `Android/data/io.github.gminustwo.otherm/files`:
saves, settings (`Config.toml`), save states, logs, mods, `Metroid.ini` and the game's files (`Disc/DATA`). Android's
own file managers don't show that folder; connect the device to a PC over USB to reach it. An update keeps it;
uninstalling the app deletes it, so copy your saves first. Android's back button opens Options.

## Installing on a Mac

For Macs with Apple silicon (M1 or later), macOS 12 or newer.

1. Download `OtherMRecompiled-<version>-macos-arm64.zip` from
   [Releases](https://github.com/gminustwo/otherm-recomp/releases) and open it: it holds `Other M Recompiled.app`.
2. Drag the app into Applications (or any folder of your own) before you start it: started from where it was
   unpacked, macOS runs a read-only copy, and the game can't update itself in place.
3. The app is not notarized by Apple, so the first start is refused. Open System Settings > Privacy & Security, scroll
   down to "Other M Recompiled was blocked" and click **Open Anyway**. Later starts open normally.
4. Pick **Play** (or **Game files**) on the start screen and point it at the folder that holds your disc image:
   `.rvz`, `.iso`, `.wia`, `.gcz`, `.ciso` or `.wbfs`, any name. **Use this folder** sets the game up from it, once
   (a few minutes, with a progress bar; about 7.5 GB). Afterwards you can delete the image.

On a Mac everything the game writes goes to `~/Library/Application Support/OtherMRecompiled` (in Finder: Go > Go to
Folder): saves, settings, save states, logs, mods, `Metroid.ini` and the `Disc/DATA` set up from your disc image.
An update replaces only the app and keeps all of it.

## The start screen

Every start opens the start screen, in the style of the game's menus, before the game boots: **Play** starts the
game, **Game files** says where the game's files are and sets them up from another folder, LB / RB reach the Options
pages, and **Quit** closes it. It works with a controller (A picks, B goes back or up a folder), the keyboard and the
mouse. To boot straight into the game, turn **Start screen** off (on the start screen, or in Options > Game), or
start the program with `--play` (for example in a Steam shortcut's launch options). While the game is not set up,
the start screen comes up anyway.

## Shaders

The game's shaders are compiled ahead of time: the program ships a list of the ~14,600 that playthroughs have used,
and every start compiles them behind the start screen and the game's first picture. The first start after an update
takes longest (about 15 seconds on a fast PC, longer on a Steam Deck); later starts take a couple of seconds.

A shader the list does not have yet compiles the first time the game needs it. Meanwhile **ubershaders** draw it: one
generic shader that reads what the game asked for, so there is no stutter and nothing goes missing. Options >
Display > **Shader compilation** offers **Ubershaders** (the default), **Skip draws** (the effect is missing for a
moment) and **Wait** (a short stutter).

## Controls

The default mapping is the Dolphin profile `Metroid.ini` (in `project/controls/`; it is built
in). With an Xbox-style pad, or the Steam Deck's own controls:

| Pad | Wii Remote (held sideways) |
|---|---|
| Y | A |
| B | B |
| X | 1 |
| A | 2 |
| Back / Start | - / + |
| Start + Back together | HOME: the port's Options |
| Left stick or D-pad | D-pad (turned for the sideways remote), and the pointer while aiming |
| Hold RT | point at the screen (first-person view) |
| LT | tilt the remote back |

In the game's menus X and B swap, so B goes back. Any Dolphin Wii Remote profile works the same way: put it next to
the game as `Metroid.ini`, or point `OTHERM_WIIMOTE_PROFILE` at it.

## Reporting a problem

Open an issue on the [Issues](https://github.com/gminustwo/otherm-recomp/issues/new/choose) page and pick **Bug
report** (or **Feature request** for an idea): the form asks for what helps. Please check the open issues first, and
report one problem per issue. What to have at hand:

- **The version**: Options (Start + Back together, or F1) > Game > Update, e.g. "v0.1.0". It is also on the log's
  first line.
- **What happened and where**: the area and room (or menu, cutscene, loading screen) and the moment, whether it
  happens every time, and how to get there.
- **The session log (required)**: a bug report needs the log of the play session where the problem happened. Every
  start of the game makes a new folder in `UserData/Logs`, named `base_<number>_pid<number>` (the first number counts
  seconds, so a later start has a bigger one), with that session's `console.log` inside. A later start never
  overwrites an earlier log, but it does add a newer folder: pick the one from the session where the problem
  happened by its date and time, and drag its `console.log` into the issue. Folders older than 4 days are deleted, so
  attach it soon. It's in:
  - Steam Deck / Linux: `UserData/Logs` in the folder that holds the AppImage
  - Windows: `UserData\Logs` next to `MetroidOtherM.exe`
  - Android: `Logs` in `Android/data/io.github.gminustwo.otherm/files` (connect the device to a PC over USB to copy
    it)
  - Mac: `~/Library/Application Support/OtherMRecompiled/Logs`

  It lists your settings and what the game was doing in a slow frame, a freeze or a crash. It also shows folder
  paths, which can include your user name: edit those out if you like.
- **Your setup**: the controller (and whether Steam starts the game), your display, aspect ratio and frame rate, a
  texture pack if you use one, your CPU and graphics card.
- **A screenshot or a short video** for anything you can see.

A save state from just before the problem can help too, but states only load into the build that made them: mention
that you have one, and we'll ask for it if needed.

## Building

### Windows

You need Windows, Python 3, the .NET 8 SDK, Dolphin (for `DolphinTool.exe` and its signature database) and your own
disc image.

```powershell
# Unpack the disc into Disc\DATA, check it, link game.rel, build the function map and names:
powershell -ExecutionPolicy Bypass -File tools\setup-otherm.ps1 -Image "<your disc>.rvz" -DolphinDir "<Dolphin folder>"
# Translate, then the C++ build in build\ (downloads the pinned toolchain into toolchain\ and the dependency
# sources into dependencies\ the first time), into Play\:
powershell -ExecutionPolicy Bypass -File tools\build-otherm.ps1
# The release zip, dist\OtherMRecompiled-v1.0.0-windows-x64.zip:
powershell -ExecutionPolicy Bypass -File tools\package-windows.ps1 -Version v1.0.0
```

### Linux and the Steam Deck AppImage

After the Windows translation above, in WSL Ubuntu (or on Linux with the same checkout):

```sh
bash tools/build-linux.sh                      # compiles the translated game with the pinned clang: ~/otherm-linux/build
bash tools/package-linux.sh --version v0.1.0   # dist/OtherMRecompiled-v0.1.0-x86_64.AppImage
```

`package-linux.sh` needs `mksquashfs`, `patchelf` and `curl`; it downloads the AppImage runtime and `nodtool` once.

### Android

After the Windows translation above, on Windows with the Android SDK (an NDK, build-tools and
`platforms;android-35`), a JDK 17 or newer and Rust (`rustup`):

```powershell
# dist\OtherMRecompiled-v1.0.0-android-arm64.apk:
powershell -ExecutionPolicy Bypass -File tools\build-android.ps1 -Version v1.0.0
```

It builds nod's C library once (the app reads disc images with it), compiles the game with the NDK into
`build-android\`, then packs and signs the APK. The signing key is made on first use as
`%USERPROFILE%\.android\otherm-release.jks`, its password beside it: keep both, since Android installs an update only
over an app signed with the same key.

### macOS (Apple silicon)

After the Windows translation above, copy its `generated\` and `dependencies\` folders into the same checkout on an
Apple silicon Mac with Xcode (or its Command Line Tools) and CMake + Ninja (`brew install cmake ninja`):

```sh
bash tools/build-macos.sh                      # compiles the translated game with Apple Clang: ~/otherm-macos/build
bash tools/package-macos.sh --version v1.0.0   # dist/OtherMRecompiled-v1.0.0-macos-arm64.zip (ad-hoc signed app)
```

`package-macos.sh` downloads `nodtool` once.

| Folder | What it is |
|---|---|
| `translator` | PowerPC to C++ static recompiler (C#) |
| `runtime` | The runtime and Wii SDK HLE, with the port's own features; Other M's own code is `runtime/src/hle/otherm_*.cpp` |
| `aurora` | Aurora: Wii GX on WebGPU (Dawn), with the port's changes |
| `android` | The Android app: its manifest, icons and the Java side (SDL's activity, the file picker, updates) |
| `project` | This game: `recomp.yml`, function map and names, controls, research tools |
| `tools` | Setup, build, packaging and toolchain scripts, `ppcbin.py` (DOL reader) |

## Credits

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors, GPLv3: the recompiler and
  runtime this port's own are built on
- **Aurora** by Luke Street, MIT; Dawn, SDL3 and the other components in
  [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)
- **[nod](https://github.com/encounter/nod)** by Luke Street (`nodtool`, which reads `.rvz` / `.wia` / `.gcz` images
  for the start screen's set-up; on Android its C library reads every image format), MIT / Apache-2.0
- **Dolphin Emulator**: DolphinTool and the signature database used to name SDK functions, GPL-2.0-or-later

## License

GPLv3, see [`LICENSE`](LICENSE).

Metroid: Other M is a trademark and copyright of Nintendo. This is an unofficial fan project, not affiliated with or
endorsed by Nintendo or Koei Tecmo. This repository contains no game code or data.
