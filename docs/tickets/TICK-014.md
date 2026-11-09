# TICK-014 — Correct game speed above 60 fps  · model: Opus
**Why**: TICK-010 found that with "Game speed: Real time" on (default), Link walks further in the same seconds at ~65 fps than at 60. At 120 fps the game must run at exactly the same speed as at 60.
**Goal**: measure the speed error precisely (a guest-side clock: the game's frame counter / delta-time the host feeds, nn::os tick, sead delta; or distance walked per second from a fixed input script at 60, 90, 120 limits), find the cause (host/nn_vi.c fps limit pacing and what delta the game is told, opts.realtime handling, vsync event period, timers that count frames not time), and fix it.
**Acceptance**: same distance/clock per second within 1% at limits 60 and 120 (and at the uncapped rate the GPU allows); picture correct; no fps loss.
**Branch**: `feature/TICK-014-game-speed`
