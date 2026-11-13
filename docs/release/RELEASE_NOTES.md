# HeyListen {TAG}

HeyListen turns **your own** Switch 2 copy of *The Legend of Zelda: Ocarina of Time* into a native Windows game. The release holds
only the **HeyListen Builder**: no game code, data, keys or shaders. You dump your own game as a `.nup`, the builder reads it,
translates and compiles the game on your PC, prepares its shaders, and starts it. See the
[user guide](https://github.com/Sacrappy/HeyListen/blob/main/docs/USER_GUIDE.md).

HeyListen is a fork of Hookshot by NotRexed and its contributors (GPL-3.0); all credit
for the recompiler, the builder and the Vulkan back end goes to them. HeyListen is GPL-3.0-or-later; the source is this
repository (the engine under `engine/`).

## Download

- **HeyListenBuilder.exe**: the setup. Run it; it downloads the builder and its tools (checked by SHA-256) into
  `%LOCALAPPDATA%\HeyListenBuilder` and opens it.
- `heylisten-setup.txt`: the list of parts the setup reads (you do not need to download it).
- `SHA256SUMS.txt`: checksums of the files above.

## New in this release

**v0.3.1 repairs the installation.** The setup downloaded four of the builder's tools (Python, emsdk, the CPM packages and
wgpu-native) from the Hookshot repository, which is no longer online, so a new install of v0.3.0 stopped with a download
error. Those parts are now served from this repository, byte for byte the same files (same SHA-256), and the release tool
refuses a list that names any other host than this repository and llvm-mingw's official release. The game and the builder
are those of v0.3.0: if v0.3.0 is installed and working, the update downloads only the builder's own few MB and you need
not build again for anything new.

The rest of these notes is v0.3.0's. Everything below is new since v0.2.0. It is a speed release: nothing changes in how the game looks or plays.

Measured on the fast test PC (Ryzen 7 7800X3D + RTX 5070), frame generation off, the frame limit lifted, v0.2.0 and this
release run in turn on the same day:

| Scene | v0.2.0 | this release |
|---|---:|---:|
| Castle Town, moving (3 runs each) | 77.7 fps | **83.7 fps** |
| Kokiri Forest, standing (1 run each) | 80.6 fps | **84.5 fps** |
| Hyrule Field, walking (1 run each) | 79.5 fps | **83.1 fps** |

One run differs from the next by 1 to 2 fps, and the slowest frames (1% low, about 46 fps in Castle Town) are where they
were. The picture (screenshots against v0.2.0's) and the game's speed were checked in the three scenes.

What changed:

- **Faster floating-point code.** Straight runs of the game's floating-point instructions now execute as one block on the
  processor's SIMD registers, with a single check that falls back to the exact instruction-by-instruction code. The builder
  ships an optimization profile recorded on that code. Results are bit-identical to before (2 million random test runs,
  0 differences).
- **The frame's memory copy starts earlier.** The pages the game changed are copied ahead, in parallel, while its job
  threads wait at the end of the frame; when the frame is submitted only what changed since is copied.
- **Less waiting between the game and the GPU.** A page written late into a texture that is still in use no longer makes
  the copy wait for the GPU; finished GPU work is recognized without asking the driver again; the engine no longer repeats
  barriers the game already asked for.
- **Lighter bookkeeping every frame.** The mirror syncs, the copy's page stamps, the hazard scan and the set-up of the
  parallel recording each do less work, and so do the waits of the game's threads (the switches measured in tickets 056
  to 062, now on by default).
- **A background thread of the game no longer takes a processor from the frame's threads** (it runs in short slices).
- Each engine change above has a switch that turns it off (`=0` in the game folder's `hookshot_env.txt`;
  `docs/tickets/SPRINT-5.md` lists them). The floating-point blocks are chosen when the game is built.

Not measured again for this release: the slow-link test PC (RTX 3050 on PCIe 3.0 x4). Its copy-ahead stays off (it keeps
v0.2.0's chunk-diff syncs); the floating-point blocks and the tickets' switches were checked there when they were built, the
late texture pages and the background thread's slices were not.

For developers: `docs/tickets/SPRINT-5.md` is the log of this work, with what holds the frame near 85 fps on the test PC
(three chains of 11-12 ms each: the GPU round trip, the game's own job chain, the processors) and the experiments that did
not pay: a copy by written 512-byte granules (in the code, off), a light-meter readback one frame late and twelve job
executors instead of six (patches in `docs/research/`).

## What HeyListen adds to Hookshot

- **Frame rate**: game speed correct at 60-360 fps (was +12% too fast at 120); engine copies cut (write-backs 67.5 -> 13.9 MB
  a frame, mirror syncs as one compute pass); the replay of the game's draws recorded in parallel and overlapping the game's
  next frame; floating-point code run as real SIMD, a block at a time, with its own optimization profile; Castle Town
  turn/move drops fixed (31-43 -> 51-57 fps at the time).
- **Slow graphics links** (PCIe x4 cards): the CPU records the next frame while the GPU finishes its write-backs, the copy
  hashes pages for the syncs, and the syncs send only the 256-byte chunks that changed (+18-21% on the RTX 3050 test PC).
- **Fewer stutters**: pipelines used once are noted and precompiled at the next start; texture descriptor cache; ASTC
  transcodes batched, their BC copies cached in system RAM.
- **Memory**: a VRAM breakdown in the log, texture budget from the driver, big images in their own memory, idle mirrors
  freed, ASTC textures kept as BC1/BC7 (decoded textures 3.0 -> 1.1 GB); about 2 GB less RAM.
- **Picture fixes**: the Castle Town Market ground while walking (no more flat mint, white or teal ground).
- **Builder**: the HeyListen Builder builds this fork (its own releases and updates, its own work folder), and copies Ocarina
  of Time saves **from and to emulators** (Lichii, Eden and its forks, NX2navi, or any folder) with a backup first
  (upstream Hookshot's `oot_saves.py`).
- An original icon (a blue fairy orb), on the builder, the setup and the game; the game is `HeyListen.exe`; the Discord
  status shows "Playing HeyListen"; a frame limit row in the menu and an FPS counter (F10).

## Known issues

- **120 fps is not reached.** The test PC runs the three scenes at about 84 fps on average with drops to about 46; the work
  goes on (`docs/tickets/SPRINT-5.md`).
- **Hookshot's newest engine work (after v2.0.13) is not in this release.** Merged with HeyListen, it is about 29% slower
  here for now (a readback wait and thread scheduling changes); it waits on the `sync/hookshot-12` branch.
- **Occlusion is off** (a fixed value, as in Hookshot): real occlusion queries break the picture; some scenes draw more than
  they need.
- **12 GB of VRAM recommended.** 8 GB cards are untested.
- **Tested on NVIDIA RTX only.** AMD and Intel cards are untested in HeyListen (see Hookshot's notes for them).
- **Frame generation is off by default** (turn it on in the Insert menu if you want to try it).
- The first build takes a long time (tens of minutes, then shader preparation); later updates rebuild only what changed.
- The in-game update offer is opt-in (set `HOOKSHOT_UPDATE_CHECK=1` in the game folder's `hookshot_env.txt`); the builder
  itself says when a new release is out.
