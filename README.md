# SASRT Modern Options

A DLL mod for **Sonic & All-Stars Racing Transformed** (PC) that will move the settings from the
external `Launcher.exe` (resolution, anti-aliasing, controls, ...) into the game's own Options menu.
Loaded by [Rutai Mod Manager](https://github.com/thezenoverse/RutaiModManager)'s [Sumo Engine Mod Loader](https://github.com/thezenoverse/SumoModLoader).

## Current state (work in progress)

- **DISPLAY OPTIONS** in the Options menu (third row, where the Xbox 360 version has it), with every
  graphics setting from `Launcher.exe`: display mode, screen resolution, ambient occlusion,
  anti-aliasing, motion blur, screen splash, V-Sync, water / texture / model / shadow quality.
  - Same layout as Audio Options; display mode, resolution and the quality levels use the
    console versions' selector box with arrows. The list scrolls past the bottom.
  - Accept saves to `Documents\SART\settings.xml` (the launcher's file) and applies the settings
    straight away, including resolution, display mode and V-Sync. Back discards.
- **DISPLAY** in the race pause menu's Options (above Audio, as on the consoles), with the same
  settings and behaviour.
- **Controller Options layout fix**: the PC port drew Controller Options and the pause menus with
  oversized compact rows; the mod restores the Xbox 360 sizes.

`ModernOptions.log` in the mod folder records what the mod set up (and any crash address), for bug reports.

## Debug builds

`make DEBUG_TOOLS=1` adds the research helpers used during development. Files go to `<mod folder>\Dumps\`:

| Key | Writes |
| --- | --- |
| F5 | `screen_<time>.png` (the current frame) and `files_<time>.txt` (every file the game opened since the previous F5) |
| F8 | `elements_<time>.txt`, the game's UI element tree |
| F6 / F7 | step the Controller Options menu position (logged; for finding the value to build in) |

They also dump the unpacked executable and log every file the game opens.

## Layout

```
mod/                  -> copy to <game>\mods\Modern Options
  mod.ini
  Data/               mirrors the game's Data folder (replacement files go here)
  ModernOptions.dll   built by `make`
src/
  main.c              Init/PostInit, log, crash logger, debug keys (debug builds)
  game.h              game addresses / calling conventions
  menu.c              DISPLAY OPTIONS row (cOptions patches, addresses documented inside)
  video.c             DISPLAY OPTIONS screen (runs inside the cOptionsAudio scene class)
  settings.c          settings.xml read/write, resolution list
  layout.c            row sizes, the ported selector box
  live.c              applies settings while the game runs
  pause.c             DISPLAY in the race pause menu
  uidump.c            UI element dump (debug builds)
  strings.c           extra UI strings (hooks the game's string-table text setter)
  d3d9hook.c          back buffer capture (debug builds)
  gdicapture.c        window capture fallback
  filelog.c           CreateFileA/W logging (chains onto the mod loader's hooks)
  png.c               dependency-free PNG writer
```

## Building

MinGW-w64 for i686 (Linux/WSL: `apt install gcc-mingw-w64-i686`; MSYS2: `mingw-w64-i686-gcc`):

```sh
make                                   # builds mod/ModernOptions.dll
make DEBUG_TOOLS=1                     # the same with the research helpers
make install GAME_DIR="/path/to/Sonic & All-Stars Racing Transformed"
```

Then enable **Modern Options** in Rutai Mod Manager.
