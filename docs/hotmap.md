# Hot-function map: Castle Town Market (TICK-026, 2026-11-09)

Source: one all-thread profile (`HOS_PROFILE=1 HOS_SAMPLE_ALL`, sampling about 75-95 s after launch, 4,858 instants of
2 ms) of `kokiri-idle` at the most recent save (Castle Town Market). The build is the TICK-026 build with every
replacement off, so its code is the guest's. Ryzen 7 7800X3D and RTX 5070. Made with
`RX_MAPS=<that build's maps> python -I engine/scripts/hotmap.py <file>`. Hyrule Field was not profiled: no save there
is the most recent, and that cannot change.

**The chain** (hotmap.py) is the main thread's own running samples (1,151) plus every game thread's running samples
while the main thread waits in the job group's wait f_8007fe60 (14,603), which is 15,754 samples. OptimizeChangeBanc
is kept out: it runs all the time and is not waited on (TICK-025). Shares are of those 15,754 samples. "self" counts
the innermost guest function, including the runtime helpers it calls (FP helpers, memory marks). This chain is an
upper bound: a thread that runs while main waits is not proven to be what main waits for. ParallelDecompThread (asset
decompression) is the clearest case, because it runs while main waits but is probably not a dependency of the frame.

**Kind**: *self-contained* means few or no calls and compute over memory (math, search, copy, decompress), which
makes it a candidate for a native version. *Entangled* means many direct or indirect calls, atomics, or engine
objects. The counts are from the lifted C: lines, direct calls, indirect calls, FP/vector lines and atomics.
**System** is a best guess from the call tree and threads. It was not checked against the game's strings.

## Top 15 by self time

