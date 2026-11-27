# The real time per frame on the PC 3050 (stream T2, 2026-11-16)

Question: how much of a frame does the GPU really work, how much does it sit idle, and what do the CPU threads do, in
kokiri-forest, hyrule-field and castle-move, so that the path to 60 fps is known to be GPU-side or CPU-side.

Answer in one line: **at 2560x1440 on the RTX 3050 the GPU is busy 87% of the frame in the two saves (74-85% in castle-move) and
that work is real, so the GPU is the limit; the earlier "6.5-8.6 ms of GPU work" came from a build that did not time the
frame's heavy segments.**

## Setup

PC 3050 (i5-10400F, RTX 3050 8 GB, 2560x1440, P0, 1912 MHz, 71-78 W), whole machine locked as `t2`, slot a, build `t2a` = main
at e7211a7a (`RX_VK_EARLY_GPU=1` by default) plus one logging-only change (below). `HOS_OUTPUT_RES=1 HOS_DYNRES=0 RX_VK_FRAMEGEN=0`
in every run, `--fps-limit 360`, 110 s runs, one throwaway warm run first. Zones: kokiri-forest and hyrule-field with `--save`
(scene kokiri-idle), castle-move (scripted). Per zone, two runs of each: plain (`RX_VK_FRAME_TRACE`, `HOS_THREAD_TIMES=5`),
`RX_VK_PASS_TIMING=1`, timeline (`RX_VK_TL_S=58 RX_VK_TL_FRAMES=5`), plus one sampler run (`HOS_SAMPLE_ALL`, `scripts/crit.py`).
Raw runs and scripts: `E:\OOT\t2-scratch` (`agg.py`, `tlsum.py`, `tlavg.py`).

New switch: `RX_VK_PASS_TIMING=1` now also ends its report with a "T2 queue picture" (vk_gprof.cpp): the command buffers of the
window laid on one time line, giving the spans' own time (real work), the holes inside buffers (time between a buffer's first and
last timestamp that no span names), buffers overlapping, and the empty queue between buffers, by gap length and by what the
buffer after the gap starts with. Default off with the rest of `RX_VK_PASS_TIMING`.

## Which earlier number was right

* **G1 and C1 (GPU ~18-20 ms busy) are right.** Three independent measurements agree here:
  1. the unprofiled log (buffer begin to end of each recording, uploads and gaps): GPU + uploads + gaps = the frame in every zone;
  2. the sum of the passes' and dispatches' own timestamps (`RX_VK_PASS_TIMING`, each span closed and opened by an all-commands
     timestamp, so a span is that work alone): 22.1 ms in kokiri, **only 0.25 ms of it is hole inside buffers** (1%);
  3. the driver's own `nvidia-smi` utilisation over the same seconds: 86-87% (kokiri, hyrule), 76-78% (castle), against 87%, 87%,
     74% from the timestamps.
  A hole inside a buffer is therefore not what makes the busy time long: there are none worth naming. The 18-20 ms is passes
  (14.3 ms in kokiri, a few of them fullscreen 1440p passes of 1-2.4 ms each) and dispatches (4.4 ms).
* **C2's and R3c's 6.5-8.6 ms is an undercount for this PC.** C2's build (896668e3) predates 17308b8c ("the early segments' GPU
  time is in the log's GPU time"): the early-segment path (the frame's heavy buffers) never wrote its timestamps, so the log's
  "GPU time" counted only the small buffers and the segments showed up as "gaps between recordings" (C2: castle 6.8 ms GPU,
  17.7 ms gaps; kokiri 4.2 and 20.4). The same line now reads castle 17-19 ms GPU + 2.6 uploads + 6.5 gaps, kokiri 20.2 + 1.0 + 3.0:
  the "gaps" were the GPU running the uncounted segments. (That commit's own message gives 3.7-3.9 ms counted against ~6.3 real
  on an RX 9070 XT in kokiri: on a card several times faster than the 3050 the GPU work really is ~6.5 ms, which is the R3c
  figure; it does not apply to the 3050.)
* **Windows GPUBusy "counts holes as busy"** may be true of the 5070 machine's holes (R3c: the queue sitting between pieces). On
  this PC there is no such inflation to find: the driver's utilisation equals the work from the timestamps.

## One table per zone: the frame, GPU side (ms a frame, mean of 2 runs)

