# triggerlook

Client mod for Counter-Strike: Source (64-bit, Linux and Windows): the triggers a server shows you (`!st`,
`!showtriggers`, `!sts` and the like) drawn in your own look instead of the orange "TRIGGER" texture. Pick a colour,
an opacity and a border for each kind of trigger in an in-game menu, and watch the triggers in front of you change
while you click.

It only changes what your client draws. It doesn't show anything the server doesn't already send you: with the
server's show-triggers off, you see nothing new. The server doesn't need anything installed, and other players
don't need it.

Downloads are on the [releases page](https://github.com/brybry333/triggerlook/releases).

**Only use it with `-insecure`.** It loads code into the game process, which is a VAC risk on secured servers. It
refuses to start in a game that wasn't started with `-insecure`.

Licensed under GPL-3.0, see `LICENSE`.

## Install

**Linux:** put `libtriggerlook.so` and `triggerlook.so` together in a folder without spaces in its path and set
CS:S's launch options to

```
LD_PRELOAD="/full/path/to/libtriggerlook.so:$LD_PRELOAD" %command% -insecure
```

**Windows:** keep `triggerlook.exe` and `triggerlook.dll` in the same folder and run `triggerlook.exe`. It starts
CS:S with `-insecure` (your Steam launch options still apply) and loads the DLL. If CS:S is already running with
`-insecure`, it loads the DLL into that game instead; it refuses a game started without `-insecure`.

Any mod that loads `triggerlook.dll` / `triggerlook.so` (and on Linux calls `TriggerLook_Init()` on the game's main
thread) works the same; the DLL starts itself.

On its first start it writes its material and the menu font into the game folder
(`cstrike/materials/triggerlook/`, `cstrike/resource/triggerlook/`).

## Use

Bind a key to the menu, for example in the console: `bind f6 triggerlook_menu`. Then join a server and turn its
show-triggers on.

- **SHOW**: one button per kind of trigger. White = drawn, grey = hidden.
- **EDIT**: the same kinds, each in its own colours (fill inside, border round it). The settings below change the
  picked ones. Click picks one, shift + click picks more than one.
- **FILL / BORDER**: which part the colours and the opacity change.
- Colour blocks, your last custom colour, **CUSTOM COLOUR** (a picker, also live), **OPACITY**, **BORDER WIDTH**
  (OFF up to 16 units).

Esc or X closes it. Settings are saved by themselves, per PC: `~/.config/triggerlook/settings.txt` on Linux,
`%APPDATA%\triggerlook\settings.txt` on Windows.

| Kind | Which triggers |
|---|---|
| TELEPORT | `trigger_teleport`, `trigger_teleport_relative` |
| BOOSTER | triggers with a `basevelocity` output |
| PUSH | `trigger_push` |
| GRAVITY | `trigger_gravity`, triggers with a `gravity` output |
| OTHER | every other `trigger_*` |

Console: `triggerlook_menu`, `triggerlook_enable 0/1` (0 = the game's own trigger look), `triggerlook_info`,
`triggerlook_set <kind|all> [show 0/1] [fill r g b a] [border r g b a] [width n]`. (`triggerlook_cull`,
`triggerlook_ui` and `triggerlook_selftest` are for testing.)

## Limits

- It replaces the look of triggers drawn with the standard trigger texture (`tools/toolstrigger`). A trigger brush
  with some other texture shows both looks.
- Tested in game on Linux and on the Windows build under Proton (menu driven with real mouse and keyboard input
  there), not yet on a Windows PC.

## How it works

- The trigger shapes come from the map file: each trigger's brushes are rebuilt from their planes, so every side is
  one polygon with its real edges (the drawn faces are cut up by the map compiler, which would put borders across
  the middle of a side). The border is a band inside each side along its edges.
- Every frame it goes through the entities the server sent, picks the triggers, and draws their sides that face you
  and are in view, in one material with per-vertex colour. The game's own trigger material is set to draw nothing
  while it's on.
- It draws only in your own view (not in the 3D skybox, reflections or monitors).
- The map is read through the game's filesystem (the same copy the game loaded), with limits that keep a broken or
  hostile map from hanging or crashing it.
- No game code is patched: it uses the interfaces the game exports (entity list, model info, leaf system, material
  system, VGUI, console). On Windows it gets onto the game's main thread through a one-shot message hook on the game
  window's thread. A second copy (two loaders) stays off.

## Building

It needs only Source SDK 2013 headers (and the SDK's tier0 import library for Windows). Clone this repo somewhere
without spaces in the path, then get the SDK:

```
git clone --filter=blob:none --sparse https://github.com/ValveSoftware/source-sdk-2013 third_party/source-sdk-2013
cd third_party/source-sdk-2013 && git checkout be57e3d01ac7d7aa6a2cacbfef187c0b46616db0
git sparse-checkout set src/common src/public src/tier1 src/utils/lzma src/lib/public
```

**Linux:** `make && make check-glibc` builds `build/triggerlook.so`, `build/libtriggerlook.so` and
`build/geomtest`. `check-glibc` makes sure both still load on the Steam runtime's glibc 2.31 and the mod exports
only `TriggerLook_Init` and `triggerlook_enable_set`.

**Windows** (cross-compiled on Linux): `win/build.sh` builds `triggerlook.dll` (clang-cl, MSVC ABI) and
`triggerlook.exe` (MinGW-w64) into `win/build/out/`. It needs clang, lld, mingw-w64-gcc and an
[xwin](https://github.com/Jake-Shadle/xwin) splat in `~/.local/share/xwin` (or set `XWIN=`).

**Test:** `LD_LIBRARY_PATH=<game>/bin/linux64 build/geomtest <game>/cstrike maps/<map>.bsp` reads a map's triggers
and checks every side and its mesh (flat, convex, facing out, closed solids, band and fill covering each side
exactly once). `GEOM_WIDTH=<n>` sets the border width, `GEOM_LIST=1` lists the triggers.

## Files

| Path | What |
|---|---|
| `src/tl_runtime.cpp` | start, drawing, console commands |
| `src/tl_ui.cpp` | the menu |
| `src/tl_map.cpp`, `src/tl_mesh.cpp` | trigger shapes from the map, fill + border meshes |
| `src/tl_look.cpp` | the look per kind and its settings file |
| `src/tl_con.cpp` | console commands without tier1 |
| `src/tl_platform_linux.cpp`, `src/tl_platform_win.cpp` | OS layer |
| `src/glibc_compat.cpp` | keeps the Linux build loadable on glibc 2.31 |
| `src/loader/triggerlook_loader.c` | `libtriggerlook.so`, the Linux preload that loads the mod |
| `win/launcher.c` | `triggerlook.exe` |
| `game/` | the material and the menu font (embedded into the mod by `tools/embed.py`) |
| `tools/geomtest.cpp` | the offline map test |
| `tools/wininject.c`, `tools/wininput.c` | Windows test helpers (load the DLL; drive the menu with real input) |

## Credits

triggerlook is by brybry333. `src/glibc_compat.cpp` is adapted from
[bhoptools](https://github.com/ddyfad/bhoptools) (GPL-3.0). `win/sdk-compat/` holds two Source SDK 2013 headers with
small changes for clang-cl. The menu font is [Roboto Mono](https://fonts.google.com/specimen/Roboto+Mono)
(Apache License 2.0, `game/resource/triggerlook/RobotoMono-LICENSE.txt`).
