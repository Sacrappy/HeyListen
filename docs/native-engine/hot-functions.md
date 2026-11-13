# The lifted functions on the frame's critical path (castle-move, 2026-11-13)

What the frame's own threads run, by translated function (`f_<guest address>`), in **ms per frame**, measured on the
committed build (`build/s5/bin/rel8`, the same files as the game folder) on the main PC (Ryzen 7 7800X3D, RTX 5070), scene
`castle-move` (Castle Town Market, walking and turning), three runs. Every number below is from these three runs; where a
number is the mean of the three, the min and max are given beside it. Nothing here is extrapolated. The sampler slows the
game (below, "Distortion"): read the numbers as shares of a frame's work, not as the speed of an unsampled frame.

## The answers

**Is the work concentrated in 50-100 functions (80% of the job group's 6.9 ms)? No.** Measured three ways:

| measure | functions for 50% | for 80% | for 90% | top 100 hold |
|---|---:|---:|---:|---:|
| workers' (6 executors + 5 module workers) guest CPU time, 35.8 ms/frame, per run | 73 / 74 / 75 | 380 / 392 / 395 | 763 / 814 / 832 | 55.7-56.1% |
| executors alone (26.5 ms/frame, 3 runs pooled) | 56 | 270 | 535 | 61.7% |
| module workers alone (9.3 ms/frame, pooled) | 24 | 200 | 454 | 71.0% |
| the job-group chain's sampled work (5.8 ms/frame, pooled, ~400 samples a run) | 42 | 162 | 264 | 69.5% |

The chain row is an upper bound on concentration: with ~400 samples a run on the chain, rarely-hit functions are missing,
which raises the top-N shares. 2,136-2,284 distinct functions had guest samples on the workers in each run (3,405 over the
three). "Flat by function" is confirmed: the largest single function is 3.4% of the workers' guest time, the top 10 are
16.9%, the top 50 are 42.6%. Replacing the 100 hottest lifted functions, all of them, would touch 56% of the workers'
guest CPU and about 70% of the chain's sampled work at most.

**By job it is not flat.** The same time grouped by the job it runs under (the function called by the dispatcher
0x8004ab40 or the job-group runner 0x8007fe60): 6 jobs hold 50% of the job-group chain's sampled work, 17 jobs 80%, 28 jobs
90% (74 jobs seen on it; two of the "jobs" in those counts are pseudo-entries: stacks deeper than the sampler's 16 frames,
and the dispatcher's own time). By CPU, 17 entries (two of them those pseudo-entries) hold 80% of the workers' non-waiting time (table "Jobs" below). The strategy question
is therefore not "which 100 functions" but "which ~6-17 jobs, whole".

**What bounds the job-group wait.** The main thread's job-group wait (lr 0x80080188) was 8.09 / 7.65 / 7.62 ms a frame in
the three traces (0.3-1.2 ms more than the 6.9-7.3 ms measured without the sampler in sprint 5). Walking the chain
backwards from each wake-up (`critpath.py --anchor main`), 5.7-6.4 ms of each wait is sampled as a thread running on the
chain: executors 4.5-4.7 ms, module workers 1.0-1.4 ms, the rest under 0.15 ms each (SceneLowPriority, Presentation
Thread, NavMesh); the remainder is hand-offs, polls and core waits. The jobs on that chain, largest first:

- **0x800b3ce0, the player (Link) job**: 0.83 ms of the chain (0.59-1.18), 4.2 ms of CPU. Inside it the single largest
  function on the chain, 0x801d99a0 (0.22 ms on the chain, 1.1 ms CPU).
- **0x8006cd60, a module job recording graphics command buffers under per-object mutexes**: 0.63 ms (0.59-0.69), 2.3 ms CPU.
- **0x803749e0, an executor job (unidentified)**: 0.48 ms (0.45-0.51) of only 0.77 ms CPU: 62% of its time is on the chain
  (against ~20% for the workers' average).
- **0x819b8e20, an executor job of the numeric family N (below)**: 0.40 ms (0.32-0.55) of 0.52 ms CPU: 77% on the chain.
- **0x8191ee00, the other job of family N**: 0.37 ms (0.30-0.43) of 1.8 ms CPU.
- **0x83af59c0, the player-dependent jobs** (vector kernels 0x800a8400, 0x80ba37a0): 0.32 ms of 1.6 ms CPU.
- the dispatcher 0x8004ab40 itself 0.27 ms; 0x8016b320 (model/animation chain) 0.25 ms.

Those eight are 3.5 ms of the 5.8 ms sampled. The parallel bulk matters much less: the model/animation job 0x80166100
(2.4 ms CPU) is 0.08 ms on the chain, the actor update 0x802ca820 (1.0 ms CPU) 0.04 ms. The last stage 0x800b73d0 (4.1 ms
CPU) is never on the job-group chain: it is the frame's last wait (lr 0x83acfcf4), 0.74 ms of chain there. The module
jobs 0x800998a0 (culling / render-list) and 0x80084de0 (render recording) are on the chain mostly while the main thread
itself runs (0.55 and 0.51 ms), not during its job-group wait.

Over all worker functions, the chain holds about 20% of their CPU (5.8 + 3.2 ms of chain against 44.4 ms of non-waiting
worker time). A function well above 20% runs in the serial tail; one near 20% is parallel bulk that the chain crosses in
proportion; one well below runs off the chain. The column "on any chain" gives that ratio.

## Top 20 (workers: 6 BaseProcExecutor + 5 ModuleSystemWorker)

The first 20 rows of the full table below, repeated for reading.

| # | address | threads | guest self ms/frame (mean, min-max of 3 runs) | self + native under it | inclusive | % of workers' guest time | cum. % | on the job-group chain: ms/frame | on any chain | what it is | kernel or not | confidence |
|---:|---|---|---|---:|---:|---:|---:|---:|---|---|---|---|
| 1 | 0x8009bb80 | MW | 1.219 (1.046-1.418) | 1.226 | 1.93 | 3.4 | 3.4 | 0.058 | partly (22%) | module job 0x800998a0 body: visibility / culling and render-list build (calls the box/plane test 0x80029240 and vector kernel 0x800a8400) | logic | likely |
| 2 | 0x801d99a0 | EX | 1.115 (1.099-1.127) | 1.115 | 1.38 | 3.1 | 6.5 | 0.215 | partly (19%) | player (Link) job 0x800b3ce0: one huge straight-line FP routine (14.4k insns, ~70 sqrt calls); character movement/pose math | kernel-ish (large, 36 callees) | likely (what exactly: guess) |
| 3 | 0x800e98c0 | EX | 0.595 (0.574-0.632) | 0.604 | 2.18 | 1.7 | 8.2 | 0.005 | partly (16%) | recursive tree walk with virtual calls (actor/scene graph traversal) | logic | guess |
| 4 | 0x8004ab40 | EX | 0.582 (0.572-0.593) | 1.099 | 24.44 | 1.6 | 9.8 | 0.204 | partly (22%) | executors' job dispatcher (queue pop, run, sleep/wake) | infra (job system) | certain |
| 5 | 0x800a8400 | EX | 0.539 (0.519-0.578) | 0.539 | 0.54 | 1.5 | 11.3 | 0.131 | partly (24%) | vector/matrix kernel (1 callee: sqrt) | kernel | likely |
| 6 | 0x80084de0 | MW | 0.440 (0.386-0.480) | 1.451 | 3.11 | 1.2 | 12.5 | 0.005 | partly (14%) | module job: render command recording (NVN command buffers, handle checks); calls 0x8008fc00 | logic | likely |
| 7 | 0x814f9460 | EX | 0.420 (0.374-0.444) | 0.420 | 0.42 | 1.2 | 13.7 | 0.015 | partly (15%) | FP kernel in the last stage | kernel | likely |
| 8 | 0x80021ca0 | MW+main | 0.393 (0.342-0.443) | 0.417 | 0.43 | 1.1 | 14.8 | 0.000 | partly (19%) | module job: large engine module update (7.3k insns, 51 indirect calls), subsystem not identified | logic | guess |
| 9 | 0x81746440 | EX | 0.383 (0.336-0.447) | 0.383 | 0.38 | 1.1 | 15.9 | 0.112 | partly (29%) | small FP kernel (124 insns, no calls), family N | kernel | likely |
| 10 | 0x8173b340 | EX | 0.380 (0.314-0.431) | 0.382 | 1.42 | 1.1 | 16.9 | 0.059 | partly (15%) | FP/vector routine (800 insns, 4 callees), family N | kernel-ish | likely |
| 11 | 0x8003dac0 | EX | 0.371 (0.324-0.415) | 0.371 | 0.45 | 1.0 | 18.0 | 0.039 | partly (11%) | FP-heavy routine under model chain (13 callees, 33 indirect calls) | logic | guess |
| 12 | 0x808226e0 | MW | 0.365 (0.336-0.391) | 0.365 | 0.39 | 1.0 | 19.0 | 0.000 | no (8%) | FP leaf in the culling job (1.5k insns, no indirect calls): bounds/LOD math | kernel | likely (what: guess) |
| 13 | 0x8173a830 | EX | 0.358 (0.332-0.392) | 0.358 | 0.36 | 1.0 | 20.0 | 0.063 | partly (18%) | FP kernel, no calls, family N | kernel | likely |
| 14 | 0x81c86720 | EX | 0.348 (0.320-0.368) | 0.348 | 0.35 | 1.0 | 21.0 | 0.079 | partly (23%) | model/animation update step, model chain | logic | likely |
| 15 | 0x80ba37a0 | EX | 0.340 (0.311-0.386) | 0.340 | 0.56 | 0.9 | 21.9 | 0.059 | partly (17%) | transform/matrix work under job 0x83af59c0 (calls the 23-insn vector kernel 0x80ba6d80) | kernel-ish | likely |
| 16 | 0x81c7f880 | EX | 0.337 (0.325-0.358) | 0.337 | 0.34 | 0.9 | 22.8 | 0.010 | no (not seen) | FP kernel, no calls, no stores, under model chain | kernel | likely |
| 17 | 0x81b8c000 | EX | 0.335 (0.321-0.347) | 0.335 | 0.36 | 0.9 | 23.8 | 0.078 | partly (23%) | very large SIMD kernel (17k insns, 8.4k vector ops), under model chain: skinning / matrix palette style | kernel (large) | likely (what: guess) |
| 18 | 0x802b9900 | EX | 0.313 (0.295-0.329) | 0.313 | 0.33 | 0.9 | 24.7 | 0.000 | partly (16%) | FP-heavy per-actor routine in the last stage | logic | guess |
| 19 | 0x800c0920 | EX | 0.296 (0.259-0.329) | 0.296 | 0.82 | 0.8 | 25.5 | 0.005 | no (not seen) | executor job body (actor logic, 38 indirect calls) | logic | likely |
| 20 | 0x8008fc00 | MW | 0.281 (0.252-0.309) | 1.086 | 1.09 | 0.8 | 26.3 | 0.015 | partly (18%) | draw-command encoder helper (260 callers, 159 indirect calls, NVN handle checks) | logic | likely |

## Method

**Runs.** `build/hotfn/drive.py r1|r2|r3` (scratch, untracked) runs `tools/bench/run_bench.py --exe
E:\OOT\hookshot-fork-game\HeyListen.exe --scene castle-move --duration 110 --warmup-s 66 --fps-limit 360` with
`HOS_SAMPLE_ALL=<file>`, `HOS_SAMPLE_NS=1`, `HOS_JOBTRACE=<file>`, `HOS_JOBTRACE_AT=80`, `HOS_JOBTRACE_SECS=6`, and
creates `<samples file>.go` at 70 s and removes it at 106 s (the all-thread sampler runs only while that file exists).
No other environment: the build's defaults. One game at a time, killed by the harness at 110 s; the saves are the
harness's own (copied before, put back after, `run_bench.py`).

**Sampler.** `HOS_SAMPLE_ALL` (`engine/host/mkw_glue.c`, `sample_all_thread`): every round it suspends each of the 100
named threads, reads its context and unwinds its stack with `RtlVirtualUnwind` (16 frames at most, leaf first), with the
instant of the sample (`HOS_SAMPLE_NS`). The round period measured is 4.77 / 4.62 / 4.61 ms (the 2 ms sleep plus the
suspensions), so each sample stands for one period of that thread's time.

**Resolving to functions.** Frames are `HeyListen.exe+<rva>`. The build's `HeyListen.map` (lld-link map) lists every
translated function as its own symbol (295,270 `f_` symbols in section 0001; the `.text$hot$...` lines are only the
grouping sections, which is why the older tools reported "pieces"). A frame is resolved to the closest symbol at or below
its address (bisection over the map's code symbols), so to the `f_` function holding it, or to the runtime or OS-layer
function holding it. Frames in other modules are named by module (`ntdll.dll`, `hookshot_vk.dll`, ...).

**Attribution per sample.** A sample whose leaf is in ntdll under one of the host's sleeping waits (`hos_cv_sleep`,
`queue_get`, `event_wait`, `vc_acquire`, `hos_sleep_ns`) or in the spin `spin_until_set` is a wait and counts for no
function. The others:
- *guest self*: the innermost `f_` frame of a sample whose leaf is lifted code, or the translation's runtime helpers
  called from it (FP exactness helpers `a64_fpx_*`, `a64_vfpx_*`, `recomp_*`, lookups `a64_lookup`, `site_call`, polls);
- *self + native under it*: the innermost `f_` frame of any non-waiting sample, so also OS-layer and GPU back-end calls
  made from that function (NVN recording, invalidates, handle checks, object table, event signals);
- *inclusive*: every distinct `f_` in the sample's 16 frames. Deeper stacks are cut (58% of the workers' non-waiting
  samples reach 16 frames), so inclusive is a lower bound for outer functions, and frames removed by tail calls are not
  seen.
- *job*: the `f_` frame just inside the first dispatcher frame (0x8004ab40, 0x8007fe60, the sub-runners 0x80049280,
  0x8004fee0, 0x8004fa40, the thread loops 0x80df25c0 / 0x80df2680 / 0x834a2e50 / 0x834a3060, the worker handler
  0x8004aae0, the last stage 0x83acfb90, the stage wait 0x806afc40).
ms per frame = samples x round period / presents between the first and last sample (2,422 / 2,584 / 2,581 frames over
35.7-35.9 s).

**Critical chain.** In each run's 6 s trace window (80-86 s, ~315-321 frames), `tools/bench/critpath.py`'s `build` and
`walk` (anchor: the main thread's end-of-frame waits, lr 0x83acfcf4, i.e. the game's own chain) give the segments of
each thread's work that the main thread waited for. A sample (by its `@ns` instant) inside such a segment is "on the
chain"; it is "job-group chain" when the main thread is inside its job-group wait (lr 0x80080188) at that instant, else
"elsewhere" (the main thread's own phases, the last stage). ms per frame there = samples x (window / main thread's
samples in the window) / anchor frames. One sample is ~0.015 ms/frame in one run (0.005 in a three-run mean): values
under ~0.03 rest on one or two samples a run.

**Reading the functions.** Static shape from the local lift (`%LOCALAPPDATA%\HookshotBuilder\sa\gen`, the source of
this build): guest instruction count (`sa\image\functions.json`), direct callees, indirect calls, FP and vector helper
sites, guest loads and stores, backward branches; direct callers over the whole lift; the runtime parent and job from the
sampled stacks; earlier readings in `docs/hotmap.md`, `docs/research/suspects/A-guest-code.md`, TICK-042 and TICK-044. No
string references were found in these functions by the scan used (addresses materialised in the lifted code point into
code, not read-only data), so no identification here rests on a string. Kinds: *kernel* = self-contained numeric or
memory loop, no or one call (sqrt), no indirect calls, data in arrays: a candidate for a native replacement answered at its
address; *kernel-ish* = mostly numeric, a few calls; *logic* = many callees, indirect (virtual) calls, engine objects;
*infra* = job system, locks, wrappers whose time is in the native layers; *libc* = the SDK's C library.

Scratch scripts (untracked, `build/hotfn/`): `drive.py` (runs), `analyze.py` (samples x trace -> per-run counts),
`report.py` (pooled tables and curves), `budget.py` (thread time budget), `extract.py` (static shape), `ctx.py` (parents
and jobs), `final.py` (these tables). Exact sequence:

```
python -X utf8 -I -B build/hotfn/drive.py r1      (then r2, r3; one game at a time)
python -X utf8 -I -B build/hotfn/analyze.py r1 r2 r3
python -X utf8 -I -B build/hotfn/report.py r1 r2 r3 > build/hotfn/report.txt
python -X utf8 -I -B build/hotfn/budget.py r1 r2 r3
python -X utf8 -I -B build/hotfn/targets.py && python -X utf8 -B build/hotfn/extract.py build/hotfn/targets.txt build/hotfn/features.json
python -X utf8 -I -B build/hotfn/ctx.py && python -X utf8 -I -B build/hotfn/final.py 100
python -X utf8 -I -B tools/bench/critpath.py build/hotfn/r2.jt.txt --anchor main
```

## Distortion by the sampler

| run | fps over the whole run | frame in the sampling window | job-group wait (trace) |
|---|---:|---:|---:|
| r1 | 62.6 | 14.76 ms | 8.09 ms |
| r2 | 72.2 | 13.87 ms | 7.65 ms |
| r3 | 71.8 | 13.89 ms | 7.62 ms |

Without the sampler the build runs castle-move at ~82-85 fps (sprint 5) with a job-group wait of 6.9-7.3 ms. Suspending
100 threads every ~4.7 ms costs the frame ~2 ms. r1 had a hitch (1% low 6.8 fps) and the longest frames; its per-function
numbers are still within a few percent of r2 and r3 (the min-max columns).

## Where all of the frame's thread time goes (ms per frame, mean of 3, min-max)

Waits are included so that each group adds up to its threads' wall time (threads x frame time).

| threads | where the time goes | ms/frame (min-max) | largest items |
|---|---|---|---|
| executors (6) | TOTAL thread time | 85.035 (83.194-88.548) |  |
| executors (6) | blocked / waiting (sleep) | 55.218 (53.944-57.671) | ntdll.dll under hos_cv_sleep 33.94, ntdll.dll under queue_get 16.22, ntdll.dll under event_wait 4.84 |
| executors (6) | guest code (leaf in a lifted function) | 25.153 (24.690-25.965) |  |
| executors (6) | blocked / waiting (spin) | 1.438 (1.373-1.499) | spin_until_set 1.44 |
| executors (6) | runtime: translation (lookups, indirect calls, polls, object table) | 1.306 (1.189-1.527) | ntdll.dll under a64_icall_site 0.42, obj_lookup 0.29, obj 0.24 |
| executors (6) | runtime: FP exactness helpers | 1.106 (1.035-1.180) | a64_fpx_mul32_fma32 0.19, a64_fpx_fma32_fma32 0.18, recomp_fpe_fma64 0.11 |
| executors (6) | OS layer and other native (events, mutexes, time, handles, TLS) | 0.749 (0.734-0.765) | ntdll.dll under event_signal 0.26, ntdll.dll under hos_now_ns 0.13, hos_now_ns 0.11 |
| executors (6) | (rest: rare leaves) | 0.052 (0.048-0.057) |  |
| module workers (5) | TOTAL thread time | 70.862 (69.329-73.790) |  |
| module workers (5) | blocked / waiting (sleep) | 54.276 (53.115-56.390) | ntdll.dll under hos_cv_sleep 30.01, ntdll.dll under event_wait 18.08, hos_sleep_ns 3.50 |
| module workers (5) | guest code (leaf in a lifted function) | 8.995 (8.386-9.765) |  |
| module workers (5) | GPU back end (NVN layer, recording, invalidate, driver) | 3.551 (3.433-3.626) | ntdll.dll under rx_nvn_invalidate 1.83, ntdll.dll under n_nvnCommandBufferAddControlMemory 0.40, ucrtbase.dll under n_nvnCommandBufferEndRecording 0.22 |
| module workers (5) | runtime: translation (lookups, indirect calls, polls, object table) | 1.392 (1.274-1.607) | ntdll.dll under a64_icall_site 0.59, obj_lookup 0.57, obj 0.11 |
| module workers (5) | OS layer and other native (events, mutexes, time, handles, TLS) | 1.298 (1.261-1.336) | ntdll.dll under check_handle 0.64, check_handle 0.21, mutex_lock 0.13 |
| module workers (5) | blocked / waiting (spin) | 0.510 (0.478-0.535) | spin_until_set 0.51 |
| module workers (5) | (rest: rare leaves) | 0.339 (0.318-0.351) |  |
| module workers (5) | runtime: FP exactness helpers | 0.232 (0.223-0.238) | a64_fpx_mul32_fma32 0.06, a64_fpx_fma32_fma32 0.05, recomp_fpe_fma64 0.05 |
| module workers (5) | system, no exe frame | 0.182 (0.166-0.200) | ntdll.dll under None 0.18 |
| module workers (5) | GPU back end (hookshot_vk.dll) | 0.087 (0.081-0.093) | renderer 0.09 |
| main thread | TOTAL thread time | 14.172 (13.866-14.758) |  |
| main thread | blocked / waiting (sleep) | 9.973 (9.626-10.448) | ntdll.dll under hos_cv_sleep 6.12, ntdll.dll under event_wait 2.98, hos_sleep_ns 0.63 |
| main thread | guest code (leaf in a lifted function) | 2.096 (1.971-2.209) |  |
| main thread | GPU back end (NVN layer, recording, invalidate, driver) | 1.043 (0.907-1.124) | ntdll.dll under rx_nvn_invalidate 0.48, ucrtbase.dll under n_nvnCommandBufferEndRecording 0.17, ucrtbase.dll under inline_recording 0.14 |
| main thread | runtime: translation (lookups, indirect calls, polls, object table) | 0.476 (0.386-0.594) | ntdll.dll under a64_icall_site 0.22, obj_lookup 0.17, obj 0.04 |
| main thread | OS layer and other native (events, mutexes, time, handles, TLS) | 0.277 (0.268-0.284) | ntdll.dll under check_handle 0.10, check_handle 0.04, mutex_lock 0.03 |
| main thread | blocked / waiting (spin) | 0.110 (0.097-0.120) | spin_until_set 0.11 |
| main thread | (rest: rare leaves) | 0.063 (0.049-0.079) |  |
| main thread | GPU back end (hookshot_vk.dll) | 0.053 (0.030-0.083) | renderer 0.05 |
| main thread | runtime: FP exactness helpers | 0.047 (0.043-0.053) | recomp_fpe_fma64 0.02, a64_fpx_fma32_fma32 0.01, a64_fpx_mul32_fma32 0.01 |
| main thread | system, no exe frame | 0.035 (0.022-0.048) | ntdll.dll under None 0.03 |
| NavMesh threads (2) | TOTAL thread time | 28.345 (27.731-29.516) |  |
| NavMesh threads (2) | blocked / waiting (sleep) | 24.511 (23.978-25.464) | ntdll.dll under hos_cv_sleep 22.98, ntdll.dll under vc_acquire 1.53, vc_acquire 0.00 |
| NavMesh threads (2) | guest code (leaf in a lifted function) | 3.408 (3.275-3.624) |  |
| NavMesh threads (2) | runtime: translation (lookups, indirect calls, polls, object table) | 0.209 (0.196-0.218) | obj 0.09, hos_vcore_checkpoint 0.06, hos_thread_of 0.03 |
| NavMesh threads (2) | OS layer and other native (events, mutexes, time, handles, TLS) | 0.151 (0.144-0.159) | mutex_lock 0.03, ntdll.dll under hos_now_ns 0.03, imp__ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE 0.02 |
| NavMesh threads (2) | runtime: FP exactness helpers | 0.063 (0.063-0.064) | a64_fpx_div32_div32 0.03, a64_fpx_mul32_fma32 0.01, a64_fpx_add32_add32 0.01 |
| Presentation Thread | TOTAL thread time | 14.172 (13.866-14.758) |  |
| Presentation Thread | GPU back end (NVN layer, recording, invalidate, driver) | 5.371 (5.308-5.435) | ntdll.dll under n_nvnQueueSubmitCommands 1.70, ntdll.dll under n_nvnQueuePresentTexture 1.33, nvoglv64.dll under n_nvnQueueSubmitCommands 1.00 |
| Presentation Thread | blocked / waiting (sleep) | 3.888 (3.725-4.098) | ntdll.dll under queue_get 2.46, ntdll.dll under hos_cv_sleep 1.21, ntdll.dll under hos_sleep_ns 0.22 |
| Presentation Thread | GPU back end (hookshot_vk.dll) | 3.730 (3.568-3.988) | renderer 3.73 |
| Presentation Thread | system, no exe frame | 1.132 (1.027-1.185) | win32u.dll under None 0.94, ntdll.dll under None 0.17, ucrtbase.dll under None 0.02 |
| Presentation Thread | runtime: translation (lookups, indirect calls, polls, object table) | 0.027 (0.020-0.031) | ntdll.dll under a64_cntpct_host 0.02, a64_cntpct_host 0.00, obj 0.00 |
| Presentation Thread | OS layer and other native (events, mutexes, time, handles, TLS) | 0.024 (0.016-0.034) | ucrtbase.dll under hos_vi_native_presented 0.01, ucrtbase.dll under fprintf 0.00, hos_ticks 0.00 |

Notes: "GPU back end (NVN layer ...)" with an ntdll leaf is mostly blocked, not computing: `rx_nvn_invalidate` "can wait
for replay or GPU write-back" (`engine/host/rx_backend.c`), and the NVN recording calls take the back end's locks. The
sampler cannot tell a system call that works from one that sleeps (no ntdll symbols). The Presentation Thread runs no
lifted code of its own (its entry 0x805902c0 calls straight into the native replay): 10.3 ms/frame non-waiting, of which
3.7 ms in `hookshot_vk.dll` and 6.2 ms in the system and driver under the NVN queue calls; it is 0.06 ms on the game's
job-group chain. The six PreActorModelCompose threads slept for the whole window (no lifted code sampled).

## Workers: the top 100 functions (by guest self time, 6 executors + 5 module workers)

Columns: *threads*: EX executors, MW module workers, main (where the function also runs on the main thread, its main
time is in the main table). *guest self*: lifted code and its translation helpers. *self + native under it*: also the
OS-layer and GPU back-end calls it makes. *inclusive*: with callees, within the sampler's 16 frames. *% of workers' guest
time*: of 35.82 ms/frame. *on the job-group chain*: sampled chain time while the main thread waits for the job group.
*on any chain*: (job-group chain + chain elsewhere) / its CPU; ~20% is the workers' average (parallel bulk), 40% and
more means it runs in a serial stretch. "family N": the numeric family at 0x8173xxxx-0x81axxxxx (jobs 0x8191ee00,
0x819b8e20, and 0x817da900 under 0x803749e0): FP and vector routines, deep call chains, whose subsystem is not identified
(physics or animation blending would fit; not established). "model chain": the jobs 0x80166100 and 0x8016b320 through
0x8016b620 / 0x81b47080 / 0x81b56de0 (earlier readings: model and animation update). "last stage": job 0x800b73d0, the
frame's last stage on four executors.

| # | address | threads | guest self ms/frame (mean, min-max of 3 runs) | self + native under it | inclusive | % of workers' guest time | cum. % | on the job-group chain: ms/frame | on any chain | what it is | kernel or not | confidence |
|---:|---|---|---|---:|---:|---:|---:|---:|---|---|---|---|
| 1 | 0x8009bb80 | MW | 1.219 (1.046-1.418) | 1.226 | 1.93 | 3.4 | 3.4 | 0.058 | partly (22%) | module job 0x800998a0 body: visibility / culling and render-list build (calls the box/plane test 0x80029240 and vector kernel 0x800a8400) | logic | likely |
| 2 | 0x801d99a0 | EX | 1.115 (1.099-1.127) | 1.115 | 1.38 | 3.1 | 6.5 | 0.215 | partly (19%) | player (Link) job 0x800b3ce0: one huge straight-line FP routine (14.4k insns, ~70 sqrt calls); character movement/pose math | kernel-ish (large, 36 callees) | likely (what exactly: guess) |
| 3 | 0x800e98c0 | EX | 0.595 (0.574-0.632) | 0.604 | 2.18 | 1.7 | 8.2 | 0.005 | partly (16%) | recursive tree walk with virtual calls (actor/scene graph traversal) | logic | guess |
| 4 | 0x8004ab40 | EX | 0.582 (0.572-0.593) | 1.099 | 24.44 | 1.6 | 9.8 | 0.204 | partly (22%) | executors' job dispatcher (queue pop, run, sleep/wake) | infra (job system) | certain |
| 5 | 0x800a8400 | EX | 0.539 (0.519-0.578) | 0.539 | 0.54 | 1.5 | 11.3 | 0.131 | partly (24%) | vector/matrix kernel (1 callee: sqrt) | kernel | likely |
| 6 | 0x80084de0 | MW | 0.440 (0.386-0.480) | 1.451 | 3.11 | 1.2 | 12.5 | 0.005 | partly (14%) | module job: render command recording (NVN command buffers, handle checks); calls 0x8008fc00 | logic | likely |
| 7 | 0x814f9460 | EX | 0.420 (0.374-0.444) | 0.420 | 0.42 | 1.2 | 13.7 | 0.015 | partly (15%) | FP kernel in the last stage | kernel | likely |
| 8 | 0x80021ca0 | MW+main | 0.393 (0.342-0.443) | 0.417 | 0.43 | 1.1 | 14.8 | 0.000 | partly (19%) | module job: large engine module update (7.3k insns, 51 indirect calls), subsystem not identified | logic | guess |
| 9 | 0x81746440 | EX | 0.383 (0.336-0.447) | 0.383 | 0.38 | 1.1 | 15.9 | 0.112 | partly (29%) | small FP kernel (124 insns, no calls), family N | kernel | likely |
| 10 | 0x8173b340 | EX | 0.380 (0.314-0.431) | 0.382 | 1.42 | 1.1 | 16.9 | 0.059 | partly (15%) | FP/vector routine (800 insns, 4 callees), family N | kernel-ish | likely |
| 11 | 0x8003dac0 | EX | 0.371 (0.324-0.415) | 0.371 | 0.45 | 1.0 | 18.0 | 0.039 | partly (11%) | FP-heavy routine under model chain (13 callees, 33 indirect calls) | logic | guess |
| 12 | 0x808226e0 | MW | 0.365 (0.336-0.391) | 0.365 | 0.39 | 1.0 | 19.0 | 0.000 | no (8%) | FP leaf in the culling job (1.5k insns, no indirect calls): bounds/LOD math | kernel | likely (what: guess) |
| 13 | 0x8173a830 | EX | 0.358 (0.332-0.392) | 0.358 | 0.36 | 1.0 | 20.0 | 0.063 | partly (18%) | FP kernel, no calls, family N | kernel | likely |
| 14 | 0x81c86720 | EX | 0.348 (0.320-0.368) | 0.348 | 0.35 | 1.0 | 21.0 | 0.079 | partly (23%) | model/animation update step, model chain | logic | likely |
| 15 | 0x80ba37a0 | EX | 0.340 (0.311-0.386) | 0.340 | 0.56 | 0.9 | 21.9 | 0.059 | partly (17%) | transform/matrix work under job 0x83af59c0 (calls the 23-insn vector kernel 0x80ba6d80) | kernel-ish | likely |
| 16 | 0x81c7f880 | EX | 0.337 (0.325-0.358) | 0.337 | 0.34 | 0.9 | 22.8 | 0.010 | no (not seen) | FP kernel, no calls, no stores, under model chain | kernel | likely |
| 17 | 0x81b8c000 | EX | 0.335 (0.321-0.347) | 0.335 | 0.36 | 0.9 | 23.8 | 0.078 | partly (23%) | very large SIMD kernel (17k insns, 8.4k vector ops), under model chain: skinning / matrix palette style | kernel (large) | likely (what: guess) |
| 18 | 0x802b9900 | EX | 0.313 (0.295-0.329) | 0.313 | 0.33 | 0.9 | 24.7 | 0.000 | partly (16%) | FP-heavy per-actor routine in the last stage | logic | guess |
| 19 | 0x800c0920 | EX | 0.296 (0.259-0.329) | 0.296 | 0.82 | 0.8 | 25.5 | 0.005 | no (not seen) | executor job body (actor logic, 38 indirect calls) | logic | likely |
| 20 | 0x8008fc00 | MW | 0.281 (0.252-0.309) | 1.086 | 1.09 | 0.8 | 26.3 | 0.015 | partly (18%) | draw-command encoder helper (260 callers, 159 indirect calls, NVN handle checks) | logic | likely |
| 21 | 0x818623a0 | EX | 0.281 (0.248-0.311) | 0.281 | 0.55 | 0.8 | 27.0 | 0.049 | partly (30%) | FP/vector routine with 12 indirect calls (under 0x81a20b40) | logic | guess |
| 22 | 0x81b5a3c0 | EX | 0.268 (0.239-0.303) | 0.268 | 0.68 | 0.7 | 27.8 | 0.054 | partly (20%) | model/animation update, model chain; calls the SIMD kernel 0x81b8c000 | logic | likely |
| 23 | 0x814efd20 | EX | 0.256 (0.232-0.279) | 0.259 | 0.69 | 0.7 | 28.5 | 0.005 | partly (21%) | small FP routine + indirect call, last stage | kernel-ish | guess |
| 24 | 0x817472e0 | EX | 0.251 (0.225-0.279) | 0.251 | 0.25 | 0.7 | 29.2 | 0.058 | partly (23%) | small vector kernel, family N | kernel | likely |
| 25 | 0x81779980 | EX | 0.243 (0.213-0.261) | 0.243 | 0.28 | 0.7 | 29.9 | 0.049 | partly (20%) | vector kernel (1.4k insns, 1 callee), family N | kernel | likely |
| 26 | 0x81730760 | EX | 0.231 (0.220-0.246) | 0.231 | 1.78 | 0.6 | 30.5 | 0.044 | partly (19%) | vector routine (2k insns, 1 callee, many callers), family N | kernel-ish | likely |
| 27 | 0x814f59a0 | EX | 0.231 (0.206-0.254) | 0.231 | 0.23 | 0.6 | 31.2 | 0.000 | partly (13%) | tiny loop over an FP kernel, last stage | kernel | likely |
| 28 | 0x814f3500 | EX | 0.228 (0.209-0.259) | 0.228 | 0.92 | 0.6 | 31.8 | 0.005 | partly (11%) | small loop wrapper, last stage | kernel-ish | guess |
| 29 | 0x80ba6d80 | EX | 0.223 (0.210-0.238) | 0.223 | 0.22 | 0.6 | 32.4 | 0.068 | partly (31%) | 23-insn vector kernel | kernel (tiny) | certain |
| 30 | 0x8049dd00 | MW | 0.222 (0.211-0.238) | 0.223 | 0.23 | 0.6 | 33.1 | 0.000 | partly (15%) | FP routine in the culling job (23 callees) | logic | guess |
| 31 | 0x81a4a540 | EX | 0.215 (0.204-0.228) | 0.215 | 0.24 | 0.6 | 33.7 | 0.029 | partly (20%) | FP/vector routine (2.4k insns, 20 indirect calls) | kernel-ish | guess |
| 32 | 0x80029240 | MW | 0.212 (0.180-0.228) | 0.212 | 0.21 | 0.6 | 34.3 | 0.005 | partly (23%) | box/plane visibility test loop (42 insns) | kernel | likely |
| 33 | 0x81501180 | EX | 0.206 (0.188-0.221) | 0.206 | 0.21 | 0.6 | 34.8 | 0.019 | no (9%) | FP kernel, no calls (actor update 0x802ca820 / player job) | kernel | likely |
| 34 | 0x81b38800 | EX | 0.192 (0.161-0.226) | 0.192 | 0.19 | 0.5 | 35.4 | 0.015 | no (8%) | model/animation step, model chain | logic | likely |
| 35 | 0x801d7ac0 | EX | 0.188 (0.167-0.200) | 0.188 | 1.66 | 0.5 | 35.9 | 0.034 | partly (18%) | player job: caller of 0x801d99a0, FP-heavy | logic | likely |
| 36 | 0x814efa00 | EX | 0.186 (0.177-0.195) | 0.186 | 0.19 | 0.5 | 36.4 | 0.000 | partly (13%) | 56-insn FP kernel, last stage | kernel | likely |
| 37 | 0x80195aa0 | EX | 0.186 (0.165-0.199) | 0.186 | 0.19 | 0.5 | 36.9 | 0.000 | partly (23%) | FP routine (3 callees incl. sqrt), last stage | kernel-ish | guess |
| 38 | 0x81c7bd00 | EX | 0.174 (0.150-0.189) | 0.174 | 0.17 | 0.5 | 37.4 | 0.015 | no (8%) | FP/vector kernel, no calls, under model chain | kernel | likely |
| 39 | 0x804f5030 | EX | 0.168 (0.154-0.185) | 0.179 | 0.49 | 0.5 | 37.9 | 0.132 | yes (74%) | FP-heavy logic of job 0x803749e0 (42 callees, 48 indirect calls) | logic | likely |
| 40 | 0x800d43d0 | EX | 0.163 (0.146-0.181) | 0.165 | 1.41 | 0.5 | 38.3 | 0.024 | partly (15%) | actor update logic (101 indirect calls) | logic | likely |
| 41 | 0x8575cdc0 | EX+MW | 0.163 (0.154-0.168) | 0.163 | 0.16 | 0.5 | 38.8 | 0.020 | partly (15%) | SDK copy routine (memcpy-like: loads/stores, loop, no calls) | libc | likely |
| 42 | 0x81ec2ac0 | EX | 0.156 (0.148-0.168) | 0.156 | 0.16 | 0.4 | 39.2 | 0.005 | partly (16%) | vector routine with 16 callees (deep chains) | logic | guess |
| 43 | 0x800738e0 | MW | 0.156 (0.131-0.187) | 0.176 | 0.23 | 0.4 | 39.7 | 0.024 | yes (47%) | module job (integer, loops) | logic | guess |
| 44 | 0x81c7d2e0 | EX | 0.154 (0.141-0.171) | 0.154 | 0.15 | 0.4 | 40.1 | 0.000 | no (not seen) | model/animation step, model chain | logic | likely |
| 45 | 0x8005ed00 | MW+main | 0.154 (0.129-0.170) | 0.179 | 0.23 | 0.4 | 40.5 | 0.049 | partly (30%) | generic container/object routine (107 callers, no FP, locks) | logic | guess |
| 46 | 0x806b0e80 | EX+MW | 0.149 (0.134-0.163) | 0.149 | 0.15 | 0.4 | 40.9 | 0.054 | partly (39%) | job system: stage/worker registration (no FP, 615 loads) | infra (job system) | guess |
| 47 | 0x814ef920 | EX | 0.148 (0.126-0.166) | 0.148 | 0.15 | 0.4 | 41.4 | 0.000 | partly (13%) | 56-insn FP kernel, last stage | kernel | likely |
| 48 | 0x80168320 | EX | 0.148 (0.120-0.171) | 0.148 | 2.88 | 0.4 | 41.8 | 0.034 | partly (23%) | model/animation update, model chain | logic | likely |
| 49 | 0x800cdbe0 | EX | 0.145 (0.138-0.152) | 0.145 | 1.77 | 0.4 | 42.2 | 0.029 | partly (20%) | actor update (callees of 0x82c43780) | logic | likely |
| 50 | 0x800c2800 | EX | 0.143 (0.128-0.154) | 0.231 | 0.23 | 0.4 | 42.6 | 0.034 | partly (15%) | list/container processing with mutex lock/unlock (no FP) | logic | guess |
| 51 | 0x80162aa0 | EX | 0.142 (0.113-0.177) | 0.142 | 0.14 | 0.4 | 43.0 | 0.044 | partly (31%) | FP-heavy step of the model chain (712 FP ops, 9 callees) | kernel-ish | guess |
| 52 | 0x8008d640 | MW | 0.140 (0.129-0.148) | 0.227 | 0.68 | 0.4 | 43.4 | 0.000 | partly (26%) | render command recording helper (module job 0x80084de0) | logic | likely |
| 53 | 0x85751760 | EX | 0.138 (0.127-0.148) | 0.138 | 0.14 | 0.4 | 43.8 | 0.015 | partly (11%) | SDK libm routine (FP, 412 callers) | libc | likely |
| 54 | 0x8572f300 | EX+MW | 0.131 (0.127-0.136) | 0.131 | 0.13 | 0.4 | 44.1 | 0.005 | partly (11%) | SDK memset-like routine (stores only, 4,237 callers) | libc | likely |
| 55 | 0x81a20b40 | EX | 0.131 (0.123-0.142) | 0.132 | 0.70 | 0.4 | 44.5 | 0.024 | partly (29%) | FP routine with indirect calls (no direct calls) | kernel-ish | guess |
| 56 | 0x81fbbe40 | EX | 0.130 (0.116-0.148) | 0.130 | 0.20 | 0.4 | 44.8 | 0.000 | partly (34%) | small FP routine (1 callee) | kernel-ish | guess |
| 57 | 0x8177b2a0 | EX | 0.118 (0.114-0.120) | 0.119 | 0.23 | 0.3 | 45.2 | 0.097 | yes (81%) | vector-heavy routine of job 0x819b8e20 (24 callees) | logic | guess |
| 58 | 0x8007fe60 | MW+main | 0.115 (0.104-0.129) | 0.430 | 15.17 | 0.3 | 45.5 | 0.034 | partly (16%) | job-group runner (module workers and main thread) | infra (job system) | certain |
| 59 | 0x8018dba0 | EX | 0.115 (0.105-0.127) | 0.120 | 0.35 | 0.3 | 45.8 | 0.000 | partly (20%) | per-actor logic, last stage (82 callees) | logic | likely |
| 60 | 0x81ce6500 | EX | 0.112 (0.108-0.118) | 0.112 | 0.11 | 0.3 | 46.1 | 0.005 | no (not seen) | model/animation step, model chain | logic | likely |
| 61 | 0x81b47080 | EX | 0.112 (0.095-0.130) | 0.112 | 3.05 | 0.3 | 46.4 | 0.005 | no (not seen) | model/animation update driver, model chain | logic | likely |
| 62 | 0x81db9a40 | EX | 0.110 (0.100-0.127) | 0.110 | 0.21 | 0.3 | 46.8 | 0.010 | no (not seen) | large vector routine (13.5k insns, 2.5k vector ops, 14 callees) in the model chain | kernel-ish (large) | guess |
| 63 | 0x800a8120 | MW | 0.107 (0.073-0.125) | 0.107 | 0.11 | 0.3 | 47.1 | 0.000 | partly (18%) | small numeric kernel in the render recording job | kernel | likely |
| 64 | 0x802ca820 | EX | 0.103 (0.095-0.118) | 0.103 | 1.04 | 0.3 | 47.3 | 0.005 | no (not seen) | actor update job (53 callees) | logic | likely |
| 65 | 0x83b873c0 | EX | 0.103 (0.100-0.108) | 0.103 | 0.10 | 0.3 | 47.6 | 0.000 | partly (14%) | per-actor logic, last stage | logic | guess |
| 66 | 0x817d9d80 | EX | 0.101 (0.086-0.112) | 0.101 | 0.16 | 0.3 | 47.9 | 0.068 | yes (67%) | vector routine of job 0x819b8e20 | kernel-ish | guess |
| 67 | 0x80341840 | EX | 0.100 (0.084-0.112) | 0.100 | 0.11 | 0.3 | 48.2 | 0.034 | partly (34%) | FP-heavy player routine (2k FP ops, 16 callees) | kernel-ish | guess |
| 68 | 0x81c88940 | EX | 0.100 (0.087-0.110) | 0.100 | 0.10 | 0.3 | 48.5 | 0.005 | no (not seen) | model/animation step, model chain | logic | likely |
| 69 | 0x8014aad0 | EX | 0.097 (0.079-0.124) | 0.103 | 0.12 | 0.3 | 48.7 | 0.073 | yes (71%) | FP routine using exact 64-bit FMA helpers (5 callees, locks) | kernel-ish | guess |
| 70 | 0x81928de0 | EX | 0.094 (0.086-0.100) | 0.094 | 2.26 | 0.3 | 49.0 | 0.024 | partly (26%) | routine of job 0x8191ee00 (23 callees, many stores) | logic | guess |
| 71 | 0x81cdff40 | EX | 0.091 (0.079-0.104) | 0.091 | 0.09 | 0.3 | 49.3 | 0.005 | no (not seen) | pure SIMD kernel (1.7k insns, no calls, 714 vector ops) in the model chain | kernel | likely |
| 72 | 0x801d7040 | EX | 0.091 (0.080-0.104) | 0.091 | 0.09 | 0.3 | 49.5 | 0.000 | no (not seen) | FP routine of the player job | kernel-ish | guess |
| 73 | 0x80117780 | EX | 0.091 (0.087-0.095) | 0.091 | 0.16 | 0.3 | 49.8 | 0.039 | yes (43%) | FP logic with 24 indirect calls | logic | guess |
| 74 | 0x8023c820 | EX | 0.090 (0.079-0.096) | 0.094 | 0.10 | 0.3 | 50.0 | 0.000 | partly (21%) | FP per-actor routine, last stage | kernel-ish | guess |
| 75 | 0x81a33440 | EX | 0.088 (0.079-0.104) | 0.088 | 0.10 | 0.2 | 50.3 | 0.068 | yes (77%) | integer loop, no calls, many stores (copy/convert) | kernel | guess |
| 76 | 0x8173ab70 | EX | 0.088 (0.068-0.108) | 0.088 | 0.09 | 0.2 | 50.5 | 0.019 | partly (22%) | small FP/vector kernel, family N | kernel | likely |
| 77 | 0x819783e0 | EX | 0.087 (0.083-0.089) | 0.087 | 1.77 | 0.2 | 50.7 | 0.015 | partly (17%) | vector routine, family N | logic | guess |
| 78 | 0x800c34e0 | EX | 0.087 (0.083-0.091) | 0.087 | 0.15 | 0.2 | 51.0 | 0.025 | partly (28%) | executor job body (37 callees) | logic | likely |
| 79 | 0x81a2bba0 | EX | 0.086 (0.075-0.102) | 0.086 | 0.09 | 0.2 | 51.2 | 0.015 | partly (17%) | integer loop, no calls (search/scan) | kernel | guess |
| 80 | 0x800493e0 | EX | 0.083 (0.075-0.089) | 0.213 | 0.23 | 0.2 | 51.5 | 0.098 | yes (46%) | executor job run by the dispatcher's sub-runner (actor-related, object lookups) | logic | guess |
| 81 | 0x81864820 | EX | 0.083 (0.075-0.088) | 0.083 | 0.37 | 0.2 | 51.7 | 0.000 | no (not seen) | vector routine | kernel-ish | guess |
| 82 | 0x80251760 | EX | 0.082 (0.069-0.089) | 0.082 | 0.09 | 0.2 | 51.9 | 0.005 | no (not seen) | list iteration with virtual calls (no FP) | logic | guess |
| 83 | 0x8014ba80 | EX | 0.082 (0.080-0.083) | 0.116 | 0.12 | 0.2 | 52.1 | 0.000 | partly (13%) | integer logic with locks (30 callees) | logic | guess |
| 84 | 0x8176c980 | EX | 0.080 (0.064-0.090) | 0.081 | 1.87 | 0.2 | 52.4 | 0.015 | partly (18%) | vector routine (475 vector ops, 8 callees), family N | kernel-ish | guess |
| 85 | 0x81500200 | MW+main | 0.080 (0.066-0.100) | 0.084 | 0.08 | 0.2 | 52.6 | 0.000 | partly (17%) | small vector routine in the culling job | kernel | likely |
| 86 | 0x81b3a558 | EX | 0.080 (0.073-0.089) | 0.080 | 0.15 | 0.2 | 52.8 | 0.005 | no (not seen) | small FP routine in the model chain | kernel | likely |
| 87 | 0x817036a0 | EX | 0.080 (0.075-0.082) | 0.080 | 0.08 | 0.2 | 53.0 | 0.010 | no (not seen) | small FP/vector kernel, job 0x8191ee00 | kernel | likely |
| 88 | 0x806eefc0 | MW+main | 0.078 (0.068-0.090) | 0.078 | 0.10 | 0.2 | 53.3 | 0.010 | no (not seen) | FP-heavy module routine (1k FP ops, 25 callees) of job 0x80435300 | logic | guess |
| 89 | 0x85751000 | EX+MW+main | 0.078 (0.064-0.087) | 0.080 | 0.08 | 0.2 | 53.5 | 0.010 | no (not seen) | SDK libm routine | libc | likely |
| 90 | 0x81260600 | MW | 0.075 (0.066-0.087) | 0.075 | 0.11 | 0.2 | 53.7 | 0.000 | partly (26%) | vector routine in the culling job | kernel-ish | guess |
| 91 | 0x8030a320 | EX | 0.075 (0.068-0.089) | 0.075 | 0.08 | 0.2 | 53.9 | 0.005 | no (not seen) | FP routine of the player job (indirect calls only) | kernel-ish | guess |
| 92 | 0x807a9600 | MW | 0.075 (0.072-0.081) | 0.079 | 0.10 | 0.2 | 54.1 | 0.000 | no (not seen) | FP-heavy module routine (28 callees) | logic | guess |
| 93 | 0x806f5860 | MW+main | 0.072 (0.043-0.100) | 0.072 | 0.16 | 0.2 | 54.3 | 0.000 | no (not seen) | FP-heavy module routine (1.6k FP ops, 29 callees, libm calls) of job 0x80435300 | logic | guess |
| 94 | 0x800f0670 | EX | 0.072 (0.063-0.086) | 0.072 | 0.26 | 0.2 | 54.5 | 0.005 | no (not seen) | small integer loop with one indirect call (deep in actor chains) | kernel-ish | guess |
| 95 | 0x81a55380 | EX | 0.072 (0.041-0.098) | 0.072 | 0.20 | 0.2 | 54.7 | 0.000 | no (not seen) | vector routine (2 callees, deep chains) | kernel-ish | guess |
| 96 | 0x8035a8c0 | MW | 0.071 (0.057-0.079) | 0.071 | 0.10 | 0.2 | 54.9 | 0.010 | yes (48%) | FP routine with static-init guards (module job 0x800766e0) | logic | guess |
| 97 | 0x82c3d740 | EX | 0.070 (0.061-0.081) | 0.143 | 4.77 | 0.2 | 55.1 | 0.000 | partly (31%) | per-actor job of the last stage (92 callees) | logic | likely |
| 98 | 0x81a040bc | EX | 0.070 (0.064-0.075) | 0.070 | 0.09 | 0.2 | 55.3 | 0.029 | yes (42%) | small FP/vector routine, family N | kernel | likely |
| 99 | 0x801881a0 | EX | 0.069 (0.061-0.085) | 0.079 | 0.09 | 0.2 | 55.5 | 0.015 | partly (19%) | FP routine of the player job (19 callees) | logic | guess |
| 100 | 0x800998a0 | MW+main | 0.069 (0.059-0.077) | 0.417 | 3.61 | 0.2 | 55.7 | 0.010 | partly (15%) | module job root of the culling / render-list job | logic | likely |

Kinds in this top 100 (CPU = guest self, ms/frame): kernel 25 functions, 5.38 ms CPU, 0.78 ms on the job-group chain;
kernel-ish 23, 4.40 ms, 0.69 ms; logic 45, 8.81 ms, 1.05 ms; infra 3, 0.85 ms, 0.29 ms; libc 4, 0.51 ms, 0.05 ms. The 100
are 19.9 of the 35.8 ms of worker guest time and 2.9 of the 5.8 ms sampled on the job-group chain. The self-contained
kernels of the top 100 together are 15% of the workers' guest time and 13% of the chain.

## The chain itself: the 30 functions most on the job-group chain

| address | on the job-group chain ms/frame (min-max) | % of the chain's sampled work | its CPU ms/frame (all threads) | job | what it is | kernel or not |
|---|---|---:|---:|---|---|---|
| 0x801d99a0 | 0.215 (0.148-0.338) | 3.7 | 1.115 | 0x800b3ce0 | player (Link) job 0x800b3ce0: one huge straight-line FP routine (14.4k insns, ~70 sqrt calls); character movement/pose math | kernel-ish (large, 36 callees) |
| 0x8004ab40 | 0.204 (0.132-0.273) | 3.5 | 1.099 | 0x800b3ce0 | executors' job dispatcher (queue pop, run, sleep/wake) | infra (job system) |
| 0x804f5030 | 0.132 (0.130-0.133) | 2.3 | 0.179 | 0x803749e0 | FP-heavy logic of job 0x803749e0 (42 callees, 48 indirect calls) | logic |
| 0x800a8400 | 0.131 (0.104-0.158) | 2.3 | 0.539 | 0x83af59c0 | vector/matrix kernel (1 callee: sqrt) | kernel |
| 0x806688c0 | 0.113 (0.058-0.148) | 1.9 | 0.140 | 0x8006cd60 | graphics recording: command-list call into NVN (job 0x8006cd60) | infra (GPU front end) |
| 0x81746440 | 0.112 (0.089-0.130) | 1.9 | 0.383 | (deep) | small FP kernel (124 insns, no calls), family N | kernel |
| 0x800493e0 | 0.098 (0.059-0.133) | 1.7 | 0.213 | 0x800493e0 | executor job run by the dispatcher's sub-runner (actor-related, object lookups) | logic |
| 0x8177b2a0 | 0.097 (0.044-0.173) | 1.7 | 0.119 | 0x819b8e20 | vector-heavy routine of job 0x819b8e20 (24 callees) | logic |
| 0x81c86720 | 0.079 (0.029-0.119) | 1.4 | 0.348 | 0x80166100 | model/animation update step, model chain | logic |
| 0x81b8c000 | 0.078 (0.029-0.104) | 1.4 | 0.335 | 0x80166100 | very large SIMD kernel (17k insns, 8.4k vector ops), under model chain: skinning / matrix palette style | kernel (large) |
| 0x8014aad0 | 0.073 (0.072-0.074) | 1.3 | 0.103 | 0x803749e0 | FP routine using exact 64-bit FMA helpers (5 callees, locks) | kernel-ish |
| 0x80ba6d80 | 0.068 (0.029-0.089) | 1.2 | 0.223 | 0x83af59c0 | 23-insn vector kernel | kernel (tiny) |
| 0x817d9d80 | 0.068 (0.044-0.101) | 1.2 | 0.101 | 0x819b8e20 | vector routine of job 0x819b8e20 | kernel-ish |
| 0x81a33440 | 0.068 (0.015-0.130) | 1.2 | 0.088 | 0x81a191a0 | integer loop, no calls, many stores (copy/convert) | kernel |
| 0x8173a830 | 0.063 (0.058-0.073) | 1.1 | 0.358 | (deep) | FP kernel, no calls, family N | kernel |
| 0x8006cd60 | 0.063 (0.015-0.088) | 1.1 | 0.209 | 0x8006cd60 | module job root: graphics recording job with many mutexes (0x80364080, 0x806688c0) | logic |
| 0x80ba37a0 | 0.059 (0.058-0.059) | 1.0 | 0.340 | 0x83af59c0 | transform/matrix work under job 0x83af59c0 (calls the 23-insn vector kernel 0x80ba6d80) | kernel-ish |
| 0x8173b340 | 0.059 (0.044-0.073) | 1.0 | 0.382 | (deep) | FP/vector routine (800 insns, 4 callees), family N | kernel-ish |
| 0x8009bb80 | 0.058 (0.044-0.072) | 1.0 | 1.226 | 0x800998a0 | module job 0x800998a0 body: visibility / culling and render-list build (calls the box/plane test 0x80029240 and vector kernel 0x800a8400) | logic |
| 0x817472e0 | 0.058 (0.044-0.072) | 1.0 | 0.251 | (deep) | small vector kernel, family N | kernel |
| 0x80354060 | 0.054 (0.029-0.088) | 0.9 | 0.530 | 0x8007c120 | GPU memory invalidate wrapper (module job 0x8007c120); time is the native back end's wait | infra (GPU back end) |
| 0x8037adc0 | 0.054 (0.043-0.074) | 0.9 | 0.073 | 0x803749e0 | job 0x803749e0's driver (locks, 15 callees) | logic |
| 0x806b0e80 | 0.054 (0.015-0.118) | 0.9 | 0.149 | 0x806b0540 | job system: stage/worker registration (no FP, 615 loads) | infra (job system) |
| 0x81b5a3c0 | 0.054 (0.030-0.088) | 0.9 | 0.268 | 0x80166100 | model/animation update, model chain; calls the SIMD kernel 0x81b8c000 | logic |
| 0x817da900 | 0.053 (0.030-0.086) | 0.9 | 0.054 | 0x803749e0 | FP/vector routine of job 0x803749e0 (2 callees) | kernel-ish |
| 0x818623a0 | 0.049 (0.014-0.073) | 0.8 | 0.281 | 0x8042b0c0 | FP/vector routine with 12 indirect calls (under 0x81a20b40) | logic |
| 0x81779980 | 0.049 (0.029-0.089) | 0.8 | 0.243 | 0x8191ee00 | vector kernel (1.4k insns, 1 callee), family N | kernel |
| 0x8005ed00 | 0.049 (0.014-0.103) | 0.8 | 0.179 | 0x8006cd60 | generic container/object routine (107 callers, no FP, locks) | logic |
| 0x802737a0 | 0.048 (0.015-0.086) | 0.8 | 0.057 | 0x8006cd60 | lock-protected object routine (160 callers, no FP) in graphics job 0x8006cd60 | logic |
| 0x80547330 | 0.044 (0.014-0.059) | 0.8 | 0.049 | 0x80547330 | executor job run by the dispatcher's sub-runner (5 callees, no FP) | logic |

## Jobs (the frame under the dispatcher or the job-group runner), workers

*CPU*: non-waiting time of the workers under that job (inclusive of native calls). The chain columns as above. "(deeper
than 16 frames)": the stack did not reach a dispatcher frame within the sampler's 16 frames.

| job (frame under the dispatcher / job-group runner) | CPU ms/frame (min-max) | cum. % | on the job-group chain ms/frame (min-max) | on the chain elsewhere (last stage, module phase) | share of its CPU on a chain | what |
|---|---|---:|---|---:|---:|---|
| 0x800b3ce0 | 4.215 (4.097-4.422) | 10 | 0.831 (0.590-1.175) | 0.000 | 20% | player (Link) job |
| 0x800b73d0 | 4.091 (4.023-4.139) | 19 | 0.000 (0.000-0.000) | 0.742 | 18% | last stage (4 executors) |
| (deeper than 16 frames) | 3.990 (3.779-4.196) | 28 | 0.288 (0.252-0.338) | 0.302 | 15% |  |
| 0x800998a0 | 3.614 (3.284-4.127) | 36 | 0.073 (0.044-0.088) | 0.550 | 17% | module: culling / render-list build |
| 0x80084de0 | 3.108 (3.011-3.209) | 43 | 0.005 (0.000-0.015) | 0.511 | 17% | module: render command recording |
| 0x80166100 | 2.379 (2.150-2.750) | 49 | 0.083 (0.030-0.162) | 0.000 | 3% | model/animation chain |
| 0x8006cd60 | 2.254 (2.131-2.394) | 54 | 0.629 (0.593-0.691) | 0.034 | 29% | module: graphics recording with per-object mutexes |
| 0x8191ee00 | 1.772 (1.690-1.900) | 58 | 0.365 (0.296-0.432) | 0.000 | 21% | numeric family job (unidentified) |
| (dispatcher itself) | 1.742 (1.701-1.782) | 62 | 0.273 (0.176-0.345) | 0.068 | 20% |  |
| 0x83af59c0 | 1.620 (1.544-1.668) | 65 | 0.321 (0.235-0.417) | 0.000 | 20% | player-dependent jobs (vector kernels 0x800a8400, 0x80ba37a0) |
| 0x8007c120 | 1.561 (1.422-1.767) | 69 | 0.083 (0.043-0.132) | 0.171 | 16% | module: GPU memory invalidate/flush |
| 0x8016b320 | 1.046 (1.005-1.087) | 71 | 0.245 (0.130-0.311) | 0.000 | 23% | model/animation chain (second root) |
| 0x802ca820 | 1.038 (0.991-1.070) | 74 | 0.044 (0.029-0.072) | 0.000 | 4% | actor update |
| 0x80435300 | 0.832 (0.782-0.866) | 75 | 0.044 (0.030-0.058) | 0.083 | 15% | module job (unidentified) |
| 0x800c0920 | 0.823 (0.721-0.940) | 77 | 0.029 (0.000-0.044) | 0.000 | 4% | actor logic job |
| 0x803749e0 | 0.767 (0.703-0.854) | 79 | 0.478 (0.445-0.514) | 0.000 | 62% | executor job (unidentified; FP logic with exact 64-bit FMA) |
| 0x82c3d740 | 0.661 (0.612-0.734) | 81 | 0.000 (0.000-0.000) | 0.205 | 31% | per-actor job of the last stage |
| 0x819b8e20 | 0.515 (0.513-0.516) | 82 | 0.399 (0.323-0.547) | 0.000 | 77% | numeric family job (unidentified) |
| 0x82c43780 | 0.471 (0.445-0.492) | 83 | 0.102 (0.044-0.173) | 0.000 | 22% | actor update |
| 0x8042b0c0 | 0.436 (0.357-0.478) | 84 | 0.049 (0.000-0.103) | 0.000 | 11% | executor job (unidentified) |
| (no dispatcher) | 0.395 (0.363-0.421) | 85 | 0.039 (0.014-0.059) | 0.025 | 16% |  |
| 0x80021ca0 | 0.366 (0.326-0.425) | 85 | 0.000 (0.000-0.000) | 0.063 | 17% | module job (unidentified) |
| 0x8007d2e0 | 0.303 (0.280-0.315) | 86 | 0.000 (0.000-0.000) | 0.005 | 2% |  |
| 0x8073a4a0 | 0.292 (0.259-0.311) | 87 | 0.000 (0.000-0.000) | 0.000 | 0% |  |

## Main thread (sead::MainThread)

4.09 ms/frame non-waiting (2.10 of it lifted code). 3.57 ms of it (inclusive) is inside the job-group runner 0x8007fe60:
the main thread runs module jobs itself before and while it waits. Its own work on the chain: 1.31-1.44 ms/frame
(critpath "work main"). It runs the module workers' functions; the top 12 by guest self time:

| address | guest self ms/frame (min-max) | self + native | inclusive | % of main's guest time | on the chain (main's own work) | what it is | kernel or not | confidence |
|---|---|---:|---:|---:|---:|---|---|---|
| 0x8009bb80 | 0.236 (0.215-0.262) | 0.236 | 0.37 | 10.8 | 0.068 | module job 0x800998a0 body: visibility / culling and render-list build (calls the box/plane test 0x80029240 and vector kernel 0x800a8400) | logic | likely |
| 0x80021ca0 | 0.107 (0.102-0.112) | 0.113 | 0.12 | 4.9 | 0.020 | module job: large engine module update (7.3k insns, 51 indirect calls), subsystem not identified | logic | guess |
| 0x80084de0 | 0.078 (0.064-0.087) | 0.221 | 0.49 | 3.6 | 0.034 | module job: render command recording (NVN command buffers, handle checks); calls 0x8008fc00 | logic | likely |
| 0x808226e0 | 0.066 (0.059-0.072) | 0.066 | 0.07 | 3.0 | 0.005 | FP leaf in the culling job (1.5k insns, no indirect calls): bounds/LOD math | kernel | likely (what: guess) |
| 0x8005ed00 | 0.051 (0.041-0.057) | 0.059 | 0.08 | 2.3 | 0.073 | generic container/object routine (107 callers, no FP, locks) | logic | guess |
| 0x8008fc00 | 0.051 (0.047-0.057) | 0.177 | 0.18 | 2.3 | 0.044 | draw-command encoder helper (260 callers, 159 indirect calls, NVN handle checks) | logic | likely |
| 0x80029240 | 0.041 (0.026-0.048) | 0.041 | 0.04 | 1.9 | 0.010 | box/plane visibility test loop (42 insns) | kernel | likely |
| 0x800738e0 | 0.033 (0.027-0.039) | 0.037 | 0.06 | 1.5 | 0.015 | module job (integer, loops) | logic | guess |
| 0x82bde830 | 0.027 (0.023-0.033) | 0.028 | 0.03 | 1.3 | 0.000 | module job with locks (21 callees) | logic | guess |
| 0x802737a0 | 0.024 (0.021-0.026) | 0.025 | 0.03 | 1.1 | 0.015 | lock-protected object routine (160 callers, no FP) in graphics job 0x8006cd60 | logic | guess |
| 0x81500200 | 0.024 (0.016-0.034) | 0.024 | 0.02 | 1.1 | 0.005 | small vector routine in the culling job | kernel | likely |
| 0x8049dd00 | 0.023 (0.018-0.028) | 0.023 | 0.02 | 1.1 | 0.010 | FP routine in the culling job (23 callees) | logic | guess |

