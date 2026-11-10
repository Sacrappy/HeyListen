# TICK-047: the game's long jobs first in the BaseProc scheduler (2026-11-10/11)

## Setup
- Hardware: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11.
- Builds: worktree `feature/TICK-047-job-order` (main ed919dc), built in the dev-build work folder with the builder's tools, 8 jobs, copied to `build/tick047/bin/<variant>` (`build/tick047/vbuild.ps1`). Hot pieces with TICK-045's profile (`profile/`); no mismatched-profile warning. Shaders not re-prepared.
- Runs: `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off, cut at 66 s, through `build/tick047/ab.py` (copies the variant's exe and dll, checks after every run that no game process is left and that the saves equal the backup).
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walking and turning), `hyrule-walk` (the street to the gate).

## Variants
- **A**: the TICK-045 build (the game folder's, exe 223765ae, dll 86175aee).
- **J5**: TICK-047's hooks in f_8004ab40 / f_8004fa40 (lifted code key 092c8553e3), `HOS_JOB_ORDER` unset.
- **J5@on**: J5 with `HOS_JOB_ORDER=1`: a ready job whose last runs took over 300 us goes into its level's A ring, ahead of that level's waiting jobs.
- **J6**: J5 plus a room check (the A ring under three quarters full); host code only, same lifted code. Used for the behaviour checks.

## fps / 1% low / p99 ms, 6 interleaved runs a variant (rounds 1-3 in the order A, J5, J5@on; 4-6 reversed)

| scene | A | J5 (off) | J5@on |
|---|---|---|---|
| kokiri-idle | 76.3 / 40.0 / 18.3 | 76.7 / 49.9 / 17.7 | 77.4 / 52.7 / 17.5 |
| castle-move | 71.6 / 33.4 / 23.3 | 71.5 / 33.8 / 23.3 | 71.8 / 34.5 / 23.1 |
| hyrule-walk | 74.4 / 40.3 / 20.7 | 74.9 / 36.2 / 21.2 | 73.4 / 35.4 / 21.6 |

The runs (fps, rounds 1-6):

| | kokiri-idle | castle-move | hyrule-walk |
|---|---|---|---|
| A | 73.1 80.0 79.6 74.3 76.0 74.9 | 69.6 71.5 73.0 71.8 73.9 69.9 | 75.6 74.9 73.7 73.4 75.5 73.5 |
| J5 | 75.1 75.5 80.1 76.7 78.6 74.1 | 71.1 71.0 74.2 69.2 70.8 72.6 | 71.5 72.9 76.3 83.2 73.6 72.2 |
| J5@on | 75.1 76.3 78.7 80.8 76.4 77.4 | 70.3 71.0 73.6 71.2 72.0 73.0 | 74.2 72.6 75.1 72.8 73.0 72.9 |

- J5@on against A: +1.1 standing, +0.2 walking, -1.0 on the street. Inside the 2-4 fps spread (sd 1.0-2.9).
- J5 (switch off) against A: inside the spread (one hyrule-walk run at 83.2 is an outlier).
- Not adopted.

## The chain (HOS_JOBTRACE at 90 s for 4 s, job-marked builds AM / JM6, `tools/bench/jobgraph.py`)

| | castle-move base | castle-move on | kokiri-idle base | kokiri-idle on |
|---|---|---|---|---|
| Link's calc 800b3ce0 starts | #13 of 40, 238 us late | #5 of 40, 11 us late | #17 of 49, 371 us late | #5 of 49, 11 us late |
| its time alone | 738 us | 287 us | 711 us | 254 us |
| main waiting, 1 executor in a job | 1.43 ms | 1.22 ms | 1.61 ms | 1.46 ms |
| main waiting, total | 9.26 ms | 8.58 ms | 8.77 ms | 7.94 ms |

## Behaviour (A against J6 with `HOS_JOB_ORDER=1`)

| HOS_SPEED_LOG, last 40 lines | A | J6 on |
|---|---|---|
| kokiri-idle | 60.00 (59.69-60.37) | 60.00 (59.37-60.61) |
| castle-move | 59.98 (58.69-61.10) | 59.99 (59.43-60.63) |

A first pair (shots not written: the folders did not exist) gave A 60.00 / 59.99 and J6 59.99 / 59.69, the last with one 47.4 second.
Shots every 1200 presents: the same sequence, the Market, NPCs, Link and text bubbles drawn alike. Saves identical after every run.
The rings' sizes (`HOS_JOB_ORDER_LOG=1`): A 128 / 128 / 64 cells, B 256 / 16384 / 1024 (levels 0 / 1 / 2).

Raw data (not committed), in `build/tick047/` in the worktree: ab1/ (runs, ab.json), shotruns/, shotruns2/, shots/, tr-*/ (job traces and jobgraph output), bin/<variant>/, the build logs.

Details: engine/docs/perf-audit.md, "TICK-047".
