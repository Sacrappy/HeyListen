# TICK-057 — PC 3050: cut the GPU time per frame (picture unchanged) · model: Opus 5.5 · after TICK-053/054
**Why**: PC 3050 (i5-10400F, RTX 3050 8 GB on PCIe 3.0 x4) runs castle-move at ~29 fps. The frame is serial: GPU work, then
CPU (with the game thread's late-page wait for the previous frame's GPU work). TICK-053's run-ahead stalls in session 0, so the
sure lever was taken to be less GPU time a frame.
**Goal**: name the top GPU costs; cut the engine's own (barriers, the dispatches, rbrefresh, msync, clears/copies, load/store
ops), each behind a switch, on only after an A/B gain; the game's own passes only where the output stays identical.
**Branch**: `feature/TICK-057-gpu-time` (from main d110269). Local only; bundle `E:\OOT\tick057.bundle`.

## Result (2026-11-12): the GPU frame is the game's own work; no engine cut moved fps, nothing adopted

On PC 3050 the unprofiled GPU frame is ~19.4 ms of recordings plus ~2.6 ms of upload command buffers; the queue idles ~12 ms
while the CPU records. The GPU runs ~15 ms without a gap right before each late-page wait ends, so a GPU cut there does shorten
the wait. But the engine's part is small once the timestamps stop inflating it: removing the mirror churn, ~110 duplicate
barriers inside segments, two thirds of the readback-refresh bytes, robustness, the engine's pass-start and dispatch barriers,
or narrowing the game's ORDER_FRAGMENTS barriers each moved gpu_ms by 0-0.4 ms and fps inside the spread. The one cost that
shows is the drains of the game's own barriers: leaving out the pass-start, dispatch and game barriers (a measurement, wrong
picture possible) gives +2.5 fps (28.95 -> 31.47, gpu_ms 19.6 -> 16.8, late wait 7.8 -> 5.4), and only the leaving out of
the game's own does it. All switches stay off.

## The GPU frame (castle-move; details and tables in `docs/benchmarks/2026-11-12-tick057.md`)
Profiled (`RX_VK_PASS_TIMING=1`, serial replay, inflated), main32d: 26.1 ms = the game's passes 12.2, the game's dispatches 4.4
(its upscaler network `9dc98871eb7fc101` 0.53), write-backs 1.0-2.0 (14.6 MB, diffed), rbrefresh 1.9 (5.2 MB, 26 dispatches),
msync 1.5 (4.0 MB), barriers 1.1 + 0.25 (536 full a frame, 260 left out), mmake 0.3-1.8 (mirror churn), ASTC 0.5-2.2.
Top 10 spans: wb copies, rbrefresh scatter, msync scatter, pass `1a7482c0ec9991ea` (2560x1440, one draw, 1.1 ms), mmake,
dispatch `9dc98871eb7fc101`, pass `d8180bd8c6ecc77b`, BC-cache copies, the game's barriers (144), dispatches `ee7764a6b1ef459c`
and `27ee9883e2a20323`. In the late waits' tail (pt3, 18 ms a wait): passes 8.4, dispatches 1.9, ASTC 0.55, rbrefresh 0.54,
wb 0.44, msync 0.42, barriers 0.44, mmake 0.31.
Unprofiled (t57a/b/c base): gpu_ms 19.4, gpu_up_ms 2.6, gaps 12.0, late wait 7.5, tail 14.5-15.5, nvidia-smi 62-63%.
The game composes 2560x1440 here because PC 3050's `hookshot_options.ini` has `output_res=1` (the game's own is 2048x1152).

## Screens (castle-move, 3 interleaved runs, one binary; fps mean / 1% low / p99 ms)
| screen | variant | fps | 1% low | p99 | note |
|---|---|---:|---:|---:|---|
| s57a main32d | base / `RX_VK_MIRROR_KEEP=1` / TICK-016 trust | 29.35 / 28.63 / 29.43 | 15.0 / 15.7 / 15.5 | 53.6 / 53.8 / 52.8 | keep: +250 MB VRAM, worse; the old trust is inert in segments |
| s57b t57a | base / `RX_VK_MIRROR_STICKY=1` / `RX_VK_BARRIER_DEDUPE_PAR=1` | 29.29 / 29.37 / 29.38 | 15.1 / 15.4 / 14.0 | 52.8 / 53.2 / 51.2 | churn gone; ~110 more barriers left out |
| s57c t57a | base / combo (sticky + dedupe + `RX_VK_RB_RANGES=1`) / `RX_VK_ROBUST=0` | 29.52 / 29.50 / 29.93 | 15.0 / 14.1 / 15.0 | 52.2 / 53.4 / 52.7 | robust off: one run 30.8, gpu_ms unchanged |
| s57d t57b | base / `RX_VK_TEST_BARRIER_DROP=1` / `RX_VK_MIRROR_VRAM_FREE=1` | 28.95 / **31.47** / 28.89 | 14.2 / 14.2 / 14.6 | 52.8 / 52.9 / 52.7 | drop is a bound only |
| s57e t57c | base / `RX_VK_BARRIER_TRUST=1` / `RX_VK_GAME_BARRIER_NARROW=1` / both | 29.27 / 29.18 / 29.50 / 29.51 | 15.1 / 16.3 / 14.6 / 15.3 | 55.3 / 52.0 / 51.9 / 51.3 | gpu_ms 19.3-19.6 in all |
No confirm was run (nothing cleared the spread except the bound).

