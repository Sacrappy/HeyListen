# TICK-060 — PC 3050: cut the frame time without lowering quality · model: Opus 5.5 · after TICK-053/057/058
**Why**: PC 3050 (i5-10400F 6C/12T, RTX 3050 8 GB on PCIe 3.0 x4) runs the Market at ~28-29 fps with the GPU idle a third of
the frame (60-69%). TICK-053 found the frame serial (the late-page wait); TICK-057 found no engine GPU cut that moves fps.
**Goal**: where the GPU's idle third goes (without PASS_TIMING's serialisation where possible), then quality-preserving cuts, each
behind an env switch, off until proven; picture identical; the original behaviour kept.
**Branch**: `feature/TICK-060-pc3050-gpu` (from main af1cc93). Local only; built and measured on PC 3050 (session 0 over SSH);
bundle `E:\OOT\tick060.bundle`. Details and every table: `docs/benchmarks/2026-11-12-tick060-pc3050.md`.

## Result (2026-11-12): +18-21% on PC 3050, picture unchanged; adopted (on by default)

The GPU idled a third of the frame because the CPU stopped recording while it waited ~8 ms for the previous frame's late pages,
then needed ~6 ms of mirror pre-sync (mostly re-hashing capture buffers for TICK-048's chunk diff) before the GPU had any of the new
frame. `RX_VK_LATE_DEFER` moves that wait from the capture's take to the queue (the GPU reads the same bytes; the CPU records
meanwhile) and `RX_VK_CAPTURE_HASH` makes the diff's hashes in the capture's own copy; with TICK-056's `RX_VK_MSYNC_FAST`:

| scene (confirm: 6 rounds, 4-6 reversed, one binary) | base fps / 1% low / p99 | new | gain | rounds above base |
|---|---|---|---:|---:|
| kokiri-idle | 30.85 / 24.5 / 38.9 | **37.24** / 26.4 / 33.6 | +6.4 (+20.7%) | 6 of 6 |
| castle-move | 29.13 / 14.7 / 54.2 | **34.40** / 16.9 / 47.0 | +5.3 (+18.1%) | 6 of 6 |
| hyrule-walk | 31.35 / 19.5 / 45.1 | **37.20** / 25.9 / 36.2 | +5.9 (+18.7%) | 6 of 6 |

GPU utilisation 62-72% -> 73-83%, the queue's gaps 8-13 -> 4-7 ms a frame, the late wait 6-8.5 -> 0 ms. Shots alike standing and
walking (Market luma 103.7 / 103.5 base, 103.3 / 102.6 new; no white or black frame in play), `HOS_SPEED_LOG` median 60.00-60.07.
**Adopted**: `RX_VK_LATE_DEFER` and `RX_VK_CAPTURE_HASH` on by default (=0 off); `RX_VK_MSYNC_FAST` on by default where the
chunk diff is on (PCIe links of 8 GB/s or less). **Main-PC check needed** (not run from this ticket): there the diff, the hashes and
MSYNC_FAST stay off; only the deferral is new, and it acts only when a take's late value is not reached yet.
Game folder on PC 3050: build `t60e` (exe 17c3e4fb, dll 2dc76b85); main32d kept in `C:\OOT\builds\main32d`.

## Where the frame went (base, castle-move, unprofiled)
- Frame 35.3 ms (trace on); GPU: recordings 19.7 + uploads 2.8, **idle 12.8** between recordings (one long gap a frame, 10-17 ms);
  nvidia-smi 60%, PCIe rx 0.54 GB/s of 3.9 (the link is not the limit).
- The loop (job trace): held flush 11.5 ms, of it **8.1 the late-page wait** (~100 Market pages a frame the previous frame's last
  submission writes back), holding the replay's lock; then the Presentation Thread's 23 ms of recording, of which **5.8 the mirror
  pre-sync** before the frame's first big command buffer reaches the GPU (~4 ms of it TICK-048's chunk diff re-hashing 37 MB of
  capture buffers from DRAM); present 2.2.
- So the GPU idles because the CPU does not record while it waits for the GPU, and then needs ~10 ms before the GPU has the new
  frame's work: not the uploads, not PCIe. The GPU's own time (`RX_VK_PASS_TIMING`, serialised): the game's passes 13.4 and
  dispatches 4.6 of 26 ms profiled (top: pass `1a7482c0ec9991ea` 2560x1440 1.08 ms, upscaler dispatch `9dc98871eb7fc101` 0.52).

