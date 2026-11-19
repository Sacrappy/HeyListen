<p align="center">
  <img src="engine/assets/heylisten.png" alt="HeyListen: a glowing blue fairy" width="160">
</p>

<h1 align="center">HeyListen</h1>

<p align="center">
  <b>A native Windows and Linux port of <i>The Legend of Zelda: Ocarina of Time</i> (Nintendo Switch 2),<br>
  built on your PC from your own copy of the game.</b>
</p>

<p align="center">
  <a href="https://github.com/Sacrappy/HeyListen/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Sacrappy/HeyListen?label=release&color=3b82f6"></a>
  <a href="https://github.com/Sacrappy/HeyListen/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/Sacrappy/HeyListen/total?color=3b82f6"></a>
  <a href="https://github.com/Sacrappy/HeyListen/actions/workflows/linux.yml"><img alt="Linux checks" src="https://github.com/Sacrappy/HeyListen/actions/workflows/linux.yml/badge.svg?branch=main"></a>
  <a href="https://discord.gg/34GV5hdC3Q"><img alt="Discord" src="https://img.shields.io/badge/discord-join-5865F2?logo=discord&logoColor=white"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue"></a>
</p>

<p align="center">
  <a href="https://github.com/Sacrappy/HeyListen/releases/latest"><b>Download</b></a> ·
  <a href="docs/USER_GUIDE.md"><b>User guide</b></a> ·
  <a href="https://github.com/Sacrappy/HeyListen/issues"><b>Report a problem</b></a> ·
  <a href="CONTRIBUTING.md"><b>Contributing</b></a> ·
  <a href="https://discord.gg/34GV5hdC3Q"><b>Discord</b></a>
</p>

