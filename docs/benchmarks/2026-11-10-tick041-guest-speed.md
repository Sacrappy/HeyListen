# TICK-041: the lifted guest code at the Market, and the NEON FP lane loops (2026-11-10)

## Setup
- Hardware: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11.
- Builds: worktree `feature/TICK-041-guest-speed` (main 8437249), built in the dev-build work folder (`%LOCALAPPDATA%\HookshotBuilder`) with the builder's tools (llvm-mingw of v0.1.0-rc1), 8 jobs, the result copied to `build/tick041/bin/<variant>` (`build/tick041/vbuild.ps1`). Shaders not re-prepared (no stamp input changed).
- Runs: `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off (options file), audio off; cut at 66 s. Each variant's exe and dll copied into the game folder before its run (`build/tick041/ab.py`, which also checks after every run that no game process is left and that the saves equal the backup).
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walking and turning), `hyrule-walk` (profile only).

## Variants
- base: main (upstream's hot list and PGO profile): the TICK-040 base rebuild.
- v1: `A64_VFPX_ELEM=1` lift (FMUL / FMLA / FMLS by element and vector FMLA / FMLS .4S vectorized), first version (the broadcast element through a 16-byte stack array), upstream's profile.
- base-np: base's lift, no PGO profile.
- v1b-np: the final lifter change (broadcast in a register), no PGO profile.
- v1b-p: v1b with a profile recorded on its own lift (`RX_PGO=gen` build, one run of each scene with `HOS_PGO`, counters 72-106 s, merged 6:1 with upstream's profile, as TICK-040).

## fps / 1% low / p99 ms, interleaved, 3 runs a variant per session

| session | variant | kokiri-idle | castle-move |
|---|---|---|---|
| 1 | base | 71.0 / 40.2 / 20.5 | 64.4 / 27.7 / 29.8 |
| 1 | v1 | 70.9 / 38.3 / 21.0 | 63.6 / 27.7 / 30.2 |
| 2 | base-np | 66.3 / 38.2 / 21.9 | 60.5 / 27.7 / 30.1 |
| 2 | v1b-np | 70.7 / 36.7 / 22.1 | 64.5 / 29.5 / 27.9 |
| 3+4 | base (6 runs) | 71.8 / 39.1 / 20.7 | 64.7 / 30.2 / 28.6 |
| 3+4 | v1b-p (6 runs) | 73.9 / 38.2 / 20.8 | 66.4 / 28.6 / 29.3 |

The runs (fps):

| variant | kokiri-idle | castle-move |
|---|---|---|
| base (1) | 68.8 74.7 69.6 | 64.3 64.5 64.5 |
| v1 | 71.5 69.9 71.4 | 63.2 63.0 64.7 |
| base-np | 66.3 66.5 66.1 | 59.7 60.9 60.9 |
| v1b-np | 68.7 71.8 71.7 | 63.0 66.7 63.7 |
| base (3+4) | 69.7 69.5 71.4 74.8 69.4 76.1 | 66.6 65.1 63.6 63.8 65.2 63.8 |
| v1b-p | 71.5 75.0 74.0 76.7 73.2 73.2 | 65.4 68.2 68.0 67.4 63.1 66.5 |

- Without a profile: +4.4 standing, +4.0 walking, the ranges apart.
- Each with a profile on its own lift: +2.1 / +1.7, 1% lows -0.9 / -1.6: inside the 2-4 fps spread. Not adopted; the switch stays off.
- Session 4 ran the variants in the other order (v1b-p first).

## Job path (HOS_JOBTRACE 4 s at 90 s + HOS_SAMPLE_NS, kokiri-idle, one run each)

| | base | v1 |
|---|---:|---:|
| path work, ms a frame | 6.87 | 6.98 |
| wake, ms a frame | 0.21 | 0.22 |
| guest code / runtime / system on the path | 82.2 / 9.8 / 8.0% | 82.6 / 8.8 / 8.6% |
| guest samples of the job and main threads, 36 s | 22,425 | 22,273 |
| FPX lane loops + reload, share of the guest samples | 15.9% | 2.2% |
| mean executors working during the group's waits | 3.36 | 3.31 |

## Game speed (HOS_SPEED_LOG, castle-move, the last 40 one-second lines above 30)

| base | v1b-p |
|---|---|
| 60.04 (58.44-61.14) | 60.00 (58.16-61.76) |

## Build cost (8 jobs)
| | time |
|---|---|
| lift | 171-179 s |
| every chunk compiled again after a64_fpx.h changed | 581-597 s |
| hot pieces only (another profile) | 182-194 s |
| `RX_PGO=gen` build | 503 s |

Raw data (not committed), in `build/tick041/` in the worktree:
- ab1/ to ab4/ (runs and ab.json), speed/, smoke/, final-restored/;
- tr-base-idle/, tr-v1-idle/ (trace, samples, jt.txt);
- mix-base/, mix-v1/ (guestmix output);
- pgo-v1b/, prof/ (profiles);
- bin/<variant>/ (binaries and maps), shots/.

Details: engine/docs/perf-audit.md, "TICK-041".
