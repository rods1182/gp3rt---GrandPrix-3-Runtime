GP3RT 1.2.1 for Linux
Grand Prix 3 and Grand Prix 3 2000 running natively on Linux - no Wine, no Proton, no Steam
==========================================================================================

(Português: veja LEIAME.txt)

This used to be called GP3 Reborn. It is now the Linux edition of GP3RT:
same code, same version numbers and same gp3rt.ini options as the Windows
ddraw.dll, released together at https://github.com/rods1182/gp3rt---grandprix-3-runtime


1. QUICK START
--------------
1. Have the game folder ready: Grand Prix 3 1.13 (Gp3.exe) or Grand Prix 3
   2000 ("gp3 2000.exe"), executable WITHOUT SafeDisc (see section 4). A
   folder copied from Windows is fine; put it in your home folder.
2. Unpack this package and copy ALL its files into the game folder (the
   folder that holds Gp3.exe).
3. Double-click "gp3". The game starts, and the first time a
   "Grand Prix 3" (or "Grand Prix 3 2000") entry appears in your
   applications menu. From then on, use that entry.
   If nothing happens: right-click "gp3" > Properties > Permissions > tick
   "Allow executing file as program". Do the same for "gp3-game" and
   "GPxPatch".
4. Optional: settings (picture quality, widescreen, window, graphics card)
   - open "GP3RT-Configurador.html" in your web browser (section 6).
5. Optional: GPxPatch - read section 2.


2. GPxPATCH: HOW TO GET IT AND PUT IT IN THE GAME
--------------------------------------------------
GPxPatch 4.52 is René Smit's famous patch for GP3 (TV graphics, track and
car sets, seasons, lap times, extra sounds...). It is HIS work and is NOT
included in this package. This package only brings "GPxPatch", a
configuration screen and launcher made for Linux, which reads and writes
the same GPxPatch files as on Windows.

a) Download GPxPatch 4.52 from the author's site:
     https://www.sdidit.nl/download.html  ->  inst-gpxpatch452.exe
   It is a Windows installer.

b) Put it in the game folder. What GP3RT needs, in the game folder:
     GPxPSup.dll        (file)
     gpxpdata\          (folder, with all its contents)
   Choose ONE way:

   - Already have GP3 with GPxPatch on Windows? Copy GPxPSup.dll and the
     gpxpdata folder from it into the Linux game folder. Your GPxPatch
     settings, track sets and car sets come along.

   - On a Windows PC: run inst-gpxpatch452.exe, install it into a GP3 folder
     there, then copy GPxPSup.dll and the gpxpdata folder into the Linux
     game folder.

   - On Linux with Wine (only for the installer; the game itself never
     uses Wine): in a terminal, run
         wine inst-gpxpatch452.exe
     and, when it asks where to install, pick your game folder. In Wine,
     your Linux files are on drive Z: (for example
     Z:\home\yourname\GP3). If it installs somewhere else, copy
     GPxPSup.dll and gpxpdata from there into the game folder.

c) Double-click "GPxPatch" in the game folder. You get the GPxPatch tabs
   (GPxSet, GPxTrack, GPxLap, GPxSAT, GPxCInfo, GPxSound, keys, fonts,
   video and game). On the first tab, "Create shortcuts" adds
   "GPxPatch (Grand Prix 3)" to the menu and the desktop.
   "Start with GPxPatch" / "Start without GPxPatch" start the game.

Important GPxPatch settings:
- Debug: ON. GP3RT needs it (with it off the game can close when you
  press "Drive").
- CPU: all cores ("full").

Notes:
- Do NOT run GPxPatch.exe: it is a Windows program. On Linux, "GPxPatch"
  takes its place; the game loads the GPxPatch modules by itself.
- Settings are saved in the same gpxpdata files, so the same folder works
  on Windows and on Linux.
- Some community tracks need specific GPxPatch settings (for example a
  larger maximum track length). Follow the readme that comes with the
  track.
- gp3rt.ini "gpxpatch=0" (or "Start without GPxPatch") starts the plain
  game even with GPxPatch installed.


