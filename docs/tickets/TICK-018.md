# TICK-018 — "storage buffer NOT bound" in the two costliest passes  · model: Sonnet
**Why**: TICK-016: the two most expensive passes of the frame (one draw each at 2048x1152 into B10G11R11 + depth; programs 4f0c0b40ab3a32cd 0.82 ms and 739acd678ae08655 0.56 ms) log "storage buffer NOT bound (reads zeros)". A shader reading zeros where the game meant data can be a rendering bug (wrong lighting/effect) and can change cost.
**Goal**: find what the game binds there (NVN storage buffer bindings through the driver banks / c0), why the engine does not resolve it, and fix the binding; compare screenshots of the affected effect before/after.
**Acceptance**: no "NOT bound" for those programs; picture checked (describe what changed); GPU time of the passes before/after.
**Branch**: `feature/TICK-018-ssbo-binding`