| | kokiri-forest | hyrule-field | castle-move |
|---|---:|---:|---:|
| frame, plain run | 24.3 (41.2 fps) | 21.6 (46.4 fps) | 27.5 (36.4 fps) |
| **GPU real work** (recordings 20.2 / 17.9 / 17.9 + uploads 1.0 / 0.9 / 2.6) | **21.2** (87%) | **18.8** (87%) | **20.5** (74%; timeline union 23.2) |
| of it: passes / dispatches (PASS_TIMING) | 14.3 / 4.4 | 12.3 / 4.1 | 12.9-13.3 / 4.2 |
| of it: write-back copies, barriers, uploads | 0.74, 0.59, 1.75 | 0.84, 0.58, 1.6 | 0.8, 0.7, 4.4 (texture streaming: refresh 1.9, msync 1.5, mirrors 0.7-0.9) |
| **holes inside buffers** (PASS_TIMING: first to last timestamp minus spans) | 0.25 | 0.29 | 0.37-0.41 |
| **empty queue between buffers** (plain log gaps; timeline idle) | 3.0 (3.0) | 2.7 (2.6) | 6.5 (3.6-7.7) |
| nvidia-smi GPU utilisation | 86-87% | 87% | 76-78% |

With `RX_VK_PASS_TIMING=1` itself the frame lengthens by 3-4 ms (kokiri 28.7 ms) and the empty queue grows to 6.2-6.6 ms, 97%
of it before the upload buffer that starts each recording: the profiler's timestamps cost the CPU's submit path, not the
GPU's work (spans 22.07 vs 21.2 unprofiled). The gaps by length in kokiri (PASS_TIMING run): under 0.1 ms 0.36 (44 a frame),
0.1-1 ms 1.9 (6), 1-5 ms 4.1 (2.2).

In the unprofiled timeline (kokiri) the GPU finishes its frame ~0.02 ms before the main thread's once-a-frame signal and the
idle 3.0 ms is the front of the next frame (the CPU phase up to the first heavy submit, 4-5 ms after the signal): the GPU is
waiting for the CPU only there.

Resolution check, kokiri, 1920x1080 (1.78x fewer pixels): passes 14.3 to 11.2 ms, dispatches 4.4 to 3.9, GPU busy 22.3 to 18.6
(profiled), plain frame 24.3 to 20.1 ms (41.2 to 49.7 fps): the frame follows the GPU's work ms for ms. Only ~3.5 ms of the
pass time scales with pixels; the rest is per-draw/fixed (lower-resolution targets, 1280x720 passes, dispatches).

## One table per zone: the CPU threads (ms a frame)

Busy = the thread's CPU time (`HOS_THREAD_TIMES`, 2 plain runs; includes spinning in the driver). Waits = timeline events
(`RX_VK_TL`, 4 frames x 2 runs: `readback` = the CPU's read of a GPU result, `wait-rec` = the part of it spent waiting for the
GPU to finish a recording) and sampler leaf (`crit.py`, 2 ms samples).

| thread | kokiri busy | hyrule busy | castle busy | waits on (kokiri / hyrule / castle) |
|---|---:|---:|---:|---|
| main | 5.1 | 5.2 | 6.5 | readbacks 8.9 / 8.4 / 6.0, of which for the GPU's recording 7.1 / 6.0 / 2.8; rest: job-group event wait for module workers and executors (sampler: 44% / 57% / 60% of its blocked instants) |
| Presentation Thread | 1.7 | 1.7 | 5.4 | inside `present`, blocked on the replay lock: 11.0 / 9.0 / 8.0 |
| replay thread (`mkw_vk order`) | 9.6 | 8.2 | 9.2 | timeline: replay of submissions 9.7 / 8.0 / 7.3 (CPU time includes driver spin); idle the rest, waiting for the main thread's signal |
| held-submits thread | 2.2 | 1.5 | 3.4 | its own submits 0.9 / 1.2 / 0.5 |
| replay workers (4, sum) | 16.5 | 13.8 | 17.4 | parallel part of the replay; wait for work |
| module workers (5, sum) | 23.2 | 21.9 | 28.2 | each reads back 4.9 / 4.7 / 2.7 ms, of which 4.0 / 3.3 / 1.4 waiting for the GPU's recording |
| executors (6, sum) | 24.8 | 21.6 | 50.3 | hidden: waited for by the main thread's job-group wait; castle has 2x the executor work |