## Changes (all off by default unless said)
- Diagnostics, on: the parallel replay's segment recordings get the GPU timestamps the other recordings had (`gpu_time_begin` /
  `gpu_time_end`, one line each in vk_replay.cpp's segment code): the trace's `gpu_ms` and `gpu_gap_ms` were 5.2 / 26.4 ms with
  segments missing, now 19.4 / 12.0. New log line `vk: GPU tail before the late waits` (the GPU busy with no gap up to the
  value a capture waited for: `gpu_tail_note_late` at the wait, `gpu_tail_compute` when its recordings are collected); with
  `RX_VK_PASS_TIMING` the tail's spans by kind and label (`gprof_tail`). Mirror counters on the `mirrors given back` line: by why
  (the VRAM rule, the mirrors' budget, long idle) and made again within 6000 submissions. The barrier line counts the ones
  left out inside segments and the trust's.
- `RX_VK_MIRROR_STICKY=1`: a mirror made again within kLongIdle of going is not given back for the 80% VRAM rule alone.
- `RX_VK_MIRROR_VRAM_FREE=1`: the 80% rule counts the free space in VMA's blocks as free (as the textures' budget does).
- `RX_VK_BARRIER_DEDUPE_PAR=1`: the full-barrier dedupe inside parallel segments, by a per-command-buffer work count
  (`vk_work.h` `all_work_slot`; the draw, pass-edge, barrier and begin macros take the command buffer; NGX's commands count in
  `foreign_work`).
- `RX_VK_BARRIER_TRUST=1`: the engine's pass-start barrier becomes attachment-only (or none right after a barrier) and its
  dispatch barrier is left out when, since this thread's last barrier on that command buffer, only draws (and dispatches, for a
  dispatch) were recorded; works inside segments (the TICK-016 switches did not).
- `RX_VK_GAME_BARRIER_NARROW=1`: the game's barriers with only ORDER_FRAGMENTS(_TILED) and INVALIDATE_TEXTURE (0x12 and kin)
  wait in the fragment stages only (src all commands). ORDER_PRIMITIVES (0x11) orders every stage after it: kept full.
- `RX_VK_TEST_BARRIER_DROP=1` (measurement only, never for play): pass-start, dispatch and the game's barriers left out.
- Tests: `tests/test_gpu_time_057.py` (all of the above, from the source); `tests/parallel_init_order_test.cpp` stubs the segment
  timestamp calls.

## Correctness
- Every run: saves hash-equal to the backup before and after, no HeyListen.exe left.
- Default build (t57c, all off: only the timestamps and bookkeeping are new), `sh57-def-*`: speed ratio 59.0-61.1 in play
  (~60); shots every 300 presents standing and walking match the base series by eye and by mean brightness (no white or black
  frame in play).
- The switches' pictures were not shot and no validation run was made: none is adopted. The validation layer (SDK 1.4.363.0) is
  in `C:\OOT\tools\vkvalid` with `C:\OOT\scripts\val57.bat` (sync validation, `VK_LAYER_PATH` only) for the next step.
- Laptop suite on the final branch: 253 passed, 2 failed, 20 skipped; the two (`test_parallel_init_order`,
  `test_parallel_rb_refresh`) were their harnesses missing the new segment timestamp calls and the trust point: declared,
  both pass on their own. `test_gpu_time_057` passes.

## Not verified
- Desktop session (all runs over SSH, session 0), kokiri-idle and hyrule-walk screens, other GPUs; a confirm (none needed).
- Sync validation of `RX_VK_BARRIER_TRUST` / `RX_VK_GAME_BARRIER_NARROW` (not adopted, so not run).

## Next lever
- The game's own barriers' drains (~2.8 ms GPU, +2.5 fps bound): narrowing does not help on this driver; what could is fewer of
  them, e.g. the game's back-to-back ORDER_FRAGMENTS barriers between passes that touch disjoint targets (needs the targets'
  read/write sets per pass and sync validation), or split barriers (vkCmdSetEvent2/vkCmdWaitEvents2) placed at the pass end so
  the drain overlaps the next pass's setup.
- The output size: `output_res=1` (2560x1440) on an RTX 3050; the game's own 2048x1152 would cut the post-upscale passes. The
  user's setting, a picture change: not for an engine ticket.
- The serial frame itself (TICK-053's run-ahead and its 66 ms stall): the 12 ms of queue gaps a frame are the larger prize.