3. WHAT IS IN THIS PACKAGE
--------------------------
  gp3                        starts the game (window, video, sound, controls)
  gp3-game                   runs Gp3.exe inside it (gp3 starts it by itself)
  GPxPatch                   GPxPatch configuration screen and launcher for
                             Linux (GPxPatch itself: see section 2)
  GP3RT-Configurador.html  edits gp3rt.ini in your browser
  GP3Replay.dll              whole-race replay on GP3 1.13 (optional)
  gp3rt.ini                  video and game settings
  instalar-atalho.sh         (optional) creates the menu shortcuts from a
                             terminal
  icones/                    icons
  HOW-IT-WORKS.txt           how the game runs on Linux (plain language)
  README.txt / LEIAME.txt    this text, in English / Portuguese
  COMO-FUNCIONA.txt          how it works, in Portuguese
  LICENSE-font-Inter.txt     licence of the font used by the GPxPatch screen
  codigo-fonte.tar.gz        source code


4. REQUIREMENTS
---------------
- 64-bit Linux on a regular PC (Intel or AMD, "x86-64"). Tested on Linux
  Mint and Ubuntu 24.04; built to run on Debian, Fedora, Arch/Manjaro,
  openSUSE, Pop!_OS, Zorin and derivatives (glibc 2.17 or newer).
- NOT on ARM (Raspberry Pi, Apple Silicon Macs, phones): Gp3.exe is
  Intel/AMD code.
- A graphics card with OpenGL 3.3 (NVIDIA, AMD or Intel). Laptops with two
  GPUs use the dedicated one automatically.
- SDL2 (already installed on most systems). If missing:
    Mint / Ubuntu / Debian:  sudo apt install libsdl2-2.0-0
    Fedora:                  sudo dnf install SDL2
    Arch / Manjaro:          sudo pacman -S sdl2
  The GPxPatch screen needs SDL2 2.0.18 or newer; on older systems it
  opens in the web browser instead, with the same options.
- Sound through PulseAudio or PipeWire. Wheels and joysticks (with force
  feedback) through Linux's own drivers.
- The game: the installed folder with an executable WITHOUT SafeDisc:
    Grand Prix 3 1.13     Gp3.exe          (~8.6 MB)
    Grand Prix 3 2000     gp3 2000.exe     (~8.7 MB)
  GP3RT finds either name; nothing needs renaming. If the game is on
  a Windows partition, copy the whole folder into your home folder (the
  game writes saves, settings and logs there).


5. DO NOT USE Gp3.exe / gp3 2000.exe
------------------------------------
They are Windows programs. Keep them in the folder (GP3RT loads them
from inside), but always start the game with "gp3", the "Grand Prix 3"
shortcut, or the GPxPatch screen. Wine is not needed; if your system
offers to open .exe files with Wine, say no.


6. SETTINGS (gp3rt.ini)
-----------------------
Open GP3RT-Configurador.html in your browser (it works offline):
"Open the game folder", pick the game folder: it reads gp3rt.ini and the
last logs by itself, shows the graphics card and game it found, and
explains every option ("More details"). Change what you like, "Save". In
Chrome/Chromium the file is saved in place (and the folder is remembered). In Firefox it is
downloaded to your Downloads folder: copy it into the game folder,
replacing the old one. The GPxPatch screen ("Video and game" tab) changes
the same options.

Main options:
  Picture: 3D internal resolution (auto, 1x-4x), anisotropic filtering,
           smooth textures, V-Sync, quick profiles.
  Widescreen: in the game's graphics menu pick 1280x1024; it becomes your
           desktop resolution (or one you choose), with Hor+ field of view
           (more picture at the sides, no stretching).
  Screen:  fullscreen or window (Alt+Enter in game), window size, which
           graphics card on laptops with two.
  Rain:    water spray and smoke opacity (100% = original; lower avoids
           the "grey wall" near the TV cameras).
  Game:    GPxPatch on/off, tracks beyond the original limits, intro
           movies, 3D sound, numeric keypad behaviour.
  Replay:  whole-race replay and GP3 2000 keys on GP3 1.13, spray and
           smoke in the replay, 2-hour replay on GP3 2000.


