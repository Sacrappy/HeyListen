# TICK-011 — Game CPU critical path at 120 fps  · model: Opus · depends: 010
**Goal**: once the pacing gate is gone, profile the frame's critical path (crit.py over HOS_SAMPLE_ALL, prof.py per thread) and cut the guest CPU work on it to fit 8.3 ms: hot guest functions replaced by host code (libc-like, math, memcpy already host), lifter-level improvements (engine/nx2: atomics lowering, PAC/LSE patterns, hot function splitting, PGO pieces), synchronisation overhead in host/nn_os_sync.c and hos_thread.c (light events, message queues, mutex fast paths), thread priorities/affinity.
**Acceptance**: Hyrule and (if reachable) Kokiri at limit 120 and 144: fps, p99, % frames > 8.33 ms; game speed correct; picture correct.
**Branch**: `feature/TICK-011-cpu-critical-path`
