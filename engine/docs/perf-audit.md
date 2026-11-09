# Performance audit: where the time goes in Kokiri (2026-11-08)

Machine: Ryzen 7 7800X3D, RTX 5070 12 GB, window 1280x720, the game image 2560x1440. Scene: Kokiri Forest, Link standing, about
3,050 draws a frame. Run: `scripts/bench-replay.sh` (the steady scene), `scripts/walk-test.sh` (a scripted walk), `HOS_SAMPLE` with
`scripts/prof.py` and `scripts/prof_callers.py` (a thread sampled every millisecond, named from the linker maps).

## What bounds the frame rate

| Thread | Busy | What it does |
|---|---|---|
| Presentation Thread (the game's render thread, which replays the NVN stream inline) | 80-86% | the frame's critical path: ~15-16 ms a frame |
| sead::MainThread (the game's logic) | 23% | 77% of its samples wait for the render thread; its busiest guest function is under 1% |
| GPU | ~8.7 ms a frame | not the limit (RTX 5070); at 64 fps the overlay shows 5.9 ms busy |

At 52-55 fps the frame is the replay's 15-16 ms plus a little guest work. A frame rate above 60 needs the replay under ~8 ms.

## The replay thread (steady scene, % of its samples)

| Cost | Share | Notes |
|---|---|---|
| `draw()` and what it calls | 48% | 3,050 draws, ~3.2 us each |
| of which `build_descriptors` (self + callees) | 19% | resolves ~30 descriptors a draw: uniform ranges, storage buffers through the driver banks, textures through the handle table |
| NVIDIA driver (`nvoglv64.dll`, leaf) | 20-25% | `bind_descriptors` (push descriptors) 4%, `vkCmdDraw` and the pipeline binds 4%, barriers and copies of the mirror syncs 3% |
| compute dispatches (culling passes, 147 a frame) | 7% | |
| mirror syncs and write-backs | 5-6% | 22 MB in 5,600 copies and 67 MB written back a frame |
| waiting for the game's thread (`queue_get`) | 13-14% | the replay is not always fed |
| `program_for` (per draw program lookup) | 8% | mesh draws take the shader lock, build the key and check texture types at every draw. A quick answer for them was tried and backed out: no measurable gain, and a mesh variant depends on texture types the bindings' generations do not follow |
| counter reports (`memory_cpu_owned`) | was 3.5% | fixed: a bitmap per guest page |

Fixed: the TLS init wrapper calls (~3%), the CPU-owned word tracking (~3.5%). Together about 5% of a frame, within the run to run noise
of ~5%.

## Guest functions that a host function could replace

None worth replacing. The game's logic thread is 77% idle, and its self time is spread thin: the busiest guest function takes under 1% of
its samples, `collect_in` (a host function) 1.1%. The libc style work (memcpy, memset, sqrt) already goes through the host (`ucrtbase` 5%).
Replacing guest code would gain nothing on the critical path, which is the render thread.

## What could still move the frame rate (largest first, with the cost)

1. Fewer draws to replay. The game draws every candidate (the fork has no real occlusion counts: each draw adds 16 samples). Real
   counts (GPU occlusion queries, results a frame late) removed ~29% of the draws in the oot-pc port, at the price of stale-count flashes.
   `HOOKSHOT_OCCLUSION_OFF=1` stops the game making its occlusion queries (50 fps against 45 in one run, inside the noise).
2. Fewer driver calls per draw: push descriptors to pre-built descriptor sets (bindless, as the oot-pc port did), or descriptor buffers.
   The driver is 20-25% of the thread and `build_descriptors` 19%.
3. Recording on more than one thread. `RX_VK_PARALLEL=4` gave 55 -> 65 fps but one-frame flashes come back (5 in 25 s with the flash
   detector); `RX_VK_ASYNC=1` was slower (32 fps).
4. A frame generation setting (the menu's 2x, 3x) for the display rate.

## Parallel recording (2026-11-08): the largest gain found, and why it is off

`RX_VK_PARALLEL=4` records a command buffer's segments (160 draws each) on four threads after a serial dry pass. With
`HOOKSHOT_OCCLUSION_OFF=1` the replay fell from 12.6 to 8.5 ms a frame and Kokiri reached the game's own 60 Hz pacing (58 fps median with
some frames over 20 ms, from memory sync spikes of 3-6 ms and descriptor stalls). The one-frame black flashes of out-of-order segments did
not come back with Occlusion Off (no real flash in six walks of ~25 s; five in one walk without it; the flashes at presents 147-149 are the
title's fade).

It is not on by default: **the picture is wrong**, constantly, not as a flash. With segments of 160 or 320 draws the fog is washed out
and the ground loses its shadows (as if the shadow map were empty), with or without Occlusion Off. With one big segment, with 640-draw
segments, or with two threads on 1,000-draw segments it is right. Forcing a single boundary at any of the first boundaries
(`RX_VK_PARALLEL_BOUNDARY=1|2`), or none before draw 700 (`RX_VK_PARALLEL_MINWORK=700`), still breaks it, so it is not one boundary's
content: a race between the recording threads or an ordering dependency of the shared state (texture write tracking, mirrors, the
per-thread caches). Finding it is the way to 60+ fps.

### Root cause and fix (TICK-006, 2026-11-09)

- **Not a race.** A diagnostic build that recorded the segments in order on the replay thread, and another that carried each segment's
  final state into the next instead of the dry pass's snapshot, were just as wrong (the snapshots matched the carried state except the
  dispatch grid, which a dispatch sets before use). Kokiri's command buffers are ~750 draws (4.7 segments of 160), so 640- and
  1,000-draw segments never split them: "right with big segments" only meant "right when not split".
- **Cause: the exposure meter's readback refresh, once per recording instead of once per submission part.** OoT's exposure meter is a
  sum the GPU adds into a small page each frame; the CPU reads it and stores 0 back. `memory_rb_refresh` (`gpu/vk/vk_memory.cpp`,
  called from `texture_uploads_recorded`) copies those pages from guest memory into the pool's mirror at the start of a recording's
  uploads. Serial parts (`maybe_split`) write the mirrors back (`memory_end_submission`) and start a new mirror epoch before the next
  refresh, so the sum carries over. The parallel path records each segment, and what follows them, into recordings of their own in the
  same epoch with no write-back between: every segment's refresh copied the CPU's 0 over what the segments before it had added. The meter
  saw only the last recording's share, the auto-exposure went to its maximum: washed-out fog, the ground's shadows lost in the glare.
- **Fix**: `memory_rb_refresh` runs once per `mirror_epoch` (the part's first recording, which the parallel path submits before its
  segments). Serial replay is unchanged (one refresh per part, as before).

Results (this build, RX 5070 machine, `tools/bench/run_bench.py --duration 115 --warmup-s 45`, frame generation off):

| Scene | Replay | fps | p50 | p99 | max |
|---|---|---:|---:|---:|---:|
| Kokiri walk | serial | 51.1 | 19.8 ms | 27.8 ms | 61 ms |
| Kokiri walk | `RX_VK_PARALLEL=4` (run 1) | 66.3 | 14.8 ms | 19.1 ms | 50 ms |
| Kokiri walk | `RX_VK_PARALLEL=4` (run 2) | 66.2 | 14.8 ms | 19.4 ms | 113 ms |

(Baseline doc, 2026-11-08: 58.9 fps, p99 26.0 ms serial; today's serial run was slower, the parallel gain is against the same build.)
Screenshots of the same walk (`engine/scripts/walk-test.sh`) match serial: fog and Link's and the trees' shadows. Flash detector
(`scripts/framelog.py`) over three parallel walks: 12, 9 and 6 events, all 1-2 blocks (motion), against 5 of the same kind serially; no
scene-wide flash. Hyrule Field was not measured: the title loads the most recent save (Kokiri), and switching saves was not allowed.
Parallel stays **off by default** until Hyrule is checked.

## Descriptor resolution cached per slot (TICK-007, 2026-11-09)

Profile before (Hyrule Field walk, parallel recording on, `HOS_SAMPLE="mkw_vk replay worker"`, second half of 110 s): a recording
worker is busy 11.2% of the time (the Presentation Thread waits for the game in `queue_get` 55% of its samples: the replay is no longer
the frame's limit at ~63 fps). `build_descriptors` is 3.7% of the worker (a third of its recording). Inside it: `texture_view_of` 24.5%
(the `handle_hits` hash search), `sampler_for`/`sampler_of` 10.8% (`reg_mu`, one mutex for all recording threads, and a hash of the
sampler's parameters, every texture of every draw), `gpu_buffer_bind` 11% (written storage buffers take `ParLock` + `pools_mu`), the
TLS init wrapper 6.4% (shown as `dump_mesh_indirect`, ICF-folded: every access of `dw`/`draw_sampled`), mutex locks ~16%.

Change (`gpu/vk/vk_draw.cpp` `build_descriptors`, `vk_bank.cpp`):
- A per-thread table, per program, of its texture descriptors as last resolved: 4 ways a slot, by the handle. A slot is reused when the
  handle read this draw is the same and the texture slot's version (`texture_slot_version`), the sampler slot's new version
  (`sampler_slot_version`, bumped by `register_sampler_copy`) and `texture_destroy_generation` are unchanged. The versions are read
  before resolving, so a registration racing it is seen at the next draw. The handle itself is read every draw (driver bank, uniform
  buffer, handle program): only its resolution to view/texture/sampler is kept. The once-a-submission texture touch is kept. Thread-local
  (the 4 recording threads, no lock; the dry pass records no draw). `RX_VK_NO_SLOT_CACHE=1` turns it off.
- The thread_locals taken once at the function's start.
- Storage buffers are not cached: a written one's `gpu_buffer_bind` has per-submission side effects (write-backs, written stamps, the
  mirror sync); reads already have `bind_memo`. That (dedup of written ranges per mirror epoch) is the next step.

After (same scene and sampling): `build_descriptors` 2.9% of the worker (busy 10.5%); `texture_view_of` 9%, `sampler_of` ~1% of it.
The log's replay times (a frame, all threads, mean of the second half): descriptors 2.78 -> 2.23 ms, draw 7.21 -> 6.62 ms, submit 5.73
-> 5.46 ms. Frame rate unchanged within noise (the game's pacing and `queue_get` bound it).

Bindless texture heap: with the per-slot cache the texture part left in `build_descriptors` is the handle read and the 4-way check;
the push itself is ~0.35 us/draw (TICK-003). A heap (one descriptor per registered texture slot x sampler, updated at
`register_texture`/`register_sampler`, the shader indexing it by the handle) would remove the pushes of textures and most of
`bind_descriptors` (1.1% of the worker), but needs shader translation changes (`vk_shader.cpp`, all programs re-prepared) and view
types per slot. Not worth it while the replay is not the bound; storage-buffer writes (`ParLock`, 11% of `build_descriptors`) come first.

Bench (Hyrule Field: the most recent save; Kokiri not measured. `run_bench.py --duration 115`, cut 60 s after launch):

| Build | fps | p50 | p99 | max | descriptors ms/frame | submit ms/frame |
|---|---:|---:|---:|---:|---:|---:|
| base run 1 | 63.1 | 16.1 ms | 18.7 ms | 42 ms | 2.83 | 5.71 |
| base run 2 | 63.4 | 15.9 ms | 18.8 ms | 60 ms | 2.70 | 5.67 |
| TICK-007 run 2 | 63.3 | 16.1 ms | 18.8 ms | 62 ms | 2.30 | 5.56 |
| TICK-007 run 3 | 63.7 | 15.8 ms | 18.4 ms | 34 ms | 2.27 | 5.50 |

(TICK-007 run 1 is left out: the new executable re-prepared the shaders at start, 44 s, and the cut fell in the load.) Screenshots of
the walk (`RX_VK_SHOTS`) match the base build's at the same place (present 7200).
