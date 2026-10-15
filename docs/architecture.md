# Architecture

```
 game.zip / .pkg + .rap
        |  sc2hd install            (C++, inside the host: unpack, decrypt)
        v
 <data>/hdd0/game/NPUB31366/       installed game: EBOOT.elf, decrypted ROOT/UPDATE archives, media
        |  ppu_loader / ppu_lifter / SPU lift      (Python, from the SDK)
        v
 <data>/games/<id>/recompiled/, spu/              C++ and C source of the game
        |  clang or zig cc
        v
 <data>/games/<id>/module/game.dylib             the game, as a loadable library (.so / .dll elsewhere)
        |  dlopen
        v
 host/sc2hd_host --module game.dylib EBOOT.elf    the runtime: PPU/SPU support, HLE of the PS3 libraries, Metal/Vulkan/D3D12, SDL
```

## Why a host and a module

Everything derived from the game must stay on the user's machine, and the prebuilt part must be the same for everyone.
So the program is split in two:

- **host** (`src/`, built by `scripts/build_host.sh`): the ps3recomp runtime, the toolkit's PPU boot scaffold and this port's
  sources (`main.cpp`, the HLE overrides in `sc2hd_hle.cpp`, `sail_hle.cpp`, `spurs_lfqueue.cpp`, `mp3_hle.c`, the installer).
  SDL2, glslang, SPIRV-Tools and SPIRV-Cross are linked in statically. It contains no game data and no LGPL code.
- **module** (`game.dylib`, `.so` or `.dll`): the lifted PPU functions and SPU images and nothing else. Its undefined symbols
  (`vm_read32`, `ps3_hle_call`, the SPU channel API, …) are resolved against the host's exports when it is loaded.

The measured cost of the split is nil: CPU time of a run is the same within noise as with everything linked into one binary.

## The module ABI

- The host exports the whole runtime (`-force_load` + `-export_dynamic` on macOS, `--whole-archive` + `--export-dynamic` on ELF).
- The host finds `function_table` and `function_table_count` in the module with `dlsym` and keeps them in
  `g_game_function_table`. The host's `ppu_recomp.h` is the lifter's own header preamble without the per-game table
  (`scripts/make_host_header.py`), so the two cannot drift apart.
- A thread-local variable cannot be imported from another image (Mach-O and PE linkers refuse it). The lift reaches the
  trampoline state through accessor functions the host exports: `PPU_MODULE_ACCESSORS` (lifter prologue) and
  `SPU_MODULE_ACCESSORS` (`spu_context.h`), both in `patches/0022`.
- The module carries `sc2hd_module_abi`, a hash over the headers and lifter it was built against (`sdk/ABI`). The host refuses a
  module with another value, so a module is never run against a host it was not built for.

## The SDK

`host/sdk/` next to the host holds the lifter tools, the headers the lift includes and `spu_dispatch_mt.c`, the one SPU runtime
source that has to be compiled with the lift (it relies on `musttail`, so it needs clang, which `zig cc` is). The launcher
compiles the module against it.

## Installer

`sc2hd_host install --rap <file> --to <dir> <pkg>…` (`src/install.cpp`, `src/crypto.cpp`) unpacks retail PS3 packages to
`<dir>/game/<title id>/`, decrypts `USRDIR/EBOOT.BIN` (an NPDRM SELF) into `USRDIR/EBOOT.elf` and every `.EDAT` in place. The runtime
cannot decrypt licensed EDATs, so it serves the decrypted archive under the original name. Supported: retail PKGs, NPDRM SELFs, EDATs with
the layouts of this game; compressed and SDAT EDATs are not. The crypto (AES, SHA-1, HMAC, CMAC, inflate) is in `crypto.cpp`
with a self-test (`sc2hd_host selftest`). The algorithms and key tables follow RPCS3's `unself.cpp`, `unedat.cpp` and `key_vault.cpp`.

## Launcher