What the threads wait on is, in kokiri and hyrule, the GPU: the game threads' readbacks wait 4-7 ms a frame for recordings that
the GPU is still running, and the Presentation Thread waits 9-11 ms for the replay that submits them. Nothing on the CPU side is
the long pole in those two zones: the CPU's own chain finishes while the GPU still works. In castle-move the GPU is idle 6.5 ms a
frame (a CPU-bound front: 5.4 ms busy Presentation Thread, 50 ms of executor work over 6 threads, the texture streaming) and the
GPU's 20.5 ms still exceeds it.

## What 60 fps (16.7 ms) at 1440p on this PC needs

* **GPU side, first and mandatory**: busy time 21.2 / 18.8 / 20.5 ms a frame must fall to about 13-14 ms (the 2.5-3 ms of CPU-bound
  front stays idle): **cut ~7-8 ms in kokiri (35%), ~5 ms in hyrule (28%), ~7 ms in castle (33%)**. Where: passes (14.3 / 12.3 /
  13 ms: a few fullscreen 1440p passes of 1-2.4 ms each in kokiri are 5 ms by themselves, and pixels are worth 3.5 ms for the
  whole 1.78x) and dispatches (4.4 / 4.1 / 4.2 ms, 120 of them a frame, a dozen of 0.3-0.4 ms); in castle also the streaming
  uploads (2.6 ms unprofiled, 4.4 profiled). Barriers (0.6 ms), write-back copies (0.7-0.8 ms) and holes (0.3 ms) are small.
* **CPU side, secondary**: no CPU cut shows in the frame in kokiri or hyrule until the GPU is below ~16 ms. Once it is, the frame
  becomes the CPU front (GPU idle 3 ms) + the replay and readbacks it waits on; castle-move needs ~3 ms off its 6.5 ms idle
  front first (the executors' and the Presentation Thread's work) to reach 60 even with the GPU fixed.
* Not a lever here: barriers, in-buffer holes, uploads outside castle, the GPU clock (P0, 1912 MHz, power-limited near 77 W).

## Not verified

* The timeline windows are 4 frames each (the dump is slow and the game is killed at 110 s): the CPU waits per thread are from
  8 frames per zone; sampler shares from one run per zone.
* PASS_TIMING serialises the passes with all-commands timestamps; the plain-log and `nvidia-smi` numbers say the unprofiled
  work is within 1 ms of the profiled sum, but overlap that narrower barriers would allow is not measured.
* castle-move's GPU time varies run to run by 1-2 ms (15.7 to 19.1 in the logs) with where the scripted player stands.
* C2's and R3c's builds were not re-run; the explanation of their low figures is from commit 17308b8c and their logs' gap
  columns.
* No picture checked (diagnostic runs only, one extra run at 1920x1080).

## Sections from branches that were not merged (brought over 27 Nov 2026)

Written on those branches and left out when their notes came to main (256de8e6f); kept as they were, each under the branch it comes from. The verdicts on that work are in docs/research/perf-plan-2026-11.md and the branch audit.

### From perf/fixed-overhead (7a3e2ab47)

### Fixed GPU overhead bound (P3, PC 3050, kokiri-forest, main 8045fe73, 2 runs each, interleaved, warm run first)

