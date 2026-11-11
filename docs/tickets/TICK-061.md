# TICK-061 — Cut chain B (the render chain) by 2+ ms with no change to the picture  · model: Opus · after TICK-054/056/058
**Why**: the main PC (7800X3D, RTX 5070) runs ~70-77 fps against a 120 fps goal. The frame is two chains of ~13-14 ms (TICK-054):
chain A, the game's own code (TICK-062/063), and chain B, the held flush (capture copy ~2.1 ms) then the Presentation Thread's
replay (~7.8 ms: dry passes ~1.2, mirror pre-sync 0.8, parallel segments ~2.4, small command buffers ~2 serial) and the present.
A cut on chain B alone may not show in fps until chain A is cut too, so chain B's own length is measured as well as fps.
**Goal**: the chain-B suspects of `docs/research/suspects/README.md` (C1, C2, C3 = E2, E3, D1, D2/D3, D5, D6, D7, D8, F2), each
behind its own env switch, off by default; screened alone, then stacked with TICK-056's `RX_VK_MSYNC_FAST=1 RX_VK_SEG_EARLY=1`.
Correctness: the exact bytes the GPU reads and the order the game observes kept (argued in code comments); shots alike standing
and walking; `HOS_SPEED_LOG` ~60; engine tests.
**Benchmarks**: castle-move screen (3 interleaved runs, one binary, variants by env); the full confirm (kokiri-idle, castle-move,
hyrule-walk, 6 rounds, 4-6 reversed) only for a stacked set with a clear gain. 110 s runs, 66 s cut, `--fps-limit 360`, frame
generation off. The main PC shared through `E:/OOT/locks/mainpc`.
**Branch**: `feature/TICK-061-chain-b` (from main 9bfc390). Local only.

RESULT_PLACEHOLDER
