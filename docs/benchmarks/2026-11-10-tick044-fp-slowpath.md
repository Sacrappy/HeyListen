# TICK-044: the FP slow paths of the Market's model code (2026-11-10)

## Setup
- Hardware: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11.
- Builds: worktree `feature/TICK-044-fp-slowpath` (main e548f92, Sync 11), dev-build work folder (`%LOCALAPPDATA%\HookshotBuilder`, upstream's hot list and profile), 14 jobs, results in `build/tick044/bin/<variant>` (`build/tick044/vbuild.ps1`). No shader stamp input changed, so `--prepare-shaders` was not run.
  - `base`: main's lift (the default; the game folder's Sync 11 build, backup in `E:\OOT\HeyListen\build\backup-tick044`).
  - `fp`: lifted with `A64_FPX_SLOWPATH=1`.
  - `fpfma`: lifted with `A64_FPX_SLOWPATH=1 A64_FPX_FMA3=1`.
  - The lifted code's object key is `35c564f1c7` in all three (no lifted function changed); only a64_fpx.h, a64_suyu.c (and with FMA3 a64_suyu.h) differ.
  - `probe`, `probe2`, `probefp`: diagnostic builds (the counting `a64_suyu.c` of `build/tick044/fpe_probe.py`), never timed.
- Runs: `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off, cut at 66 s; each variant's exe and dll copied into the game folder before its run (`build/tick044/ab.py`). One game at a time; after every run no game process was left and the saves were equal to the backup.
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walking and turning).

## Slow-path counts (probe builds; calls a second over the last 30 s)

| | base, walking | base, standing | `fp`, walking |
|---|---:|---:|---:|
| all out-of-line FP calls | 5.37 M (~81,000 a frame at 66 fps) | 3.29 M (~44,000 a frame at 74 fps) | 2.12 M |
| exact bodies with a quiet NaN operand (FMUL / FSUB / FADD) | 3.17 M | 1.41 M | 0 |
| FDIV S, an infinity of finite operands with DZC / OFC raised | 0.33 M | 0.45 M | 0 |
| FRSQRTE / FRECPE (always out of line) | 1.79 M | 1.32 M | 2.03 M (table fast path) |
| FSUB with a subnormal operand under FZ (IDC raised) | 0.04 M | 0.06 M | 0.05 M |
| FADD with an infinite operand | 0.02 M | 0.03 M | 0.02 M |
| other (gate closed, midpoints, tiny results) | < 0.02 M | < 0.02 M | < 0.02 M |

FPSR at the call 0x9f in 89-95% of the calls, FZ set in 99.6%. Stacks (1 in 256): f_81c86720, f_81c7f880, f_81b8c000, f_81c7d2e0, f_81c7bd00, f_81ce6500, f_81c88940, f_81b5a3c0 (80166100's model subsystem) for the NaN lanes and FRSQRTE; f_8009bb80 for FRECPE; f_820e2520, f_81779980 for FDIV. Logs: `build/tick044/probe-run`.

## A/B (fps / 1% low / p99 ms; `build/tick044/ab1` rounds 1-3 base first, `ab2` rounds 4-6 base last)

| scene | variant | runs (fps) | mean fps / 1% low / p99 |
|---|---|---|---|
| kokiri-idle | base | 78.7 78.3 74.7 76.6 73.2 75.3 | **76.1 / 48.6 / 17.6** |
| kokiri-idle | fp | 75.3 75.1 73.9 75.4 74.1 74.3 | **74.7 / 49.8 / 18.0** |
| kokiri-idle | fpfma | 76.7 75.2 73.7 76.5 73.6 74.0 | **75.0 / 46.9 / 18.4** |
| castle-move | base | 66.4 68.1 71.9 68.9 67.9 71.4 | **69.1 / 35.5 / 23.0** |
| castle-move | fp | 68.8 71.4 70.6 68.3 72.2 68.8 | **70.0 / 36.3 / 22.7** |
| castle-move | fpfma | 72.0 72.9 72.8 69.7 70.4 68.9 | **71.1 / 37.1 / 22.2** |

Walking +0.9 / +2.0 fps, standing -1.4 / -1.1: inside the 2-4 fps spread. Not adopted.
One fpfma kokiri-idle run (round 3) stayed on the title screen (338 fps, 50 draws a frame, 0 frames measured; `ab1/title-stall-fpfma-kokiri-idle-r3`); it was rerun and is not counted.

## Picture and game speed (`build/tick044/shotruns`, `shotruns2`; shots in `build/tick044/shots`, not committed)

`HOS_SPEED_LOG=1` and `RX_VK_SHOTS` every 1200 presents, base against fpfma (both switches), one run each:

| run | fps | game speed ratio (last 40 s: mean, min-max) | Market shots |
|---|---:|---|---|
| base, standing | 74.1 | 60.01 (59.73-60.26) | ground, fountain, people, shadows, HUD, the girl's dialogue |
| fpfma, standing | 77.4 | 60.02 (59.71-60.64) | the same |
| base, walking | 68.4 | 60.01 (58.51-62.33) | the same along the walk |
| fpfma, walking | 70.9 | 60.02 (59.44-60.67) | the same: no wrong geometry, animation or textures seen |

The shots show other moments of the scene (not pixel-equal). A first pair of runs (`shotruns`) wrote no PNG (the shot folders did not exist yet); its speed ratios were 60.00 / 60.01 standing and 59.47 / 59.96 walking (the base walking mean pulled down by one 45.2 second during a hitch).

## Unit tests

run_all (llvm-mingw on PATH, `-X utf8`): 216 pass, 10 fail, 35 skip. The 10 are the known failures (test_nup_read_name, test_nup_paths, test_fps_rate, test_fpx_sticky, test_fps_overlay_order, test_precompile_compute_guard, test_present_trace, test_present_semaphores, test_parallel_rb_refresh, test_ub_threads); test_nup_verify / test_nup_codec passed this time. New: `tests/test_fpx_slowpath.py` passes (12 s in the suite); `tests/test_vfpx_elem.py` passes, also with `A64_FPX_SLOWPATH=1`. 40 M cases a march by hand: 0 mismatches (`build/tick044/test-40M.txt`).

## Game folder

Nothing adopted: the Sync 11 exe and dll restored from `E:\OOT\HeyListen\build\backup-tick044` (md5 29d96559... / 03402eed..., as before). The builder's work folder was lifted again without the switches: a64_fpx.h and a64_suyu.c byte for byte as before (md5 a7a50abc... / 61e7ce1b...), object keys as base (headers c1e9fe25, lifted code 35c564f1), nothing recompiled.
Smoke afterwards (restored folder, kokiri-idle): 75.0 fps / 50.2 / 17.8 ms; saves identical, no game process left.