Main-only functions further down: 0x802eb020 (recursive scene/task walk under 0x8348c680, 0.11 ms with native recording
under it) and 0x807cd380 (a command-list call into NVN, 0.10 ms, nearly all native).

## Other threads on the chain

**NavMeshCAStepDepWorker1** is on the chain outside the job-group wait (0.54-0.62 ms/frame of chain work, critpath) and
almost never inside it (0.02 ms). Its lifted code is 3.4 ms/frame (two NavMesh threads 3.8 ms non-waiting):

| address | guest self ms/frame (min-max) | inclusive | % of the thread's guest time | on the chain | what it is | kernel or not | confidence |
|---|---|---:|---:|---:|---|---|---|
| 0x820e2520 | 1.302 (1.216-1.401) | 1.30 | 36.9 | 0.234 | navmesh crowd step kernel (NavMeshCAStepDepWorker1) | kernel | likely |
| 0x81e69500 | 0.528 (0.487-0.556) | 1.27 | 15.0 | 0.092 | navmesh crowd step (12 callees) | kernel-ish | likely |
| 0x80043e50 | 0.337 (0.304-0.386) | 0.50 | 9.6 | 0.098 | sync wrapper on the navmesh thread (condition variable / mutex / thread priority) | infra (locks) | likely |
| 0x820e1d60 | 0.336 (0.306-0.358) | 0.34 | 9.5 | 0.020 | navmesh FP/vector kernel, no calls | kernel | likely |
| 0x81fbb000 | 0.180 (0.146-0.205) | 0.24 | 5.1 | 0.059 | small FP routine (navmesh and SceneLowPriority threads) | kernel-ish | guess |
| 0x800453f0 | 0.149 (0.145-0.153) | 0.28 | 4.2 | 0.044 | sync wrapper on the navmesh thread | infra (locks) | likely |

