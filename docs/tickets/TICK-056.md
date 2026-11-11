# TICK-056 — Shorten the frame's loop on the main PC (chain B: the Presentation Thread's part)  · model: Opus · after TICK-054
**Why**: TICK-054 made the frame one serial loop (castle-move 13.6 ms, ~70 fps): the main thread's draw code 3.0 ms -> the held
flush 2.1 (capture copy 1.36, its take 0.6) -> the Presentation Thread's submits 7.8 (dry passes ~1.2, the mirror pre-sync 0.78,
its own segment 2.35, waits for workers 0.68) -> the end of the present 0.6. Nothing waits for the GPU.
**Goal**: the levers TICK-054 proposed, in order: (1) the mirror pre-sync off the PT's path; (2) each segment started as soon as
the dry pass has passed its boundary (the last frame's boundary profile); (3) a smaller held-flush capture; (4) anything else
chainb shows. Each behind an env switch, off until measured; adopted only after the full confirm. Correctness: speed ~60, shots
alike standing and walking, no new Vulkan validation errors, no data races (reasoned).
**Benchmarks**: castle-move screen (3 interleaved runs, one binary); the full confirm (kokiri-idle, castle-move, hyrule-walk, 6
runs, rounds 4-6 reversed) only to adopt. 110 s runs, 66 s cut, `--fps-limit 360`, frame generation off.
**Branch**: `feature/TICK-056-chain-b` (from main 32dc46f). Local only.

## Result (2026-11-12): the PT's frame prep -0.6 to -0.9 ms, fps inside the spread: nothing adopted, the switches stay off
**Found**: the pre-sync is one big job a frame (castle-move: 8,900 dirty pages, 35 MB, of 105,000 pages looked at; 0.86 ms), its
cost per page, not its place. Run on a worker beside the dry pass it does not fit: that buffer's dry pass is ~0.25 ms.
**Built** (all off; `=1` turns each on):
- `RX_VK_MSYNC_FAST` (vk_memory.cpp): the mirror sync walks straight to the next dirty page (clean 2 MB regions skipped wherever
  the walk is in them), keeps the last capture buffer and run list, and notes the dirty pages for the write-backs once a sync
  with a bitmap of the pages already noted. The same copies and sets (`tests/test_msync_fast.py`). Pre-sync 0.86 -> 0.45 ms.
- `RX_VK_PRESYNC_EARLY=<draw>` (1: 64; vk_replay.cpp): the pre-sync on a replay worker during the dry pass, into a command buffer
  of its own submitted ahead of rec_cur. Not kept: the job (~0.49 ms) outlasts the dry pass (~0.25 ms); screen 1 no gain.
- `RX_VK_SEG_EARLY` (vk_replay.cpp `replay_handle_early`): segments handed out as the dry pass passes their planned ends (the
  plan from the replay thread's same-order command buffer last frame); the PT records the last segment after the dry pass. The
  PT's waits for segments 0.73 -> 0.28 ms a frame. Plans used / no boundary reached / rejected are logged (castle-move r1: 9575 /
  18 / 10; the "no boundary reached" ones are at the title -> Market load, where the last frame's buffer was the title's).
- `RX_VK_PRESYNC_STATS` (diagnosis): the pre-sync's pages, pools, bytes and time.
- `tools/bench/chainb.py`: the pre-sync job's marks (22-25). Mark 26: a segment handed out during the dry pass.
**Screens** (castle-move; fps / 1% low / p99 / frame prep): (1) base 69.7 / 23.3 / 36.5 / 7.96, FAST **71.1** / 33.1 / 22.1 /
7.72, FAST+PRESYNC_EARLY 69.9 / 29.1 / 30.4 / 7.94; (2) FAST 70.9 / 23.8 / 32.6 / 7.34, FAST+SEG_EARLY **71.7** / 37.9 / 22.0 /
7.33.
**Confirm** (base = the binary with no switch = main's behaviour; new = `RX_VK_MSYNC_FAST=1 RX_VK_SEG_EARLY=1`; 6 rounds, 4-6
reversed; fps / 1% low / p99 / frame prep): kokiri-idle 74.7 / 44.3 / 19.5 / 7.27 -> **76.6** / 41.8 / 19.3 / 6.41 (+1.9, 4 of 6
rounds above); castle-move 69.9 / 35.4 / 22.6 / 8.67 -> **70.2** / 28.8 / 25.9 / 7.73 (+0.3, 4 of 6); hyrule-walk 72.7 / 33.4 /
21.1 / 7.60 -> **72.4** / 36.5 / 22.0 / 7.02 (-0.3, 2 of 6, 1 tie). Frame prep falls 0.6-0.9 ms in every scene, fps does not
follow: with the PT's part shorter, the main thread's part of the loop grows (one walking trace each: main 2.40 -> 3.08 ms,
PT 8.24 -> 8.02): the game's own chain (the main thread's job group and module phase) is the limit again.
**Decision**: not adopted (TICK-054's bar: above the base in every round; here +0.6 fps over the three scenes, inside the 2-4 fps
spread). Game folder: TICK-054's build restored (`build/backup-tick056`).
**Correctness** (the new path, against the base): shots alike walking and standing (Market lighting, shadows, fog, HUD, the
dialogue box; not time-locked), brightness series in the same range with black frames only at the load; `HOS_SPEED_LOG` median
60.01 / 60.00 (base / new) standing, 60.05 / 59.97 walking (min 59.18 / 58.66); validation (`RX_VK_VALIDATION=1`, castle-move, the load and the
Market): base, FAST and FAST+SEG_EARLY show the same shader denorm VUIDs (pre-existing); `VUID-vkCmdDrawIndexed-viewType-07752`
(a 2D view for an arrayed sampler, binding 7) appeared with FAST alone (2) and in one of two FAST+SEG_EARLY runs (6), never in
base's one run: FAST touches no texture code, so it is a scene-dependent pre-existing one; no layout or synchronization VUID.
Data races: reasoned in perf-audit "TICK-056" (each shared structure, its lock, and why no reader holds a reference across a
reallocation). Saves identical after every run; no game left. Tests: `test_msync_fast.py`, `test_presync_early.py`,
`test_seg_early.py`, `test_chainb.py` (extended), `test_capture_copy_runs.py` (updated); run_all (llvm-mingw on PATH) 240 pass, 2 fail (the nup tests, known), 35 skip (test_parallel_rb_refresh failed first on the early path's copy of its marker comment: the comment changed after the confirm, no code change).
**Not verified**: PC 3050 (6 cores: 4 recorders); `RX_VK_OCC_REAL` with SEG_EARLY; a scene change by walking (only the title ->
Market load); frame generation on; SEG_EARLY under `RX_VK_PARALLEL_SELF=0` (the PT always records the last segment there).
**Next**: the loop's limit moved to the game's chain: the main thread's draw code and what it waits for (its job group, the
module phase), then the held flush (capture 1.5-1.7 ms + memory work 1.3 ms in the walking trace). The pre-copy (lever 3) copies
for "the next capture", which is often one of the PT's small ones, not the main thread's big one: it needs to target the held
submits' capture. FAST and SEG_EARLY are ready to turn on once the PT is the critical part again.