7. IN THE GAME
--------------
Alt+Enter   fullscreen / window
Print Scr   screenshot (PNG, in the game's "screens" folder)
Alt+F4      quit
Esc         skip the intro movies
Wheel: it appears in GP3's controls menu under the name Linux gives it.
Calibrate it in the game menu, as on Windows.

IN THE REPLAY (GP3 1.13; R opens the replay, already playing from the
start of the lap, with or without P first; the legend is at the bottom):
  F1 / F2     previous / next lap
  Shift+F1    go to the start of the race
  Shift+F2    go to where the race stopped (stays paused)
  F3 or P     play / pause
  F4          slow motion (while held); paused: one frame
  F5 / F8     rewind / fast forward (while held)
  F6 / F7     one frame back / forward (repeats while held)
  F9          show / hide the legend
The replay ends where you paused; then P resumes the race.
With GP3Replay.dll in the folder, the replay keeps the WHOLE RACE (over 2
hours), without GP3 Video's gp3v.exe. During the race GP3RT keeps a
snapshot every second (about 10 KB each): that is what makes rewind and
fast forward smooth, with the right camera. After loading a saved race,
the replay still goes back to the start: it uses the snapshots GP3Replay.dll
keeps in the save (one every 32 s) and makes the 1-second ones again as it
plays through.
On GP3 2000 the replay keys are the game's own, and the replay goes up to
2 hours (Options > Graphics > Length of replay; the game stopped at 1h30).


8. ACCENTED LETTERS (ç, ã, é, ñ...)
-----------------------------------
GP3 and GPxPatch store text in the old Windows format (Latin-1 /
Windows-1252); Linux uses UTF-8. GP3RT reads both and finds files
even when an accent in the name got garbled when unzipping. The GPxPatch
screen shows accents correctly and can convert UTF-8 files back to
Latin-1. To edit by hand (Linux Mint's Xed): File > Open > Character
encoding "Western (WINDOWS-1252)", and keep it when saving. Prefer file
and folder names without accents.


9. IF SOMETHING GOES WRONG
--------------------------
Log files in the game folder:
  gp3linux.log           window, video, sound, controls
  gp3linux-game.log      the game
  gp3linux-gpxpatch.log  the GPxPatch screen
  gpxpdata/log/          the GPxPatch modules
Send them with a description of the problem (game version 1.13 or 2000,
track, camera).
- Nothing happens on double-click: execute permission (section 1, step 3).
- "game executable not found": Gp3.exe / "gp3 2000.exe" missing.
- Game closes at once and gp3linux-game.log mentions "SafeDisc": use an
  executable without SafeDisc.
- The GPxPatch screen says GPxPatch is not installed: GPxPSup.dll or
  gpxpdata is missing from the game folder (section 2).


10. CREDITS
-----------
GP3RT for Linux: Rodrigo Motta, with Claude (Anthropic).

Special thanks to:
- Stephan Séguin ("Seguin" on GrandPrixGames.org): the first user and the
  first feedback; he spread the word on GrandPrixGames.org, GP2Forum and
  the Grand Prix Games group on Facebook; and the report of the replay
  after loading a saved race (fixed in 1.2.1). Merci, Stephan!
- JF ("BeADriver" on GrandPrixGames.org): the log that revealed GP3 2000's
  3D sound bug in long races, and testing the fix.
- Rafa, creator of Gp3Video, who years ago gave GP3 1.13 a whole-race
  replay. He was the inspiration for GP3Replay.dll, which does the same
  replay enlargement without a separate executable.

GPxPatch 4.52 (c) 2001-2019 René Smit - not included; get it from
https://www.sdidit.nl. Thank you, René!
Grand Prix 3 and Grand Prix 3 2000 (c) Geoff Crammond / MicroProse. The
game files are not part of this package.
Components: SDL2 (zlib licence), Nuklear (public domain / MIT), Inter font
(c) Rasmus Andersson (SIL Open Font License, see LICENSE-font-Inter.txt).
Source code: codigo-fonte.tar.gz.
