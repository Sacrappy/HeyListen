<!-- Copyright (C) 2026 Sacrappy -->

# FOOT3 (27 Nov 2026): perf/foot2's memory switches on main, the release's freeze, and the defaults

Branch perf/foot3 (worktree E:\OOT\HeyListen-wt\foot3, from main bc16e548a). FOOT2's findings and numbers are in
`foot2-host-memory.md`; this note is what the port, the soak and the measures of 27 Nov add.

## 1. What was ported (each its own commit, each off by default)

| switch | what it does | what it saves, by perf/foot2's own numbers | what it touches |
|---|---|---|---|
| `RX_VK_PCACHE_RELEASE=1` | after the start-up precompile the VkPipelineCache made from mkw_vk_pipelines.bin is destroyed and an empty one, made from `mkw_vk_pipelines_play.bin`, takes its place; play's new pipelines go to the play file only | the driver's arenas 6.0 -> 2.5 GB of commit, host side 9.8 -> 5.0 GB (f4-A2 / f4-REL2), page faults -11.8% | the pipeline cache and its saves: the main file is never written after the release (its save returns), the play file is written whole as .tmp and renamed |
| `HL_MEM_TRIM_ARENAS=<s>` | every <s> s, the committed pages of untagged private allocations of 512 MB or more outside the guest window leave the working set (VirtualUnlock of unlocked pages) | working set mean -4.5 GB, free RAM's lowest +2.4 GB (foot2b); commit unchanged; about +1.9 M soft faults | host/hl_mem.c only: a thread; no allocation is freed or changed |
| `RX_VK_BC_CACHE_IDLE_S=<s>` | BC memory-cache entries not served for <s> s go (the cap's own eviction road) | 50 MB in a ten-minute tour at 120 s, for 6.8 GB more copied back from the transcoder: not worth it | vk_texture.cpp's BC cache |
| lazy rx page tables (`RX_PAGE_TABLE_LAZY`) | not ported: perf/foot2 removed it itself (8ee70acd4): the Vulkan back end never allocates those tables, so it saved nothing | 0 | - |
| census tags and the ledger's "driver's blocks for VRAM" | diagnostics for HL_MEM_ACCOUNT / HOS_HOST_SPLIT / RX_VK_LEDGER | - | logs only |

The pipeline cache save path, as asked: after the release nothing ever writes mkw_vk_pipelines.bin again in that session
(`pipeline_cache_save` sends every save to `pcache_play_save`), and the play file's save writes `.tmp` then renames it, so a
cut save leaves the old play file whole. The one way a release session could still damage the main file was a save of it
already under way when the release swapped the cache (section 2): fixed.

## 2. The freeze: what is known, and the fix in the branch

- The one frozen session (f3i-R, 24 Nov 04:31-04:42, release on) stopped presenting at play+625 s; its game log is gone (only
  `build/s5/f3i-R.mem.csv` is left). That csv shows the process alive for minutes after the stop, page faults still rising by
  50-80 k every 30 s, private commit flat at 25.8 GB, 1.1-1.9 GB of RAM free, system commit 55 GB of 83. A second FOOT3 tour
  (f3o-C, the same lease tag T14) ran against the same slot c at the same time and copied the 2.1 GB cache file into it at
  04:32: that session is not a clean one.
- Not reproduced: one 20-minute soak arm on 27 Nov (`f3s-rel1`, build foot3a, release alone, slot b, 1202 s of play, 29
  warps, 29 zones) ran with no freeze and no fatal line, at the machine's commit limit (82.8 of 83.0 GB, 33 MB of RAM free at
  worst: other agents' games ran at once), 24 present gaps over 1 s, the longest 7.0 s, all at zone loads. The rest of the
  soak was parked by the owner's order (release first).
- Found by reading the code, not by a repro: the release destroyed the old VkPipelineCache with nothing to keep another thread
  out of the driver with that handle (the fetch lock exists only on Intel). It runs on the precompile's last worker, which the
  start does not wait for, so play has begun; a draw-time pipeline build, a lazy compute program's first dispatch or a save of
  the main file could be inside the driver with the destroyed cache, and a save that straddled the swap could take the size
  from one cache and the data from the other and write the small cache over mkw_vk_pipelines.bin. Fixed in
  `fix(vk): the pipeline cache release waits for the builds and saves that use the old cache`: while the release is still to
  come, the builds and the main file's save hold a shared lock and read `pcache` under it, the release takes it exclusive and
  destroys the old cache after the swap. With the switch off no lock is taken (the only change is that the build reads the
  global `pcache` instead of the caller's copy of it, the same value). Whether this was the 24 Nov freeze is not known.
- New diagnostic for the soak: `HL_STALL_STACKS=<ms>` (with HL_STALL_WATCH) writes every thread's stack while a gap is
  still open, the piece the stall watch lacked (it logs a gap only when the present comes, so a freeze for good left nothing).
  Checked in f3s-rel1: 11 files of 142-193 threads each. 4 s is too low on a loaded machine (zone loads take 5-7 s): use 10000.

## 3. The laptop's slowdown with the release, from the code

perf/foot2's laptop run: 1.1-1.2 fps with `RX_VK_PCACHE_RELEASE=1` against 1.5 without. The release takes away the cache that
the session's later pipeline creations were counting on:

- The compute programs are lazy (TICK-038, `RX_VK_LAZY_COMPUTE`): the precompile leaves them on disk *because* the pipeline cache
  began with this driver's data (`pcache_loaded`), and builds each at its first dispatch from `pcache`, expecting a few ms. After
  the release `pcache` is the empty (or play-file) cache.
- The laptop has no mesh shaders, so its draws go through the mesh fallback, whose compute pipelines are also built at their
  first draw from `pcache` (`fallback_pipelines_ready` -> `fallback_compute_pipelines` -> `gated_compute(pcache, ...)`); the draw
  that starts one waits up to 50 ms, the others are left out while it compiles (AMD's compiler took up to 12 s on one).
- On NVIDIA the driver's own disk cache serves those anyway: FOOT3's round of 24 Nov found every compute pipeline built in play
  under 30 ms with the release (0 compiled). Nothing shows that amdvlk on Windows has such a cache under the app's: there each of
  them becomes a real compile in play on a 15 W CPU, with draws left out and 50 ms waits, which is the fps the laptop lost.

So the switch must stay off wherever the driver is not NVIDIA's, and wherever the mesh fallback runs (no mesh shaders); its
saving (the 2 GB data arena) is also NVIDIA's driver's own behaviour (it keeps the file's whole data), measured nowhere else.

## 4. Parked (owner, 27 Nov: the release first, the measures at the end)

In order: (1) reproduce the freeze by soak: 20-minute warp tours with HL_STALL_WATCH=1500 and HL_STALL_STACKS=10000, each
switch alone, then at least 6 runs with all three on (the freeze was 1 session in 7), on a build with the swap-lock fix;
(2) measure memory and zone loads on the PC 3050, where the commit limit is met (private bytes, working set, system commit,
lowest free RAM, page faults, GPU memory, seconds from a warp to the first steady frame, off against on, 3 tours a side);
(3) the defaults: on only where it saves memory with no freeze and no slower load; for `RX_VK_PCACHE_RELEASE`, NVIDIA with
mesh shaders only (section 3); `RX_VK_BC_CACHE_IDLE_S` stays off on FOOT2's numbers. All three stay off in this branch.
