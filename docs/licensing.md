# Licensing of the recompilation port

## Our sources

The port (`src/`), the launcher (`launcher/`), the scripts (`scripts/`), the patches in `patches/` and the host's icon
(`assets/icon/`, original artwork drawn by `scripts/gen_icon_art.py`: no game or publisher artwork)
are **GPL-2.0-or-later**. The installer (`src/install.cpp`, `crypto.cpp`) ports the NPDRM algorithms and key tables of
RPCS3 (GPL-2.0-or-later); the patches apply to the MIT-licensed toolkit and stay under its license where they touch its code.

## What goes into the host binary

| Component | License |
|---|---|
| ps3recomp toolkit (runtime, scaffold, HLE libraries) | MIT |
| SDL2 (static) | zlib |
| glslang (static) | BSD-3-Clause, with MIT and Apache-2.0 parts |
| SPIRV-Tools (static) | Apache-2.0 |
| SPIRV-Headers | MIT-style (Khronos) |
| SPIRV-Cross (static; not in the Windows host) | Apache-2.0 |
| Vulkan-Headers (Windows: compiled in, `vulkan-1.dll` is the system's) | Apache-2.0 / MIT |
| winpthreads and libstdc++ of the MinGW-w64 toolchain (Windows, static) | MIT-style / GPL-3.0 with the GCC Runtime Library Exception |
| dr_mp3 / minimp3 core (`mp3_hle.c`) | public domain |
| H.264, AAC-LC decoders and MP4 demuxer (`src/media/`) | ours, GPL-2.0-or-later; the numeric tables of the two standards in `h264_tables.h` / `aac_tables.h` are generated from FFmpeg's copy (`scripts/gen_media_tables.py`), no FFmpeg code is used |
| ATRAC3plus decoder (LGPL-2.1) | **not linked**: built with `PS3RECOMP_ATRAC_LGPL=OFF`, the game imports no cellAtrac |

Apache-2.0 is compatible with GPL-3.0 but not with GPL-2.0-only. Our code allows "or later", so the host binary as a whole
is distributed under **GPL-3.0-or-later**. Every component above allows that. Ship the licenses and notices of the table
with a binary.

## What must not be shared

The game module (`game.dylib` / `.so` / `.dll`), the optional monolith (`sc2hd_monolith`), the lift (`recompiled/`, `spu/`),
the installed game (`hdd0/`) and the decrypted ELF are derived from the user's copy of the game. They live in the data
directory and are never part of a release. A monolith also carries the game's `ICON0.PNG` as its icon (the host uses
the neutral one), which is one more reason not to share it.

## Downloaded at run time

`zig` (MIT), pinned by version and SHA-256 in `launcher/sc2hd_launcher/toolchain.py`, into the data directory, on the user's consent.
