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
