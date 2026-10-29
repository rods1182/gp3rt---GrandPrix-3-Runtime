# Windows user guide — GP3RT 1.0.1

## Requirements and installation

Use your own GP3 1.13 or GP3 2000 installation, Windows 10/11 with x86 process support, and an OpenGL 3.3 driver. The supplied DLL is a 32-bit PE binary. The runtime does not by itself solve the original SafeDisc-protected executable issue described in the original package. No replacement game executable is distributed.

Back up the game folder. Move aside graphics wrappers you previously installed, preserving original game libraries such as GP3 2000's eax.dll. Place ddraw.dll and gp3rt.ini next to the executable you actually launch. Never put the DLL in Windows/System32. Disable compatibility modes on the game and GPxPatch. Start with all CPU cores and Debug disabled.

Use hardware graphics; select 1280×1024 for widescreen. With wide_width=-1 and wide_height=-1, that entry maps to desktop resolution. hor_plus=1 corrects FOV on recognized builds. Check game build and GPU in gp3rt.log. If calibration fails, close the game, back up gconfig.txt, remove it and retry.

## GPxPatch Debug

Version 1.0.1 adds str_guard=1 for a GP3 1.13 string-table overread observed with GPxPatch Debug. For tracks requiring Debug, enable it with the guard active and check the result. It is not a universal crash fix and is not applied to GP3 2000. This supersedes the original blanket instruction to always disable Debug.

## Configuration

The optional HTML configurator opens and saves the INI. If the browser downloads a new INI, replace the one in the game folder manually. Restart the game after edits.

Common options: renderer=opengl (or software fallback), mode=fullscreen/window, internal_scale=auto or 1–4, gpu=auto/dedicated/integrated/system, aspect=keep/stretch, spray_opacity=60, vsync=1. See the commented INI and Portuguese configuration reference for all distributed values.

## Diagnostics and removal

Print Screen saves PNG screenshots. Ctrl+F11 captures a diagnostic frame under screens/frame_NNNNN. Copy gp3rt.log before restarting because it is overwritten. Include OS/GPU/driver, game build, GPxPatch settings, track/mods and reproduction steps in an Issue. Review logs and frame dumps for personal information and mod content before sharing.

Close the game and remove the GP3RT DLL and INI to uninstall; restore your previous wrapper if needed. Logs/screenshots can be removed separately. A Windows GPU preference written by the runtime may remain and can be changed in system graphics settings.

Runtime hooks/patches modify process memory, while the game executable file on disk is preserved. Performance depends on hardware/tracks/mods. The unfinished Linux project is separate; this repository covers Windows only. A reproducible build verification is not supplied. directplay=1 permits the legacy network path but does not establish working internet multiplayer. Antivirus detections have not been investigated by this documentation review; a detection is not automatically a false positive.
