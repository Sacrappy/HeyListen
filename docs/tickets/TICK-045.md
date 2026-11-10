# TICK-045 — The three lift switches together, with a PGO profile recorded on their lift  · model: Opus · after TICK-044, Sync 11
**Why**: three bit-identical, opt-in lift-time switches exist: `A64_VFPX_ELEM=1` (TICK-041: NEON by-element and vector FMLA ops vectorized), `A64_FPX_SLOWPATH=1` and `A64_FPX_FMA3=1` (TICK-044: per-lane quiet-NaN fallback in the cold `_lanes` helpers, scalar rescues, an estimate table; scalar binary32 FMA by FMA3). Each alone is +1-2 fps walking, inside the 2-4 fps spread. A change to the lifted code without a matching PGO profile loses 4-5 fps (TICK-040/041), and today's default build uses upstream's play profile (`%LOCALAPPDATA%\HookshotBuilder`: `hot_funcs.txt`, `hookshot_pieces.prof`, `hot_order.txt`). TICK-041 suspected part of its profiled gain came from the profile alone. Market reference (Sync 11): ~75-76 fps standing, ~69 walking.
**Goal**: build **C** (all three switches on, a profile recorded on exactly that lift: an `RX_PGO=gen` build, our scenes with `HOS_PGO`, merged 6:1 with upstream's profile as TICK-040; upstream's hot list kept unless a profile on our scenes clearly helps) and, for a fair comparison, **B** (the base lift with a profile recorded the same way). Interleaved A/B of **A** (the Sync 11 build), B and C. If C or B wins beyond the spread: make it the default the build actually uses (switches on by default in the lifter with `=0` as opt-out; the merged profile and hot list installed where `dev-build.ps1` and the builder take them; in the repo, used by the release, if it holds no game code or data).
**Acceptance**: fps / 1% low / p99 in kokiri-idle, castle-move and hyrule-walk (`--fps-limit 360`, frame generation off, 66 s cut, 6 interleaved runs a variant); a default changed only for a win beyond the spread; HOS_SPEED_LOG ratio ~60 and shots alike A; `test_vfpx_elem` and `test_fpx_slowpath` pass, no new run_all failure; saves identical after every run (gen runs included); the game folder left with the adopted build or the restored backup, starting and playing.
**Branch**: `feature/TICK-045-profile-refresh`

## Result (2026-11-10): C adopted; the three switches on by default with their profile in the repository

**Profiles** (as TICK-040):
- an `RX_PGO=gen` build of each lift (base 617 s; switches on 822 s, every chunk again);
- one `HOS_PGO` run of kokiri-idle, castle-move and hyrule-walk (counters 72-106 s, run_bench save guard);
- `llvm-profdata merge`, then 6:1 with upstream's profile.

Upstream's hot list and link order kept.

**A/B** (6 interleaved runs a variant, order reversed in rounds 4-6; fps / 1% low / p99):

| | A: Sync 11 | B: base lift, own profile | C: switches, own profile |
|---|---|---|---|
| kokiri-idle | 75.3 / 47.6 / 18.0 | 77.2 / 54.2 / 17.2 | 78.0 / 51.1 / 17.2 |
| castle-move | 68.1 / 29.6 / 28.7 | 67.5 / 29.9 / 24.1 | 72.9 / 35.4 / 22.3 |
| hyrule-walk | 70.9 / 37.7 / 21.0 | 70.5 / 36.7 / 22.4 | 74.4 / 34.1 / 21.4 |

- C is +2.7 standing, +4.8 walking (castle-move 70.5-75.2 against A's 64.2-69.8) and +3.5 on the street: beyond the
  spread.
- B stays inside the spread: the profile alone is not the gain.

**Changed** (commit 2a359a0):
- `engine/third_party/a64-kit/lift/a64_lift.py`, `a64_rtgen.py`: the switches are on unless `=0`.
- `profile/`: `hookshot_pieces.prof` is C's profile; `hot_funcs.txt` and `hot_order.txt` are upstream's. `-text` in
  `.gitattributes`. Names, hashes and counts only: no game code or data.
- `engine/scripts/dev-build.ps1`: installs `profile/` into the work folder when it differs.
- `engine/scripts/release_parts.py` (`package --profile`) and `tools/release/make_release.py`: a release ships
  `profile/` in `tools/work`.
- The two tests set the switches explicitly.

**Correctness**: HOS_SPEED_LOG 60.01 / 59.99 (A 60.01 / 60.01); shots alike. run_all: 216 pass, 12 fail (the 10 known, plus
the flaky nup_verify / nup_codec), 33 skip; `test_vfpx_elem` and `test_fpx_slowpath` pass. Saves identical after every run.
No run stayed on the title screen.

**Game folder**: the dev-build of the new defaults, the same code as C. A check at the end of the session (a slower
session) gave A 71.7 / 68.4 against it 73.9 / 72.5 fps (standing / walking). The Sync 11 backup is in
`E:\OOT\HeyListen\build\backup-tick045`. Work folder: the new default lift with the new profile installed (the upstream
files are backed up in `backup-tick045\work-profile`).

**Not done**: no release made with `--profile`. The installed v0.1.0-rc1 builder keeps upstream's profile, which matches
its own older lifter; a build by it puts upstream's profile back in the work folder until the next dev-build.

**Next**: the job start order (TICK-042); FRSQRTE / FRECPE inline and the inline quiet-NaN test, each with a profile refresh.

Docs: engine/docs/perf-audit.md "TICK-045", docs/benchmarks/2026-11-10-tick045-profile-refresh.md.
