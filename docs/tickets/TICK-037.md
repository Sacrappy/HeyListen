# TICK-037 — Find what holds the frame at ~65 fps after Sync 9, and remove it  · model: Opus
**Why**: after TICK-036 and Sync 9 the Market runs 64-69 fps and Hyrule ~65 fps (RTX 5070). Sprint 3/4 notes say the game's CPU job chain (~8.4 ms) and the GPU (~10-13 ms) both sit near the frame time; those numbers predate TICK-021..036 and Sync 9. The user's ask (2026-11-10): identify the bottleneck that stops fps from rising, then optimize it.
**Goal**:
1. One fresh, current attribution of a frame at the Market (standing, walking) and Hyrule Field: CPU critical chain (main thread + dependent jobs, guest vs host/engine time), replay/recording threads, submit/present waits, GPU time per pass (RX_VK_PASS_TIMING / Nsight Systems), with the frame limit off. State which one limits fps, by how much, and what the next limit is.
2. Remove the biggest piece that is ours to fix (engine, replay, sync, GPU passes, or a native replacement of a hot guest function through the TICK-026 framework), each behind a switch, picture unchanged.
**Acceptance**: docs/benchmarks + engine/docs/perf-audit.md section with the breakdown; measurable fps / 1% low gain in castle-move and kokiri-idle (Market) beyond the 2-4 fps run-to-run spread, or a proof of why no engine-side gain is left and a ranked plan.
**Branch**: `feature/TICK-037-bottleneck`
