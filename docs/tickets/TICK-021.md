# TICK-021 — Mirror-sync copies: fewer, larger, cheaper  · model: Opus
**Why**: TICK-020: the engine's copies cost ~2.2 ms of GPU a frame before the per-frame drain (msync 1.3 ms for ~1570 small regions at ~8.7 GB/s, write-backs 0.43, readback refresh 0.43). With GPU busy ~10-11 ms of a 13.6 ms frame, this is the largest engine-side GPU item left.
**Goal**: cut msync cost: coalesce regions across pages and captures (TICK-015 merged adjacent pages only), copy at sub-page granularity where CPU-write tracking allows, one vkCmdCopyBuffer with many regions vs many calls, or a compute scatter-copy shader for many tiny regions; skip re-copying pages whose bytes did not change (hash/compare on the CPU side before upload); reduce readback refresh to pages the CPU will read.
**Acceptance**: engine copy GPU ms (RX_VK_PASS_TIMING msync/wb/rbrefresh) down by half; fps / 1% low at limit 120 up (2 runs + log mean); picture, flash detector and game speed checks.
**Branch**: `feature/TICK-021-msync`
