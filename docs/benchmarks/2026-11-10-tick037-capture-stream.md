# TICK-037: what holds the frame, and the capture's streaming copy (2026-11-10)

## Setup
- Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11. Build: worktree `feature/TICK-037-bottleneck` (main 7f41ec3 + the change), dev-build, no lift, shaders not re-prepared (no stamp input changed).
- `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360` (limit off), frame generation off (options file), audio off.
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walks and camera turns), `hyrule-walk` (the Market street toward the gate; no Hyrule Field save exists).
- Cut: 66 s after launch (the scene-load hitch lands 56-72 s). The 80-110 s column is the steady part, after the late-load hitches.
- A/B: the same binary, interleaved; off = `RX_VK_CAPTURE_STREAM=0` (memcpy, as before), on = default.

## Where the frame goes (before the change, kokiri-idle, main thread, ms a frame of a ~14 ms frame)
| | ms |
|---|---:|
| waits for the game's job group | 6.4-6.8 |
| guest code | 1.9 |
| waits for an event of the job threads (f_83acfb90) | 1.2 |
| SleepThread poll for the module workers | 0.7 |
| **capture copy (engine)** | **1.1** |
| rest of the engine's submit path | ~0.6 |
| host runtime (mutex lookups, NVN command copies) | ~0.5 |
| GPU / fence waits | < 0.1 |

GPU ~8-10 ms a frame (`RX_VK_PASS_TIMING`, inflated). The replay (Presentation thread) has ~5 ms of slack a frame. Limit: the guest CPU chain. Next: the GPU.

## Results (fps / 1% low / p99, 66 s cut; fps 80-110 s in brackets)
| scene | off (3 runs, mean) | on (3 runs, mean) | change |
|---|---|---|---|
| kokiri-idle | 67.4 / 34.9 / 23.0 ms (68.6) | 70.3 / 35.6 / 22.2 ms (71.6) | +2.9 fps |
| castle-move | 63.0 / 25.7 / 29.0 ms (62.2) | 66.3 / 25.2 / 28.4 ms (65.7) | +3.3 fps |
| hyrule-walk (2 runs each) | 63.4 / 15.1 / 40.1 ms | 65.8 / 18.0 / 51.6 ms (one run had a hitch after 66 s) | +2.4 fps |

Runs: kokiri-idle off 66.5 / 66.4 / 69.4, on 71.8 / 69.8 / 69.4; castle-move off 62.5 / 64.3 / 62.2, on 66.7 / 66.1 / 66.2; hyrule-walk off 62.8 / 63.9, on 65.1 / 66.5.
The standing Market alternates between ~3,050 and ~2,450 draws a frame from run to run. In the same state the gain is +3.0-3.4 fps (3,050 draws) and +2.4 (2,450).
Capture time a submit: 0.31-0.37 -> 0.27-0.29 ms standing, 0.42-0.45 -> 0.32-0.34 ms walking. The main thread's engine time drops from 2.5 to 2.2 ms a frame (sampled).

Raw data (not committed): `build/bench/ab1/*`, profiles `build/bench/prof2-idle`, `prof-move`, `prof-nt-idle`, `gpu-idle`, with the maps of each build in `build/maps-base`, `build/maps-nt`.
Details: engine/docs/perf-audit.md, "TICK-037".
