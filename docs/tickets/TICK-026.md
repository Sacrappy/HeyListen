# TICK-026 — Hot-function map and native replacement framework  · model: Opus
**Goal**:
1. Map: from all-thread profiles at Castle Town Market (most recent save, `kokiri-idle` scene) and Hyrule Field (if reachable; else Castle Town only), the functions on the critical chain (main thread's own work + the job group it waits on) by inclusive and self time, with callers/callees (prof_callers.py), grouped into systems (by call tree and strings/symbols the guest code references). Output a ranked table in docs/hotmap.md.
2. Framework: a way to replace a guest function f_<addr> by a host function through engine/nx2/oot_patches.py (or a generalised version): per-function switch (env var), and a differential mode that runs both on the same inputs and logs any difference in registers/memory written (sampled, debug-only).
3. First replacement: the Castle Town hot block in f_805aeea0 (binary search over a sorted table with an inlined strcmp, guest ~0x805afbc0-0x805afda4) or the top-ranked self-contained function from the map; native version, differential check clean, measured A/B.
**Acceptance**: hotmap.md; framework documented; one replacement with 0 differences over a run and its measured effect (fps / 1% low at Castle Town, 3+ runs each + log mean).
**Branch**: `feature/TICK-026-native-hot`
