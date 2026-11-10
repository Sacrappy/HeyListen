# TICK-042 — The game's job graph at the Market: serial parts, tail, executor count  · model: Opus · after TICK-041
**Why**: TICK-039/040/041: the main thread waits 7-8 ms a frame for its job group (f_8007fe60 -> WaitLightEvent, lr 0x80080188), 96% of it job execution on a dependency chain, not total work; during that wait the six executors (BaseProcExecutor0-5) are all busy only 41-44% of the time, one or none 36-39%. HOS_CORES=8 changed nothing (the game sizes its executor pool and per-core job tables itself, in or around f_80b5e9e0). Making the guest code faster at compile or lifter level gave no gain beyond the spread (TICK-040, TICK-041). The GPU (8-10 ms a frame) is the next limit.
**Goal**: map the frame's job graph in the Market, standing and walking: which jobs run alone (the serial parts and the tail), how long, in which guest functions, waiting on what; separate the work the game cannot split from work only scheduled badly (a long job started last). Prototype the biggest win: a native or cheaper path for the heaviest serial job's top functions, job order / priority (long jobs first), more executors through a game patch if the per-core tables allow it safely, moving work that blocks the main thread off the critical path. Behaviour identical (HOS_SPEED_LOG ratio ~60, shots alike, no new glitches); risky changes behind an env switch, off by default unless clearly safe and winning.
**Acceptance**: the job graph (serial jobs, times, functions); fps / 1% low / p99 gain in kokiri-idle and castle-move beyond the 2-4 fps spread (interleaved A/B, at least 3 runs a variant, 6 if close; PGO caveat of TICK-041: prefer changes outside the lifted code), no new unit-test failure; or proof and a ranked plan.
**Branch**: `feature/TICK-042-job-graph`

## Result (2026-11-10): the job graph mapped; no default changed

**How it was mapped.** A diagnostic job-marked lift (`tools/bench/jobmarks.py`, never for fps) records each job's start and end, object and function in `HOS_JOBTRACE` (`JT_JOB_B/E`, `hos_jt_job()`); `tools/bench/jobgraph.py` turns that (with `HOS_SAMPLE_ALL` + `HOS_SAMPLE_NS`) into the frame's breakdown, the stages, the long jobs and a per-frame gantt.

**The frame** (with the marks; standing 14.4 ms / walking 16.3 ms):
- main thread alone 2.1 / 3.0 ms: ~90% the engine's submit (capture copy, collect_in, take_capture), now the largest serial piece;
- the BaseProc calc: six stages on the six executors, driven by one module-worker job (83aff1f0); main waits for it as one ~6.5 ms wait;
- module-worker work (80083580: 2.7 ms a frame, runs across the frame boundary; the next frame's main thread waits 0.4-0.6 ms for it);
- the last executor stage (800b73d0 and three more): always 4 of 6 executors.

**Serial parts** (one job alone, medians):

| job | length | alone | why |
|---|---:|---:|---|
| Link's calc (800b3ce0 on Link) | 1.5-1.7 ms | 0.83-0.86 ms | one actor; also waits 0.37 ms for its own parallel-for 8007f6c0; starts #13-17 of ~45 (0.27-0.45 ms late) |
| 80166100 (model/physics object) | 1.4-1.7 ms | 0.1-1.6 ms (run state) | f_80168320 > f_8016b620 > f_81b47080 > f_81b56de0; FP runtime slow paths 20-26% |
| 802ca820 (0.5-1 a frame) | 1.1-1.3 ms | 0.88-0.92 ms | same subsystem; starts #119-124 of 128 |
| 8073a4a0, 803749b0 | 0.4-0.5 ms | 0.36-0.42 ms | single jobs |

Cannot be split from outside: each is one object's update. Scheduled badly: the start order (0.17-0.45 ms a frame), and the last stage's 4 executors (the two threads still in 80083580 post their executor late in f_83acfb90; all six virtual cores are held then).

**A/B** (one binary, 3 interleaved rounds; fps / 1% low / p99): per-thread object cache 1024 -> 16384 entries (`HOS_OBJ_CACHE_BITS=14`, for 80083580's ~8,000 mutexes a frame): standing 68.6 / 36.2 / 22.9 -> 67.7 / 35.5 / 22.6, walking 66.4 / 28.4 / 27.5 -> 65.2 / 27.4 / 28.4. Inside the spread; removed again.

**Adopted**: nothing; no default changed. Committed: the job marks in the trace (`hos_jt_job`, called only by a job-marked lift), `tools/bench/jobmarks.py`, `tools/bench/jobgraph.py`, the docs (engine/docs/perf-audit.md "TICK-042", docs/benchmarks/2026-11-10-tick042-job-graph.md).

**Game folder**: the Sync 10 exe and dll restored from `E:\OOT\HeyListen\build\backup-tick042`. The builder's work folder is on the default lift (the two patched files copied back, md5 checked).

**Next** (ranked): 1. the main thread's submit off the chain (asynchronous capture: 1-2 ms); 2. a scheduler patch that starts the long jobs first (0.2-0.45 ms, up to ~0.8 in some runs); 3. the model subsystem's FP slow paths (per-lane fallback in the cold `_lanes` helpers); 4. more executors/cores (game patch, high risk); 5. a cheaper first lookup of nn::os objects for 80083580 (~0.3-0.5 ms of that job).

**Tests**: run_all 179 pass, 9 fail, 23 skip; the 9 are on the known list, no new failure. **Smoke**: restored game folder, kokiri-idle 69.4 fps, saves identical.
