# HeyListen {TAG}

HeyListen turns **your own** Switch 2 copy of *The Legend of Zelda: Ocarina of Time* into a native Windows game. The release holds
only the **HeyListen Builder**: no game code, data, keys or shaders. You dump your own game as a `.nup`, the builder reads it,
translates and compiles the game on your PC, prepares its shaders, and starts it. See the
[user guide](https://github.com/Sacrappy/HeyListen/blob/main/docs/USER_GUIDE.md).

HeyListen is a fork of [Hookshot](https://github.com/NotRexed/Hookshot) by NotRexed and its contributors (GPL-3.0); all credit
for the recompiler, the builder and the Vulkan back end goes to them. HeyListen is GPL-3.0-or-later; the source is this
repository (the engine under `engine/`).

## Download

- **HeyListenBuilder.exe**: the setup. Run it; it downloads the builder and its tools (checked by SHA-256) into
  `%LOCALAPPDATA%\HeyListenBuilder` and opens it.
- `heylisten-setup.txt`: the list of parts the setup reads (you do not need to download it).
- `SHA256SUMS.txt`: checksums of the files above.

## New in this release

Everything below is new since v0.1.0 (v0.2.0 and v0.2.1 were built but never published). Figures are from the two test PCs:
a Ryzen 7 7800X3D + RTX 5070 on PCIe 5.0 x16, and an i5-10400F + RTX 3050 on PCIe 3.0 x4.

- **Much faster on PCs with a slow graphics link** (the RTX 3050 test PC, PCIe 3.0 x4):
  - The CPU now records the next frame while the GPU finishes its write-backs, instead of waiting for them (`RX_VK_LATE_DEFER`).
  - The capture copy also hashes the pages for the mirror syncs, so the syncs no longer hash them again (`RX_VK_CAPTURE_HASH`).
  - Mirror syncs copy only the 256-byte chunks that changed (32 -> 4.4 MB a frame).
  - These are on by default where the link is 8 GB/s or less (read from the driver). Together they give +18-21% fps there:
    Kokiri Forest 30.9 -> 37.2, Castle Town 29.1 -> 34.4, Hyrule Field 31.4 -> 37.2 fps at 1440p. Fast links keep the old
    copies and the same speed.
- **Faster frames on every PC**:
  - The game's frame and the replay of its draws overlap better: the replay is held and recorded in parallel. That gives
    +1.9 to +3.8 fps in Kokiri Forest, Castle Town and Hyrule Field on the 7800X3D.
  - The capture copies use streaming stores (+2.4 to +3.3 fps).
- **Faster floating-point code**:
  - The game's vector math runs as real SIMD instead of lane by lane, and its slow paths (quiet NaN lanes, estimates) are cut.
  - The builder ships an optimization profile recorded on that code.
  - Market about 75 -> 78 fps standing, 68 -> 73 walking. Results are bit-identical to before.
- **Fewer stutters on mid-range PCs**: ASTC texture transcodes are batched and their BC copies are cached in system RAM.
- **Castle Town Market ground fixed** while walking (no more flat mint, white or teal ground):
  - ASTC textures are kept as BC1/BC7 (decoded textures 3.0 -> 1.1 GB, VRAM peak -14%).
  - A pool-mirror ordering bug is fixed.
- **Less RAM**: about 2 GB less (compute pipelines are built on first use and their modules dropped); shaders start faster.
- **Hookshot v2.0.12/v2.0.13**:
  - The new Dear ImGui menu (Insert), keyboard shortcut choice, capture and screenshot fixes, and builder hardening.
  - HeyListen keeps its frame limit row and FPS counter (F10).
- **Discord**: the status now shows "Playing HeyListen" with its own icon (the Insert menu's "Discord status" row; it does
  nothing without a Discord client running).
- **For developers**:
  - Many measured performance switches, off by default, each with its ticket and benchmark in `docs/`.
  - Frame tracing tools (job trace, critical path, frame breakdown) and an analysis of where frame time can still be saved
    (`docs/research/suspects/`).

## What HeyListen adds to Hookshot

- **Frame rate**: game speed correct at 60-360 fps (was +12% too fast at 120); engine copies cut (write-backs 67.5 -> 13.9 MB
  a frame, mirror syncs as one compute pass); Hyrule Field about 68 -> 74 fps on the test PC; Castle Town turn/move drops
  fixed (31-43 -> 51-57 fps).
- **Fewer stutters**: pipelines used once are noted and precompiled at the next start; texture descriptor cache.
- **VRAM**: a VRAM breakdown in the log, texture budget from the driver, big images in their own memory, idle mirrors freed
  (peak use about 88% -> 81-87%); the teal ground regression fixed.
- **Builder**: the HeyListen Builder builds this fork (its own releases and updates, its own work folder), and copies Ocarina
  of Time saves **from and to emulators** (Lichii, Eden and its forks, NX2navi, or any folder) with a backup first
  (upstream Hookshot's `oot_saves.py`).
- An original icon (a blue fairy orb), on the builder, the setup and the game; the game is `HeyListen.exe` (a folder built
  with an earlier build had `hookshot.exe`: rebuilding it replaces that).

## Known issues

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
