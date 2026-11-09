# TICK-028 — Subgroup reconvergence and dark-frame diagnostics  · model: Sonnet
**Goal**: (1) enable VK_KHR_shader_maximal_reconvergence where supported and mark modules that use subgroup ops (ballot/shuffle/vote) MaximallyReconvergesKHR; check the Eden translation never moves subgroup ops into a branch; env switch; compare foliage screenshots. (2) Diagnostic switches for the one-off dark sky / black silhouettes: a wait after each frame's first submission, conditional-render reports pending until memory is written, re-upload check for CPU-written small linear textures. Ideas from docs/references/NX2navi.md (ideas only, own code).
**Acceptance**: switches documented; picture unchanged or improved; fps unchanged with defaults.
**Branch**: `feature/TICK-028-reconvergence`
