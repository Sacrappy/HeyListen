# TICK-038: resource use at the Market (2026-11-10)

## Setup
- Ryzen 7 7800X3D, 32 GB, RTX 5070 12 GB, Windows 11. Build: worktree `feature/TICK-038-resources` (main 21f47ac + the change), dev-build, no lift, shaders not re-prepared (no stamp input changed).
- `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off, 66 s cut; `tools/bench/ressample.py` beside it (memory, threads, nvidia-smi, address-space scan).
- A/B in one binary: off = `RX_VK_LAZY_COMPUTE=0 RX_VK_CS_MODULE_DROP=0` (as before), on = defaults. Round 1 with `RX_VK_RES_LOG=1` and shots every 1200 presents; rounds 2-5 without.

## Before (kokiri-idle at 95 s)
| | committed | resident |
|---|---:|---:|
| guest memory | 11.2 GB | 7.75 GB |
| NVIDIA driver arenas (5 x ~1 GB) | 5.18 GB | 5.0 GB |
| driver commit beside VRAM (32-63 MB regions) | 5.5 GB | 0.4 GB |
| exe image | 1.03 GB | 0.27 GB |
| captures (25 kept + 12 spare) | 0.47 GB | |
| process | 23.7 GB private | 14.2 GB working set |

VRAM by kind: textures 1.27 GB, render targets 1.0-1.1, mirrors 1.52, write-back shadows 0.22, BC scratch 0.16, staging 0.08-0.14 and streams 0.10 (BAR), the driver's other ~1.03 GB.
The precompile built 7808 compute and 2827 graphics pipelines at each start; the Market binds ~1,110 of them (~110 compute).

## After (means; fps / 1% low / p99, working set, private)
| scene | off | on |
|---|---|---|
| kokiri-idle (3 runs each) | 69.7 / 30.7 / 23.1 ms, 14.20 GB, 23.68 GB | 71.4 / 35.2 / 21.2 ms, 11.90 GB, 21.23 GB |
| castle-move (5) | 66.6 / 26.3 / 28.0 ms, 15.51 GB, 25.70 GB | 66.2 / 26.7 / 29.2 ms, 13.59 GB, 23.74 GB |
| hyrule-walk (2) | 65.2 / 17.9 / 36.5 ms, 14.85 GB, 25.11 GB | 66.1 / 27.2 / 29.9 ms, 12.98 GB, 23.19 GB |

Driver arenas 5.0 -> 2.8 GB resident. VRAM "other" 1033-1048 -> 840-918 MB; total VRAM within noise. Start-up shaders 7.7-8.5 -> 2.8-5.2 s. CPU 5.9 cores both. In-play compute builds: 106-117 at the load, ~2.5 ms each, none left out.

Raw data (not committed): `build/tick038/ab/r*-*` in the worktree (bench.json, res.csv, threads.csv, regions.txt, smi.csv; shots in r1-*).
Details: engine/docs/perf-audit.md, "TICK-038".
