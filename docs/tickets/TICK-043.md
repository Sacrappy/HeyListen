# TICK-043 — The engine's submit off the main thread's critical path  · model: Opus · after TICK-042, Sync 11
**Why**: TICK-042: a Market frame starts with the main thread alone for 2.1 ms standing / 3.0 ms walking, ~90% of it the engine's submit (the capture copy, collect_in, take_capture): now the largest serial piece of the frame, ahead of the game's job chain (~6.5 ms). Sync 11 runs the Market at ~73.5 fps standing, ~67.5 walking. The GPU is not the limit: the overlay shows "GPU frame prep 7.15 ms" but "GPU busy per frame" 2.22 ms, so the CPU-side frame prep and submit dominate.
**Goal**: measure the main thread's submit path standing and walking (HOS_JOBTRACE, HOS_SAMPLE_ALL + HOS_SAMPLE_NS, RX_VK_PASS_TIMING, jobgraph.py / crit.py): the capture copy (bytes, time), collect_in, take_capture, the rest of the frame prep; which parts must stay synchronous because the guest may overwrite the memory right after. Prototype moving work off the critical path without changing the picture: only dirty ranges (write watches / `a64_dirty` marks), the capture copy on workers with a fence before the guest can write those pages again, frame prep pipelined with the next frame, less collect_in work. Keep the TICK-006 / TICK-036 ordering (mirror inheritance, segments). Behind an env switch; on by default only if correct and clearly faster.
**Acceptance**: the submit path's breakdown; shots equal to the base build in the Market standing and walking (several over time: no flicker, no stale textures); HOS_SPEED_LOG ratio ~60; no new unit-test failure, a test for new logic; fps / 1% low / p99 in kokiri-idle and castle-move beyond the 2-4 fps spread (interleaved A/B, at least 3 runs a variant, 6 if close; prefer host-side changes, TICK-041's PGO caveat); or proof and a ranked plan.
**Branch**: `feature/TICK-043-async-submit`

## Result (2026-11-10): the submit path measured; copying the capture ahead loses fps; no default changed

**The main thread's submit** (default build, HOS_SAMPLE_ALL + HOS_SAMPLE_NS, sampler on): 1.81 ms a frame standing, 2.36 walking. Of it:
- the capture copy 0.99 / 1.06 ms (6.5-7.7 MB a submit, six submits a frame);
- the mark scan (collect_in) 0.23 / 0.23;
- take_capture 0.21 / 0.53, of which the GPU wait for late pages is 0.11 / 0.22;
- memory_collect_written 0.06 / 0.15;
- the pool-range refresh 0.03 / 0.08;
- the recording of main's own submits ~0.1.

The overlay's "GPU frame prep" (6.9-8.9 ms) is the replay summed over all threads, ~90% of it on the Presentation Thread, off the main thread's path.

**What must stay synchronous**: each page as it is at the submit (OoT rewrites double-buffered data right after a submit), the stamps before the recording that follows, and the late pages' GPU wait before their recopy. Only pages the game does not write again before the submit can be copied earlier.

**Tried** (env switches, off by default, kept in the code):
- `RX_VK_CAPTURE_PRECOPY=1` (`RX_VK_PRECOPY_US`, default 250): a thread copies the written pages into the next capture's buffer before the submit. The submit copies only what was written since, plus the pages written again or written back since (`gpu/vk/capture_merge.h`, test `tests/test_capture_merge.py`). It took ~0.6 ms a frame off the main thread, but fps fell: 75.4 -> 71.4 standing, 68.8 -> 66.3 walking (2 ms period: 73.3 / 67.2). The job threads lose more than main gains (the rewrites copied again, shared cache lines, DRAM traffic). One walking run stayed on the title screen; not explained.
- `RX_VK_CAPTURE_SCAN_SPLIT=1`: the mark scan split over the copy helpers. 75.4 -> 73.0 standing, 68.8 -> 69.8 walking: inside the spread.

**A/B** (one binary, interleaved; fps / 1% low / p99):

| | default (6) | PRECOPY 250 us (3) | PRECOPY 2 ms (3) | SCAN_SPLIT (3) |
|---|---|---|---|---|
| kokiri-idle | 75.4 / 51.6 / 18.0 | 71.4 / 51.7 / 18.2 | 73.3 / 53.0 / 17.5 | 73.0 / 47.6 / 18.0 |
| castle-move | 68.8 / 36.0 / 22.6 | 66.3 / 29.8 / 23.3 | 67.2 / 35.5 / 23.3 | 69.8 / 35.4 / 21.9 |

**Adopted**: nothing. The default capture path is restructured but does the same steps; its log line now gives the copy's time.

**Correctness**: Market shots of the default build and the Sync 11 build alike, standing and walking (with PRECOPY too). HOS_SPEED_LOG ratio 59.99-60.05. run_all 215 pass, 12 fail (the 10 known + the flaky nup_verify / nup_codec), 33 skip; the new test passes.

**Game folder**: the Sync 11 exe and dll restored from `E:\OOT\HeyListen\build\backup-tick043` (md5 as before). Smoke: kokiri-idle 77.8 fps, saves identical. The builder's work folder is on the default lift (no lift run).

**Next** (ranked):
1. Copy fewer bytes: only the pages a submission's bindings read (~75% of the capture is ever synced; needs the read sets).
2. Move the late pages' recopy (0.1-0.2 ms of GPU wait on main) into the recording, as a GPU copy after the write-back.
3. A flat table for take_capture's texture shadows (0.1-0.3 ms walking).
4. A mark summary for the scan, at the next PGO refresh (it is in the lifted code).
5. The game's job chain (TICK-042) remains the larger lever.

Docs: engine/docs/perf-audit.md "TICK-043", docs/benchmarks/2026-11-10-tick043-async-submit.md.