`launcher/` is a small Python program (standard library only, Python 3.10+): `game.py` installs and reads `PARAM.SFO`, `lift.py` runs
the SDK's tools, `module.py` compiles the translation units in parallel (cached per unit by source and command) and links
the module, `toolchain.py` picks a compiler or downloads a pinned zig, `runner.py` sets the environment and starts the host.
`launcher/helpers/` are the SC2-specific steps: SPU overlay images for the audio engine's DSP plugins, and stand-ins for the two
firmware SPU modules the game loads (no Sony code).

## Monolith (personal use)

`./sc2hd.py monolith` then `./sc2hd.py --monolith` link the lifted code into one executable (CMake `-DSC2HD_MONOLITH_DIR`), the way
the port was built before the split (with the game's own `ICON0.PNG` as the icon). It needs a source checkout and the static dependencies, and the result contains code
derived from the game, so it must not be shared.

## Not done

- Windows: `SC2HD_TARGET=windows scripts/build_host.sh` cross-compiles the host with MinGW-w64; it exports its runtime and writes
  an import library (`sdk/libsc2hd_host.dll.a`) that the module DLL links against (the module exports only what the host looks
  up; the DLL names `sc2hd_host.exe`, so the host must keep that file name). Both run under Wine on a Mac up to the end of the
  intro; after it the game runs at about 1 frame/s with an audio engine that delivers 6 of every 14 blocks. The audio gaps are not
  the renderer's (`DRAW_LIMIT=1` leaves them as they are) and not Rosetta's speed (the x86-64 Linux build under Rosetta mixes cleanly), so
  it is either Wine or a Windows-specific wait in the runtime; it needs a run on real Windows. The D3D12 backend is otherwise
  untried. `scripts/package.sh` packs it; that package has not been run on Windows.
- Linux: `build_host.sh` builds the toolkit's Vulkan backend (`-DPS3RECOMP_RSX_VULKAN=ON`, headers only: it loads `libvulkan` at run
  time) and `main.cpp` opens it with `PS3RECOMP_VK_WINDOW=1` and the guest's own shaders (`PS3RECOMP_VK_GUEST_PROGRAMS=1`): the
  frame is rendered offscreen and blitted into an SDL2 window's swapchain. SDL needs the X11 (or Wayland) headers at build time,
  or it ends up with no video driver. The window is not given an icon yet. No `DISPLAY`/`WAYLAND_DISPLAY`, `SC2HD_HEADLESS` or a
  failed Vulkan init select the null backend instead. Checked under Xvfb with Mesa llvmpipe: the intro movie and the menus draw
  correctly (`PAD_AUTOPRESS=1` gets past the intro), and `Esc` closes the window. The toolkit's keyboard fallback was Windows and
  macOS only; `patches/0029` adds an SDL one (`SDL_GetKeyboardState`, the same keys, while the window has the focus), checked with
  `xdotool` under Xvfb: Return leaves the intro, arrows and Z work in the menus.
- Icon: the host carries the neutral `assets/icon/sc2hd.png` (`scripts/make_icon.py` turns it into the Windows icon resource
  and a bitmap for the macOS Dock icon, set through a hook the Metal backend calls, `patches/0028`).

## Movies

`src/media/` is a self-contained, portable (C99 plus a C++ worker) movie player: `mp4.c` (demuxer), `h264*.c` (High profile CABAC
decoder: I/P/B, multiple references, weighted prediction, direct modes, 8x8 transform, deblocking; progressive 4:2:0 only),
`aac.c` (AAC-LC, mono/stereo), `movie.cpp` (a decoder thread feeding small picture and PCM queues). `sail_hle.cpp` connects it to
the game's cellSail adapters. `tests/check_media.py` encodes eleven clips with ffmpeg/libx264 (B-pyramid and MMCO, temporal and spatial direct, weighted
prediction, slices, constrained intra, cropping, several sample rates, TNS) and compares both decoders with ffmpeg: video must be
bit-identical, audio must match to float precision (`SC2HD_TESTS=ON` registers it with ctest). The decoders cover what the game's
movies use plus that; PNS, intensity stereo, CAVLC, interlace, I_PCM (untested) and frame_num gaps are marked TODO in the code.
