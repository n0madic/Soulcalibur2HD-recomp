# Soulcalibur II HD Online — native build by static recompilation

This project runs the PlayStation 3 game **Soulcalibur II HD Online** natively on a Mac/PC. It is not an emulator: the game's
PowerPC (PPU) and Cell SPU code is translated ahead of time into C/C++ and compiled for your machine, on top of the
[ps3recomp](https://github.com/sp00nznet/ps3recomp) runtime. Graphics go through Metal on macOS and Vulkan on Linux and Windows, sound through SDL (WASAPI on Windows).

**You bring the game.** Nothing from the game, from Sony's firmware or from anyone's PS3 is in this repository or in the
release zip. On first start the launcher installs *your* copy of the game, translates it and compiles the result on *your*
computer. The translated code never leaves it.

> Status: **works on Apple Silicon Macs (Metal)**, and on Windows (Vulkan; tested on an Intel Core Ultra 7 258V / Arc 140V
> laptop, see [Known issues](#known-issues)). The host and the game module also build and run on Linux (a Vulkan window, tried
> only on a software rasteriser, including under WSL2).

## What you need

| | |
|---|---|
| Computer | Apple Silicon Mac (tested on an M4 Max, macOS 26) |
| Python | 3.10 or newer (`brew install python`, or python.org). Only the standard library is used |
| Disk | about 1.5 GB for the installed game and the build, plus about 60 MB for the compiler if it has to be downloaded |
| Internet | only on the first run, to download the compiler (see below) |
| Game | the PSN release **NPUB31366** (USA): the game package and its license; the 01.01 update package is optional |

The files look like `…SC2HDONLINEGAME1.rap` (license), a long-named `….pkg` (the game, about 700 MB, often inside a `.zip`) and
`UP0700-NPUB31366_00-SC2HDONLINEGAME1-A0101-V0100-PE.pkg` (the update). Versions 01.00 (the game alone) and 01.01 (with the update) are supported; the update is optional.

The translated game is compiled with the Xcode command line tools' `clang` if you have them, otherwise the launcher downloads
[zig](https://ziglang.org) 0.17.0 (about 55 MB, MIT, checked against a pinned SHA-256) into the data directory and uses its `zig cc`.
It asks before downloading (`--yes` answers for you).

## Quick start

```sh
unzip sc2hd-0.1.0-macos-arm64.zip
cd sc2hd
xattr -dr com.apple.quarantine .      # only if macOS refuses to open the downloaded program

# first run: give it the game (the zip is fine as it is) and, optionally, its update; the .rap is found inside the zip
./sc2hd.py --pkg "Soulcalibur II HD Online (USA).zip" --pkg UP0700-NPUB31366_00-SC2HDONLINEGAME1-A0101-V0100-PE.pkg

# every later run
./sc2hd.py
```

`./sc2hd.py` is a Python 3.10+ script: on Windows (or if the execute bit is lost) start it as `py sc2hd.py` or `python3 sc2hd.py`.

If the license is not in the zip, add `--rap <file>.rap`. Give the game first and its update (if you want it) second. To add the update later, run again with both packages: the game is reinstalled and translated again (your saves are kept).

The first run takes a few minutes (about two on a fast machine):

1. **install** (seconds): unpacks the packages and decrypts them with your license;
2. **lift** (about 10 s): translates the game's PPU code and its five SPU programs to C/C++;
3. **compile** (about 30 s on eight cores): builds that into one library, `game.dylib`;
4. **run**.

Later runs go straight to step 4. Nothing is redone unless the inputs change.

## Options

```
./sc2hd.py [--fxaa | --no-fxaa] [--af N] [--window WxH]
```

| Option | Meaning |
|---|---|
| `--fxaa`, `--no-fxaa` | FXAA anti-aliasing of the finished picture (default on). It also softens the HUD a little (macOS) |
| `--af N` | anisotropic texture filtering 1 (off) … 16 (default 16) (macOS) |
| `--window WxH` | initial window size in points, e.g. `1600x900` (default 1280x720). The window can be resized; the 16:9 picture keeps its shape and the rest is black (macOS) |
| `--data DIR` | where everything derived from the game lives (default `./data` in the directory you start from, or `SC2HD_HOME`) |
| `--compiler auto\|system\|zig` | which compiler builds the game library (default: a suitable system clang, else zig) |
| `build` / `run` | only build / only run |

`./sc2hd.py --help` lists the rest.

## Controls

The keyboard is mapped like a pad: arrows = D-pad, `Z` = ✕, `X` = ○, `A` = □, `S` = △, `Q`/`W` = L1/R1, `1`/`2` = L2/R2,
`Return` = Start, `Tab` = Select. `Cmd+Q` (Linux: `Esc`) or closing the window quits. Game controllers go through SDL; the keyboard and
controllers are not yet tested thoroughly.

## Where things are

Everything derived from the game is in the data directory (`./data` unless you moved it):

| Path | What |
|---|---|
| `hdd0/game/NPUB31366/` | the installed game (decrypted) |
| `hdd0/home/…/savedata/` | **your save data** |
| `games/NPUB31366-<version>/` (01.00 or 01.01) | the translated code and `module/game.dylib` |
| `toolchain/` | the downloaded compiler, if any |

Delete the data directory to start over (keep `hdd0/home` if you want your saves). **Do not share the data directory or the files in it:** they are
derived from the game.

## Known issues

- A frame from the end of the previous fight can flash for a moment between the fighter intro and the arena. The game's own
  screen-copy command is sometimes missing from the stream the recompiled code produces; the cause is not found yet.
- The intro and ending movies are decoded by a built-in H.264/AAC decoder (only what those movies use). The pictures follow
  the sound as the game takes it, with a fixed 100 ms allowance for the game's own buffering (`SC2HD_AV_OFFSET_MS` changes it);
  the game's timing feedback is not used, so the sync can be a little off.
- Online Mode is not available: the game shows its own "sign in to PlayStation Network" message and the menu stays usable.
- FXAA softens the HUD slightly. The game renders at 1280x720; there is no internal resolution scaling.
- MP3 music is decoded on the host; about 9 % of the frames differ slightly from the console's decoder at loud transients.
- Rare dropped frames from display-list decoding failures, mostly while a battle loads.
- Linux: the picture is shown in an SDL2 window through the toolkit's Vulkan backend (X11 or Wayland, `Esc` or closing the window quits). It has
  only run on Mesa's software rasteriser (llvmpipe), under Xvfb and in WSL2's WSLg, never on a real GPU: the picture is right, but a
  fight runs at about 10 frames a second there (in WSL2, Ubuntu's Mesa has no Vulkan driver for the GPU, so llvmpipe is what runs). There
  is no FXAA, no anisotropic filtering and the window is a fixed 1280x720. With no `DISPLAY` or `WAYLAND_DISPLAY` (or with
  `SC2HD_HEADLESS=1`), and when there is no Vulkan device, the host runs on the toolkit's null backend instead, with no window.
- Windows: the game is drawn through the Vulkan backend in an SDL window (D3D12 is only the fallback for a machine with no
  Vulkan driver, `SC2HD_D3D12=1`; it draws nothing after the intro). Menus and movies run at 60 frames a second, and so does a
  fight on an integrated GPU (Intel Arc 140V) once its stage has loaded (the first seconds, while its shaders are built, are
  slower); `SC2HD_FPS=1` prints the rate. `Esc` closes the window, as on Linux. The intro movie now plays in full
  (about two and a half minutes); Start skips it.

## How it works, building from source

- [docs/architecture.md](docs/architecture.md): the pieces (host, game library, launcher), how the game is installed, lifted and
  built, the ABI check.
- [docs/porting-notes.md](docs/porting-notes.md): what had to be done to the toolkit and to the game's own quirks to make it run.
- [docs/licensing.md](docs/licensing.md): the licenses of everything involved.

To build the release yourself you need git, cmake, ninja, Python 3.10+ and a C/C++ compiler (on Linux also the Vulkan and X11
headers, and the PulseAudio or ALSA headers for sound: `apt install libvulkan-dev libx11-dev libpulse-dev libasound2-dev`, and at
run time a Vulkan driver such as `mesa-vulkan-drivers`):

```sh
scripts/build_host.sh     # clones the pinned toolkit, applies patches/, builds the static dependencies and the host
SC2HD_TARGET=windows scripts/build_host.sh   # cross-compile for Windows with MinGW-w64 (clang preferred) into dist/host-windows
                          # (in Git Bash on Windows, plain scripts/build_host.sh builds the same natively)
scripts/package.sh        # a dist/sc2hd-<version>-<os>-<arch>.zip for every host that is built (macOS/Linux, Windows)
./sc2hd.py --pkg …                # in a checkout the launcher uses dist/host
```

## License

The code in this repository is free software under the **GNU GPL, version 2 or (at your option) any later version**
([LICENSE-GPL-2.0.txt](LICENSE-GPL-2.0.txt)). The prebuilt program also contains Apache-2.0 licensed parts, so as a whole it is
distributed under **GPL version 3 or later** ([LICENSE](LICENSE)). Soulcalibur is a trademark of Bandai Namco Entertainment; this project is not affiliated with or
endorsed by them. It contains none of their material.

## Credits

[ps3recomp](https://github.com/sp00nznet/ps3recomp) (the recompilation toolkit and runtime), [RPCS3](https://rpcs3.net) (the
package and license formats, whose algorithms the installer follows), SDL, glslang, SPIRV-Tools and SPIRV-Cross, minimp3/dr_mp3,
and zig.
