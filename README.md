# MHGU Recomp

A native PC port of **Monster Hunter Generations Ultimate** (Nintendo Switch, version 1.4.0), made by statically
recompiling the game's 32-bit ARM code to C and running it on a host runtime: Switch system services and the NVN
graphics API are reimplemented on Windows, with Vulkan for rendering.

**No game files are included.** You need your own copy of the game, dumped from your own Switch. This project does
not explain how to dump it and does not provide game files, keys or links to them.

Status: work in progress, but playable: village, hub, quests, Local Play (over the network), menus, movies and sound
work. See [docs/STATUS.md](docs/STATUS.md) for the details, the known issues and the developer notes.

## What it adds over the Switch version

- Any resolution, a separate 3D render resolution (supersampling), MSAA, FXAA on/off
- Frame rate cap from 30 to 240 fps (game speed stays correct), VSync on/off
- Keyboard controls, any SDL-supported controller, names and chat typed on the keyboard
- Local Play over LAN or the internet (one player hosts, the others join by address)
- Small monsters (Jaggi, Aptonoth...) synced between players in multiplayer, like large monsters already are: everyone
  sees them in the same place (optional, `SyncSmallMonsters` in the ini)
- Optional monster HP scaling by the number of hunters in Hub quests
- Unskippable cutscenes (the camera pans past new NPCs and facilities) fast-forward when you press A
- Most mods are supported, including Forge plugins: drop them into `mods\` (see [Mods](#mods))

## Requirements

- Windows 10 or 11, 64-bit (the Windows build also runs on Linux through Proton / Wine)
- A CPU with AVX2 (Intel Haswell / 4th generation Core or newer, AMD Excavator / Ryzen or newer)
- A GPU with Vulkan 1.2 and an up-to-date driver
- Monster Hunter Generations Ultimate, **version 1.4.0** (the base game with its 1.4.0 update applied), dumped from
  your own Switch. Title ID `0100770008DD8000`; the `main` executable's build ID starts with `FB08F1D20FD1204F`.
  Other versions don't work: the port checks this at startup and tells you if the files don't match.

## Installing a release

1. Extract the release zip anywhere, for example `C:\Games\MHGURecomp\`.
2. Put the game's files in the `data` folder next to `mhgurecomp.exe`, as two folders, `exefs` and `romfs`:

   ```
   MHGURecomp\
     mhgurecomp.exe
     mhgugame.dll
     (other .dll files)
     data\
       exefs\
         main
         main.npdm
         rtld
         sdk
         subsdk0
       romfs\
         nativeNX\
         (the rest of the game's romfs)
   ```

   `exefs` must come from the 1.4.0 update, and `romfs` must be the base game's romfs with the update's files
   merged over it.
3. Start `mhgurecomp.exe`.

If the files are missing or the wrong version, a message box says so.

### Where things are

| What | Where |
| --- | --- |
| Settings | `mhgurecomp.ini` next to the exe (written with the defaults on first start; delete it to reset) |
| Save data | `%APPDATA%\MHGURecomp\save\` (kept apart from the game folder so updating the port can't touch it) |
| Log | `log.txt` next to the exe (look here first when something goes wrong) |
| Screenshots | `shots\` next to the exe (F12) |
| Mods | `mods\` next to the exe: one folder per mod (see below) |
| Shader cache | `cache\` next to the exe (safe to delete; the first minutes after that may stutter) |

To move an existing Switch save over, put its `system` and `system_backup` files in
`%APPDATA%\MHGURecomp\save\save_0\`.

## Controls

A controller works out of the box (Xbox, PlayStation, Switch Pro and anything else SDL supports). By default the
face buttons follow the Switch layout by position (the right button is A); set `FaceButtons = Label` in the ini to go
by the letters printed on the buttons instead.

Keyboard:

| Key | Switch button |
| --- | --- |
| W A S D | left stick (hold Left Shift to walk) |
| Arrow keys | right stick (camera) |
| L / K / I / J | A / B / X / Y |
| Q / E | L / R |
| Z / C | ZL / ZR |
| Enter / Backspace | Plus / Minus |
| T / F / G / H | d-pad up / left / down / right |
| X / V | left / right stick click |

When the game asks for text (names, chat), the keyboard types: Enter confirms, Escape cancels, Ctrl+V pastes.

Other keys: Alt+Enter switches between windowed and full screen, F12 takes a screenshot, F7 cycles MSAA.

## Settings

Everything is in `mhgurecomp.ini`; each setting is explained in the file itself. In short:

- `[Display]`: `Mode` (Borderless / Fullscreen / Windowed), `Resolution`, `RenderResolution` (the 3D resolution)
- `[Anti-aliasing]`: `MSAA` (Off / 2x / 4x / 8x), `FXAA` (On / Off)
- `[Performance]`: `FPS` (30 to 240), `VSync` (On / Off; above your monitor's refresh rate, turn VSync off)
- `[Controls]`: `FaceButtons` (Position / Label)
- `[Multiplayer]`: `Port`, `Join` (addresses of hosts outside your LAN), `SyncSmallMonsters`
- `[Monster HP]`: HP factors for 1 to 4 hunters in Hub quests, and per-quest exceptions (default: all 1, the
  original game)
- `[Bonus Content]`: `BonusContent` (default Off), see below

### Local Play over the network

Local Play works like on the Switch, but over the network. In the game, choose Local Play. One player creates a hub:
the others reach that computer on port 39500 (TCP and UDP). On a LAN, Search Hubs finds it by itself. Over the
internet, the host forwards that port in their router, and the others put the host's address in `Join =` in the ini.
Online play (Capcom's servers) is not available.

### Bonus content

The free downloadable content (event and challenge quests, item packs, Palicos) came from Capcom's servers, which can't
be reached. If you have a save that already has the downloads, the port can copy them into saves that have none:
set `BonusContent = On` and put that save's `system` file in a `bonus` folder next to the exe (`bonus\system`). Only
the download data is copied, once, into saves that never had downloads. With the default `Off`, saves are never
changed.

### Mods

Put each mod's folder in `mods\`, and delete the folder to uninstall. Nothing on disk is changed: mods are layered over
the game at startup.
- romfs mods (files that replace or add game data) are found inside the mod's folder, including the
  `atmosphere\contents\0100770008DD8000\romfs` layout.
- exefs patches (`.pchtxt`, `.ips`) are applied when they change data. Patches to game code can't be applied, because
  the code is precompiled; the log lists them.
- Forge plugins (`.nro` in `romfs\nativeNX\plugins\`) are loaded. The plugin overlay (ImGui) is not available.
- The common 60 fps, 4K and connection-error-fix exefs mods aren't needed: the ini settings cover them.

## Building from source

The build recompiles the game code from your own copy of the game. Nothing in this repository contains game code or
data, and the build never uploads anything.

### 1. Tools (Windows)

1. Install [MSYS2](https://www.msys2.org/) to `C:\msys64`.
2. In the **MSYS2 UCRT64** shell, install the compiler and libraries:

   ```bash
   pacman -S --needed make git \
     mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-pkgconf \
     mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-shaderc \
     mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
     mingw-w64-ucrt-x86_64-vulkan-memory-allocator
   ```

3. Install [Python](https://www.python.org/) 3.10 or newer for Windows. Then, in a Command Prompt in the repository
   folder, make the tools' virtual environment (the Makefile uses `tools\venv` when it exists):

   ```bat
   py -3 -m venv tools\venv
   tools\venv\Scripts\pip install -r tools\requirements.txt
   ```

### 2. Recompile and build

From the UCRT64 shell, in the repository folder, with your game files in a folder holding `exefs\` and `romfs\` (as
under "Installing a release"):

```bash
make gen GAME=/c/path/to/game      # translate the game code to C in generated/ (~300 MB)
make -j4 GAME=/c/path/to/game      # compile everything into build/
```

The first full build takes a while (about 8 minutes on a 4-core Haswell with `-j4`; more cores: higher `-j`). Later
builds only recompile what changed.

`make` turns `build\` into a ready-to-run game folder:
- `mhgurecomp.exe`, the runtime;
- `mhgugame.dll`, the recompiled game code;
- the MSYS2 DLLs they need;
- `data\exefs` and `data\romfs`, hard links to your game files, so they take no extra disk space.

Start `build\mhgurecomp.exe`, or `run-game.bat`.

Useful targets and options:

- `make test-lift`: checks the recompiler against the Unicorn CPU emulator on thousands of the game's instructions.
- `make clean`: removes the compiled files only. It keeps `build\data`, the settings and the caches.
- `GENARCH=-march=x86-64-v2`: compiles the game code without AVX2, for older CPUs. Untested.

### Making a release

A release is the contents of `build\` without the game files and without your own files:
- Include `mhgurecomp.exe`, `mhgugame.dll` and all other `.dll` files.
- Leave out `data\`, `cache\`, `shots\`, `mods\`, `bonus\`, `mhgurecomp.ini`, `log*.txt` and the build's own files
  (`obj\`, `rt\`, `*.dll.a`, `gamelib.id`, test files).

The exe creates the `mods` folder and the ini on first start. Players then add their own `data\` as described above.

### Linux

The Windows build runs under Proton / Wine. A native Linux build is planned: the Makefile and runtime have a Linux
path, but `runtime/platform/linux.c` isn't written yet.

## Repository layout

| Folder | What's in it |
| --- | --- |
| `tools/` | the recompiler: `lifter.py` (ARM to C), `neon.py` (NEON / VFP), `gen.py` (whole modules), `nso.py`, test and research tools |
| `runtime/` | the host side: CPU state, kernel and threads, Switch services (`services/`, `hle/`), networking (`net/`), the NVN graphics API on Vulkan (`nvn/`), settings, input, mods |
| `docs/` | `STATUS.md`: progress, design notes, reverse-engineering findings, debug options |

## Legal

This project is not affiliated with or endorsed by Capcom or Nintendo. Monster Hunter is a trademark of Capcom. The
repository contains no game code, data or keys; the build reads them from a copy you provide. Please support the
developers by owning the game.
