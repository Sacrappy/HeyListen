# TICK-039: the job-group wait at the Market (2026-11-10)

## Setup
- Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11. Build: worktree `feature/TICK-039-job-chain` (main 6702fee + the trace), dev-build, no lift, shaders not re-prepared (no stamp input changed).
- `tools/bench/run_bench.py`, windowed, 110 s, `--fps-limit 360`, frame generation off (options file), audio off; cut 66 s. Trace runs 105 s with the trace window at 86-91 s.
- Save: Castle Town Market by the fountain. Scenes: `kokiri-idle` (standing), `castle-move` (walking and turning).

## The main thread's job-group wait (lr 0x80080188), its critical path (`tools/bench/jobtrace.py`)
| | kokiri-idle | castle-move |
|---|---:|---:|
| wait, ms a frame | 7.0 | 7.8-8.1 |
| job execution on the path | 96.8% | 95.8-96.7% |
| wake latency on the path | 3.0% (0.21 ms) | 2.9-3.8% (0.24-0.30 ms) |
| virtual-core waits / polls on the path | 0.1% / 0.1% | 0.2% / 0.1% |
| path work: guest code / runtime / system (samples) | 84.8 / 8.1 / 6.9% | 80.1 / 10.3 / 9.6% |
| six executors busy / one or none, share of the wait | 44% / 39% | 41% / 36% |

## Env experiments, castle-move (fps / 1% low / p99 ms)
| config | runs | mean fps |
|---|---|---:|
| default | 65.4 / 28.4 / 28.9; 67.8 / 30.2 / 27.3; 66.0 / 26.6 / 29.3 | 66.4 |
| HOS_SPIN_US=0 | 63.8 / 26.1 / 29.6; 64.9 / 27.3 / 27.9 | 64.4 |
| HOS_SPIN_US=5 | 62.4 / 24.2 / 31.8; 62.7 / 27.5 / 30.5 | 62.6 |
| HOS_SPIN_US=100 + HOS_SPIN_ALWAYS=1 | 61.9 / 25.1 / 31.5 | 61.9 |
| HOS_PIN_BY_CORE=1 | 36.4 / 7.8 / 115.8 | 36.4 |

## Build check, kokiri-idle, interleaved (old = the TICK-038 game-folder build, new = this build, trace off)
| | run 1 | run 2 | mean |
|---|---|---|---:|
| old | 74.0 / 35.0 / 22.0 | 71.1 / 38.8 / 20.4 | 72.6 / 36.9 / 21.2 |
| new | 70.2 / 38.7 / 21.7 | 74.8 / 40.5 / 19.7 | 72.5 / 39.6 / 20.7 |

No default changed: the new build is the old one plus record points that cost a load and a branch while the trace is off.

Raw data (not committed): `build/tick039/` in the worktree (jt-*.txt traces, samp-*.txt samples, maps-trace/, ab/ runs).
Details: engine/docs/perf-audit.md, "TICK-039".
