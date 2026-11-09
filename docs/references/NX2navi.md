# NX2navi — what is useful to HeyListen (analysed 2026-11-09)

Repo: https://github.com/tangerineturtle1/NX2navi (local read-only clone: E:\OOT\references\NX2navi, not part of HeyListen). Sibling of Hookshot: static recomp of the Switch 2 OoT (1.1.1), NVN2 replayed on Vulkan, SASS lowered to Maxwell and translated by Ryujinx's translator (we use Eden/sirit). Very active (243 commits in 6 days). Claims ~60 fps; HeyListen is already faster, its value for us is correctness and features.

## License — read before using anything
- `LICENSE` (MIT) covers only the base import (commit 0478c13). `LICENSE-ADDITIONS`: everything added afterwards is **all rights reserved**.
- MIT and usable in our GPL-3.0 tree with a THIRD_PARTY_NOTICES row: files as of 0478c13 (`git show 0478c13:<path>`), e.g. runtime/vk_exec_xfer.c (occlusion counts), translator patches 0001-0016.
- Everything later (patches 0017-0038, vk_gpa.c, DLSS/FSR/motion vectors, current vk_*.c): **ideas only**, our own clean implementation from the description.

## Shortlist (ranked) -> tickets
1. Device-address page table + twin writes for GPU memory reached through pointers (their vk_gpa.c / patches 0030, 0034, 0036, 0037; ideas only): fixes "storage buffer NOT bound (reads zeros)" (our two costliest passes, TICK-018) and likely the floating-tree / foliage flicker. High risk (Eden backend, shader stamp). -> TICK-018 scope.
2. Real occlusion queries instead of "Occlusion Off" (MIT base runtime/vk_exec_xfer.c "occlusion counts": 4096-query ring, results summed on completion, not-ready = visible, CPU-evaluated conditions with a pending-report table). oot-pc measured ~29% fewer draws. -> TICK-027.
3. VK_KHR_shader_maximal_reconvergence + never move subgroup ops into branches (patch 0026 idea): missing foliage leaves / culling counts. Low risk. -> TICK-028.
4. Diagnostics for the one-off dark sky / black silhouettes: wait after each frame's first submission (their SUBMIT_FINISH), conditional-render reports cleared only after memory is written, CPU-written small linear textures (64x1 sky ramps) re-uploaded, uniform double-buffer rewrite detection. -> TICK-028 (diagnostic part).
5. Frame generation from depth + motion vectors (FSR 3.1 FG / DLSS-FG) and DLSS/FSR upscaling: the only path to 120+ displayed fps without the CPU chain; third-party SDK licences; clean-room MVs. -> not scheduled (user chose native replacements; frame generation declined so far).
6. Memory pressure: texture eviction on low system commit, bounded write-back snapshot pool, render-target image reuse (same-parameter, released >= 2 frames), per-frame pipeline wait budget with target history, push descriptors with templates. -> TICK-029.

Already in Hookshot/HeyListen: NaN guard on colour outputs (0027), NaN-safe min/max (0031), per-tap shadow gather (0032), invariant positions (non-NVIDIA), zeroed local memory (0016), GPL fast link, write-backs left out (TICK-015), dynamic-state delta, game speed above 60 (TICK-014).
