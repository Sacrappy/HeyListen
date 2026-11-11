# TICK-052: why parallel replay draws cost more (2026-11-11)

## Setup
- Ryzen 7 7800X3D, RTX 5070, Windows 11; Market save; `run_bench.py` 110 s, `--warmup-s 66`, `--fps-limit 360`, frame generation off,
  `RX_VK_FRAME_TRACE=1`; no pass timing, dumps or probes. Harness `build/tick052/ab.py` (one game at a time, saves checked after
  every run), sampler `build/tick052/sample.py` (all-thread sampler 72-104 s), `build/tick052/contend.py` (waits included).
- Binaries: B = main b17b933 (exe 3c981c9b, dll 1b9b4ece); I1-I6 = this branch (dev-build, profile/ checked equal, map kept).

## Sampler (castle-move, B; workers running 17.7%)
Running samples of the 4 workers: ParLock wait 13.1% (gpu_buffer_bind 4.9, texture_view_of 4.9, build_descriptors 1.2,
targets 0.6), driver 20.6%, program_for 14.2%, texture_view_of 12.2% incl., gpu_buffer_bind 9.3% incl., heap ~1.3%.
With the fixes (I6): ParLock 8.7% (the per-submission touches), gpu_buffer_bind 4.7%, texture_view_of 7.6%, driver 23%, program_for 16.5%.

## RX_VK_PARLOCK_STATS (castle-move, one run each, a command buffer)
| | B-equivalent (I1/I3) | fixes (I4/I5) |
|---|---:|---:|
| ParLock takings | ~1,050 | ~690 |
| waiting / holding (ms, all threads) | 0.45 / 0.35 | ~0.27 (spin) / ~0.3 |
| bind page range (aliased pool entry) | 324-347 | 6-18 |
| view destroyed / slot destroyed | 206-240 / 500-600 | 4-30 / 20-74 |
| touches (first check in a submission) | ~300 | ~390-520 |
| first 32 draws of a segment (us a draw) | 6.9-7.4 | 6.8-7.2 |
| draws 64-191 / 192+ (us a draw) | 3.5-4.1 / 2.8-3.1 | 3.3-3.9 / 2.4-3.0 |
Affinity (`RX_VK_PARALLEL_AFFINITY=1`): view misses 260 -> 165, first draws unchanged. `RX_VK_PARALLEL_IDEAL=1`: no change.

## Screen 1 (castle-move, 3 interleaved runs, I5)
| variant | fps | frame prep | per 1000 draws | slowest | PT wait | draws |
|---|---:|---:|---:|---:|---:|---:|
| base (switches 0) | 71.1 | 8.23 | 3.34 | 1.32 | 1.45 | 2466 |
| fixes | 72.3 | 7.66 | 3.33 | 1.24 | 1.34 | 2301 |
| fixes + affinity | 70.9 | 8.37 | 3.38 | 1.20 | 1.49 | 2473 |

## Confirm (B against I6 with GONE_RING=1 TL_POOLS_WIDE=1 PARLOCK_SPIN=4000; stopped at 29 of 36 runs)
| scene | variant | fps per run | mean fps | 1% low | p99 | frame prep | slowest | PT wait |
|---|---|---|---:|---:|---:|---:|---:|---:|
| castle-move | B | 69.8 70.8 72.6 71.2 70.9 | 71.1 | 35.5 | 23.1 | 8.12 | 1.32 | 1.44 |
| castle-move | fixes | 76.8 70.0 72.4 72.4 69.2 | 72.2 | 35.1 | 22.7 | 7.91 | 1.24 | 1.36 |
| hyrule-walk | B | 75.9 72.3 75.4 73.5 | 74.3 | 39.7 | 20.9 | 7.75 | 1.20 | 1.34 |
| hyrule-walk | fixes | 73.5 72.4 73.3 73.4 70.0 | 72.5 | 37.4 | 21.9 | 7.86 | 1.15 | 1.29 |
| kokiri-idle | B | 79.7 77.0 75.1 82.5 74.7 | 77.8 | 50.6 | 17.7 | 7.40 | 1.16 | 1.33 |
| kokiri-idle | fixes | 76.8 77.3 77.2 (r4 invalid) 77.0 | 77.1 | ~53 | ~17.6 | 7.29 | 1.13 | 1.31 |
kokiri-idle fixes r4 stayed in the menus (0 frames after 66 s, no fault): left out. Draws' CPU per 1000 draws: 4.19 -> 3.89,
3.94 -> 3.74, 3.79 -> ~3.4.

## Decision
Not adopted: frame prep -0.21 / +0.11 / -0.11 ms, inside the spread; fps inside the spread. Switches off by default. Game folder:
main build restored (exe 3c981c9b, dll 1b9b4ece), saves equal the backup, no game process left.
