# TICK-053 — PC 3050: +20 fps by ending the CPU's per-frame wait for the GPU and cutting GPU time · model: Fable 5.1 · after TICK-051
**Why**: PC 3050 (i5-10400F, RTX 3050 8 GB on PCIe 3.0 x4) runs the Market at 25-30 fps. TICK-051 measured the game's thread waiting
~10 ms a frame for the previous frame's GPU work (the capture's wait for its late pages), and the GPU busy only part of the frame.
**Goal**: +20 fps on PC 3050 (castle-move) with the picture identical and the game speed right; each change behind a switch.
**Branch**: `feature/TICK-053-gpu-sync` (built and measured on PC 3050, over SSH: session 0, no DWM)

## Result (2026-11-11): the frame is sync-bound, not GPU-bound; letting the CPU run ahead meets a stall on the queue that is not ours

**Screen (castle-move, 3 interleaved runs, PC 3050)**: main 28.21 fps (1% low 14.4, p99 56.6 ms), branch defaults 28.64 (14.9, 54.4),
`RX_VK_RB_RANGES=1` 27.97 (13.8, 53.9). Nothing moves: the +20 fps is not reached on PC 3050.
**Defaults chosen**: `RX_VK_LATE_MERGE` off, `RX_VK_ACQUIRE_FENCE` off, `RX_VK_RB_RANGES` off (no gain: -0.7 fps against the new defaults);
GPU gap/upload/blit timestamps and the trace columns on. `C:\OOT\game` holds the branch build (`t53f`, = `C:\OOTuilds	ick053`).
**Correctness**: default speed ratio ~60.0 (59.3-60.5); with `RX_VK_LATE_MERGE=1` 55.8-63.4 and several Market shots are white
with only the HUD (base has none): the merge also needs a look. See `docs/benchmarks/2026-11-12-tick053.md` (screen, shots, the
desktop-check instructions).

## Where a PC 3050 frame goes (main 4b170f0, castle-move, 110 s runs, 66 s cut)

| | ms a frame | source |
|---|---:|---|
| frame (28.5 fps) | 35.1 (p50 34.2) | `RX_VK_FRAME_TRACE` |
| the game's thread in submits (the replay records on it: `RX_VK_ASYNC` is off) | 27.4 | `submit_ms` |
| of it: the capture's wait for the previous frame's GPU work (late pages) | 8.8, once a frame | `late_wait_ms` |
| of it: recording (draws 20.3 summed over the parallel segments, descriptors 7.8, commands 7.7, programs 3.1) | ~18.6 | |
| GPU busy | 63% = ~22 ms | nvidia-smi, 1 s samples |
| GPU work, profiled (inflated by the timestamps) | 25.1: recordings 19.7 (passes 12.4, dispatches 4.5, barriers 1.1, write-backs 0.75), uploads 5.4 (rbrefresh 1.9, msync 1.5, mmake 0.8, ASTC 0.65) | `RX_VK_PASS_TIMING` |

The chunk diff is on (`vk: mirror syncs: PCIe link Gen3 (maximum) x4 (current; card maximum x16) (3.9 GB/s)`, `chunk diff on by
default`): the scatter is 4-6 MB a frame, 1.5 ms. The engine's own GPU work is ~5.5 ms of the ~22; the game's passes and dispatches
~17. So the GPU has ~13 ms of idle a frame: the game's thread drains the GPU at its first submit of every frame (the late-page wait
in `take_capture`, on the submitting thread), then records the frame's six submissions while the GPU runs them. Serial: CPU + GPU,
not max(CPU, GPU). Cutting GPU time alone cannot give +20 fps here (TICK-048 cut 9 ms of GPU and gained 3 fps); the overlap can.

## What the overlap does (measured, one run each unless said)

