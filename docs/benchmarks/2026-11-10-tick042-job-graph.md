# TICK-042: the game's job graph at the Market (2026-11-10)

## Setup
- Hardware: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11.
- Builds: worktree `feature/TICK-042-job-graph` (main 4fbe59f), dev-build work folder (`%LOCALAPPDATA%\HookshotBuilder`, the default lift and upstream's hot list and profile), 8 jobs, results in `build/tick042/bin/<variant>` (`build/tick042/vbuild.ps1`). No shader stamp input changed.
- Runs: `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off, cut at 66 s; each variant's exe and dll copied into the game folder before its run (`build/tick042/ab.py`: one game at a time, no game process left and saves equal to the backup after every run).
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walking and turning).

## Traces (job-marked builds, not for fps)
- jt: marks on f_8004ab40 / f_8007fe60 / f_80df25c0 indirect calls only (a third of the executors' work unmarked).
- jt2: + f_80049280 and its tail jump (still the f_8004fee0 sub-runner unmarked).
- jt3: + f_8004fee0, f_8004fa40 and their call sites: all executor work marked. `build/tick042/tr-jt3-{kokiri-idle,castle-move}` (trace 3 s at 90 s, samples 72-108 s, `jobgraph.txt`).
- One standing run (jt2) ended at 91 s in a crash of the HOS_SAMPLE_ALL sampler thread (`sample_all_thread`, an access violation in ntdll); the same run again was clean. The sampler is a diagnostic; no default path.

## The frame with the marks (jt3, ms a frame)

| | kokiri-idle | castle-move |
|---|---:|---:|
| frame | 14.38 | 16.25 |
| main thread alone | 2.14 | 3.03 |
| main thread + job threads | 2.65 | 2.62 |
| main waits, 0 executors in a job | 2.34 | 2.96 |
| main waits, 1 executor in a job | 1.68 | 1.54 |
| main waits, 2-3 | 0.98 | 1.36 |
| main waits, 4-5 | 1.71 | 1.58 |
| main waits, 6 | 2.87 | 3.16 |

## The long jobs (medians)

| job | length | starts late by | rank | alone after |
|---|---:|---:|---:|---:|
| Link's 800b3ce0, standing | 1514 us | 451 us | #17 of 48 | 829 us |
| Link's 800b3ce0, walking | 1727 us | 275 us | #13 of 41 | 864 us |
| 80166100 (walking) | 1462 us | 3 us | #5 of 15 | 85 us |
| 802ca820 (walking, 0.54 a frame) | 1136 us | 172 us | #124 of 128 | 876 us |
| last stage 800b73d0 | 45 jobs (standing), 34-41 (walking) | | | always 4 executors |

## A/B: per-thread object cache 1024 -> 16384 entries (env, one binary `bin/dbg3`, 3 interleaved rounds)

| variant | kokiri-idle runs | mean fps / 1% low / p99 ms | castle-move runs | mean |
|---|---|---|---|---|
| default | 67.1 69.9 68.9 | 68.6 / 36.2 / 22.9 | 67.7 66.7 64.9 | 66.4 / 28.4 / 27.5 |
| HOS_OBJ_CACHE_BITS=14 | 63.0* 69.2 70.9 | 67.7 / 35.5 / 22.6 | 66.3 66.5 62.7 | 65.2 / 27.4 / 28.4 |

*: overlapped an analysis job; without it 70.0. Not kept (inside the spread, walking -1.2).

## Object statistics (HOS_OBJ_STATS, castle-move / kokiri-idle, per 5 s)
- ~20 M nn::os object calls, 2.2-2.4 M per-thread cache misses (8,000 distinct objects), 1.6 probes a miss, 0.2-1.6% of misses on tombstones, 9-80 k locked lookups; 220 k objects in a 524,288-slot table.
- With 8,192 cache entries: 1.78 M misses.

## Unit tests
run_all (llvm-mingw on PATH, `-X utf8`): 179 pass, 9 fail, 23 skip. The 9 are on the known list (test_ub_threads, test_fps_rate, test_precompile_compute_guard, test_present_trace, test_fpx_sticky, test_fps_overlay_order, test_present_semaphores, test_nup_paths, test_parallel_rb_refresh); no new failure. (Without `-X utf8` run_all itself stops on a cp1252 print of test_nup_paths' output.)

## Game folder
The backed-up Sync 10 exe and dll restored (md5 as before); a kokiri-idle run afterwards: 69.4 fps, saves identical.
