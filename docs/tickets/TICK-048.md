# TICK-048 — Fewer bytes from CPU to GPU (the msync scatter)  · model: Opus · after TICK-045, PC 3050 baseline
**Why**: on PC 3050 (i5-10400F, RTX 3050 on a PCIe 3.0 x4 link) the mirror syncs' scatter took 8-12 ms of GPU time a frame to copy 22-34 MB of CPU pages into the pools' VRAM mirrors (~2.8 GB/s), and the main thread waited ~13 ms a frame in the driver. The same bytes cost 0.4-0.8 ms on the main PC. Many players have slow links.
**Goal**: measure the scatter's bytes by pool and kind, cut the bytes (pages the GPU reads, unchanged sub-ranges, host reads, batching), behind `RX_VK_*` switches; A/B on PC 3050; check whether ReBAR (newly on) and TICK-045 change PC 3050's numbers; check the link width under load.
**Branch**: `feature/TICK-048-upload-bytes` (built and measured on PC 3050 only)

## Result (2026-11-11): the syncs copy only the 256-byte chunks that changed: msync 32 -> 4.4 MB a frame, +2.6 to +4.1 fps on PC 3050; on by default

**Step 0 (PC 3050, 5 interleaved runs a scene)**:
- TICK-045 vs Sync 11: kokiri-idle 25.69 -> 26.03, castle-move 23.75 -> 24.51, hyrule-walk 25.78 -> 27.34 fps. Only the street is near a real gain; the other two are inside the spread.
- ReBAR alone (Sync 11 now vs the baseline before the reboot): 25.7 / 23.75 / 25.8 vs 26.2 / 23.0 / 26.3: no effect. The scatter is the GPU reading host memory, which BAR1's size does not touch.
- The link under load: `nvidia-smi` every second through every run: **Gen 3 x4** after the 66 s cut in all runs (Gen 1 x4 at idle). x4 is not an idle state.

**What the scatter copies** (`RX_VK_MSYNC_STATS=1` / `RX_VK_MSYNC_DIFF=1`, new): ~30 submission parts a frame at the Market, 0.86 MB a part standing (0.74 walking), all synced at a binding outside the parallel segments; the presync copied nothing (all pools were already synced in their part); 99% of it from earlier submits' captures, ~0 from guest memory. The top pools are the game's 16-32 MB pools of flags 0 and a 48 MB pool of flags 0xa1, each 0.05-0.1 MB a part. Of the synced bytes, **85% are unchanged since the page's last sync** at 256-byte granularity (80% at 512 B, 73% at 1 KB, 47% of whole pages): the game rewrites its per-frame data with the bytes it held. The readback refresh copied another 5.4-5.9 MB a frame of whole 4 KB pages (2.0-2.2 ms).

**Adopted: the chunk diff** (`gpu/vk/msync_diff.h`, `vk_memory.cpp` "the mirror syncs' diff"; `RX_VK_MSYNC_DIFF`, default 2): each page a sync copies from a capture is hashed by 256-byte chunk (64-bit XXH3-style hash, AVX2) and the hashes kept by guest page; the next sync of the page copies only the chunks whose hash changed. The hashes hold only while nothing else may have written the mirror there: recorded only outside parallel segments, tied to the mirror (`mirror_id`) and the submission (epoch); voided by any GPU write noted for the page (written bindings, GPU-write stamps, bare-entry and wide ranges, the readback refresh) in the same or a later submission, and by pages copied another way (live, in a segment, kept by `RX_VK_PROTECT_BARE`). GPU time a frame (`RX_VK_PASS_TIMING`): msync 11.26 ms / 32.1 MB -> 1.61 ms / 4.4 MB standing, 10.39 / 29.5 -> 1.63 / 4.4 walking; the profiled frame 35.8 -> 26.9 ms. CPU cost: hashing ~0.13-0.14 ms a part on the replay thread (~4 ms a frame summed); table 68-73 MB of RAM.

**A/B** (one dll built on PC 3050, env switch, interleaved, 5 runs a variant, 110 s runs, 66 s cut; fps / 1% low / p99):

| scene | base (diff off) | diff on | gain |
|---|---|---|---|
| kokiri-idle | 26.01 / 20.5 / 45.9 ms | 30.11 / 23.5 / 40.0 ms | +4.1 (+16%) |
| castle-move | 24.36 / 10.8 / 77.4 ms | 26.91 / 10.7 / 80.7 ms | +2.6 (+10%) |
| hyrule-walk | 25.71 / 16.8 / 50.1 ms | 29.37 / 16.1 / 53.6 ms | +3.7 (+14%) |

Every diff run is above every base run standing and walking; on the street the lowest diff run (27.7) is above the highest base run (27.2).

**Tried, not adopted: `RX_VK_RB_RANGES=1`** (the readback refresh copies only the bytes the CPU's invalidates covered, not the whole page): rbrefresh 5.4 -> 1.7-1.9 MB and 2.0 -> 0.7-0.8 ms a frame, but castle-move 27.05 -> 27.35 fps with the diff (3 runs each): nothing. Off by default, kept as a switch.

**Not done**: GPU reads of host memory for some pools (the capture's pages are not laid out as the pool; live guest memory is the game's next frame); a transfer queue (the syncs are now ~1.6 ms); dropping the presync's unbound pools (it copied 0 bytes here).

**Correctness**: shots every 300 presents, standing and walking, base / diff / diff + rb ranges: ground, fountain, people, shadows, exposure and HUD alike (not pixel-equal: other moments). `HOS_SPEED_LOG` ratio 59.7-60.1 in the steady part. Saves identical to the backup after all runs, no game left. Tests: `tests/test_msync_diff.py` (new; hash forms, masks, runs, the source's void points), `test_mirror_inherit.py`, `test_capture_stream_copy.py` pass on PC 3050 (clang++ of llvm-mingw).

**Benchmark timing**: on PC 3050 the scene is reached 50-62 s after launch (boot, title, save load at the 360 cap: fps 100+ before). fps 5-15 s after reaching the scene are 0.90-0.99 of the 66-110 s figure, so the 66 s cut cannot be shortened there; `pcbench.py` takes `DURATION=<s>` to shorten the measured part instead.

**To check on the main PC (not run there)**: fps standing/walking/street with `RX_VK_MSYNC_DIFF=0` vs default (the hashing costs the replay thread ~2-4 ms a frame; the bytes there cost only 0.4-0.8 ms of GPU, so the diff may lose a little); shots; `HOS_SPEED_LOG`; Kokiri Forest (exposure, foliage records); the dll built with the builder's own tools.

Docs: engine/docs/perf-audit.md "TICK-048", docs/benchmarks/2026-11-10-tick048-upload-bytes.md.
