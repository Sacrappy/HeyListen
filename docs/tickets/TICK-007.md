# TICK-007 — Cache descriptor resolution per binding generation  · model: Opus · depends: 005
**Why**: `build_descriptors` is 19% of the replay thread (~30 descriptors resolved per draw: uniform ranges, storage buffers through the driver banks, textures via the handle table); push descriptor recording itself is only ~0.35 µs/draw (TICK-003).
**Goal**: skip re-resolution when a draw's bindings are unchanged since the previous draw/generation; resolve only changed slots. Then evaluate moving textures to a bindless heap (TICK-003: 0.05 µs/draw).
**Acceptance**: identical screenshots; `build_descriptors` share measured with `engine/scripts/prof.py` before/after; bench fps/p99 in both scenes.
**Branch**: `feature/TICK-007-descriptor-cache`
