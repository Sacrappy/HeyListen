# TICK-006 — Make parallel command recording correct (RX_VK_PARALLEL)  · model: Opus · depends: 005
**Why**: largest measured gain. `RX_VK_PARALLEL=4` takes the replay from 12.6 to 8.5 ms a frame (55→65 fps in Kokiri) but renders wrong: washed-out fog, ground without shadows, with segments of 160/320 draws; correct with one segment or 640+-draw segments (engine/docs/perf-audit.md, "Parallel recording").
**Goal**: find the shared-state race or ordering dependency between recording threads (texture write tracking, mirrors, per-thread caches, layout/barrier state, the shadow map pass) and fix it so 4 threads with 160-draw segments render identically to serial.
**Acceptance**:
- Screenshots (`RX_VK_SHOTS`) of the same scripted Kokiri walk, serial vs parallel, match (fog, shadows) at several points; flash detector clean over 3 walks.
- Bench (`tools/bench/run_bench.py --scene kokiri-walk`, Hyrule too) with parallel on: fps and p99 reported against docs/benchmarks/2026-11-baseline.md.
- Parallel on by default only if both hold; otherwise a written root-cause report.
**Branch**: `feature/TICK-006-parallel-recording`
