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
