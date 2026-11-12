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

## Result (2026-11-12): every item built and measured; stacked with TICK-056's two switches chain B is -1.8 ms, the frame is not (chain A next to it): nothing adopted, all switches off
**Built** (each off by default, its argument in the code; perf-audit "TICK-061" has the table):
`RX_VK_CAPTURE_CLAIM` (C1), `RX_VK_COLLECT_FAST` (C2, in the exe), `RX_VK_SHADOW_FAST` (C3 = E2), `RX_VK_STAMP_SPLIT` (E3),
`RX_VK_DRY_LEAN` (D2: `=1` the exact one-segment case; `=2` the D3 boundary filter, not exact in play), `RX_VK_WORK_LOCAL` (D6,
checkpoints kept on), `RX_VK_SEG_SETUP_PAR` (D7), `RX_VK_OCC_PROXY_SKIP` (F2). Not built: D5 (vk_shader.cpp / vk_bank.cpp are
shader-stamp inputs: a change re-prepares every shader and resets the shared cache), D2's real dry-pass skip (not provably the same
order of the stream's CPU writes), D8 (`maybe_split` never runs with the parallel replay on: nothing to screen).
**Measured, per item** (castle-move screens, chain B = held flush + PT submits + PT tail, `chainb.py`):
- C2: the scan 0.069 -> 0.048 ms a submit; C3: the shadow block 0.009 -> 0.004 ms a take; E3: 10 -> 8 us a take; D7: the
  segments' setup 0.10 -> 0.02 ms a frame; D1 (`RX_VK_PARALLEL_SEG=96`): the PT's own segment -0.17 ms; C1: the copy unchanged on
  average. Each under the screens' spread (~0.5 ms of chain B, 2-4 fps); fps of every item inside it.
- D2/D3 as first built lost 1.4 ms of chain B (other segment boundaries); F2 leaves out ~11 draws a frame, not the ~900 proxies.
**Stacked** (screen 3): all items: chain B 11.99 -> 11.20 ms (runs overlap); + `RX_VK_MSYNC_FAST=1 RX_VK_SEG_EARLY=1`: **10.20 ms
(-1.8, every run below every base run)**, the main thread's part 1.92 -> 3.06 ms, the traced frame 13.91 -> 13.27, fps 75.7 ->
77.6 (inside the spread). No confirm run (no clear fps gain); adoption bar not met.
**Correctness**: shots alike standing and walking (base against the full stack + TICK-056), `HOS_SPEED_LOG` median 60.00-60.01;
saves identical to `user-save-backup-1111-2131` after every run (47); no game left. New tests: test_capture_claim,
test_collect_fast, test_shadow_fast, test_dry_lean; the touched areas' checks 37 pass, 3 skipped (no Vulkan headers); the whole
suite not run (the laptop's SSH was down).
**Incident** (fixed): the first build added a field to `Program`, part of the program cache's stamp; its first run truncated the
shared `hookshot_programs.bin`. The field went (79da345) and the cache was prepared again at once with the game folder's own build.
**State**: branch `feature/TICK-061-chain-b` (commits fad43fb .. this one), not merged. Game folder: as found (TICK-054's build, exe
b6462ab3, dll 07b8dcd6). Binaries in the worktree's `build/tick061/bin` (v5 = 8b63d823).
**Next**: chain A (TICK-062/063), then the stack + MSYNC_FAST + SEG_EARLY again with a confirm; F2 with the proxies' reason
named; D5 with a planned re-prepare; the held flush's capture (1.2-1.3 ms) with C4's identical-page count first.