**SceneLowPriority** (two threads): 0.36 ms/frame non-waiting, 0.09 ms on the job-group chain, spread thinly (0x81fbb000
0.07 ms, nothing else above 0.02).

**Presentation Thread**: no lifted code of its own (above). 0.05-0.26 ms/frame of chain work in the traces.

## What is not established

- **Unsampled speed.** All ms are under the sampler, whose frame is ~2 ms longer than an unsampled one (above). Shares and
  rankings are the result; absolute ms would be somewhat lower without it.
- **What the unidentified jobs are**: 0x803749e0, 0x819b8e20, 0x8191ee00, 0x8042b0c0, 0x80435300, 0x80021ca0 and family N
  as a whole. They are named here by shape and call context only. "Physics (Havok)" is not shown for any function: no
  phive/Havok thread was sampled in this scene and no string tied any function to it.
- **Classification confidence**: "certain" only where the role is read from the job system's documented entry points or
  from the native call under it; "likely" from shape plus call context plus earlier readings; "guess" is shape alone.
- **Inclusive time of outer functions** is cut at 16 frames (58% of the workers' non-waiting samples hit the limit) and by tail calls.
- **Chain attribution** rests on ~380-430 samples a run inside the job-group chain; per-function chain values under
  ~0.03 ms/frame are one or two samples a run. The chain walk itself has the limits of `critpath.py` (polls whose setter
  is not traced stay on the waiting thread; 0.34 ms/frame in r2).
- **One scene, one machine.** castle-move only; kokiri-idle and hyrule-walk were not profiled in this session.
- **ntdll time** under the GPU back end and the OS layer is not split into running and blocked.