| run | fps | 1% low | p99 ms | note |
|---|---:|---:|---:|---|
| main 4b170f0 (m0) | 28.5 | 15.5 | 50.8 | GPU 63% |
| `RX_VK_CAPTURE_DEFER=1`, no pacing wait (m1) | 29.3 | 11.0 | 71.4 | **24-26 ms frames (40 fps) for 60 of 110 s, 66 ms frames for 16 s** |
| defer, slack 2 (m1) | 26.9 | 15.3 | 53.0 | the wait moves to the game's thread: no overlap (TICK-051's finding) |
| defer, slack 6 / 9 (m3) | 22.1 / 21.1 | 10.0 / 8.2 | 80 / 87 | the 66 ms mode most of the run |
| defer + `RX_VK_IMMEDIATE=1` (m5) | 22.3 | 9.8 | 79.5 | the swapchain really immediate: no change |
| `RX_VK_LATE_MERGE=1` (m8, m9) | 23.4 | 12.7 | 55.8 | half the takes merge (no wait), the rest wait once for a page with no shadow; the 66 ms mode too |
| merge + `RX_VK_ACQUIRE_FENCE=1` (m10) | 22.1 | | | the acquire's fence signals in 0.00 ms: the image is always free |
| merge + fence + `RX_VK_MIRROR_MB=1024` (m11) | | | | VRAM 76% of the allowance: the gaps stay |

### The 66 ms mode
Whenever the game's thread gets ahead of the GPU across a frame boundary (any slack of 6 or more, or no wait), stretches of the
walk run at exactly ~66 ms a frame: nvidia-smi says the GPU is 99% busy at 62-63 W (75 W when it renders at 40 fps), clocks
1905-1935 MHz, P0, no throttle; `dmon` says SM 99%, memory 8-12%, PCIe rx 20-85 MB/s (350-720 in the fast phase). The game's
thread sits 55% of its time in `vkWaitSemaphores` (wait_recorded: its own fence waits, 10 values ahead of the GPU); CPU use falls
from 55-65% to 30%. The recordings' own GPU timestamps say the recordings take 5-8 ms a frame and the upload command buffers ~2 ms
(new trace columns `gpu_up_ms`, `gpu_gap_ms`): **the queue is idle on our work 27-50 ms a frame, in one gap a frame**. WDDM's
counters (typeperf): our process's 3D engine 27-74%, the System process on the Copy engine 1-7% (paging), dedicated memory 7.2-7.4 of
8 GB, 3.4-3.6 GB shared. Ruled out one at a time: the swapchain's acquire (a fence on the presentation thread signals at once:
`RX_VK_ACQUIRE_FENCE`), the present mode (immediate), the present queue (off by default), VRAM headroom (mirrors 1 GB: gaps stay),
clocks, the game's event ordering (no 50 ms holds; more captures at CPU-event waits, 2346 against 185 in 110 s), the batched
submits (off). Not yet placed: what the queue waits for in that gap (the blit's own timestamps come next).

## Changes (all behind switches)
- `RX_VK_LATE_MERGE` (default: see the A/B): a late page (one a submission still on the GPU writes back when the capture copies
  it) is merged into its mirror word by word against a 4 KB shadow of what the mirror was last synced with, instead of waiting for
  the GPU; the GPU's words stay the mirror's. A page with no shadow yet waits as before, once. `vk_memory.cpp` "the late pages
  merged"; log line `vk: late pages merged`. `tests/test_late_merge.py`.
- `RX_VK_ACQUIRE_FENCE=1` (off): the swapchain image's acquire signals a fence waited for on the presentation thread; the blit is
  submitted with no wait on the queue. Log `vk: acquire waits`.
- The frame trace: `gpu_up_ms` (the upload command buffers' GPU time), `gpu_gap_ms` (the queue's gaps between recordings), and
  the log lines `vk: GPU time ... uploads ... gaps between recordings` and `vk: the blit on the GPU`.

## Correctness
Default build: speed log ~60.0, saves equal after every run; the merge's shots show white world-less frames (benchmark doc). Unit tests: `test_late_merge` and the suite on PC 3050 passed (`C:\OOTuns	53-test_*.log`).

## Not verified
- Runs in session 0 only (SSH): the presentation engine's timing there is not a desktop's; the 40 fps run-ahead stretches need the
  desktop check.
