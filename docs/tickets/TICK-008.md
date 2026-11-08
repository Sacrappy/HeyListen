# TICK-008 — Record used pipelines and precompile them at startup  · model: Sonnet · depends: 005
**Why**: scene-entry hitches of 0.7-1 s (baseline). The 36,557 shader programs are all known (TICK-002) but pipelines (program combination + state) are created on first draw.
**Goal**: persist every pipeline key created during play (append-only file next to the existing caches); at startup, create them on worker threads before/while the title runs; draws never wait (fallback: skip the draw for a frame or use the ready pipeline).
**Acceptance**: second visit to a new area after one recorded run: no frame over 50 ms in the first 10 s of the area (trace); startup time increase reported; cache invalidates when the engine's shader translation changes.
**Branch**: `feature/TICK-008-pipeline-precompile`
