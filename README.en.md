# GP3RT — Grand Prix 3 Runtime

**Grand Prix 3 and Grand Prix 3 2000 on modern computers.**

[Português](README.md) · [English user guide](docs/USER-GUIDE.en.md)

An independent project by Rodrigo Motta (Rods), developed with Claude (Anthropic) assistance. On Windows, a game-specific `ddraw.dll` implements the legacy graphics interfaces used by GP3 and renders through OpenGL. You still need your own installed game files.

## Download and status

Open **Releases** and download **GP3RT-1.0.1-Windows.zip**. GitHub's automatic **Source code** archives do not contain the ready-to-use DLL.

The supplied Windows DLL identifies as version 1.0.1 and targets GP3 1.13 and GP3 2000 on Windows 10/11. This repository is Windows-only. The unfinished Linux project will be published separately when ready. Documentation was prepared through static inspection; no game execution or rebuild was performed.

## Features

OpenGL rendering; configurable internal scaling and texture filtering; Hor+ widescreen on recognized game builds; borderless fullscreen and windowed mode; rain spray settings; GPU preferences; GPxPatch integration; PNG screenshots and diagnostic frame dumps.

Version 1.0.1 includes a GP3 1.13 string-table guard addressing a crash observed with GPxPatch Debug mode. It does not establish universal compatibility with every track or mod.

## Quick start

1. Back up your game folder and existing wrappers.
2. Move aside graphics wrappers you previously installed, especially an existing `ddraw.dll`. Preserve original game libraries.
3. Place `ddraw.dll` and `gp3rt.ini` next to the game executable, never in System32.
4. Disable compatibility modes for the game and GPxPatch.
5. In GPxPatch, select all CPU cores. Start with Debug disabled; enable it for tracks that require it, using the 1.0.1 guard as described in the guide.
6. Use hardware graphics. Select **1280×1024** for the widescreen override; the default INI maps it to desktop resolution.

Open the optional HTML configurator to edit the INI. Some browsers download the updated INI rather than replacing it: copy it back to the game folder.

The executable file on disk is not rewritten, but runtime hooks and patches modify the running process. Performance varies by hardware, track and mods; specific PO/FPS values are not guaranteed.

## Support and credits

Open an Issue with game/runtime versions, OS, CPU/GPU, driver, GPxPatch settings, track/mods, reproduction steps and `gp3rt.log`. Save the log before restarting. Ctrl+F11 captures a diagnostic frame in `screens/frame_NNNNN`; review it for personal information and mod content before sharing.

Project and testing: Rodrigo Motta. Development assistance: Claude / Anthropic. Original game: Geoff Crammond / MicroProse. Widescreen FOV approach credited in source to WidePrix3 by hatcher the patcher. Thanks to GPxPatch authors and the GP3 community.

Unofficial fan project; no commercial game assets or executables are included. No software license has been selected in this preparation; see [licensing status](STATUS-LICENCA.md).
