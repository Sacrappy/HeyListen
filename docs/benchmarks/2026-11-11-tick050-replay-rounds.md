# TICK-050: a command buffer's replay segments in one round, the Presentation Thread recording one (2026-11-11)

## Setup
- Hardware: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11.
- Builds: worktree `feature/TICK-050-replay-rounds` (main 4e6430a), `engine/scripts/dev-build.ps1 -Game E:\OOT\hookshot-fork-game -Jobs 14` (profile/ checked; the game-folder copy confirmed by md5), each build copied to `build/tick050/bin/<N1|N2|N>`. Host DLL only; no shader-stamp file touched, shaders not re-prepared.
  - N1: the instrument and `RX_VK_PARALLEL_ROUNDS` / `RX_VK_PARALLEL_SELF`.
  - N2: + `RX_VK_PARALLEL_SELF=2` (the Presentation Thread also takes queued segments) and the per-place line.
  - N (N3, committed): + `RX_VK_PARALLEL_BALANCE` and `RX_VK_PARALLEL_POINT`.
- Runs: `tools/bench/run_bench.py`, 110 s, `--warmup-s 66`, `--fps-limit 360`, frame generation off, `RX_VK_FRAME_TRACE=1` on every variant, through `build/tick050/ab.py` (copies the binary, checks after every run that no game process is left and that the saves equal `user-save-backup-213511`). No `RX_VK_PASS_TIMING`, dumps or probes.
- Warmup: TICK-047's per-frame data (`ab1/*/present_trace.csv`, 6 s bins) shows the menus at ~360 fps until 42-48 s, the scene from ~48-54 s, and fps still moving after 66 s (kokiri-idle r2: 74 at 60 s, 80-84 from 72 s). It has not settled before 66 s, so the 110 s / 66 s cut was kept.
- Save: Castle Town Market. Scene: `castle-move` (screen only, per the user's short policy).

## Base, the fine profile (kokiri-idle, N1, `RX_VK_PROF_FINE=1`, one run, 76.3 fps, 2,996 draws)
- submit 7.83, draw 11.36, descriptors 4.23 (textures 2.08, user uniform buffers 0.70, storage buffers 0.44, banks 0.17, c1 0.10), commands 4.32 (bind descriptors 1.02 of which the push call 0.66, vertex+index binds 0.91, bind pipeline 0.70, hazards+pass 0.40, push constants 0.35, dynamic state 0.31, draw call 0.16), programs 1.81, pipelines 0.40 ms a frame.
- Parallel replay, a command buffer: dry pass 0.33 ms, segments 1.51 ms (4.65), slowest 1.25, fastest 0.60, 1.67 rounds, the Presentation Thread waits 1.42 ms.

## Screen, castle-move, 3 interleaved runs a variant

fps / 1% low / p99 ms, GPU frame prep (`submit`, ms a frame), draws a frame:

| screen | variant | run 1 | run 2 | run 3 | mean fps | mean submit |
|---|---|---|---|---|---:|---:|
| 2 (N2) | base | 63.8 / 5.5 / 75.4, 9.19, 2457 | 70.5 / 30.2 / 22.7, 8.10, 2470 | 71.9 / 38.2 / 22.0, 7.99, 2450 | 68.7 | 8.43 |
| 2 | ROUNDS | 69.4 / 37.6 / 21.5, 8.42, 2186 | 71.5 / 34.9 / 23.2, 8.50, 2501 | 71.3 / 36.7 / 21.5, 8.37, 2472 | 70.7 | 8.43 |
| 2 | ROUNDS+SELF | 74.9 / 39.2 / 20.3, 7.24, 2422 | 74.9 / 34.7 / 21.6, 7.30, 2422 | 68.9 / 36.0 / 22.7, 8.03, 2500 | 72.9 | 7.52 |
| 2 | SELF=2 | 72.3 / 37.1 / 22.8, 7.75, 2511 | 71.1 / 30.9 / 24.9, 7.67, 2478 | 73.9 / 38.1 / 22.2, 7.57, 2479 | 72.4 | 7.66 |
| 3 (N) | base | 73.7 / 20.9 / 27.2, 7.59, 2161 | 72.2 / 37.6 / 22.0, 8.17, 2421 | 75.2 / 31.4 / 20.6, 7.51, 2395 | 73.7 | 7.76 |
| 3 | ROUNDS+SELF | 71.7 / 30.0 / 23.4, 7.65, 2586 | 71.9 / 36.2 / 22.2, 7.83, 2480 | 74.5 / 39.3 / 21.6, 7.40, 2418 | 72.7 | 7.62 |
| 3 | + BALANCE, POINT=32 | 76.3 / 35.8 / 22.8, 6.93, 2101 | 75.3 / 36.5 / 22.0, 7.29, 2460 | 72.2 / 32.1 / 23.8, 7.84, 2543 | 74.6 | 7.35 |
| 3 | + `RX_VK_PARALLEL=5` | 73.9 / 21.5 / 23.1, 7.20, 2209 | 73.3 / 29.4 / 21.2, 7.47, 2476 | 72.3 / 37.9 / 21.0, 7.59, 2413 | 73.2 | 7.42 |

Screen 1 (N1, one round only, superseded): base 72.7 fps / 7.95 ms, ROUNDS 71.2 / 8.28, ROUNDS+SELF 72.6 / 7.55.

The parallel line, means (a command buffer, ms):

| variant | dry pass | segments (n) | slowest | fastest | rounds | PT waits | PT records |
|---|---:|---|---:|---:|---:|---:|---:|
| base | 0.43 | 1.46-1.50 (4.6) | 1.26-1.28 | 0.57 | 1.6 | 1.37-1.40 | - |
| ROUNDS | 0.44 | 1.60 (3.5) | 1.52 | 0.83 | 1.0 | 1.52 | - |
| ROUNDS+SELF | 0.44-0.47 | 1.31-1.35 (4.2) | 1.24-1.28 | 0.72-0.74 | 1.0 | 0.15 | 1.07-1.10 |
| SELF=2 | 0.43 | 1.37 (4.7) | 1.21 | 0.63 | 1.5 | 0.22 | 1.04 |
| + BALANCE | 0.42 | 1.33 (4.2) | 1.27 | 0.69 | - | 0.30 | 0.94 |
| + 5 workers | 0.48 | 1.30 (4.6) | 1.24 | 0.67 | - | 0.30 | 0.90 |

Per place (base, castle-move): 1.15-1.23 ms (209 draws), 1.0 (190), 0.9-0.97 (165-178), 0.7-0.9 (171-178), 0.3-0.5 (157-171), 0.3-0.4 ms (148-161). Balanced: 0.88, 0.98, 1.02, 0.95, 0.94 ms: even on average, but the slowest segment of each command buffer stays ~1.25 ms.

## Reading
- ROUNDS alone loses: fewer, longer segments.
- SELF moves the Presentation Thread's wait into its own recording: segments 1.46 -> 1.31 ms a command buffer, frame prep -0.1 to -0.9 ms (screen 3 / screen 2), mean about -0.4 ms. BALANCE adds ~0.2 ms. `submit` also follows the draw count (2,100-2,590 a frame from run to run), and its run-to-run spread is 0.5-0.9 ms.
- fps: all inside the 2-4 fps spread.
- The slowest segment is ~1.25 ms whatever the count (4, 5, 6 recorders, by count or by time). A draw costs more the more threads record at once (segments sum to ~5 ms a command buffer against ~3 ms serial).

## Decision
Not adopted; the switches stay off (`RX_VK_PARALLEL_ROUNDS`, `RX_VK_PARALLEL_SELF`, `RX_VK_PARALLEL_BALANCE`, `RX_VK_PARALLEL_POINT`). Frame prep falls ~0.4 ms, not the 1.5 ms the plan required, and not clearly beyond the spread; the acceptance (<= 5.5 ms standing) is not reached. The confirm A/B (3 scenes, 6 runs) was not run. The game folder has the TICK-045 build back (exe 223765ae, dll 86175aee).

## Correctness (switches on)
- Shots every 1200 presents, castle-move, base against ROUNDS+SELF+BALANCE+POINT=32 (`build/tick050/shotruns`): the Market drawn alike (fog, shadows, exposure, NPCs, text bubbles, HUD).
- HOS_SPEED_LOG, last 40 lines: base 59.99 (56.90-62.81), balanced 60.02 (59.22-60.79).
- Saves identical after every run; no game process left.

Raw data (not committed), in `build/tick050/` in the worktree: prof/, screen-v1/, screen-v2/, screen3/ (runs, ab.json, hookshot_frames.csv), shotruns/, bin/, build logs.

Details: engine/docs/perf-audit.md, "TICK-050".
