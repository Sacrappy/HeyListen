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
- An original icon (a blue fairy orb).

## Known issues

- **Occlusion is off** (a fixed value, as in Hookshot): real occlusion queries break the picture; some scenes draw more than
  they need.
- **Rare missing or teal textures while walking** when VRAM is short: textures evicted under pressure can come back late.
- **12 GB of VRAM recommended.** 8 GB cards may show the issue above more often.
- **Tested on NVIDIA RTX only.** AMD and Intel cards are untested in HeyListen (see Hookshot's notes for them).
- **Frame generation is off by default** (turn it on in the Insert menu if you want to try it).
- The first build takes a long time (tens of minutes, then shader preparation); later updates rebuild only what changed.
- The in-game update offer is opt-in (set `HOOKSHOT_UPDATE_CHECK=1` in the game folder's `hookshot_env.txt`); the builder
  itself says when a new release is out.
