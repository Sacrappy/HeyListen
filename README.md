# HeyListen

A native Windows port of *The Legend of Zelda: Ocarina of Time* (**Nintendo Switch 2** build, title `0400486024DEC000`), made by statically recompiling **your own** copy of the game, with a Vulkan back end. HeyListen is a performance-focused fork of [Hookshot](https://github.com/NotRexed/Hookshot) by NotRexed.

This repository contains **no game code, keys, ROM or generated files**. Everything derived from the game is built locally from the user's own copy. HeyListen is not affiliated with or endorsed by Nintendo.

- [How to use](#how-to-use)
- [Performance](#performance)
- [HeyListen compared with Hookshot](#heylisten-compared-with-hookshot)
- [Repository layout](#repository-layout)
- [Credits](#credits)
- [License](#license)

## How to use

Full guide: [docs/USER_GUIDE.md](docs/USER_GUIDE.md).

**You need**
- Windows 10 or 11 (64-bit), a CPU with AVX2, 16 GB of RAM or more.
- A GPU with Vulkan 1.3 and mesh shaders; 12 GB of VRAM recommended. Tested on NVIDIA RTX only.
- About 60 GB free while building and a connection for the first download (about 500 MB).
- Your own game, dumped from your Switch 2 as a `.nup` (program `0400486024DEC000`, update v131072, build `e0f3ccfa`). HeyListen does not help with dumping and never asks for keys or firmware.

**Steps**
1. Download `HeyListenBuilder.exe` from the [latest release](https://github.com/Sacrappy/HeyListen/releases/latest) and run it. It fetches its tools (each checked by SHA-256) into `%LOCALAPPDATA%\HeyListenBuilder`.
2. Pick your `.nup` (browse or drop it on the window), choose the game language and an install folder (default `%USERPROFILE%\Games\HeyListen`), then press **Build the game**.
3. Wait for the build (about 10 to 45 minutes) and the first shader preparation (a few minutes). Later starts are quick, and updates rebuild only what changed.
4. Press **Play**, or use the *HeyListen* shortcut. **Insert** opens the in-game menu (graphics, controls, frame rate limit, cheats); **F10** shows the FPS overlay. Frame generation is off by default.

Saves live in `user\save\` inside the game folder. The builder can also **import and export Ocarina of Time saves** from and to Lichii, Eden and its forks, NX2navi, or any folder, with a backup first.

Problems: [issues](https://github.com/Sacrappy/HeyListen/issues) (include your GPU, driver and log; never attach game files, keys or saves).

## Performance

Average frame rate, **frame generation off**, windowed, with the game's frame limit lifted. Machine: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11 (scripted bench scenes from `tools/bench/`, picture and game speed checked on every run).

| Scene | Hookshot (fork point) | HeyListen now |
|---|---:|---:|
| Kokiri Forest | 54.2 fps | **90.0 fps** |
| Castle Town (the Market) | 31-43 fps while moving | **89.3 fps** |
| Hyrule Field | 49.1 fps | **85.5 fps** |

Notes on the numbers:
- HeyListen figures are the latest Sprint 5 measurements ([docs/tickets/SPRINT-5.md](docs/tickets/SPRINT-5.md)): Kokiri Forest standing, Castle Town moving, Hyrule Field walking. Run-to-run spread is a few fps.
- Hookshot figures are from the first baseline ([docs/benchmarks/2026-11-baseline.md](docs/benchmarks/2026-11-baseline.md)) with Hookshot's serial replay (Kokiri Forest and Hyrule Field), and from the Castle Town ticket (TICK-027) for the Market's move and turn drops. They are the starting point of the fork, not today's upstream Hookshot.
- A second, weaker bench PC (i5-10400F, RTX 3050 on PCIe 3.0 x4) gained 18-21% from the slow-link work: Kokiri Forest 30.9 to 37.2, Castle Town 29.1 to 34.4, Hyrule Field 31.4 to 37.2 fps at 1440p.
- The sprint goal is a steady 120 fps average; it is not reached yet. Every benchmark and its method is in [docs/benchmarks/](docs/benchmarks/).

## HeyListen compared with Hookshot

HeyListen's `engine/` is Hookshot's source imported with its history and kept in sync by subtree merges (last synced to Hookshot v2.0.13, commit `a2e6505`). Against Hookshot's current `main`, HeyListen is **317 commits ahead** (229 of them non-merge) and 643 behind: the newest Hookshot work is on the `sync/hookshot-12` branch and is not merged, because it measured about 29% slower on the same scenes with HeyListen's changes.

**What HeyListen adds**

*Frame rate*
- Parallel Vulkan command recording by default, with the held flush overlapping the game's next frame (+1.9 to +3.8 fps in the three scenes).
- Engine copies cut: write-backs 67.5 to 13.9 MB a frame, mirror syncs as one compute scatter-copy (GPU copies 2.19 to 0.81 ms), the capture copy with streaming stores (+2.4 to +3.3 fps), a wider hazard scan, pre-copy at the end-of-frame wait, barrier trust and parallel de-duplication.
- Game speed correct at 60 to 360 fps (it ran 12% too fast at 120).
- Floating-point code lifted as real SIMD instead of lane by lane, with slow paths cut and an optimization profile recorded on the game's hot code (`profile/`); results are bit-identical.
- Castle Town move and turn drops fixed (31-43 to 51-57 fps at the time).
- Slow-link mode, chosen from the GPU's PCIe link: late-page deferral, a capture that also hashes pages, and chunk-diff mirror syncs (+18-21% on PCIe x4 cards).

*Smoothness and memory*
- Fewer stutters: pipelines used once are recorded and precompiled at the next start, a texture descriptor cache, batched ASTC transcodes with a system-RAM BC cache.
- VRAM: a VRAM breakdown in the log, texture budget read from the driver, big images in their own memory, idle mirrors freed (peak about 88% to 81-87%), ASTC textures kept as BC1/BC7 (decoded textures 3.0 to 1.1 GB).
- About 2 GB less RAM (compute pipelines built on first use) and a faster shader start-up.
- Castle Town ground fixed while walking (no more flat mint, white or teal ground), and the teal-ground regression after evictions fixed.

*Tooling and packaging*
- **HeyListen Builder**: its own releases, updates and work folder, so it never touches a Hookshot install; **emulator save import and export** with backup.
- Game executable `HeyListen.exe`, an original icon (a blue fairy orb), and a Discord status that shows "Playing HeyListen".
- Overlay: frame rate limit row and FPS counter (F10); 3D scene size from the Resolution row.
- Measurement tools: a bench harness (`tools/bench/`), frame job tracing and critical-path tools, GPU segment timestamps, a hot-function map and a hash-gated native-replacement framework.
- A large set of measured performance switches (off by default unless adopted), each with its ticket and benchmark in `docs/tickets/` and `docs/benchmarks/`, plus static analyses of the remaining frame time in `docs/research/`.
- Engine tests brought up to date with current code.

## Repository layout

- `engine/`: the Hookshot-derived recompiler, host layer and Vulkan back end, with HeyListen's changes
- `docs/`: user guide, ADRs, benchmarks, ticket and sprint specs, research
- `tools/`: bench, release and orchestration scripts
- `profile/`: the optimization profile for the lifted code
- `references/`: manifest of the comparison repos and builds (kept in place, not committed)

Maintainers: `tools/release/make_release.ps1 -Tag vX.Y.Z` makes the release assets (see the script's header); the icon is `engine/assets/heylisten.svg` (original art, drawn by `tools/release/make_icon.py`).

Syncing with upstream:

```
git remote add upstream-hookshot https://github.com/NotRexed/Hookshot.git   # once
git fetch upstream-hookshot
git subtree pull --prefix=engine upstream-hookshot main
```

## Credits

HeyListen is **made by Sacrappy**, on top of the work of many people. Everything below is credit due to Hookshot's own contributors, whose recompiler, builder and renderer do the work:

- **NotRexed**: [Hookshot](https://github.com/NotRexed/Hookshot), BlueShell (Mario Kart World), Skybound (Tears of the Kingdom) and the a64-kit tools.
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **Eglantine**: the shader recompiler and Switch 2 shader work.
- **suyu**: the graphics decoders and the AArch64 translator.
- **[seedds/nxnative](https://github.com/seedds/nxnative)**: reference for how task and mesh shaders work.
- **[EvilBoyce / evilmfkr](https://github.com/evilmfkr/Hookshot)** (EvilOOT): the GPU culling that brings back Kokiri's foliage, the ambient light probes, the pond's water, the game's own fog, the freeze after Load and the exposure flicker fix.
- **[zortos293/Lichii](https://github.com/zortos293/Lichii)**: reference for Ampere shader lowering, and its compare-exchange retry fix.
- **[mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw)**: the bundled Windows compiler (no Visual Studio needed).
- **NVIDIA DLSS**: optional builds use NVIDIA DLSS Super Resolution (downloaded by each build, never bundled). NVIDIA and DLSS are trademarks of NVIDIA Corporation.
- **[kinnay/zbic](https://github.com/kinnay/zbic)**, **Opus** and **SDL**: decompression, audio and controllers.
- **[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)**: Hookshot's README layout, and notes that confirmed the mesh stage's control section.
- **[MythosOfs/Orchid](https://github.com/MythosOfs/Orchid)**: the fog and exposure workaround for Ocarina of Time.
- **Mesa (NAK)**: the reference for Ampere's instruction encoding.
- Everyone who sent fixes to Hookshot: SkippySteve (Linux and the AppImage, and many more), monsterlag, klimovich008, hckrman101 (the exposure flicker and fog fixes, build speed, CI), shayderrr, gab9281, Wpaif, nicholasgower, ZurielAtr, Joey-Magnus, Zorkats, d3zessete, chrispository, vzeol, leonidasmark, Segmentree, ShyVortex, hiKareeem and i900gaming, among others.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). HeyListen is a fork of [Hookshot](https://github.com/NotRexed/Hookshot) (GPL-3.0), which lives in `engine/` with its history; its lifter is derived from suyu (GPL-2.0-or-later) and its Vulkan back end contains Eden code (GPL-3.0). Third-party sources and their licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). See [docs/adr/0001-license.md](docs/adr/0001-license.md).

The Legend of Zelda and Ocarina of Time are trademarks of Nintendo. HeyListen ships none of its assets.