HeyListen statically recompiles **your own** copy of the game (title `0400486024DEC000`) into a native program with a
Vulkan back end: no emulator. It is a performance-focused fork of [Hookshot](https://github.com/NotRexed/Hookshot) by
NotRexed.

This repository contains **no game code, keys, ROM or generated files**. Everything derived from the game is built
locally from the user's own copy. HeyListen is not affiliated with or endorsed by Nintendo.

> **Have an AMD or Intel GPU, a Steam Deck or a Linux PC?** Our test machines are NVIDIA only: your reports are the
> most useful thing you can send. See [CONTRIBUTING.md](CONTRIBUTING.md).

- [How to use](#how-to-use)
- [Performance](#performance)
- [HeyListen compared with Hookshot](#heylisten-compared-with-hookshot)
- [Repository layout](#repository-layout)
- [Contributing](#contributing)
- [Credits](#credits)
- [License](#license)

## How to use

Full guide: [docs/USER_GUIDE.md](docs/USER_GUIDE.md).

**You need**
- Windows 10 or 11 (64-bit), or Linux (an AppImage since v0.3.2, new and little tested: [the guide's Linux section](docs/USER_GUIDE.md#linux)); a CPU with AVX2, 16 GB of RAM or more.
- A GPU with Vulkan 1.3 and mesh shaders; 12 GB of VRAM recommended. Tested on NVIDIA RTX (Windows); on Linux a contributor plays on an AMD Radeon RX 9070 XT (Mesa's RADV driver), natively, with no Wine or Proton. AMD on Windows and Intel are untested.
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

**v0.4.0 against the other Ocarina of Time ports** (2026-11-16): in-game frame rate only, 2560x1440, 3D scene at
100 %, no upscaler, no frame generation, uncapped, Link idle in each zone loaded from the same saves; PresentMon over
the 40 s after each build's "in play" marker, without its first 3 s; the mean of two runs per zone.

| Machine | Zones | NX2navi 0.11.0-rc4 | Longshot v3.0.0-rc.4 | **HeyListen v0.4.0** |
|---|---:|---:|---:|---:|
| i5-10400F, RTX 3050 8 GB | 17 | 29.7 fps (1% low 22.6) | 35.6 fps (28.1) | **40.8 fps** (24.4) |
| Ryzen 7 7800X3D, RTX 5070 12 GB | 13 | 68.0 fps (35.5) | 86.0 fps (51.4) | **96.2 fps** (43.6) |

- HeyListen has the highest average frame rate in 14 of 17 zones on the RTX 3050 and 10 of 13 on the RTX 5070
  (+15 % and +12 % over Longshot on average, +37 % and +41 % over NX2navi).
- Its 1% lows are still below Longshot's: some frames in steady play take 100-350 ms. That is the next work item.
- Per zone, the method and every caveat (present modes, a main-PC series run while the PC was in use, NVIDIA's shader
  cache at its 1 GB default there): [docs/benchmarks/2026-11-16-v0.4.0-zones.md](docs/benchmarks/2026-11-16-v0.4.0-zones.md).

## HeyListen compared with Hookshot

HeyListen's `engine/` is Hookshot's source imported with its history and kept in sync by subtree merges (last synced to Hookshot v2.0.13, commit `a2e6505`). Against Hookshot's current `main`, HeyListen is **317 commits ahead** (229 of them non-merge) and 643 behind: the newest Hookshot work is on the `sync/hookshot-12` branch and is not merged, because it measured about 29% slower on the same scenes with HeyListen's changes.

**What HeyListen adds**

*Frame rate*
- Parallel Vulkan command recording by default, with the held flush overlapping the game's next frame (+1.9 to +3.8 fps in the three scenes).
- Engine copies cut: write-backs 67.5 to 13.9 MB a frame, mirror syncs as one compute scatter-copy (GPU copies 2.19 to 0.81 ms), the capture copy with streaming stores (+2.4 to +3.3 fps), a wider hazard scan, pre-copy at the end-of-frame wait, barrier trust and parallel de-duplication.
- Game speed correct at 60 to 360 fps (it ran 12% too fast at 120).
- Floating-point code lifted as real SIMD instead of lane by lane, with slow paths cut and an optimization profile recorded on the game's hot code (`profile/`); results are bit-identical.
- Castle Town move and turn drops fixed (31-43 to 51-57 fps at the time).
- Slow-link mode, chosen from the GPU's PCIe link (NVIDIA's NVML; on Linux, for any card, the narrowest link from the card up to the CPU in sysfs): late-page deferral, a capture that also hashes pages, and chunk-diff mirror syncs (+18-21% on PCIe x4 cards).

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
- Linux: the engine builds again (the Win32 calls it gained since Hookshot, GCC fixes), and a Linux workflow (`.github/workflows/linux.yml`) runs the game-free checks on Ubuntu 24.04 and makes the AppImage on Ubuntu 22.04 for every pull request. Built from a 1.1.1 dump and played on Linux (CachyOS, Ryzen 5 5500X3D, Radeon RX 9070 XT with RADV): sound, an Xbox controller and the opening village work; a thread the engine lowers on Windows (OptimizeChangeBank) is now lowered on Linux too.

## Repository layout

- `engine/`: the Hookshot-derived recompiler, host layer and Vulkan back end, with HeyListen's changes
- `docs/`: user guide, ADRs, benchmarks, ticket and sprint specs, research
- `tools/`: bench, release and orchestration scripts
- `profile/`: the optimization profile for the lifted code
- `references/`: manifest of the comparison repos and builds (kept in place, not committed)
- `.github/workflows/`: the Linux checks and AppImage (`linux.yml`); `engine/.github/workflows` are Hookshot's and do not run here

Maintainers: `tools/release/make_release.ps1 -Tag vX.Y.Z` makes the release assets (see the script's header); the icon is `engine/assets/heylisten.svg` (original art, drawn by `tools/release/make_icon.py`).

Syncing with upstream:

```
git remote add upstream-hookshot https://github.com/NotRexed/Hookshot.git   # once
git fetch upstream-hookshot
git subtree pull --prefix=engine upstream-hookshot main
```

## Contributing

Bug reports, crash reports, feature ideas and tests on other hardware help the most, above all from **AMD and Intel
GPUs, the Steam Deck and Linux**: [open an issue](https://github.com/Sacrappy/HeyListen/issues/new) with your GPU,
driver and `hookshot_log.txt` (never game files, keys or saves). Fixes are welcome as pull requests.
[CONTRIBUTING.md](CONTRIBUTING.md) has how to report, build, test and send a change.

## Credits

HeyListen is **made by Sacrappy**, with contributions from:

- **[Mooseca](https://github.com/Mooseca)**: Linux (the engine builds again, the CI job, the game tested on a Radeon RX 9070 XT, what the players' guide says about it), the shaders prepared again when the program cache lost them, the checks that build and run again, the title movie's judder (no virtual core held while a picture decodes), the Full overlay's GPU numbers on Linux with AMD cards, RADV's readback wait limited to the light meter (+26% on an RX 9070 XT), the Linux diagnostics (#26), the update offered only for a newer release (#29), Mesa's submit thread on RADV (+10 fps on an RX 9070 XT, #32), the opening movie kept whole above 60 fps (#28, after Hookshot), the replay's per-draw costs (#38, #39, #40, #41), the recordings' copy lock (#42), the game's systems kept at their speed above a 60 cap (#46), no virtual core held while a thread sleeps on a host lock (#56), the Vulkan back end built with a play profile (#54), the upload budget counted per looking-up thread (#58), Linux's short sleeps and sound reopening (#43, #44), storage buffers at the driver's alignment (#45), older game folders starting again (#48, after Hookshot) and three Vulkan usage errors the validation layer found (#64).
- **[Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot)** (MagicBeanDev), a sibling fork of Hookshot whose fixes HeyListen takes: Amateurwizrd (the window filter that averages a larger picture, #307), Yeetbot90 (a slow movie's pictures and Intel's Windows compile fault, #252; weak Intel laptops, #404), Guep (Intel integrated GPUs' uniform updates and pipeline cache, #189 and #190), nyannoying (Arc B-series device loss in Kokiri Forest, #444; a submit's capture missing pages marked again, #1083), Zelda Ootr (Steam Deck Game Mode starts that recompiled every pipeline, #900; read-backs after a timeout, #553 and #554; guest pages under an import, #468 and #469; unrolled software matrix multiplies, #1086, with sheep's matrix code), mooki (the import table scan, #431; shader programs over 1 MiB, #427; samplers past the driver's limit, #442), Yeetbot90 (ASTC decoded on the CPU for AMD's desktop-CPU graphics, #489) and SadTransGirl (per-frame render targets reused, #994).
- **[Hookshot](https://github.com/NotRexed/Hookshot)'s pull requests** taken for performance (batch 1): Joey Magnus (four pages kept for the replay's reads), valonso, with djs5008 (32 submitted pages a thread, a captured page's words read from the capture), Douglas F (MMajor87) (write-backs of large GPU-written bindings skipped), DUDUYG (the log written in batches, the pipeline cache written only when it changed), NotRexed (passes ordered by the game's barriers, the capture's pool list spread over its buckets, a freed region's discard giving its virtual core up, the core-wait escape), Mooki / Michael Schienbein (the virtual-core escape fixes, #1929, #2301, #2302 and #2387), Hugo Claria (texture spans aged once a present) and lucasmazzoni45-del (six background workers from 16 hardware threads).
- **Hookshot fixes** taken after batch 1: NotRexed, with pixl27 (a pipeline drawn only with the program object it was built from, a cause of the device lost near water; Hookshot 65e2cb91, built on pixl27's 8b1e96ad; nyakuoff's A/B on an RX 7800 XT, #65); NotRexed (the textures leave commit free near Windows' limit, Hookshot #2972); hckrman101 (a full model heap's allocations tried in its parents, Hookshot 1b695334); mooki, with GPT-6 Astra (the Scarecrow's Song recorded and played at its own pace above 60 fps: it no longer stops on its first note, Hookshot 2d8044fb, #2906, #2828); CraftyCorvid (the battle music stops after a fight above 60 fps, and the music manager's clock keeps real time, Hookshot bf802502); PirateKingIsh (Gohma reaches her ceiling position above 60 fps, Hookshot 93a8bff3).
- **Longshot fixes** taken since 0.4.2: notquiteog (frame generation takes the multiplier its sets were made for, 1bf4284a), Arkex-ai (texel buffer views tied to the buffer they were made on, a23587f5), the renderer's allocations waiting at the commit limit (#1291); mooki (the Scarecrow's Song above 60 fps, 08771847, #87 and #294); mooki, with the reporters of Hookshot #1117 (the textures and samplers the game registers before the renderer starts are read from the host's copies: at some Windows heap addresses they came through all zero, so Castle Town and Hyrule Castle had no direct sunlight and the halos were blocks, 696d85b0, #80); CraftyCorvid (the battle music above 60 fps, eb5ba6e2, #1224; Epona jumps fences above 60 fps, 8e8a06db, #1434; the Shadow Temple's barge carries Link and the enemies above 100 fps, 4f755527, #1528); PirateKingIsh (Gohma above 60 fps, 3ab8cae6, #1098).
- **Hookshot fixes** taken for 0.4.3 (the shader stamp group, which makes every player's programs again once): jarynclouatre (a size query takes the array type its handle is sampled with, 29e2bfff; a draw runs its own queued translation, 8bac8aa6); klimovich008 (texture handles read from the stage constant banks, 46548702); hckrman101 (cached descriptor counts and the formats of noted pipelines, 59f9284e; F2I.S32 and I2F.U32.RZ conversions and the LDSM address copy, ported from Fritchii, 53bc3d4c); Joey Magnus (a program ends at its closing self-branch, 7e1db372; R2UR's source is one register, 597edcb3; an immediate's low byte is not a scratch register, d0f35116; ISBERD.O, 67c048e0; STG and LDG [URn+imm] with the ordering bits, 923a334d and 692a1c0f; TLD.MS lowered to TLDS 2D.LZ.MS, 20dd78bb); Miguel Luna (a subgroup barrier between a warp's shared memory store and its next load, dd2369ee); Kolkka (the mesh index map in whole 32-primitive blocks, f9286c45); yassine milal (8/16-bit global stores written directly, 358e7c3f); mooki (image, image-buffer and texture-buffer arrays are arrays in the SPIR-V, 656b7390).
- **Mesh shaders on AMD RDNA 2** (the compute path, taken from Hookshot and Longshot): Robin (the mesh fallback's slots, records, clears, outputs and pipelines built on a worker), Dewey Harry D. bartolo (RADV's mesh shaders off by default on Linux, #51, with hckrman101, Longshot #2881), Mesquita (task and mesh draws through the fallback on RDNA 2 on Windows, #57).

It stands on the work of many people. Everything below is credit due to Hookshot's own contributors, whose recompiler, builder and renderer do the work:

- **NotRexed**: [Hookshot](https://github.com/NotRexed/Hookshot), BlueShell (Mario Kart World), Skybound (Tears of the Kingdom) and the a64-kit tools.
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **Eglantine**: the shader recompiler and Switch 2 shader work.
- **suyu**: the graphics decoders and the AArch64 translator.
- **[seedds/nxnative](https://github.com/seedds/nxnative)**: reference for how task and mesh shaders work.
- **[EvilBoyce / evilmfkr](https://github.com/evilmfkr/Hookshot)** (EvilOOT): the GPU culling that brings back Kokiri's foliage, the ambient light probes, the pond's water, the game's own fog, the freeze after Load and the exposure flicker fix.
- **[zortos293/Lichii](https://github.com/zortos293/Lichii)**: reference for Ampere shader lowering, and its compare-exchange retry fix, and the clustered lights' shadow maps read through descriptor arrays (the dark blocks on water and ice, #16; by Zortos, taken through Hookshot and Longshot #107).
- **[mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw)**: the bundled Windows compiler (no Visual Studio needed).
- **NVIDIA DLSS**: optional builds use NVIDIA DLSS Super Resolution (downloaded by each build, never bundled). NVIDIA and DLSS are trademarks of NVIDIA Corporation.
- **[kinnay/zbic](https://github.com/kinnay/zbic)**, **Opus** and **SDL**: decompression, audio and controllers.
- **[Kenney](https://kenney.nl/assets/input-prompts)** (Input Prompts, CC0): the Xbox, PlayStation and Steam Deck button icons of the Button prompts setting. The builder draws them into the game's icon font on your PC, so nothing of the game is shipped.
- **[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)**: Hookshot's README layout, and notes that confirmed the mesh stage's control section.
- **[MythosOfs/Orchid](https://github.com/MythosOfs/Orchid)**: the fog and exposure workaround for Ocarina of Time.
- **Mesa (NAK)**: the reference for Ampere's instruction encoding.
- Everyone who sent fixes to Hookshot: SkippySteve (Linux and the AppImage, and many more), monsterlag, klimovich008, hckrman101 (the exposure flicker and fog fixes, build speed, CI), shayderrr, gab9281, Wpaif, nicholasgower, ZurielAtr, Joey-Magnus, Zorkats, d3zessete, chrispository, vzeol, leonidasmark, Segmentree, ShyVortex, hiKareeem and i900gaming, among others.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). HeyListen is a fork of [Hookshot](https://github.com/NotRexed/Hookshot) (GPL-3.0), which lives in `engine/` with its history; its lifter is derived from suyu (GPL-2.0-or-later) and its Vulkan back end contains Eden code (GPL-3.0). Third-party sources and their licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). See [docs/adr/0001-license.md](docs/adr/0001-license.md).

The Legend of Zelda and Ocarina of Time are trademarks of Nintendo. HeyListen ships none of its assets.
