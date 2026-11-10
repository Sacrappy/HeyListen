# TICK-043: the engine's submit on the main thread, and copying the capture ahead (2026-11-10)

## Setup
- Hardware: Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11.
- Builds: worktree `feature/TICK-043-async-submit` (main f29a4b3, Sync 11), dev-build work folder (`%LOCALAPPDATA%\HookshotBuilder`, the default lift with upstream's hot list and profile), no lift, 8 jobs, results in `build/tick043/bin/<variant>` (`build/tick043/vbuild.ps1`). No shader stamp input changed (only `vk_memory.cpp`, the new `capture_merge.h`), so `--prepare-shaders` was not run.
  - `base`: the game folder's Sync 11 exe and dll (backup in `E:\OOT\HeyListen\build\backup-tick043`).
  - `v3`: this branch; the switches below are env only, and off by default.
- Runs: `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off, cut at 66 s; each variant's exe and dll copied into the game folder before its run (`build/tick043/ab.py`). One game at a time; after every run no game process was left and the saves were equal to the backup.
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walking and turning).

## The main thread's submit path (default build, HOS_SAMPLE_ALL + HOS_SAMPLE_NS at 72-108 s, sampler on)

| ms a frame | standing (71.8 fps) | walking (66.2 fps) |
|---|---:|---:|
| renderer on the main thread | 1.81 | 2.36 |
| capture copy (main's share + waiting for the helpers) | 0.99 | 1.06 |
| mark scan (collect_in) | 0.23 | 0.23 |
| take_capture (of it: GPU wait for late pages) | 0.21 (0.11) | 0.53 (0.22) |
| memory_collect_written | 0.06 | 0.15 |
| pool-range refresh | 0.03 | 0.08 |
| recording of main's own submits | ~0.1 | ~0.12 |
| other | ~0.1 | ~0.1 |

Per submit (log): standing 0.28 ms (scan 0.06, copy 0.20), 1,669 pages / 6.5 MB; walking 0.34 ms (scan 0.08, copy 0.25), 1,977 pages / 7.7 MB; six submits a frame. "GPU frame prep" (overlay) = the replay's run_submit time summed over all threads, 6.9 ms standing / 8.9 walking, ~90% on the Presentation Thread (63% of its samples in the renderer), not on the main thread's path.

With `RX_VK_CAPTURE_PRECOPY=1` (walking): ~1,500 of ~1,600 pages a submit copied ahead, ~120 copied at the submit, ~100 copied again there; main thread renderer share 15.6% -> 11.6% (~0.6 ms a frame); the thread copied ~700 pages a submit more than once, ~180 buffers grown per 600 submits, kept captures ~500 MB (from ~280).

## A/B (one binary `v3`, env only, interleaved; fps / 1% low / p99 ms)

| scene | variant | runs (fps) | mean fps / 1% low / p99 |
|---|---|---|---|
| kokiri-idle | default | 77.0 72.4 74.4 73.4 77.5 77.9 | **75.4 / 51.6 / 18.0** |
| kokiri-idle | `RX_VK_CAPTURE_PRECOPY=1` (250 us) | 68.9 72.8 72.6 | **71.4 / 51.7 / 18.2** |
| kokiri-idle | `RX_VK_CAPTURE_PRECOPY=1 RX_VK_PRECOPY_US=2000` | 73.0 72.4 74.5 | **73.3 / 53.0 / 17.5** |
| kokiri-idle | `RX_VK_CAPTURE_SCAN_SPLIT=1` | 71.6 76.4 71.0 | **73.0 / 47.6 / 18.0** |
| castle-move | default | 68.6 68.2 67.6 68.5 69.2 70.6 | **68.8 / 36.0 / 22.6** |
| castle-move | `RX_VK_CAPTURE_PRECOPY=1` (250 us) | 66.7 66.0 (third run: stayed on the title, no frames measured) | **66.3 / 29.8 / 23.3** |
| castle-move | `RX_VK_CAPTURE_PRECOPY=1 RX_VK_PRECOPY_US=2000` | 65.4 65.9 70.2 | **67.2 / 35.5 / 23.3** |
| castle-move | `RX_VK_CAPTURE_SCAN_SPLIT=1` | 68.9 70.9 69.5 | **69.8 / 35.4 / 21.9** |

Rounds: `build/tick043/ab1` (default, 250 us, split: 3 rounds) and `build/tick043/ab2` (default, 2000 us: 3 rounds). Copying ahead loses 2-4 fps; the scan split is inside the 2-4 fps spread (+1.0 walking, -2.4 standing). Not adopted.

## Picture and game speed (`build/tick043/shotruns`, shots in `build/tick043/shots`, not committed)

`RX_VK_SHOTS` every 1200 presents and `HOS_SPEED_LOG=1`, one run each:

| run | fps | game speed ratio (last 40 s: mean, min-max) | Market shots |
|---|---:|---|---|
| base, standing | 78.1 | 60.01 (59.77-60.29) | ground, fountain, people, shadows, HUD |
| v3 default, standing | 77.6 | 59.99 (59.71-60.30) | the same |
| base, walking | 73.0 | 60.05 (59.29-60.73) | the same along the walk |
| v3 default, walking | 72.9 | 60.01 (59.32-60.81) | the same |
| v3 + PRECOPY, walking | 68.0 | 60.01 (57.43-62.96) | the same: no stale or flickering textures seen |

The shots show other moments of the scene (not pixel-equal), and only the last 2-3 of each run are in the Market (the title runs at 360 fps).

## Unit tests
run_all (llvm-mingw on PATH, `-X utf8`): 215 pass, 12 fail, 33 skip. The 12 are the 10 known failures plus the flaky test_nup_verify / test_nup_codec. New: `tests/test_capture_merge.py` passes, and it fails if either recopy rule is removed.

## Game folder
Nothing was adopted, so the Sync 11 exe and dll were restored from `backup-tick043` (md5 29d96559... / 03402eed..., as before). No prepared shader file was touched.
Smoke afterwards (restored folder, kokiri-idle): 77.8 fps / 56.1 / 16.9 ms; saves identical, no game process left.
