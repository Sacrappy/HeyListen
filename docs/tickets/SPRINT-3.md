# Sprint 3 — goal: 120+ fps, stable (2026-11-09)

Baseline (engine = Hookshot 1068108 + TICK-006/007/008, Hyrule Field, HOS_FPS_LIMIT=120, frame generation off): 66.8-67.2 fps, p99 ~21 ms, 13-20 frames > 50 ms per 115 s run.

All-thread sample (HOS_SAMPLE_ALL + engine/scripts/crit.py, 25 s of Hyrule gameplay):
- GPU-bound wait 0.0 ms; replay workers idle 93-99%: neither the GPU nor the replay limits the frame.
- 36% of instants no game thread runs. Then sead::MainThread is in guest f_806668b0 (88%, a spin: leaf in game code), BaseProcExecutor0 and the Presentation Thread wait in nn::os::ReceiveLightMessageQueue.
- Main thread blocked 82% of instants: f_806668b0 45%, WaitLightEvent from f_8007fe60 42%, SleepThread 4%.
=> the frame is gated by a wait (frame pacing, a vsync/present signal, a timer, or a completion the host delivers late), not by work.

| Ticket | Model | Depends | Runs the game |
|---|---|---|---|
| TICK-010 Remove the pacing gate | Opus | — | yes |
| TICK-011 Game CPU critical path at 120 | Opus | 010 | yes |
| TICK-012 Remaining hitches (textures, pools, locks) | Sonnet | — | yes |
| TICK-013 Bench metrics for 120 | Haiku | — | no |

Only one ticket that runs the game at a time (one game folder, one GPU).

## Correction after TICK-010 (2026-11-09)
The "spin in f_806668b0" was the driver's vkWaitSemaphores charged by crit.py to the innermost game frame: the main thread waits once a frame (4-5.5 ms) in the exposure-meter capture wait for the GPU. nvidia-smi: GPU 83-86% busy; RX_VK_PASS_TIMING: ~12.9 ms of GPU work a frame, of which the engine's own copies ~3.7 ms (mirror write-backs 1.6 ms / 69 MB, CPU-page copies into mirrors 2.1 ms / 11 MB). The "GPU-bound wait 0.0 ms" log figure and "GPU time 3.2 ms" undercount. Removing the wait makes it slower (58 / 43 fps): no GPU headroom.
Also found: with Real time on, the game runs somewhat faster at ~65 fps than at 60 (Link walks further in the same seconds). A 120 fps goal needs correct game speed first.

## Decisions taken autonomously (night of 2026-11-09, user asleep, standing instruction to decide)
- TICK-014 merged: guest-address hook (f_80706be0 return) for the nominal fps; no-op if the address does not match.
- TICK-015 merged: write-backs skipped for CPU_CACHED pools never invalidated (fallback: an invalidate makes the pool written back for good). Hyrule 67.7 -> 73.9 fps.
- Hookshot upstream d3659c5 pulled (OoT debug tools): F10 overlay key moved to index 17 (upstream took 13-16 for page keys). Pipeline notes were empty after TICK-015's stamp incident; re-recorded by the first run.
- Order changed: TICK-016 (GPU cost of the translated frame) before TICK-012/011, since the GPU is the limit (~10 ms a frame).
- TICK-016 merged as diagnostics only (barrier trust switches off: no fps change). GPU ~9.8 ms of a 13.5 ms frame, ~77% busy: the limit is now the CPU/GPU serialisation (capture wait) -> TICK-017. The two costliest passes read an unbound storage buffer -> TICK-018 (possible rendering bug).
- Hookshot upstream pull 4 (11 commits) done between tickets by a Sonnet sub-agent.
- TICK-017 merged as diagnostics/experiments only (all off): letting the CPU run ahead made the GPU slower (56 fps), ~5 ms GPU-busy a frame untimed; the capture wait protects pages in CPU-read pools, not the exposure meter. Next: TICK-019 (GPU reads of imported host memory) before TICK-018.
- TICK-019 merged as diagnostics only: the GPU reads no imported host memory (every pool mirrored in VRAM; page table and streams in ReBAR VRAM). Overlap via defer/slack is neutral. Next: TICK-020, full GPU timeline with Nsight Systems (installed on the machine).
- TICK-020 merged (docs): frame = GPU ~10-11 ms busy + ~3-4 ms GPU idle while the main and Presentation threads produce the next frame (the drain: capture wait by default, the game's own present-fence wait with the deferral); game CPU ~10 ms on job threads; engine GPU copies ~2.2 ms. 120 fps needs the GPU frame < 8.3 ms AND the drain overlapped: neither is reachable by small engine fixes alone. Next: TICK-021 (engine copies), TICK-018 (unbound storage buffer in the two costliest passes), then the render-resolution question for the user (2048x1152 internal).
- TICK-021 merged: engine GPU copies 2.19 -> 0.81 ms (compute scatter-copy). TICK-011 merged: +1-2 fps; the CPU chain (main + job group ~8.5 ms) is spread guest code. Hookshot pulls 6 and 7 done.
- State: Hyrule Field ~74-75 fps at limit 120 (from 49 serial at the start of sprint 2). 120 needs both a faster CPU chain (lifter-wide: TICK-022) and a shorter GPU drain/frame. Render resolution (2048x1152 internal) is the user's call.
- TICK-022: lifter overheads measured (soft FMA 98% quiet-NaN lanes, polls ~1 cycle, atomics already minimal, flags already x86-64-v3); FMA fast path FP-equivalent but no fps change -> docs merged, code left on feature/TICK-022-lifter-speed. Game folder restored to the backup-tick022 build. NOTE: %LOCALAPPDATA%\HookshotBuilder\sa\gen holds the FMA-lifted C: the next dev-build must use -Lift.
- Engine-side levers measured so far are exhausted at ~75 fps in Hyrule Field (limit 120). Remaining paths need a user decision: render resolution / upscaling (GPU ~10 ms), frame generation (exists, user keeps it off), or a deep rework of the game's job chain (~8.4 ms CPU, spread guest code).
- 2026-11-09 afternoon: user report of a one-off corruption on the Hyrule -> Kokiri transition with VRAM near full -> TICK-023 merged (VRAM ~88% -> 81-87%, evictions now free memory, mirrors freed). Overlay fixed (3D scene size, frame limit). User's slowest spot = Castle Town Market (~57-61 fps): TICK-025 found it CPU-bound by the game's own dependent job chain; OptimizeChangeBanc is real work. TICK-022's FMA path re-tested there: no gain (59 fps). NOTE: sa\gen now holds main's lifted code again only after a -Lift build; the game folder has the main build (backup-fma), but the shared work folder holds the FMA lift: next dev-build must use -Lift.
