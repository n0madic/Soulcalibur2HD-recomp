# Porting notes

What had to be changed in the [ps3recomp](https://github.com/sp00nznet/ps3recomp) toolkit (pinned at `a2c20108`, v0.14.0 + PR 246)
and added around it to make Soulcalibur II HD Online (PS3, version 01.00 or with update 01.01) run. The changes to the toolkit are `patches/`, applied
in order to a fresh clone by `scripts/build_host.sh`; the port's own code is in `src/`. The toolkit's README lists macOS as
"runs a recompiled game: no": its boot scaffold had never been booted on macOS before this port.

## What differs from the toolkit template
- `main.cpp`: on Apple the guest runs on a secondary thread (256 MB host stack) and the main thread is the frame clock.
  Metal creates its window with `dispatch_sync(main queue)`; with the guest on the main thread that call never returned, the
  RSX FIFO was never drained and the game waited forever on its first fence label.
- `main.cpp`: reads `PARAM.SFO` (`PS3_PARAM_SFO`) so `cellGame` knows title id and category.
- `patches/0001`: `cellGameBootCheck` reports an HDD game for `CATEGORY=HG` (the game aborts unless the type is 2).
- `sc2hd_hle.cpp`: the cellFiber scheduler API (`InitializeScheduler/CreateFiber/RunFibers/WaitSignal/SendSignal/JoinFiber/
  FinalizeScheduler`, on ucontext), `_sys_spu_printf_initialize`, `sceNpDrmIsAvailable`. The toolkit has none of these.
- Game data: the runtime cannot decrypt licensed EDATs, so `sc2hd install` (`install.cpp`) decrypts them under their own names
  (`ROOT.EDAT`, `UPDATE.EDAT`) and `EBOOT.BIN` into `EBOOT.elf` when it unpacks the packages.

## SPURS queues
The port has no Sony firmware code. cellSpurs is the toolkit's HLE; the one part it cannot do, the lock-free work queues the game
feeds from the PPU while its SPU tasks pop them, is `src/spurs_lfqueue.cpp`: `_cellSpursLFQueueInitialize` and
`_cellSpursLFQueuePushBody` (the only two the game imports) for ANY2ANY queues (direction 3, the kind RPCS3 has no code for),
moving the same fields of the 128-byte queue line as the SPU side does (layout and protocol at the top of the file). Its
SPURS-instance side comes from `patches/0002` (every taskset is its own workload: unique wid, bit in `wklEnabled`, taskset EA in
`wklInfo1[wid].arg`), which is where a pushed entry finds the task to signal. Attach/DetachLv2EventQueue stay no-ops, so a blocking
push on a full queue ends in STAT, as on the console without an attached event queue.
- Not implemented (the game does not call them): `_cellSpursLFQueuePopBody`, push on directions 0..2, waiting on the event queue (TODO markers).

## Status
The game runs: Namco / Bandai Namco logos, Project Soul logo, title screen, attract demos, MODE SELECT, CHARACTER SELECT, the
VS screen and a full Arcade fight (Talim vs Xianghua: stage, health bars, timer, animations) render in Metal at full brightness.
Verified with `RSX_ENGINE_SNAP_DIR` snapshots and scripted input (`PAD_SCRIPT=12:0x0008,20:0x4000,26:0x4000,...`: START, then
CROSS presses). 10 of 10 consecutive launches reach the title screen.

What was needed to get here (beyond the SPURS work above):
- `sail_hle.cpp`: cellSail player lifecycle (events, descriptors, futures) and the two adapters. The game's callback tables live on its
  stack and are copied at `*AdapterInitialize`. A picture is delivered as RGBA into a buffer the game allocated through the graphics
  adapter's alloc callback; `GetFrame` returns the frame status in its result (`NEXT << 8 | sync`, 0 = none) and the game
  draws nothing when told there is no frame, so the previous picture is returned again (`REPEATED`) between two movie frames
  (the game polls at 60 Hz, the movie runs at 29.97). Sound is handed out as float32 PCM, never more than 250 ms ahead of the
  stream clock. Decoding is in `src/media/` (see docs/architecture.md). Without the player the boot job waited on the movie forever.
- `sc2hd_hle.cpp`: `SBCSstoUTF8s` with its real 5-argument signature (it was reading a NULL at 0x1D), `cellSpursLFQueueDetachLv2EventQueue`.
- `patches/0004`: `CELL_MSGDIALOG_BUTTON_NONE` is -1.
- `patches/0005`: macOS keyboard fallback for cellPad (arrows, Z=CROSS, X=CIRCLE, A, S, Q, W, 1, 2, Return=START, Tab=SELECT).
- `patches/0006`: RSX display lists are copied when walked; the game's 4x supersampled scene is resolved by the NV3089 2:1 tile blits into
  the LDR texture the final pass reads.
- `patches/0007`: first boot has no save. `cellSaveDataAutoLoad2` used to report that as CELL_OK, so the game decoded an all-zero
  buffer as its options (brightness step 0: the picture came out at a tenth of its brightness, "dark" or "black"). `main.cpp` now sets
  `PS3_SAVEDATA_NODATA_OK=2` (ERROR_CBRESULT, which is what the hardware returns when the stat callback answers ERR_NODATA): the game
  offers to create a save, `AutoSave2` writes it, later runs load it. Saves live under `<data>/hdd0/home/...`.
- `patches/0008`: the game double-buffers its display lists assuming the RSX is at most a frame behind, and queues a dozen frames
  while the walker is starting or stuck in present(). A one-off list (it carries the 2D vertex-program matrix) was overwritten
  before the walker reached it in about every second launch and the whole UI stayed black. Lists are now copied when the guest
  stores `put` (hook in `vm_write32`), up to their RET. `main.cpp` also starts the guest only after the RSX backend is up.
- `patches/0009`: `RSX_ENGINE_SNAP_DIR=<dir>` (+ `RSX_ENGINE_SNAP_EVERY_S`, default 5) writes the presented frame to PPM every n seconds,
  which is how a run is followed without watching the window.

Debug aids: `PAD_SCRIPT="<sec>:<mask>,..."` and `PAD_FILE=<path>` (cellPad masks: START 0x0008, UP 0x0010, DOWN 0x0040, LEFT 0x0080,
RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000), `PS3_FSLOG=1` (aio reads), `SC2HD_SAIL_TRACE=1`, `RSX_ENGINE_PASS_DUMP=<dir>`
(+ `_AFTER`, `_MAX`, `_DRAWS`), `RSX_CONST_LOG=<lo>,<hi>`, `NV3089_GATE=<file>`.

Memory: the depth-map snapshot of every frame stayed allocated (3-4 MB a frame, 28 GB after two minutes of a fight);
`patches/0010` releases it, the process now stays around 0.6 GB.

Audio works (BGM, voices, effects through SDL). The game's audio engine is Sony MultiStream: an SPU program
(image 3) plus an MP3 decoder `flashMP3.pic` that it loads from `/dev_flash`. What it took:
- `patches/0011`: an SPU thread's lifted image is chosen by the content of its image (two images had code at the entry
  address 0xD0/0xF0 and MultiStream ran the wrong one); an event sent by an SPU through the interrupt mailbox waits a few
  ms for the PPU's `connect_event` that follows `group_start` (it answered ENOTCONN and ended the SPU program).
