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

**Linux (new, little tested).** This release also carries a Linux AppImage, `HeyListen-<version>-x86_64.AppImage`
(built by this repository's CI in an Ubuntu 22.04 container, glibc 2.35 and newer), with `heylisten-linux.txt`, the list its
update reads. It builds the game from your own copy on your PC, like the Windows builder; see the Linux section of the
[user guide](https://github.com/Sacrappy/HeyListen/blob/main/docs/USER_GUIDE.md). It is Hookshot's Linux port, made
buildable again on this release by a contributor's pull request (#4) and given HeyListen's names, folders
(`~/.local/share/heylisten`, `~/.config/heylisten`) and update links. What was and was not tested:
- Tested: the game-free test suite on Ubuntu 26.04 (WSL2); the AppImage built there with the repository's scripts; a
  complete player's build from it with a real game copy (32 minutes on 8 cores); the builder window starting and showing
  HeyListen's name; an update installed from a test list (parts unchanged, so nothing was downloaded).
- WSL has no hardware Vulkan on the test PC (only Mesa's CPU renderer), so the game was not played there. The built game
  starts, opens a window named HeyListen, creates the Vulkan device and begins the first start's shader preparation (2,300 of
  38,809 shaders in the 100 seconds it was left running, then stopped); the CPU renderer is far too slow to go further, and
  it reports some programs as not translated for that renderer, which says nothing about a real GPU.
- A contributor reported the Linux build of the `main` branch working on CachyOS with a Radeon RX 9070 XT. That is not this
  release's code, and no one has run this AppImage on a PC with a Linux GPU: we have no native Linux machine.
- The Linux update (a new AppImage's parts downloaded from the `linux-parts` release) has never run against a real release.


**v0.3.2 fixes crashes when moving between areas**, which have been there since v0.3.0. When graphics memory got tight
(more likely at high resolutions and after several areas), a memory-saving step could copy stale GPU data over memory the
game had just reused for its own objects, and the game crashed shortly after, usually while an area loaded or when an
area was entered again. The data is now written back only into memory the game still uses for graphics, and only if the
game has not written it since. Played by hand for 35, 20, 45 and 28 minutes through every area reachable from our save
without a crash, where every earlier session crashed within 3 to 21 minutes.

**A first play no longer starts with missing buildings.** Until now each area's graphics pipelines were compiled the first
time they were seen, which showed as buildings and ground missing over a white background, low frame rates and stalls of
up to a minute in a new area. The builder now ships a list of 9,854 pipeline states (hashes, formats and draw state only:
no game code or data) recorded over 17 areas, and compiles them once: at the end of the build, or at the first start after
this update, with a progress count (about one to two minutes on a recent PC, longer on a slow one). Measured on a cold
start in Castle Town: draws left out 75,574 -> 332, the longest stall 21 s -> under 8 s. That last stall is not gone: about
70 pipelines of a first visit cannot be known ahead yet and are still compiled when first seen.

**GPUs without mesh shaders** (GeForce GTX 10 series and older, older Radeon and integrated graphics): three errors in how
the engine used Vulkan on such cards are fixed (shader execution modes declared without checking that the card has them,
start-up programs prepared as if the card had mesh shaders, a missing vertex stream). Current drivers of newer cards
tolerated them; an older one may lose the GPU device instead, as reported after the first cutscene on a GTX 1060. We
could not reproduce that loss on our hardware, so this is a likely fix, not a confirmed one: if the game still closes
there, `docs/support/no-mesh-diagnostic.md` lists three settings to try and what to send us. On these cards the shader
cache is rebuilt once after the update.

Also fixed: a graphics memory pool destroyed twice by the game could make the engine forget a newer pool made at the same
address.

The update downloads only the builder's own few MB; the game is built again once.

v0.3.1 repaired the installation: the setup downloaded four of the builder's tools from the Hookshot repository, which is
no longer online; they are now served from this repository, byte for byte the same files.

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
