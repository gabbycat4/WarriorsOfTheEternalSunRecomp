<div align="center">

<img src="packaging/linux/wotes-easyontheeyes.png" alt="Warriors of the Eternal Sun: Easy on the Eyes" width="128" height="128">

# Warriors of the Eternal Sun: Easy on the Eyes

*A native Linux port of Warriors of the Eternal Sun (Sega Genesis, 1992) with 240 Hz smooth motion*

</div>

An unofficial, fan-made **static recompilation** of *Dungeons & Dragons: Warriors of the
Eternal Sun* (Westwood / Sega) for Linux, built on
[segagenesisrecomp](https://github.com/mstan/segagenesisrecomp). The game's 68000 code is
translated to native C and runs without an emulator; the picture is drawn with the engine's own
VDP renderer, then made **smooth and calm for sensitive eyes**.

**You need your own Warriors of the Eternal Sun ROM** ([which one](#supported-rom)). Nothing
from the game is in this repository or in the download.

## What's different from the original

All of these change only the picture or remove technical hiccups; the game plays by its
original rules.

| | |
|---|---|
| **240 Hz smooth motion** | In-between pictures at your display's rate (up to 240 a second). The world map scrolls in quarter-pixel steps or finer instead of jumping whole pixels; battle hops and spells glide. Drawn on the graphics card, checked against the real picture. |
| **Calm mountain shading** | The 1-pixel checkered shadows that shimmered while scrolling are shown as the even shade a TV made of them. |
| **Steady battle camera** | The battle camera's stop-and-go following and its sudden swings become one smooth, steady glide. |
| **Even party walking** | Your party members' lumpy steps on diagonals are evened out. |
| **No lag hitches** | The original world map skipped a frame now and then when the Genesis CPU ran out of time. Here the game gets enough CPU time; random battles still come at the usual rate. |
| **Dash** | Hold **B** while walking on the world map or in town to move twice as fast (not in dungeons). |
| **Fast travel** | On the world map, stand still and hold **A + C** for one second to go to the town (you arrive just inside its north gate). |

## Play on Linux

1. Download `WarriorsOfTheEternalSun-EasyOnTheEyes-x86_64.AppImage` from the
   [Releases](../../releases) page.
2. Make it executable: right-click it → Properties → Permissions → *Allow executing file as
   program*, or in a terminal: `chmod +x WarriorsOfTheEternalSun-EasyOnTheEyes-x86_64.AppImage`
3. Double-click it and choose your ROM in the launcher. (You can also put the ROM into
   `~/.local/share/WarriorsOfTheEternalSunRecomp/`.)

You need SDL2 installed (almost every desktop distribution has it; Arch/CachyOS: `sdl2-compat`,
Ubuntu/Debian: `libsdl2-2.0-0`). The AppImage is built on a current distribution (CachyOS), so it
runs on similarly recent systems; on an older one, [build it yourself](#build-it-yourself).

Your settings and saves are kept in `~/.local/share/WarriorsOfTheEternalSunRecomp/`.
High refresh rate is on from the first start; **Escape** opens the menu (**Mods → High refresh
rate** to change it). Play fullscreen for the finest steps.

### Switching features off

Every feature can be turned off for comparison by starting the game with an environment
variable, for example `WOTES_CAM_EASE=0 ./WarriorsOfTheEternalSun-EasyOnTheEyes-x86_64.AppImage`:

| Variable | Off | What it controls |
|---|---|---|
| `WOTES_VIDEO_GPU` | `0` | Graphics-card drawing of the in-between pictures (falls back to the CPU) |
| `WOTES_DITHER_BLEND` | `0` | Calm mountain shading |
| `WOTES_CAM_EASE` | `0` | Steady battle camera (or a stiffness, default `0.80`) |
| `WOTES_PARTY_SMOOTH` | `0` | Even party walking on the map |
| `WOTES_CPU_HEADROOM` | `1` | Extra CPU time (`1` = the original timing, with its hitches) |
| `WOTES_DASH` | `0` | Dash on B |
| `WOTES_FAST_TRAVEL` | `0` | Fast travel on A + C |

## Supported ROM

*Dungeons & Dragons: Warriors of the Eternal Sun* (USA, Europe), 1 MB, CRC32 `AF4A9CD7`
(serial GM MK-1304 -00). The game checks it at start.

## Build it yourself

Needs: git, cmake, ninja, gcc/g++, Python 3, SDL2 development files
(Arch/CachyOS: `sudo pacman -S --needed cmake ninja sdl2-compat`; Ubuntu/Debian:
`sudo apt install cmake ninja-build g++ libsdl2-dev python3`).

```sh
git clone --recurse-submodules https://github.com/gabbycat4/WarriorsOfTheEternalSunRecomp.git
cd WarriorsOfTheEternalSunRecomp
cp /path/to/your/rom.bin game/wotes.bin
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEN_ENABLE_TRACE=OFF -DSONIC_REVERSE_DEBUG=OFF
cmake --build build --target WotESRecomp
./build/WotESRecomp game/wotes.bin
```

The first build generates the game's code from your ROM and takes a while. To make the AppImage:
`bash tools/release/make_appimage.sh` (refuses to package if the ROM, saves or logs would end up
inside).

## How it was made

Recompiled with segagenesisrecomp, then measured frame by frame before anything was smoothed.
The full history of every finding (what was measured, what was tried, what worked) is in
[`notes.txt`](notes.txt); [`HANDOFF.txt`](HANDOFF.txt) summarizes the current state. The game's
own code changes are in `game/` (`wotes_video.c`: the smooth pictures; `wotes_spec.c`: CPU
headroom, encounter check, dash, fast travel); engine fixes are in the
[segagenesisrecomp fork](https://github.com/gabbycat4/segagenesisrecomp) (branch `wotes`).

## Credits and license

- [segagenesisrecomp](https://github.com/mstan/segagenesisrecomp),
  [m68k-recomp-core](https://github.com/mstan/m68k-recomp-core) and
  [recomp-ui](https://github.com/mstan/recomp-ui) by Matthew Stanley (mstan) and contributors:
  the recompiler, the runtime and the launcher this port is built on.
- *Warriors of the Eternal Sun* © 1992 Sega / Westwood Associates; *Dungeons & Dragons* is a
  trademark of Wizards of the Coast. This project is not affiliated with or endorsed by any of
  them, and contains none of the game's data.

This project is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE), the same as
segagenesisrecomp: free to use, share and change, not for commercial use. Third-party components
and their licenses are listed in
[`segagenesisrecomp/THIRD-PARTY-LICENSES.md`](https://github.com/gabbycat4/segagenesisrecomp/blob/wotes/THIRD-PARTY-LICENSES.md)
and inside the AppImage.
