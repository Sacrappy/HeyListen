# TICK-058 — Shorten the game's own CPU chain on the main PC  · model: Opus · after TICK-056
**Why**: TICK-054 (on) and TICK-056 (off) left the frame on the main PC (Ryzen 7 7800X3D, ~70-76 fps) on the game's own chain:
when the replay gets faster, the main thread's part of the loop grows. The pieces (TICK-054's timeline): the last BaseProc stage
on 4 executors 1.1-1.4 ms, module phases ~1.5 ms of polls and job waits, the job group 5.5-6.6 ms, the main thread's draw code
~3.0 ms, module job 80083580's obj_lookup + mutex_lock ~0.7 ms.
**Goal**, in order: (1) the host's synchronization cost on the critical path (wake latency of every poll, wait and wake;
sleeps, yields, timer granularity, slow paths), fixed in engine/host with the same semantics; (2) 80083580's mutex lookups;
(3) host thread priorities and affinities; (4) anything else the trace shows. Each change behind an env switch, off until
measured; adopted only after the full confirm. Correctness: speed ~60, shots alike standing and walking, no hang (the title ->
Market load), the primitives' semantics kept (memory order, lost wake-ups, timeouts), tests.
**Benchmarks**: castle-move screen (3 interleaved runs, one binary); the full confirm (kokiri-idle, castle-move, hyrule-walk, 6
runs, rounds 4-6 reversed) only to adopt. 110 s runs, 66 s cut, `--fps-limit 360`, frame generation off.
**Branch**: `feature/TICK-058-game-chain` (from main 0db10b3). Local only.

## Result (2026-11-12): the host's hand-offs are not the limit; the main thread's fence poll waits for the replay's lock, and that wait is load-bearing. Nothing adopted
**Wake latency** (`tools/bench/wakelat.py`, castle-move, base): every sleeping hand-off (297 a frame) p50 6.4 us, p90 10.1, p99 75
(the tail: virtual-core waits). The game's waits: job-group event (lr 80080188) 6.3 us on main / 6.7-10 on the module workers,
end-of-frame event (83acfcf4) 8-10.5, present event (81207a6c) 7.3, job-ready (8004af4c) 5.7-8.2 on the executors, BaseProc queue
(834a3084) 7.7-31, svcArbitrateLock / WaitProcessWideKeyAtomic (8510cda0) 7-9 (< 0.4 a frame), contended nn::os mutexes < 2. The
module poll (8009a478) sleeps 20.6-21.9 us for its 20 (a SwitchToThread loop: no timer). No 1 / 15.6 ms timer path, no sleep or
yield in a wake; the ~6 us is Windows' cross-core wake + the SRW lock + the virtual core (TICK-039: the 20 us spin is the best).
**The game chain's critical path** (`tools/bench/critpath.py --anchor main`, two traces): hand-off wakes 0.21-0.23 ms a frame,
virtual-core waits 0.09-0.31, poll lag 0.03-0.09: ~0.3-0.5 ms of ~15.7 (2-3%). 80083580's lookups: obj_lookup + obj 2.6% of the
path's samples (~0.4 ms), mutex_lock < 0.5%. Host priorities / affinities: the virtual cores keep Horizon's order; the job threads
wait 0.4-2 ms a second for a core; not the limit (TICK-011/054 measured host priorities: inside the spread).
**Found instead** (the all-thread sampler on the path): 8.5% of the main thread's time in `std::mutex::lock` under nvnSyncWait. The
game's two fence polls a frame (lr 8042cde0 / 8042ce34, f_8042cd20) took 1.42 ms a frame waiting for the renderer's `sync_order_mu`,
which `submit_q` / `present` hold for whole replays (2.65 ms a wait, p90 4.5), holding core 0's virtual core.
**Built** (off): `RX_VK_FENCE_POLL_FAST` (vk_replay.cpp): a poll of a fence whose submission finished whole answers without that
lock (the same answer, reasoned: perf-audit "TICK-058"). First version: polls 1.42 -> 0.02 ms a frame, main's part of the loop
1.82 -> 0.59 ms; screens +1.4..+6 fps, with MSYNC_FAST + SEG_EARLY +4.1; confirm (alone) 74.2 / 69.9 / 71.7 -> 77.2 / 71.4 / 73.6,
16 of 18 rounds above. **But the picture broke**: standing, the scene blown out to white (luma 249 against 105); walking, washed out.
The lock also ordered the poll after the replay in progress, and the exposure readback after it needs that replay's write-back
registrations. Kept version (v3): the readbacks of ranges with write-backs wait for the replay instead: the picture right, but ~12
such readbacks a frame on main and 2-12 on each module worker now wait (0.5-1.9 ms a frame each): screen 3 69.6 -> 63.2 fps.
**Decision**: nothing adopted; every default as on main. Game folder: TICK-054's build restored (`build/backup-tick058`).
**Kept**: `tools/bench/wakelat.py`, `tools/bench/critpath.py` (+ `tests/test_critpath.py`); the vcore wait record names its
holder; trace marks 30/31 (nvnSyncWait's first look), 32-35 (the wait's locks), 36/37 (the readback's wait); the switch, off, with
`tests/test_fence_poll_fast.py`.
**Correctness**: `HOS_SPEED_LOG` ~60.0 in every check (base 60.00 / 59.95, fast 60.02 / 60.03); shots: base right, first fast
version wrong (above), v3 right; the title -> Market load in all 103 runs, no hang or crash; saves identical after every run; no
game left. The primitives' code (hos_svc.c, nn_os_sync.c, hos_thread.c's waits) is unchanged. Tests: the touched ones pass
(test_fence_poll_fast, test_critpath, test_chainb, test_frameline, test_seg_early, test_presync_early, test_nvn_hold,
test_recorded_wait_flush, test_gpu_time_057, test_frame_trace, test_precopy_kick, and with llvm-mingw test_reserved_core,
test_multiwait_wake, test_msync_fast, test_invalidate_vcore); run_all (llvm-mingw on PATH) 243 pass, 2 fail (the nup tests, known), 35 skip.
**Not verified**: v3 on kokiri-idle / hyrule-walk (screen 3 settled it); whether readbacks other than the exposure's were wrong in
the first version (only the picture was checked); PC 3050; a scene change by walking.
**Next**: the readbacks' ordering without the replay's whole lock (a per-range "write-back pending" state published when a
submission's replay starts, or a sequence the readback compares): worth ~1.4 ms on the main thread's chain, +2 fps measured
(+4 with MSYNC_FAST + SEG_EARLY), if the picture stays right.
