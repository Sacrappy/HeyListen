# TICK-052 — Why parallel replay draws cost more, then cut the replay's per-draw cost  · model: Opus · after TICK-050
**Why**: TICK-050 found that GPU frame prep (`PROF_SUBMIT`, the Presentation Thread's `run_submit` wall) is ~7.8 ms a frame in the
Market, and that re-planning the segments saves only ~0.4 ms: the slowest segment stays ~1.25 ms whatever the count. A draw costs
~3.6 us on one thread but ~5 us when 5 threads record at once (5 recorders sum to ~5 ms a command buffer against ~3 ms serial), and
a command buffer's first draws cost ~3x its last ones. Candidates: driver locks, `pools_mu`, `ParLock`, the heap, false sharing.
**Goal**:
1. find the contention with the all-thread sampler (`HOS_PROFILE` + `HOS_SAMPLE_ALL`, `vkprof.py`) on the replay workers and the
   Presentation Thread, with the dll's map; no `RX_VK_PASS_TIMING`, dumps or probes (they make the replay serial);
2. fix the top causes host-side, behind `RX_VK_*` switches, keeping the same draws and picture;
3. A/B with the short policy: screen castle-move (3 interleaved runs a variant); confirm only what is adopted (kokiri-idle,
   castle-move, hyrule-walk, 6 runs, order reversed in rounds 4-6); 110 s runs, 66 s cut, `--fps-limit 360`, frame generation off;
4. correctness: shots standing and walking, `HOS_SPEED_LOG` ~60, unit tests, `run_all` (only the 4 nup tests may fail);
5. docs: perf-audit "TICK-052", `docs/benchmarks/2026-11-11-tick052-replay-contention.md`, this result, a SPRINT-4 entry.
**Acceptance**: frame prep clearly lower with no fps loss and the same picture (frame prep is the ceiling at 120 fps).
**Branch**: `feature/TICK-052-replay-contention` (from main b17b933). Local only.

## Result (2026-11-11): contention found and cut behind switches; frame prep not clearly lower, not adopted
**Why a parallel draw costs more** (sampler + `RX_VK_PARLOCK_STATS`, castle-move): ~10-14% of the workers' running time waited
for ParLock (~1,050 takings a command buffer for ~0.1-0.6 us holds, each contended one sleeping ~9 us). The takings came from caches
that went stale for the wrong reasons: the workers' pool table aliased pages 64 MB apart (~340 a command buffer), and every
texture eviction made every thread's views and texture slots stale (~750), plus ~300 first-in-submission touches. A segment's first
32 draws cost ~7 us against ~3.5-4 later: all threads start together and re-resolve at once. Driver 20%, program pick 14%. Not
seen: driver locks, `pools_mu` on its own, the heap (1.3%).
**Built** (off by default): `RX_VK_GONE_RING` (`gone_ring.h`), `RX_VK_TL_POOLS_WIDE`, `RX_VK_PARLOCK_SPIN`,
`RX_VK_PARALLEL_AFFINITY`, `RX_VK_PARALLEL_IDEAL`; diagnostic `RX_VK_PARLOCK_STATS`. Always on: split per-thread counters
(`split_count.h`), `texture_touch_quick` stores only changes. Tests `test_gone_ring`, `test_split_count`.
**Effect**: takings 1,050 -> 690, waits 0.45 -> 0.27 ms a command buffer; draw CPU -5..8%; slowest segment -0.03..0.08 ms. Confirm
(29 of 36 runs): frame prep 8.12 -> 7.91, 7.75 -> 7.86, 7.40 -> 7.29 ms; fps inside the spread. Not adopted.
**Correctness**: saves equal after every run; run_all 229 pass, 2 fail (nup), 35 skip. Shots / speed log not taken.
**Next**: the program pick's slow path (shader stamp); the per-submission touches ahead of the segments; TICK-050's segment
switches again with these on.
