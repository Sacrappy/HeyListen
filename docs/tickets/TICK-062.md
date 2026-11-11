# TICK-062 - host-layer overhead on chain A (2026-11-11)
Branch `feature/TICK-062-chain-a` (from main 9bfc390). Local only. Every item is behind its own env switch, off by default; one binary (v1) with variants by env.

## Switches
| Item | Switch | What |
|---|---|---|
| B1 | `HOS_CV_SIGNAL_FAST=1` | `SignalConditionVariable` returns before taking `cv->lk` when `cv_waiting == 0` (seq_cst load; the counter is now updated with seq_cst atomics). Proof that no wake is lost is in the comment in `nn_os_sync.c`: a waiter registers before it releases the guest mutex, so any signal ordered after the registration sees >= 1; a signal not ordered with it is the signal-before-wait case the slow path also loses (it bumps `gen` before the waiter reads it). Wake order untouched. |
| B9 | not done | Event signals must store `signaled` under the lock; `WakeAllConditionVariable` with no sleeper and `poke_multiwait` (already an early return on `mw_waiting == 0`) are cheap. No safe saving found. |
| B2 | `HOS_THREAD_OF_CACHE=1` | `hos_thread_of`: 8-entry per-thread cache validated by a generation counter bumped on every thread add / remove / guest_tt rewrite (read before the scan, so a change during a scan leaves the entry stale). Same answer as the scan. |
| A10/B3 | `HOS_CORE_COUNT=1` | Counters only, per guest thread: core-number calls (HLE and svc 0x10), virtual-core waits and their time, logged every 5 s as `corecount:` lines. |
| B4 | `HOS_SHORT_SLEEP_PAUSE=1` | Sleeps under 1 ms: one `SwitchToThread`, then a PAUSE loop to the deadline. |
| A4/B7 | `HOS_OBJ_PACK=1` | Object table: slot pointer and key as 16-byte pairs (one cache line per probe instead of two). Same values at the same slots. |
| A7 | no change | See below. |

## A10 counts (castle-move, 110 s, cumulative, `build/t062/cnt1`)
Only two threads ask for the core number after their start: `OptimizeChangeBanc` 224,557 calls and `SceneLowPriority` 60,877. Every other thread calls it once (at its start), including the executors, module workers and NavMeshCAStepDepWorker1; svc 0x10 is never used (0). So the emulated core number is needed at start by almost all threads and continuously by two low-priority ones. Virtual-core waits (the real cost) are large on: ModuleSystemWorker3/5 (~85k / ~84k waits), ModuleSystemWorker2/4 (53k / 65k), NavMeshDepWorker2 (51k), NavMeshCAStepDepWorker1 (21.7k waits, 4.7 s in total), OptimizeChangeBanc (35.5k), BaseProcExecutor1/4/0/3 (16-22k), sead::MainThread 8.7k.

## Results (castle-move, 110 s, 66 s cut, fps limit 360, 3 interleaved runs)
Screen 1 (fps, rounds 1-3; mean): base 66.9 72.4 72.0 (70.4); B1 72.0 71.8 74.8 (72.8, 2 of 3 above); B2 71.1 71.1 71.5 (71.2, 1 of 3); B4 69.7 71.5 75.0 (72.1, 2 of 3); A4 pack 68.3 70.0 69.9 (69.4, 1 of 3). All inside the 3-5 fps run spread. Screen 2, base against B1+B2+B4: base 74.3 71.6 71.3 (72.4), stack 75.4 75.9 70.3 (73.9), 2 of 3 above.
Chain A (`critpath.py --anchor main`, one 3 s window each, 200-230 frames, about 1 ms of noise): base 13.73 ms a frame, stack 13.26, B1 14.52, B2 12.93, B4 15.09. These do not separate the variants; no chain A cut is shown by measurement.
Shots (kokiri-idle standing, castle-move walking, base against stack): standing mean luma about 103-105 in both after the fade-in, no white-out; walking varies with the camera in both; speed ratio last 40 s: standing 59.63 (min 45.1, one hitch) vs 60.00, walking 59.97 vs 60.02. Standing 75.3 -> 79.9 fps, walking 71.2 -> 71.1 in the shot runs.
Adoption bar (15 of 18 rounds) not met: nothing adopted, all switches stay off. The full confirm was not run (no clear stacked gain).

## A7 (runtime FP helpers)
`recomp_fpe_fma64` and the other `recomp_fpe_*` bodies are generated into `a64_suyu.c` by `a64_rtgen.py` from the suyu emitter. They are only the fallback after the inline fast path (`a64_fpx_fma64_fma64`: FPSR.IXC set and FPCR clear, then `__builtin_fma` with the KEEP64 test) declines, so the 67-limb body already runs only for closed gates, NaN/inf/subnormal or tiny results. TICK-044's counts put the common out-of-line cases elsewhere (NaN lanes, FRSQRTE/FRECPE, FDIV). A fast path inside the body means changing the generated runtime (a relift step, the area TICK-063 works in) and has no measured call count behind it: not changed.

## Tests
`engine/tests/cv_signal_fast_test.c`: a model of the B1 protocol (producers signal under the mutex, consumers wait), 5 rounds with and without the early return, 400k items, passes (no hang). test_multiwait_wake, test_invalidate_vcore and test_ub_threads pass; test_reserved_core could not start its fixture here (missing DLL; a function not touched). The laptop was unreachable (ssh timeout), so the full suite was not run.

## State
Game folder restored (exe b6462ab3, dll 07b8dcd6 as found), saves equal to the backup after every run, no game process left. Lock held 17:35-18:17 and 18:17-18:40; the second hold began right after my own release, which the coordinator's later order note says not to do. Released, not retaken.

## Open ends / next
Not verified: any single switch at the 15/18 bar; the stack's chain A with several traces; B4's effect on a loaded SMT sibling; A7 call counts. Next lever: the virtual-core waits of the module workers (~85k each) and NavMeshCAStepDepWorker1, since almost no thread needs the core number after its start.
