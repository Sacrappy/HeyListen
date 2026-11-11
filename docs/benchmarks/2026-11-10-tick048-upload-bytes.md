# TICK-048 on PC 3050: the mirror syncs' chunk diff (2026-11-10/11)

PC 3050: i5-10400F, RTX 3050 8 GB on PCIe 3.0 x4, driver 591.86, now with Above 4G Decoding and Resizable BAR (BAR1 8192 MiB).
Market save by the fountain, `pcbench.py` (run_bench.py `--duration 110 --warmup-s 66 --fps-limit 360`), frame generation off,
SSH session 0. Only before/after on this machine counts.

## Builds
| name | exe | dll |
|---|---|---|
| sync11 | Sync 11 (md5 29d96559...) | Sync 11 (03402eed...) |
| tick045 | TICK-045 (223765ae...) | TICK-045 (86175aee...) |
| t48 | TICK-045's | this branch, built on PC 3050 with `RX_VK_ONLY=1` (`nx2/build.py --standalone`, its copies of the builder's llvm-mingw and cpm, 207 files in 76 s), diff off by default; the A/B's switch is `RX_VK_MSYNC_DIFF=2` |
| t48b | TICK-045's | the branch's final dll (diff on by default) |

The game folder holds the Sync 11 exe and dll again at the end (as found).

## Step 0: TICK-045 and ReBAR
5 interleaved rounds, sync11 vs tick045 (fps / 1% low / p99 means):

| scene | sync11 runs | sync11 | tick045 runs | tick045 |
|---|---|---|---|---|
| kokiri-idle | 24.1 25.8 25.3 25.7 27.5 | 25.69 / 19.2 / 47.3 | 26.1 27.2 25.7 26.0 25.1 | 26.03 / 17.9 / 49.4 |
| castle-move | 24.2 24.3 23.8 23.0 23.6 | 23.75 / 10.9 / 80.3 | 24.9 23.7 24.7 25.0 24.3 | 24.51 / 11.0 / 78.0 |
| hyrule-walk | 24.0 25.7 26.4 26.1 26.7 | 25.78 / 17.6 / 49.8 | 29.2 25.3 26.6 28.9 26.7 | 27.34 / 19.5 / 44.2 |

ReBAR: Sync 11 before the reboot 26.2 / 23.0 / 26.3, now 25.7 / 23.75 / 25.8: no effect.
PCIe link after the 66 s cut, every run: Gen 3, width 4 (Gen 1 x4 at idle).

## What the syncs copy (t48, `RX_VK_MSYNC_DIFF=1`, `RX_VK_PASS_TIMING=1`)
A "part" is a submission part (`memory_end_submission`), ~30 a frame at the Market.

| | kokiri-idle | castle-move |
|---|---:|---:|
| synced a part | 0.86 MB | 0.74 MB |
| at a binding / presync / in a segment | 100% / 0 / 0 | 100% / 0 / 0 |
| from an earlier submit's capture / this submit's / guest memory | 99% / 1% / 0 | 97% / 3% / 0 |
| unchanged since the page's last sync, 256 B chunks | 0.73 MB (85%) | 0.63 MB (85%) |
| at 512 B / 1 KB / 2 KB / whole pages | 0.69 / 0.63 / 0.54 / 0.40 MB | 0.60 / 0.55 / 0.48 / 0.37 MB |
| pages hashed / hashes that held / written by the GPU since | 220 / 215 / 5 | 190 / 183 / 6 |
| hashing (replay thread) | 0.13 ms a part | 0.12 ms a part |

Largest pools (MB a part copied / unchanged): 0x1054000000 (16 MB, flags 0) 0.105 / 0.084; 0x1145000000 (48 MB, flags 0xa1)
0.088 / 0.082; 0x1056000000 0.074 / 0.060; 0x105d000000 0.071 / 0.067; then ~0.05-0.06 each for 0x1051-0x1057, 0x104e, 0x105c.

GPU time a frame (pass timing, inflated by its timestamps):

| | base idle | diff idle | diff + rb ranges idle | base move | diff move | diff + rb ranges move |
|---|---:|---:|---:|---:|---:|---:|
| all | 35.79 ms | 26.87 | 24.65 | 36.30 | 27.87 | 26.09 |
| msync | 11.26 ms, 32.1 MB | 1.61, 4.4 | 1.61, 4.4 | 10.39, 29.5 | 1.63, 4.4 | 1.52, 4.1 |
| rbrefresh | 2.01 ms, 5.4 MB | 2.16, 5.8 | 0.77, 1.9 | 2.01, 5.4 | 1.99, 5.4 | 0.69, 1.7 |

## A/B (t48, interleaved; fps / 1% low / p99)
Screen (castle-move, 3 runs each): base 23.5 24.7 24.2 -> 24.11; diff 27.7 26.4 27.1 -> 27.05; diff + rb ranges 27.0 27.0 28.0 -> 27.35.
The rb ranges add nothing on top of the diff: not taken further.

Confirmation, base vs diff, 5 runs a scene:

| scene | base runs | base | diff runs | diff |
|---|---|---|---|---|
| kokiri-idle | 25.3 26.5 26.3 26.3 25.6 | 26.01 / 20.5 / 45.9 ms | 31.3 31.1 29.1 29.6 29.5 | 30.11 / 23.5 / 40.0 ms |
| castle-move | 23.5 24.7 24.2 25.5 24.0 | 24.36 / 10.8 / 77.4 ms | 27.7 26.4 27.1 26.8 26.6 | 26.91 / 10.7 / 80.7 ms |
| hyrule-walk | 26.0 27.2 24.0 24.6 26.8 | 25.71 / 16.8 / 50.1 ms | 32.3 28.7 28.8 27.7 29.4 | 29.37 / 16.1 / 53.6 ms |

## Correctness
Shots every 300 presents (`RX_VK_SHOTS`, `RX_VK_SHOT_EVERY=300`), base / diff / diff + rb ranges, standing and walking, picked by
time (~75-105 s): ground, fountain, trees, people, shadows, exposure and HUD as in the base runs; nothing stale or flickering in
the shots seen (not pixel-equal: other moments). `HOS_SPEED_LOG`: steady ratio 60.04 (base), 60.08 / 59.74 (diff), 60.01 (diff + rb
ranges walking); one diff + rb ranges standing run had a dip of three seconds (16, 25, 7) early in the window, at the scene's
load, then 59.1-61.7. After every run: no HeyListen.exe, save identical to `user-save-backup`.

## When fps settle
Before ~50-62 s after launch the game is on its boot, title and save load (fps 100+ at the 360 cap); the scene starts there.
fps 5-15 s after it are 0.90 (idle, street) to 0.99 (walking) of the 66-110 s figure, 15-25 s after it 0.95-1.04. So a warm-up
of 30 s would measure the title screen on PC 3050: the 66 s cut stays. `pcbench.py` now takes `DURATION=<s>` (default 110) to
shorten the measured part instead, and a `C:\OOT\runs\STOP` file to stop a series between runs.

## Not verified
The main PC (not run there: see the ticket's list), other GPUs and drivers, Kokiri Forest and Hyrule Field, long sessions (the
hash table grows with the guest memory synced: 68-73 MB here), runs on the interactive desktop.