Switches that remove the work outright (picture wrong, bounds only): base 39.75 / 41.44 fps (mean 40.6), `RX_VK_NO_RB_REFRESH=1`
39.85 / 41.77 (40.8, +0.5%), `RX_VK_NO_WRITEBACK=1` 40.32 / 41.12 (40.7, +0.2%). Run-to-run spread is ~1.7 fps. So the ~2.3 ms of
rbrefresh, msync, write-back copies and their barriers seen in PASS_TIMING is not on the frame's critical path: removing a whole
piece moves nothing measurable. Reasons from the code: rbrefresh (2.26 MB) and msync (1.82 MB) are PCIe reads of host memory
(~2.7 GB/s: bytes, not barriers or dispatches; scatters are already one dispatch per recording); the refresh must read live guest
memory at GPU time (it catches CPU writes made after the submit's capture), so a page_gen skip is unsafe; the write-back's
10.9 of 14.8 MB a frame are GPU-written ranges inside pools the CPU reads, but the CPU reads ~22 pages: a page-level filter would need a
synchronous first-read fallback for exactness, for a bound of ~0.2%. No code change made.


### From perf/gmem-hoist (37e836d38)

### G5: global-access range hoist in every stage, and the PLOP3 fold (PC 3050, 2026-11-16, 1440p, kokiri-forest and hyrule-field saves)

Switches (all off by default; "RX_VK_GMEM" is now in the program cache stamp's prefix list, vk_shader.cpp, and any translator change moves HOOKSHOT_SHADER_STAMP):
`RX_VK_GMEM_HOIST=1` (every stage) or letters `v f c m` (vertex, fragment, compute, task+mesh); `RX_VK_GMEM_FOLD=1` (PLOP3 booleans done on booleans).

GPU ms a frame (RX_VK_PASS_TIMING=2, last 600-present block; "pass" = draws, "dispatch" = compute) and plain fps (2 runs):

| arm | kokiri GPU / pass / dispatch | kokiri fps | hyrule GPU / pass / dispatch | hyrule fps |
| --- | --- | --- | --- | --- |
| default | 22.30 / 14.29 / 4.42 | 39.4, 40.8 | 19.89 / 12.30 / 4.08 | 45.7, 45.7 |
| RX_VK_MESH_G3=1 | 21.72 / 13.71 / 4.43 | 37.6, 45.3 | 18.46 / 10.89 / 4.08 | 48.6, 49.1 |
| HOIST=1 (all) | 30.73 / 20.29 / 6.84 | 24.2 (+153 outlier) | 24.74 / 14.69 / 6.52 | 37.5, 37.4 |
| HOIST=1 + FOLD=1 | 30.73 / 20.32 / 6.80 | 26.5, 27.3 | 24.72 / 14.69 / 6.53 | 37.7, 37.2 |
| HOIST=v only (kokiri) | 22.34 / 14.35 / 4.41 | 41.2 | | |
| HOIST=f only (kokiri) | 28.84 / 20.83 / 4.43 | 32.0 | | |
| HOIST=c only (kokiri) | 24.78 / 14.36 / 6.82 | 31.6 | | |

Reading: the hoist helps task/mesh only (G3's gain reproduced: -0.6 Kokiri, -1.4 Hyrule). Hoisting fragment programs costs +6.5 ms and compute +2.4 ms, vertex is neutral: the prologue (two UBO loads, ArrayLength, 64-bit ops per candidate) runs for every invocation, a pixel included, where the old search paid only for accesses actually reached. The FOLD changed nothing measurable. Pictures (same save, late scene shot, four arms) match: geometry, textures, lighting, water.
Run notes: RX_VK_SHOTS needs the folder to exist before the run (a cmd `mkdir` through pc.sh with `\$lab` in double quotes created a folder named `$lab`: close the quote); shots only after the loading screen (present ~7000+ at this speed).


### From perf/mesh-overlap (bfb23e627)

### G4: indirect mesh draws without the pass break (RX_VK_MESH_INDIRECT_INLINE=1, default off, measured: no gain)

Why each indirect mesh draw ended its pass: NVN2 keeps the record as {count, first}, Vulkan reads {x, y, z}. mesh_indirect_convert copies each
count into a {count, 1, 1} ring (a fill and a copy) and a copy cannot be made inside a render pass: end_pass, three barriers, then the pass
opens again behind a full pass-start barrier (the copy is transfer work). Kokiri has about 42 such draws a frame (4423 + 1727 in 5 s at ~30 fps with the
profile on), Hyrule about the same.
The switch converts them ahead (mesh_hoist_* in vk_draw.cpp; vk_replay.cpp scans the ops after the game's barrier, a dispatch or a met
wait, and at the start of each replay part, up to the first op that may write memory) so the draws keep their pass. 72% of the draws
were covered (the rest: draws after a dispatch whose scan did not reach them).
Measured on the PC 3050 (build g4f, 1440p, plain runs): Kokiri 40.4 / 40.9 fps default, 39.2 / 36.1 inline; Hyrule 45.7 / 45.7 default,
40.0 / 43.0 inline; GPU time Kokiri 20.3 -> 20.5-20.6 ms. With RX_VK_PASS_TIMING=2 (which serialises the spans and cannot show overlap)
-0.1 ms Kokiri, -0.46 ms Hyrule. The draws stay behind the game's own full nvnCommandBufferBarrier and the dispatch that writes their counts,
so there is nothing to overlap with; the extra full barrier a batch needs costs more than the breaks it saves. Left off.
