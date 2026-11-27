# SASRT Modern Options

A DLL mod for **Sonic & All-Stars Racing Transformed** (PC) that will move the settings from the
external `Launcher.exe` (resolution, anti-aliasing, controls, ...) into the game's own Options menu.
Loaded by [Rutai Mod Manager](https://github.com/thezenoverse/RutaiModManager)'s mod loader.

## Current state

- **DISPLAY OPTIONS** in the Options menu (third row, where the Xbox 360 version has it), with every graphics setting from `Launcher.exe`,
  in the launcher's order: display mode, screen resolution, ambient occlusion, anti-aliasing,
  motion blur, screen splash, V-Sync, water / texture / model / shadow quality.
  - On/off settings use the game's toggle; display mode and the quality levels use the game's
    3-option selector (the one Controller Options uses); resolution is a slider with one stop per mode.
  - The game's menu holds 10 rows, so the list scrolls when the cursor passes the bottom.
- **Controller Options layout fix**: the PC port moved Controller Options to the compact menu
  layout (taller rows on PC). The mod restores the normal-size rows the Xbox 360 version uses
  when the game data supports it. `mod/ModernOptions.ini` [Layout] `ControllerMenuPos` sets where
  the rows sit; F6 / F7 step it in-game (reopen Controller Options to see the change).
  - Confirm saves to `Documents\SART\settings.xml` (the launcher's file, including its preset
    selector); changes take effect the next time the game starts. Back discards.
- Research helpers, below.

## Research helpers

Press **F5** in-game. A beep confirms it, and two files appear in `<mod folder>\Dumps\`:

| File | Contents |
| --- | --- |
| `screen_<time>.png` | The current frame, read from the Direct3D 9 back buffer (falls back to a window capture). |
| `files_<time>.txt` | Every file the game opened since the previous F5: time, result, access, path. |

To find the Options menu's files: press F5 once on the main menu (clears the list), open Options,
press F5 again. The second `files_*.txt` lists what the Options screen loaded.

`Dumps\ModernOptions.log` records whether the D3D9 and file hooks installed.

## Layout

```
mod/                  -> copy to <game>\mods\Modern Options
  mod.ini
  Data/               mirrors the game's Data folder (replacement files go here)
  ModernOptions.dll   built by `make`
src/
  main.c              Init/PostInit, F5 key handling, log, exe memory dump
  game.h              game addresses / calling conventions
  menu.c              DISPLAY OPTIONS row (cOptions patches, addresses documented inside)
  video.c             DISPLAY OPTIONS screen (runs inside the cOptionsAudio scene class)
  settings.c          settings.xml read/write, resolution list
  layout.c            normal-size rows for Controller / Display Options
  strings.c           extra UI strings (hooks the game's string-table text setter)
  d3d9hook.c          IDirect3DDevice9::Present/Reset vtable hook, back buffer capture
  gdicapture.c        window capture fallback
  filelog.c           CreateFileA/W logging (chains onto the mod loader's hooks)
  png.c               dependency-free PNG writer
```

## Building

MinGW-w64 for i686 (Linux/WSL: `apt install gcc-mingw-w64-i686`; MSYS2: `mingw-w64-i686-gcc`):

```sh
make                                   # builds mod/ModernOptions.dll
make install GAME_DIR="/path/to/Sonic & All-Stars Racing Transformed"
```

Then enable **Modern Options** in Rutai Mod Manager.