## Built (vk_memory.cpp, vk_replay.cpp, vk_texture.cpp, msync_diff.h, vk_internal.h)
- **`RX_VK_LATE_DEFER`**: a capture whose late value is already sent to the queue is taken without waiting; its late slots are
  fixed (the same copies, from guest memory, at the value's end) by a fix thread, or by any submit or flush first; recordings are
  held until then (RX_VK_BATCH's pending list); waits for a fix happen outside the queue's lock, polls never wait; a replay read of
  a pending slot waits and reads guest memory; the mirror sync's diff copies such a page whole. The GPU reads the same bytes.
- **`RX_VK_CAPTURE_HASH`**: the capture's copy makes TICK-048's chunk hashes in the same pass (`msync_diff::copy_hash_page`,
  bit-exact with `page_hash`); the mirror sync takes them instead of re-reading the capture buffer. Same hashes, same copies.
  Only where the diff is on (slow links) and with AVX2.
- `RX_VK_MSYNC_FAST` (TICK-056, unchanged code) measured with them.
- Diagnostics: trace column `late_fix_ms`, marks 38/39, log lines `late pages deferred`, `late fixes`, `capture hashes`.
- Tests: `tests/test_late_defer.py`, `tests/test_capture_hash.py` (new), `tests/test_msync_diff.cpp` (copy_hash_page),
  `tests/test_late_merge.py` (updated needle).

## Measured (PC 3050, castle-move unless said)
- Traces (one run each): base 28.27 fps, gap 12.8 ms, GPU 60%; LATE_DEFER 30.12, gap 10.7, the held flush 11.5 -> 4.6 ms; all three
  33.12, gap 7.8, GPU 71%, the pre-sync 5.8 -> 1.6 ms (105k pages looked at -> 9k; 1386 slot hashes taken a submit, 17 made again).
- The first LATE_DEFER try waited for a fix under the queue's lock in the main thread's fence poll: the Presentation Thread's
  held submits waited behind it (7.5 ms a frame, +1.8 fps only); fixed (waits before the lock, polls never wait). A textured pool's
  late page at first made every Market take wait (the texture-shadow rule was the pool's, not the texture's storage): narrowed
  to a live texture's storage.
- Screen 1 (3 rounds): base 28.09, LATE_DEFER 29.80, + CAPTURE_HASH 33.19, + MSYNC_FAST 33.83 (every run above every base run).
  Screen 2 (against the three): + `RX_VK_SEG_EARLY=1` 34.11, + `RX_VK_PARALLEL=5` 34.19 against 33.18: inside the spread, not taken.
- Shots (`sh60-*`, every 300 presents, standing and walking): same sequence and Market; luma above; speed ~60.
- Confirm (36 runs): the table above; per-round fps / 1% low / p99 in the benchmark doc.
- The default build (`t60e`, no environment): 36.08 / 33.40 fps standing / walking, switches named on in the log, speed 60.02 / 60.00.

## Correctness
- The GPU reads the same bytes: every submission that may read a capture buffer goes to the queue after its pending fixes (the
  same copies, from guest memory, at the late value's end, as the take's wait made them); the CPU's readers of a pending slot wait
  (a replay read as submitted) or copy the page whole without recording a hash (the mirror sync). Same hashes with CAPTURE_HASH
  (bit-exact, tested), so the same chunks are copied.
- Saves hash-equal to `C:\OOT\user-save-backup` before and after every run (~60 runs) and at the end; no HeyListen.exe left;
  no hang; every run loaded the title -> Market.
- `run_all` (llvm-mingw on PATH): 245 pass, 2 fail (the nup tests, known), 35 skip.

## Not verified
- The main PC (to be checked there), the interactive desktop (session 0 only), other GPUs, frame generation on, a scene change
  by walking, `RX_VK_ASYNC=1`, Vulkan validation of the held recordings (the same command buffers, sent later in the same order).
- A texture created over a pending late page and uploaded before the fix would read guest memory (none seen: 0 such pages).

## Next lever
- The frame is now the CPU's recording chain (GPU 73-83%, gaps 4-7 ms): the Presentation Thread's own segments (2 x ~4-5 ms),
  the held flush's capture copy (3.1 ms, memory-bound), the dry passes. `RX_VK_SEG_EARLY` and `RX_VK_PARALLEL=5` gave +0.9-1.0
  fps in a screen (confirm both on PC 3050, and the main PC).
- Then the GPU: ~22 ms of busy time a frame at 1440p (the game's passes and compute); at ~43 fps the GPU becomes the limit.
- The trace's gpu_ms misses SEG_EARLY's early segments (no timestamps there): add them before measuring it.
