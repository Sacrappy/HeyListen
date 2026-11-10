# TICK-045: the three lift switches together, with a profile recorded on their lift (2026-11-10)

## Setup
- Hardware: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11.
- Builds: worktree `feature/TICK-045-profile-refresh` (main 173f146), built in the dev-build work folder (`%LOCALAPPDATA%\HookshotBuilder`) with the builder's tools (llvm-mingw of v0.1.0-rc1), 8 jobs, the result copied to `build/tick045/bin/<variant>` (`build/tick045/vbuild.ps1`). Shaders not re-prepared (no stamp input changed).
- Runs: `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off (options file), audio off; cut at 66 s. Each variant's exe and dll copied into the game folder before its run (`build/tick045/ab.py`, which checks after every run that no game process is left and that the saves equal the backup).
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walking and turning), `hyrule-walk` (the street to the gate).

## Variants
- **A**: the Sync 11 build in the game folder (upstream's hot list, link order and play profile; switches off).
- **B**: the base lift (switches off) with a profile recorded on it.
- **C**: `A64_VFPX_ELEM=1`, `A64_FPX_SLOWPATH=1`, `A64_FPX_FMA3=1` with a profile recorded on that lift.

Both profiles were made the same way (TICK-040):
1. an `RX_PGO=gen` build of the lift (B-gen: lift 195 s, build 617 s; C-gen: lift 206 s, build 822 s, every cold chunk again since a64_fpx.h changed);
2. one run of each of the three scenes with `HOS_PGO` (counters reset at 72 s, written at 106 s), through run_bench and its save guard; B-gen played at ~7.7 fps, C-gen at ~13 fps;
3. `llvm-profdata merge` of the three (equal weight): B 1.05e11 counts, C 1.40e11;
4. merged 6:1 with upstream's `hookshot_pieces.prof` (4.5e11): B 1.08e12, 1710 functions; C 1.29e12, 1994 functions.

Upstream's hot list (849 functions) and link order kept: TICK-040's longer list did not clearly help. The -O2 build with each profile: B 216 s, C 183 s (hot pieces only), no mismatched-profile warning.

## fps / 1% low / p99 ms, 6 interleaved runs a variant (rounds 1-3 in the order A, B, C; 4-6 in C, B, A)

| scene | A (Sync 11) | B | C |
|---|---|---|---|
| kokiri-idle | 75.3 / 47.6 / 18.0 | 77.2 / 54.2 / 17.2 | **78.0** / 51.1 / 17.2 |
| castle-move | 68.1 / 29.6 / 28.7 | 67.5 / 29.9 / 24.1 | **72.9** / 35.4 / 22.3 |
| hyrule-walk | 70.9 / 37.7 / 21.0 | 70.5 / 36.7 / 22.4 | **74.4** / 34.1 / 21.4 |

The runs (fps):

| | kokiri-idle | castle-move | hyrule-walk |
|---|---|---|---|
| A | 77.1 76.1 73.6 75.2 73.3 76.7 | 69.5 69.6 69.6 64.2 66.2 69.8 | 72.0 70.3 73.0 70.9 69.1 69.9 |
| B | 80.0 74.9 79.9 79.3 73.7 75.3 | 69.5 68.5 66.0 64.9 65.5 70.8 | 71.6 66.2 72.6 70.6 69.5 72.4 |
| C | 78.1 77.4 79.3 77.9 77.1 78.4 | 75.2 73.3 73.5 70.5 71.5 73.5 | 73.4 72.5 76.7 76.8 72.5 74.1 |

- C against A: +2.7 standing, **+4.8 walking** (castle-move; every C run above every A run), +3.5 on the street (hyrule-walk). Walking p99 28.7 -> 22.3 ms, 1% low 29.6 -> 35.4; hyrule-walk's 1% low -3.6 (one A run at 41.9 lifts A's mean). Standard deviations: A 1.4-2.4, C 0.8-2.0.
- B against A: +1.9 standing, -0.6 / -0.4 walking: inside the spread. The profile alone is not the gain; the switches with their profile are.
- Adopted: C.

## Game speed (HOS_SPEED_LOG, the last 40 one-second lines above 30)

| | A | C |
|---|---|---|
| kokiri-idle | 60.01 (58.96-61.22) | 60.01 (59.69-60.41) |
| castle-move | 60.01 (58.13-61.45) | 59.99 (59.17-60.68) |

## The installed build (dev-build.ps1 -Lift, the new defaults)
Exe and dll identical to C's but for the PE timestamp and checksum. Interleaved check at the end of the session (2 runs each; the PC
slower than earlier, A too):

| | kokiri-idle | castle-move |
|---|---|---|
| A | 71.7 / 45.8 / 19.0 | 68.4 / 30.3 / 23.0 |
| installed | 73.9 / 36.3 / 20.1 | 72.5 / 30.9 / 22.8 |

(One installed kokiri-idle run had a 23.2 1% low; the other 49.3.)

## Build cost (8 jobs)
| | time |
|---|---|
| lift | 195-206 s |
| `RX_PGO=gen` build, base lift (hot pieces only) | 617 s |
| `RX_PGO=gen` build, C lift (every chunk) | 822 s |
| hot pieces with a profile | 180-216 s |
| `dev-build.ps1 -Lift` on the new defaults | lift + 180 s |

Raw data (not committed), in `build/tick045/` in the worktree: ab1/, ab2/ (runs and ab.json), pgo-B/, pgo-C/ (the raw profiles and their runs), prof/ (merged profiles and their text form), shotruns/, shotruns2/, shots/, ab3/ and final-smoke/ (the installed build), bin/<variant>/ (binaries and maps), the build logs.

Details: engine/docs/perf-audit.md, "TICK-045".
