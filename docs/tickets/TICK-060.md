# TICK-060 — PC 3050: cut the frame time without lowering quality · model: Opus 5.5 · after TICK-053/057/058
**Why**: PC 3050 (i5-10400F 6C/12T, RTX 3050 8 GB on PCIe 3.0 x4) runs the Market at ~28-29 fps with the GPU idle a third of
the frame (62-69%). TICK-053 found the frame serial (the late-page wait), TICK-057 found no engine GPU cut that moves fps.
**Goal**: where the GPU's idle third goes (without PASS_TIMING's serialisation where possible), then quality-preserving cuts, each
behind an env switch, off until proven; picture identical; the original behaviour kept.
**Branch**: `feature/TICK-060-pc3050-gpu` (from main af1cc93). Local only; built and measured on PC 3050 (session 0 over SSH).

## Result
(filled in below)