- `main.cpp` sets `SPU_CH_BLOCK=1` (a read of an empty mailbox blocks instead of returning 0) and `SPU_INTERP_UNLIFTED=1`.
- The SPU program returns through a link register into the middle of its own functions, which the lifter had not made
  entries; `launcher/helpers/make_sc2_overlays.py` adds every call's return point. Its two DSP plugins are loaded into local store at 0x33680
  and are not patched after the copy; the script writes images of them at that address and registers them by their first
  16 bytes (`spu/sc2_overlays.c`). Interpreted, they made the SPU run at a third of real time: the game wrote 4 blocks of
  audio per 12 and the output was gated 33%, i.e. heavily distorted.
- `patches/0012`: double-precision SPU instructions in the interpreter; `patches/0013`: `AUDIO_PEAK=1`, `AUDIO_WAV=<file>`.
- `patches/0014`: Metal backend gets a main menu with Cmd+Q ("Quit <title>"); closing the window or Cmd+Q ends the process (`main.cpp` calls `_exit(0)` when `rsx_backend_pump()` reports the close).
- `patches/0016`: `spu_register_ls_hook`, `SPU_GETLOG=<lo>,<hi>` (log MFC transfers touching an LS range), lr in the "synthesised stop" message. `patches/0017`: see below.
- `patches/0018`: the Metal window drops key down/up events (the pad polls the keyboard itself), so a keypress no longer plays the system alert sound; Cmd shortcuts still reach the menu.
- `patches/0019`: image quality on the Metal path. The game renders 1280x720 with no anti-aliasing of its own. FXAA 3.11 (quality
  variant) runs in the display pass that puts the finished frame on the window (`PS3RECOMP_METAL_FXAA=0|1`, default 1), and linearly
  filtered game textures get anisotropic filtering (`PS3RECOMP_METAL_AF=1..16`, default 16). the launcher maps `--fxaa`, `--no-fxaa`,
  `--af N` to those. FXAA also softens the HUD a little, it filters the whole frame. The old non-engine (vtable) path only gets AF.
  `PS3RECOMP_METAL_HEADLESS=1 PS3RECOMP_METAL_PRESENT_DUMP=<dir>` writes every 120th presented frame (and the unfiltered one as `_raw`).
