# TICK-054 — +20 fps on the main PC by shortening the CPU critical path of a frame  · model: Fable 5.1 · after TICK-052
**Why**: the Market runs at ~78 fps standing, ~73 walking (TICK-045 build, Ryzen 7 7800X3D, RTX 5070). The earlier tickets
measured the parts (the job chain ~6.5 ms, the main thread's serial part ~5.4 ms, the replay ~7.8 ms on the Presentation Thread
plus 4 workers) but no timeline said which of them the frame actually waits for, frame by frame.
**Goal**: (1) the real critical path of a ~13.7 ms frame, as a timeline of a few frames with numbers (which thread waits on
which; whether the game frame N+1 overlaps the replay of frame N; the sync points); (2) the biggest structural lever, behind an
env switch, on by default only once its A/B shows a gain; (3) the same behaviour (speed ratio ~60, shots alike, no new crash), the
game's job ordering kept. Not touched: `vk_memory.cpp`'s capture code and `vk_texture.cpp` (TICK-053 works there).
**Benchmarks**: castle-move screen, 3 interleaved runs a variant; the full set (kokiri-idle, castle-move, hyrule-walk, 6 runs
each, order reversed in rounds 4-6) only to confirm an adoption. 110 s runs, 66 s cut, `--fps-limit 360`, frame generation off.
**Branch**: `feature/TICK-054-cpu-frame` (from main 4b170f0, TICK-052 merged in). Local only.

## Result

(filled in below as the work goes)
