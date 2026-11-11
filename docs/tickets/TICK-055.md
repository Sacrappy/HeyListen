# TICK-055 — Audit of the lifter's generated code on x86-64, then the top fixes  · model: Opus · after TICK-054 (parallel)
**Why**: the ~14 ms CPU frame at the Market is the game's own lifted code: a job chain of ~6.5 ms over 6 executors, serial long
actor jobs (Link 800b3ce0, 80166100, 802ca820, module job 80083580), ~1.3 ms of the main thread's draw code and module phases.
Faster lifted code shortens all of it at once, and more on ordinary CPUs (PC 3050, i5-10400F).
**Goal**:
1. phase 1, no game run: the generated C and the x86-64 code clang makes of it for the hottest functions, the systematic
   inefficiencies classified with counts and an estimated cost, micro-benchmarks on synthetic AArch64 (no game code),
   `docs/research/lifter-x86.md` ranked by gain against risk and effort;
2. phase 2: the top 1-3 fixes, each a lift-time switch (`lift_switch` / off until measured), bit-identical (differential tests
   against the old lowering on synthetic sequences: flags, NaN, edge cases, unaligned accesses), measured like with like on PGO
   (TICK-045: a changed lift without its profile loses 4-5 fps): the hot serial jobs' CPU time first, then fps (castle-move, 3
   interleaved runs a variant).
**Machines**: main PC: no game, no heavy build (TICK-054 benchmarks there). PC 3050: builds and game runs once its busy flag
is gone. Laptop: game-free tests and micro-benchmarks only.
**Branch**: `feature/TICK-055-lifter-x86` (from main c632e9b, main 8ece4b3 merged in). Local only.

## Result

(filled in below)