- `patches/0020`: the window is resizable without stretching. The drawable follows the window's size in device pixels (a Retina window
  is now presented 1:1 instead of being upscaled by the compositor), and the display pass fits the 16:9 picture into it and centres
  it, black elsewhere. Rendering itself stays 1280x720: the game's projection and UI are 16:9, so a different aspect would only
  stretch it. `PS3RECOMP_METAL_WINDOW=WxH` / `--window WxH` sets the initial window size. Only the draw-engine path does this.
- `patches/0021`: Online Mode. `cellNetCtlNetStartDialog*` used to report "connected" even though `cellNetCtlGetState` fails offline, so the
  game went on to create NP matching contexts whose callbacks never come and its menu sat on "Communicating. Please wait." for good.
  Offline the dialog now ends with `CELL_NET_CTL_ERROR_DIALOG_CANCELED`, and the game shows its own "You must be signed in to
  PlayStationNetwork to access online features." with Confirm; the menu stays usable. Online Mode itself is unreachable (the check
  happens when it is entered). `PS3_NET_ONLINE=1` restores the old "connected" answers.
- `patches/0022`: accessor functions for the thread-local state of the PPU trampoline and the SPU context, so a separate module can reach it (see docs/architecture.md). `patches/0023`: the LGPL ATRAC3plus decoder is optional (`PS3RECOMP_ATRAC_LGPL`), the host is built without it.
- `patches/0024`: the SDL gamepad layer runs on one dedicated thread, so a controller plugged in after the start is seen. `patches/0029`: an SDL keyboard fallback for cellPad on Linux (the pad mapping of 0005).
- Windows (MinGW): `patches/0025` (the PPU loader builds with GCC), `patches/0026` (`__thread` instead of `__declspec(thread)`, which GCC ignores, so thread-locals were shared globals), `patches/0027` (cellAudio never mixes two blocks back to back on WASAPI), `patches/0028` (window icon hook).
- `patches/0015`: the `if (_n++ < N)` log caps used signed counters that wrapped negative after 2^31 calls and then printed on every call (millions of `[spu] drain-resume` lines in battle); they now saturate.
- MP3 without firmware: MultiStream loads Sony's decoder `flashMP3.pic` into local store and calls one function of it per
  frame. `src/mp3_hle.c` answers that call on the host (decoder core: `src/third_party/dr_mp3_core.h`, dr_mp3/minimp3,
  public domain), through `patches/0016` (`spu_register_ls_hook`: host code in place of runtime-loaded SPU code at an LS
  address). The real decoder writes one 1152-sample big-endian s16 buffer per channel (left at heap+0x900, right 0x1000 bytes after it; the right one must be written or a stereo stream plays stale data as hiss) and keeps the voice's state in
  the two 4992-byte blocks MultiStream DMAs around every call; the contract is documented at the top of `mp3_hle.c`.
  `MP3_HLE=compare` runs the real `.pic` next to it (needs `SC2HD_REAL_PICS=1` so the real files are used): about 9% of the
  frames differ from the real one by more than 4 LSB (up to ~1400), all MS-stereo frames at transients, where the real
  decoder also differs from ffmpeg; the host output matches ffmpeg. `MP3_HLE=off` interprets the real `.pic`;
  `MP3_DUMP=<prefix>` writes the frames and PCM for offline checks.
- The loader needs the two `.pic` files to exist, with the right sections (it reads the section headers): `launcher/helpers/
  make_firmware_stubs.py` writes same-shaped stand-ins (no Sony code) to `<data>/games/<id>/run/dev_flash_stubs`, and the
  launcher puts that directory first in `PS3_DEV_FLASH`, now a ':'-separated list (`patches/0017`). The firmware's `.pic` files are not used, and
  nothing else is read from `/dev_flash`.

Open:
- Movie audio/video sync ignores the game's `UpdateAvSync` feedback: pictures follow the PCM the game has taken (the game's audio
  output runs a little slower than the wall clock, so a wall-clock video clock drifts away from the sound), minus a fixed 100 ms.
- Input was only driven through `PAD_SCRIPT`/`PAD_FILE`; the keyboard fallbacks (macOS, Linux; Linux only with synthetic key events) and real controllers are untested.
- Rare FIFO decode failures (`FIFO JUMP to unmapped IO ... drain stops`, 0-3 per minute, most often while a battle loads) lose that frame's commands.
- `[mfc] REJECTED malformed transfer` from SPU image 2 appears now and then.
- Windows (D3D12) has only been cross-built, Linux draws through the toolkit's Vulkan backend in an SDL2 window (null backend without a display; only seen on llvmpipe); `rsx_live_draw.c` (D3D12) lacks the supersample resolve of `patches/0006`.
- "chain gives up" after 20 s is harmless (job chains starting with a GUARD the game has not notified yet).
