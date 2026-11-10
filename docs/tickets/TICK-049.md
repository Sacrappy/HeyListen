# TICK-049 — How to cut the render replay's cost (study and design)  · model: Opus · after Sync 11
**Why**: The replay costs 7-9 ms of "GPU frame prep" a frame in the Market. That is not on the frame's path at ~74 fps, but at
120 fps (8.3 ms) it is the next ceiling. The GPU's own work is ~2.2 ms.
**Goal**: The ticket has four parts:
- where the replay's time goes;
- which existing instruments measure each part;
- two or three designs (caching, more parallel replay, cheaper per-draw paths), ranked by gain against risk;
- a one-ticket plan for the first.
No game runs, builds or benchmarks on the main PC.
**Branch**: `research/TICK-049-replay`

## Result (2026-11-11): docs/research/replay-cost.md

The study is read from the Sync 11 logs and the code. Nothing was run.

**What "GPU frame prep" measures.** It is `PROF_SUBMIT`: the wall time of `run_submit` on the submitting threads (7.1 ms
standing). That time includes ~4.3 ms of the Presentation Thread waiting for the parallel replay's workers. The CPU work
behind it is ~12.5 ms a frame, summed over 5 threads.

**Per draw** (3,040 draws), ~3.6 us of CPU:

| Part | Cost |
|---|---|
| descriptors | 1.26 us (2,675 builds a frame) |
| commands | 1.38 us |
| program pick | 0.61 us (31% of picks take the "other textures" path) |
| pipeline | 0.13 us |

**Why the Presentation Thread waits.** It waits mostly for a **second round of segments**: 4.7 segments on 4 workers,
1.44 ms a command buffer, 3 command buffers a frame.

**Ranked designs:**
1. Segments in one round, with the Presentation Thread recording one of them. Saves ~1.9-2.4 ms of the Presentation
   Thread a frame. Low risk.
2. Per-binding delta resolution and a program-pick map. Saves ~3 ms of summed CPU, ~1 ms of it on the Presentation
   Thread. Medium risk.
3. Recorded segments reused across frames as secondary command buffers. Saves 4-6 ms of summed CPU only if the streams
   repeat. It needs bindless textures and per-frame uniform addressing. High risk, gated on a repeat-rate measurement.

**Measurement recipe.** The fine split of descriptors and commands has never been logged. The next ticket should take it
with `RX_VK_PROF_FINE=1 RX_VK_FRAME_TRACE=1` in one `kokiri-idle` run. Never combine it with `RX_VK_PASS_TIMING`, which
turns the parallel replay off. The details and the step plan for design 1 are in the document.