| # | function | self | incl | threads | callers -> callees | lifted shape | kind | system (best effort) |
|---|---|---:|---:|---|---|---|---|---|
| 1 | f_801102e0 | 13.5% | 13.5% | ParallelDecompThread | f_80116490 -> none | 390 lines, 1 call, integer only | self-contained | resource streaming: decompression inner loop (f_80115bf0 -> f_80116490); probably off the frame's path |
| 2 | f_8009bb80 | 3.9% | 6.2% | ModuleSystemWorker, main | f_800998a0 -> f_80029240, f_81500200, f_800a8400 | 15,900 lines, 283 calls, 182 indirect, 79 atomics | entangled | module system job body (run from the job group f_8007fe60) |
| 3 | f_805902c0 | 3.2% | 3.9% | Presentation Thread | f_80df2680 -> f_80527e90 | 188 lines, 20 calls, 9 indirect | entangled | graphics: frame present / command recording |
| 4 | f_8004ab40 | 2.8% | 46.7% | BaseProcExecutor0-5 | f_8004aae0 -> f_800b3ce0, f_802ca820, f_83af59c0 | 5,860 lines, 151 calls, 244 atomics | entangled | proc executor job dispatcher (the root of the executors' work) |
| 5 | f_800a8400 | 2.5% | 2.5% | BaseProcExecutor | f_831e65d0, f_8009bb80 -> none | 294 lines, 1 call, 213 FP | self-contained | math (vector/matrix kernel) |
| 6 | f_81c7f880 | 1.9% | 1.9% | BaseProcExecutor | f_81c7ecc0 -> none | 265 lines, no calls, 211 FP, no stores | self-contained | math, under the model/animation chain (f_81b56de0) |
| 7 | f_806668b0 | 1.7% | 1.7% | main | f_80665980 -> none | 314 lines, 19 calls | entangled | graphics: capture / GPU wait (TICK-025) |
| 8 | f_81b5a3c0 | 1.7% | 3.3% | BaseProcExecutor | f_81b57a80 -> f_81b8c000 | 6,470 lines, 73 calls, 2,950 FP | entangled | model/animation update |
| 9 | f_81501180 | 1.7% | 1.7% | BaseProcExecutor | f_802ca820, f_802cf7f0 -> none | 179 lines, no calls, 66 FP | self-contained | math (actor update) |
| 10 | f_80ba6d80 | 1.6% | 1.6% | BaseProcExecutor | f_80ba37a0 -> none | 25 lines, no calls, FP | self-contained (tiny: the call overhead is a large part) | math |
| 11 | f_80021ca0 | 1.6% | 1.6% | ModuleSystemWorker, main | f_8007fe60, f_83b74d30 -> | 9,850 lines, 161 calls | entangled | module system |
| 12 | f_81b8c000 | 1.5% | 1.5% | BaseProcExecutor | f_81b5a3c0 -> none | 20,600 lines, 1 call, 19,400 FP | self-contained (large) | model/animation: FP kernel (skinning / matrix palette style) |
| 13 | f_81c86720 | 1.4% | 1.4% | BaseProcExecutor | f_81b56de0 -> | 459 lines, 20 calls, 4 atomics | entangled | model/animation |
| 14 | f_820e2520 | 1.2% | 1.2% | NavMeshCAStepDepWorker | f_820e2350 -> none | 637 lines, 1 call, 469 FP | self-contained | navigation mesh (crowd/agent step) |
| 15 | f_80110860 | 1.1% | 1.1% | ParallelDecompThread | f_80116490 -> none | 1,119 lines, no calls, integer | self-contained | resource streaming: decompression |

## By inclusive time (the call tree)

| function | incl | what |
|---|---:|---|
| f_80df2680 / f_80df25c0 / f_834a2e50 | 80 / 57 / 69% | job threads' loop (message -> job) |
| f_8004ab40 (f_8004aae0) | 46.7% | proc executor dispatcher: actors, models, physics |
| f_8007fe60 (f_8007fe10) | 22.3% | job group run/wait (main thread and module workers) |
| f_80115bf0 -> f_80116490 | 15.9% | ParallelDecompThread decompression (f_801102e0, f_80110860, f_801117a0) |
| f_8016b620 / f_81b47080 / f_80168320 | 13.3 / 11.4 / 11.2% | model/animation update chain -> f_81b56de0 (8.8%) -> f_81c7f880, f_81c86720 |
| f_800998a0 -> f_8009bb80 | 8.6 / 6.2% | module system job |
| f_800b3ce0 -> f_82c3fab0 -> f_82c415e0 | 8.4 / 8.4 / 7.5% | player/actor update (f_82c415e0 is the player movement function patched for flight) |
| f_82c43780 -> f_800cdbe0 -> f_800d43d0 | 6.9 / 6.9% | actor update, its callees |
| f_802eacc0 | 6.6% | main thread's recursive walk (scene/task tree) |
| f_802ca820 | 6.4% | actor update -> f_81501180 |

## Off the chain: OptimizeChangeBanc

f_805aeea0 accounts for 84% of OptimizeChangeBanc's running samples (1 thread, always running, TICK-025). Its binary
search with an inlined strcmp (guest 0x805afbc0-0x805afda4) was the first replacement (TICK-026). That thread is not on
the frame's path, so the replacement is not expected to move fps (see perf-audit.md, TICK-026).

## Reading

- No single guest function carries the chain. The largest self time on the frame's own work is a few percent: the
  module system (f_8009bb80 3.9%) and the executors' dispatcher (2.8%). After that comes a long tail of FP kernels at
  1-2.5% each.
- Self-contained candidates for TICK-027+, ranked: f_800a8400, f_81c7f880, f_81501180, f_80ba6d80, f_81b8c000,
  f_820e2520 (FP math, no or one call, about 10% together). Before any of them gets a native version, check that its
  FP is bit-exact (the guest's FPCR handling and the lifter's FP helpers, see TICK-022). The decompressor f_801102e0
  (13.5%) is the largest self-contained function, but first confirm that the frame waits on it.
- Entangled roots (f_8004ab40, f_8009bb80, f_80021ca0) are not replacement targets. Their gains come from the lifter
  (TICK-011/022).
