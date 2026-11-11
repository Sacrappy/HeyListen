# TICK-054 — +20 fps on the main PC by shortening the CPU critical path of a frame  · model: Fable 5.1 · after TICK-052
**Why**: the Market runs at ~78 fps standing, ~73 walking (TICK-045 build, Ryzen 7 7800X3D, RTX 5070). The earlier tickets
measured the parts (the job chain ~6.5 ms, the main thread's serial part ~5.4 ms, the replay ~7.8 ms on the Presentation Thread
plus 4 workers) but no timeline said which of them the frame actually waits for, frame by frame.
**Goal**: (1) the real critical path of a ~13.7 ms frame, as a timeline of a few frames with numbers (which thread waits on
which; whether the game frame N+1 overlaps the replay of frame N; the sync points); (2) the biggest structural lever, behind an
env switch, on by default only once its A/B shows a gain; (3) the same behaviour (speed ratio ~60, shots alike, no new crash), the
game's job ordering kept. Not touched: `vk_memory.cpp`'s capture code and `vk_texture.cpp` (TICK-053 works there).
**Benchmarks**: castle-move screen, 3 interleaved runs a variant; the full set (kokiri-idle, castle-move, hyrule-walk, 6 runs
each, order reversed in rounds 4-6) only to confirm an adoption. 110 s runs, 66 s cut, `--fps-limit 360`, frame generation off.
**Branch**: `feature/TICK-054-cpu-frame` (from main 4b170f0, TICK-052 merged in). Local only.

## Result (2026-11-12): the frame's loop measured; HOLD=2 + TICK-050/052's switches + SELF=2 + 5 recorders, adopted
**Timeline** (`HOS_JOBTRACE` marks, `tools/bench/frameline.py`, `tools/bench/chainb.py`; the renderer marks through
`hos_jt_mark_ext`): in the base build two chains end within ~1 ms of each other (the main thread: draw code, its two submits'
capture and replay, the job group 6.5 ms, the module phase, the last stage, the wait for the present; the Presentation Thread:
its replay 7.3 ms, 4.4 of it waiting for segments, the present). With `HOS_NVN_HOLD=2` (the main thread's submits replayed by a
host thread) the frame becomes **one serial loop**: present -> the main thread's draw code (3.0 ms) -> the held flush (2.1: a
1.36 ms capture beside the job group, its take 0.6) -> the PT's submits (7.8: dry passes ~1.2, mirror pre-sync 0.78, its own
segment 2.35, waits 0.68) -> present (0.6). The GPU is never waited for (~3 ms of GPU time a frame).
**Screens** (castle-move, 3 runs a variant): HOLD=2 alone +1.5 (inside the spread); with TICK-050/052's switches +3.1 (screen 2);
the pre-copy kick -0.6 and -0.3 (with a 12 MB buffer): not kept; HOLD=3 (the capture at the game's submit, the replay held) moves
the copy along the loop (one trace): removed; thread priorities +1.2 then -0.5: off; `RX_VK_PARALLEL_SELF=2` +3.1 and
`RX_VK_PARALLEL=5` +2.7 against the combo (screen 5), both together best in screen 6 (+0.9 against SELF=2 alone).
**Confirm** (6 rounds, 4-6 reversed; fps / 1% low / p99): kokiri-idle 72.3 / 45.3 / 18.9 -> **76.1** / 47.3 / 18.6; castle-move
68.3 / 31.8 / 24.7 -> **70.2** / 31.2 / 24.2; hyrule-walk 69.9 / 26.7 / 23.4 -> **72.1** / 36.4 / 21.7; above the base in every
round. Frame prep 8.11 -> 6.77, 8.80 -> 8.49, 8.61 -> 7.90 ms.
**Defaults now** (`=0` turns each off): `HOS_NVN_HOLD=2`, `RX_VK_PARALLEL_ROUNDS`, `RX_VK_PARALLEL_SELF=2`,
`RX_VK_PARALLEL_BALANCE`, `RX_VK_PARALLEL_POINT=32` (=0: 64), `RX_VK_PARLOCK_SPIN=4000`, `RX_VK_TL_POOLS_WIDE`,
`RX_VK_GONE_RING` (set by the host before the back end loads: its switch is in vk_texture.cpp, TICK-053's); `RX_VK_PARALLEL`
5 at 16+ hardware threads, 4 from 8. Off, kept: `RX_VK_PRECOPY_KICK` + `HOS_PRECOPY_KICK`, `HOS_NVN_HOLD_PRIO`,
`RX_VK_CAPTURE_PRIO`, `HOS_NVN_HOLD_CHECK`.
**Correctness**: shots alike standing and walking (not bit-identical: NPCs and camera at other game times), shot brightness
series equal; `HOS_SPEED_LOG` median 60.02 standing and walking with the combo (base 59.99 / 60.04), a few seconds under 59 at
hitches; the title-to-Market load in every run, no crash (a walk out to Hyrule Field was not reached by script); saves identical
after every run; run_all 235 pass, 2 fail (the nup tests), 35 skip; `test_parallel_init_order` passes with the Vulkan SDK.
**Not verified**: a 6-core PC (PC 3050: 4 recorders there by the thread count, HOLD=2 and the rest unmeasured); a scene change
by walking; HOLD=2's late writes (`HOS_NVN_HOLD_CHECK`: 0.14% of the captured pages had other bytes at the flush, an upper
bound) beyond what the shots show; frame generation on.
**Game folder**: this build (exe b6462ab3, dll 07b8dcd6). Docs: perf-audit "TICK-054", docs/benchmarks/2026-11-12-tick054.md.
**Next**: the mirror pre-sync (0.78 ms) off the PT's path; segments started as the dry pass passes their boundaries; a
smaller held-flush capture.
