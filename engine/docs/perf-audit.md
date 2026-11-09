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

## Pipelines made before the draws need them (TICK-008, 2026-11-09)

Goal: no frame waits for a Vulkan pipeline. The machinery already in the engine (a note for every pipeline, a precompile from the notes,
`--prepare-shaders` predicting pipelines for the programs no note names) was kept and extended, not replaced.

### Why graphics pipelines were not precompiled ("7805 programs, 0 graphics and 7805 compute pipelines")

- The notes (`hookshot_pipelines.bin`: a program key and variant, plus the draw state a pipeline was made for) were stamped with
  `program_cache_stamp()`, the hash of the shader translation (`HOOKSHOT_SHADER_STAMP`: Eden's recompiler, sirit, `vk_shader.cpp`,
  `vk_state.h`, `vk_api.h`). The programs cache needs that stamp; the notes do not (a note holds no compiled code). Any change of the
  translation threw the notes away together with the programs, and the `--prepare-shaders` run that follows a build found none to
  precompile and none to predict from (`predict_run` needs notes): 0 graphics. The first run after a build then made every pipeline in
  play, and recorded notes again for the next one. (The same line appears whenever the notes file is missing, as in my own runs when I
  moved it aside.)
- With the notes present (any run after a recorded one, until the next translator change) the precompile did work: 965-1131 graphics
  pipelines, built before the game's first frame, **blocking the start** (2.3 s with the driver's cache warm; pipelines the cache lacks
  are the slow ones).
- A draw whose pipeline was not ready was left out only when its targets already held an earlier frame; into a target made that frame
  it **waited** for the worker (`wait_for`). That is the freeze of a first run: with a cold driver cache each pipeline takes tens to
  hundreds of ms, and the passes of a new area all draw into fresh targets.
- With a warm driver cache (all my measurements) pipeline compiles are not what the area-entry hitch is made of: see Measurements.

### Design

- **Notes file, format 2** (`vk_draw.cpp`, same name and place): a 40-byte header (`HKPIPES2`, format, `key_version`, `sizeof(PipeNote)`,
  `HOOKSHOT_STATE_STAMP`, the translator stamp) and the records as before, appended as each pipeline is first made. The append happens on
  a worker now (the file is never read, seeked or written on the replay thread). Versioning:
  - the format, `kPipeKeyVersion` (bumped by hand when `pipeline_key()` or the way a note is built changes meaning), the record size and
    `HOOKSHOT_STATE_STAMP` (new, `nx2/build.py`: the hash of `vk_state.h`, `vk_api.h`, `vk_draw.h` and `rx_nvn.h`, the layout of `NvnState`
    and `PipeNote`) must match, or the file is started again;
  - the **translator stamp** may differ: the notes are then *carried*: kept, every pipeline made again from them with the new
    translation, the file rewritten with the new stamp at the start (`pipeline notes: N carried over to this build`). Nothing compiled
    is reused as it is (the key is recomputed from the state by `pipeline_key()`, with the program found by its key and variant; a note
    whose program is gone is skipped). So a change in the shader translation invalidates every program and every pipeline, but not the
    list of what the game uses. `RX_VK_NOTES_STRICT=1`: drop the notes on a translator change, as before. The old 16-byte header is read
    when its stamp is the current one, and rewritten in the new format.
- **Precompile in the background** (`precompile_run`): once the programs the notes name are loaded (1.3-1.5 s), an entry (not ready) is made
  in the pipeline map for each note, in the order the notes were written (the order the game first used them), and a pool of threads
  (half the CPUs, 2 to 8, below normal priority) builds them, the kept compute programs first (their `compute_state` is 1 meanwhile: a
  dispatch that meets one is left out, as before). The start goes on at once; the last worker logs
  `precompile: N graphics (M made) and K compute pipelines built in X ms (in the background...)`. A draw that finds an entry still
  building does not make another one. `RX_VK_PRECOMPILE_WAIT=1`: wait for all, with the console progress, as before;
  `--prepare-shaders` always waits. The `.running` crash guard still works (it is cleared by the last worker, so closing the game while
  it builds counts as not finished); a second unfinished start now moves the notes to `hookshot_pipelines.bin.dropped` instead of
  deleting them.
- **A draw whose pipeline is not ready is left out of its frame, whatever its targets** (`draw`, `st_pending_skips`): it never waits.
  This is the skip policy, not VK_EXT_graphics_pipeline_library. The engine has the fast link (`gpl_link`, four stage libraries; it needs
  extended dynamic state 3 blend, polygon, clamp and alpha-to-coverage, all present on this RTX 5070, "pipeline libraries yes") but a
  link only beats a full build when the libraries are in the driver's cache already, which means building two libraries for each of the
  36,557 programs at every `--prepare-shaders` (`RX_VK_GPL`, off). Setting it also changes the shader stamp (the stamp hashes `RX_VK_GP*`
  variables), so the programs cache is dropped and re-prepared: I did that once by mistake and had to run `--prepare-shaders` again
  (41 s with the driver's cache warm). The link now runs on the worker, not the replay thread, for whoever turns it on; it is not tested.
  - Visual risk: a draw left out into a target made that frame (a shadow map, bloom, the first frame of a new render target) leaves that
    frame's picture without it, and nothing draws it again until the pass is redone (OoT redoes its passes every frame; a pass drawn
    once, such as a thumbnail, would stay without that draw). The count is in the log: `a frame: X draws left out (not ready; Y of them
    for a pipeline, into targets made that frame)`. `RX_VK_PIPE_WAIT_FRESH=1`: those draws wait, as before. A program not yet
    translated still waits for fresh targets as before (all programs are prepared; none was met in play in these runs).
- Test knob: `RX_VK_PIPE_DELAY_MS=<ms>` adds that to every whole pipeline build (a cold driver cache, to see the draws wait or not).

### Measurements

Hyrule Field, slot 02 (the most recent save); `tools/bench/run_bench.py --scene hyrule-walk --duration 115`, 1280x720, frame generation
off, driver cache warm. Times from the first present: "title" under 10 s, "load" 10-42 s, "entry" 42-55 s (Hyrule's first frames).

| Run | Frames over 50 ms (title / load / entry / after) | max | launch to first present | graphics pipelines made in play |
|---|---|---:|---:|---:|
| old build, run 1 (no notes: records) | 12 (3 / 2 / 6 / 1) | 676 ms | 4.5 s | 965 |
| old build, run 2 (blocking precompile, 965 notes) | 15 (5 / 2 / 6 / 2) | 802 ms | 6.4 s | 0 |
| old build, run 2 again (with screenshots) | 12 (4 / 1 / 6 / 1) | 530 ms | 5.9 s | 0 |
| this build, run 1 (no notes: records) | 15 (5 / 1 / 8 / 1) | 642 ms | 5.2 s | 968 |
| this build, run 2 (background precompile, 968 notes) | 12 (6 / 1 / 5 / 0) | 587 ms | 4.2 s | 0 |
| this build, run 2 again (with screenshots) | 13 (6 / 1 / 5 / 1) | 608 ms | 4.3 s | 0 |

Startup: the blocking precompile (2.3 s for 965 graphics and 7,832 compute pipelines, warm driver cache) is off the start; the first
present comes about 2 s earlier (5.9-6.4 s to 4.2-4.3 s). The 1.3-1.5 s of loading the programs the notes name stays in the start; the
builds (2.4 s on 8 threads) end during the title. With a warm driver cache, run 1 and run 2 differ by noise only: 968 pipelines made in
play took 0.7-0.8 s of worker time in all, and no draw was left out for more than a frame.

**The acceptance "no frame over 50 ms in the first 10 s of the area" is not met, and pipelines are not why.** Run 2 makes no pipeline in
play (968, constant; 0.01 draws a frame left out) and the entry still has 5 frames over 50 ms (587, 409, 126, 90 and 82 ms). Their submits
(`slow submit: 291 ms: draw 1051 descriptors 316 pipelines 428 ...`, times summed over the 4 recording threads) are the recording
threads queueing behind the texture creation, the guest memory pool imports (up to 36 ms each, 20 of them) and the descriptor
resolution of ~1,700 new textures; the `pipelines` column there is `graphics_pipeline()` waiting for the parallel-recording lock, not
a build (inferred from the counts, not profiled). That is for another ticket (texture and pool creation off the recording threads). The
~250 ms frames of the title (3-6 a run, the same in all builds) are the guest scheduler (`vcores ... ran without a core after 250 ms`).

Slow compile (the cold driver cache a first run after a build has: `RX_VK_PIPE_DELAY_MS=150`, no notes, 4 workers, ~965 pipelines to make):

| Run | fps | max | Frames over 50 ms | Draws left out | Waits |
|---|---:|---:|---:|---|---|
| draws wait for fresh targets (`RX_VK_PIPE_WAIT_FRESH=1`, the old rule) | 17.8 | **24,984 ms** (also 13.1 s, 6.9 s, 1.6 s) | 18 | 169 a frame in one window | 2.7 a frame, 573 ms |
| never wait (default) | 63.3 | 505 ms (title) | 16 (4 / 1 / 11 / 0) | up to 2,447 a frame while ~650 jobs queue (86 a frame into fresh targets at the peak, under 8 after) | 0 |

The second row's picture lacks what is still being built: the slingshot's "22" is missing from the HUD at presents 7200 and 8400 (about
60 s after the load, ~100 jobs still queued) and is there in a normal run. That is the price of never waiting; with the notes of an
earlier run it does not happen (0.01 draws a frame).

Pictures (`RX_VK_SHOTS`, every 1200 presents, the same walk): the old build, this build with notes, and this build with 150 ms pipelines,
at presents 1200, 2400, 3600, 4800 (title and loading) and 6000, 7200, 8400 (Hyrule Field). Looked at: 7200 of all three, 8400 of
this build and of the slow-compile run. The old and new builds match (fog, exposure, shadows, HUD "22"; the grass's pale tufts are
in both: not a change of this ticket); the slow-compile run lacks the HUD number as said. The files are not byte-identical because
the runs do not reach the same frame at the same present.

Not verified: a truly cold driver cache (the NVIDIA cache was not cleared; `RX_VK_PIPE_DELAY_MS` stands in for it); AMD and Intel (the
skip rule changes their first run too, and their pipelines compile for seconds); a real translator change (the carry path was tested
with a notes file whose translator stamp I overwrote, and with an old-format file; `--prepare-shaders` after a real change was not run,
as the stamped files are untouched); Kokiri; the GPL fast link; closing the game while the background build runs.

## TICK-010: the "pacing gate" at a limit of 120 (2026-11-09)

**Result: there is no pacing gate. The frame is GPU-bound; the main thread's "spin" is a wait for the GPU.** Nothing in this ticket
changes behaviour; the commit adds a diagnostic of the wait (below). Hyrule Field, `hyrule-walk`, `HOS_FPS_LIMIT=120`, RTX 5070.

What the main thread waits in (`HOS_SAMPLE_ALL`, the stacks below the guest frame, `hookshot_vk.map`):

- "f_806668b0" (45% of the main thread's blocked instants, 88% of the instants no game thread runs) is not a spin in game code. The leaf is
  in `nvoglv64.dll`, under `f_806668b0` (its `nvnQueueSubmitCommands`, guest call at f_806668b0+bc6 host) -> `submit_q` ->
  `memory_capture_submit` -> `capture_make` -> `vk_timeline_wait` -> `vkWaitSemaphores`. crit.py names the innermost game frame, so the
  driver's wait was charged to the game function. `capture_make` waits (`RX_VK_CAPTURE_WAIT`, the exposure-meter fix) for the last
  submission that writes back a page the CPU wrote since the last submit.
- `WaitLightEvent` from f_8007fe60 (42%): f_8007fe60 is the game's job worker loop (guest 0x80080184 waits on the worker's own light event
  at +176; 0x80080268 signals the others'). The main thread helps with and then waits for a job group (f_80070b20). During those instants
  the Presentation thread replays the frame's NVN command buffers (`run_submit`, the replay workers) and the BaseProcExecutors run game
  jobs: it is CPU work, not a gate.
- Neither the vsync event (`host/nn_vi.c`'s 60 Hz thread; no game thread is in `GetDisplayVsyncEvent`'s event in any sample) nor the
  host's limit pacing (`hos_vi_native_acquire`: frames take ~15 ms, longer than 1/120 s, so it never sleeps) appears.

The capture wait, measured (`vk: capture waits:` every 600 waits, new in this commit; `RX_VK_CAPWAIT_TRACE=1`: 400 single waits):
one wait a frame, on the main thread, 4-5.5 ms each; the value waited for is always the one handed out just before (`v - handed out now
-1`), 4 values ahead of the GPU's completed value, already submitted 1.3-5 ms before, done ~6-7.5 ms after its submit. No queue was stopped
at an event wait then (checked with a probe of `oldest_unfinished`/`blocked_n`, not kept). So the main thread waits for the GPU to finish
the submission before its own: a full CPU/GPU serialisation once a frame.

The GPU is the limit, not idle: `nvidia-smi` during the run: 83-86% utilisation at 2895 MHz (P0, 79 W). The log's `GPU time 3.2 ms a
frame` undercounts (the unprofiled timestamps); `RX_VK_PASS_TIMING=1` gives 12.9 ms of GPU work a frame (inflated by the timestamps
themselves): passes 5.6 ms, msync (CPU pages -> mirrors) 2.1 ms / 11 MB, dispatches 1.9 ms, **write-backs (mirror -> guest memory)
1.6 ms / 69 MB a frame**, rb refresh 0.4 ms. Those copies (~3.7 ms, ~80 MB a frame over PCIe) are the recompilation's own GPU cost
and the obvious next target.

Experiments (each 1 run, 115 s; fps after the 60 s warm-up):

| Run | fps | p99 | Note |
|---|---:|---:|---|
| baseline (backup build) | 67.3 | 26.1 ms | crit.py: no game thread 38.9% |
| + wait diagnostic (this commit) | 67.4 | 26.1 ms | |
| + diagnostic, `RX_VK_SHOTS` every 60 presents | 65.4 | 28.2 ms | the shots cost |
| `RX_VK_CAPTURE_WAIT=0` (no wait; flashes in Kokiri come back) | 58.0 | 39.3 ms | slower |
| no wait, its write-backs' ranges left out of the capture's mirror copy instead (tried, not kept) | 43.5 | 52.0 ms | no game thread 53.4%: main waits in the game's own GPU syncs (f_8042cd20's `nvnSyncWait`, the Presentation thread's 1 ms `SleepThread` poll in f_805902c0) |
| `HOS_HEADLESS=1` (no swapchain) | 67.1 | 20.6 ms | not the present |
| `HOS_FPS_LIMIT=60` | 59.7-59.8 | 29 ms | the capture waits under 600 times a run (GPU keeps up) |

Removing the wait lets the CPU run ahead of a GPU that has no headroom; the game then waits for its own fences, polling with 1 ms sleeps,
and gets slower. "No game thread running < 10%" cannot be met by pacing changes: those instants are the GPU's time.

Game speed (`opts.realtime`, "Game speed: Real time", `game_speed=1` in the ini, the default `rx_option(0, 1)`): above 60 the limit needs
it (`fps_limit()` returns 60 when it is off, and `realtime_step` writes the step from the measured frame time). Screenshots by time (the
present trace maps presents to seconds from launch): at 72.6 s (before the walk) Link stands at the same spot at 60 and 120; at 86 s and
100 s he is further along at the 120 limit (~65 fps) than at 60 (end of the walk: by the path below the hill vs on the hill). So the game
runs somewhat faster at 65 fps than at 60 even with Real time on: a pre-existing difference (no change in this ticket), not measured in
units (an in-game clock was not logged).

Not verified: other GPUs; Kokiri; the cause of the 60-vs-65 fps walk difference.

## TICK-014: game speed above 60 fps

Method: `HOS_SPEED_LOG=1` (nn_vi.c `speed_log`) sums, once a wall second, the step the engine advances each frame by, read
back from the game's frame-time block (`*(*0x84c9e890)`, the object f_80706be0 hands f_83b6e300: +16 the ratio
60 / nominal fps, +0 the step after the game's own time scales), in 1/60 s: 60.0 a second is real speed. Figures: the
last ~33 s of `run_bench.py --scene hyrule-walk --duration 115` (the walk and after), game seconds / wall seconds.

| Build | limit | fps | game speed |
|---|---:|---:|---:|
| before | 60 | 59.6 | 100.94% |
| before | 120 | 66.3 / 67.1 (two runs) | 111.50% / 112.61% |
| after | 60 | 59.7 / 59.6 (shots) | 100.16% / 100.25% |
| after | 90 | 68.2 | 99.98% |
| after | 120 | 65.9 / 68.5 / 66.3 (shots) | 100.02% / 100.01% / 99.97% |
| after | 360 (uncapped, GPU-bound) | 68.5 | 100.01% |

Cause: above 60 the game sets the graphics module's nominal fps (+0x2c8) back to 60 every frame (diagnostic: of 600
presents at the 120 limit, 600 found the host's 66-120 rewritten to 60; at or under 60 it is kept). The real-time step
written at the present was lost, every frame stepped a whole 1/60 s, and the game ran at fps/60: 13% fast at 68 fps,
2x at 120 (the title). Second, smaller: `realtime_step` clamped the step to 1 / cap before it kept the carry, so frames
shorter than the cap lost their difference and longer ones did not (+0.9% at the 60 limit).

Fix (host only): `realtime_step` keeps its rate in `hos_realtime_fps`; nn_os_sync.c's SignalLightEvent, called by
f_80706be0 (lr 0x80706c88) just before it reads +0x2c8 with the module in x21, writes the rate there, after the game's
own write. The carry is kept against the unclamped time. Game speed Real time (default): game time follows the
clock at any rate from 15 fps up to the limit (60-360). Per frame: the limit is 60 and each frame is 1/60 s (unchanged).

fps / p99 (summarize.py, 60 s after launch): before 67.0 / 20.7 ms at 120; after 69.2 / 20.0 (120), 69.1 / 20.2 (360),
68.6 / 19.9 (90), 59.8 / 20.7 (60): unchanged within run spread. Screenshots (`RX_VK_SHOT_EVERY=30`, presents mapped
to seconds from launch with the present trace, 96 s: sh60 present 5340, sh120 present 8280): Link at the same stretch
of the walk, by the same tree and rock; the start of the walk also depends on load timing, so the shots are not
pixel-equal.

Not verified: other scenes (Kokiri, cutscenes, menus); whether other systems read +0x2c8 after the game's reset (they see
60 as before); other GPUs; frame rates above ~70 in gameplay (the GPU bound): the title ran at 120 with the right
step (ratio 60.26 a second at 120 fps).

## TICK-015: the engine's own GPU copies (2026-11-09)

Hyrule Field, `run_bench.py --scene hyrule-walk --duration 115`, limit 120, RTX 5070; `nvidia-smi` utilisation averaged from 60 s.

What the write-backs were: 7 MB a submission, almost all into the game's CPU_CACHED pools (NVN flags 0x24), which on the
console the CPU reads only after `nvnMemoryPoolInvalidateMappedRange`. With no write-back at all (`RX_VK_NO_WRITEBACK=1`, an
upper bound, not correct): 71.4 fps, GPU 94% busy.

**Write-backs left out** (`vk_memory.cpp`, "Write-backs left out"): the host hands the back end each game pool's NVN flags (a
second DLL export, `mkwvk_pool_flags`: `vk_api.h` is in the shader caches' stamp, and changing it dropped the program cache once
during this ticket, re-prepared in ~45 s). A pool written back for 600 submissions with no invalidate of it keeps the GPU's
writes in its mirror from then on, except the pages the CPU writes (seen by the mirror syncs: a stale page from guest memory
would otherwise go over the GPU's words). Always written back: CPU_UNCACHED pools, ranges outside any game pool, live
texture storage. An invalidate makes its pool eager for good; mirrors holding such writes are not given back. In the walk:
55 pools left out; 0 reads and 0 CPU writes after a range was left out; 0 bindings of such a pool in guest memory (all counted
in the log every 600 submits). Written back: outside game pools 0.12 MB a submit, pools the CPU reads 0.47 MB.
`RX_VK_WB_EAGER=1`: everything, as before.

**Mirror syncs from a capture**: adjacent pages (in the capture and in the pool) merged into one copy region: 2790 -> 1630
regions a frame. Copying only dirty sub-ranges of a page was not done (the write watch is per page; finding the words would
mean diffing each page on the CPU).

GPU time a frame by kind (`RX_VK_PASS_TIMING=1`, inflated by the timestamps):

| | before | after |
|---|---:|---:|
| all | 11.39 ms | 10.26 ms |
| wb (mirror -> guest) | 1.56 ms, 67.5 MB | 0.42 ms, 13.9 MB |
| msync (CPU pages -> mirror) | 2.06 ms, 10.9 MB, 2791 regions | 1.35 ms, 11.5 MB, 1629 regions |
| rbrefresh | 0.40 ms | 0.38 ms |
| passes / dispatches | 4.70 / 1.75 ms | 5.32 / 1.76 ms |

| Run | fps | 1% low | p99 | > 8.33 ms | nvidia-smi |
|---|---:|---:|---:|---:|---:|
| before 1 | 68.0 | 40.3 | 20.4 ms | 99.9% | 80.4% |
| before 2 | 67.4 | 33.3 | 21.9 ms | 99.9% | 80.3% |
| after 1 | 73.3 | 40.5 | 19.6 ms | 100.0% | 76.8% |
| after 2 | 74.4 | 41.3 | 19.1 ms | 100.0% | 77.0% |

(Walks with shots and the frame log: before 67.7 / 67.4, after 72.2 / 73.4 / 73.8 / 72.4 / 73.1.) The GPU is less busy and the
frame faster: the GPU-bound share shrank; the capture wait is 2.3-2.7 ms a frame (was 2.7-3.0 in this build's base run).

Not done: (c) a transfer queue (the write-backs and syncs sit between the graphics work with full barriers on both sides;
what remains is ~1.8 ms and their dependencies on the passes would need queue-ownership transfers); (d) the capture wait was
left as it is (removing it was slower in TICK-010, and the GPU is still ~77% busy).

Correctness: flash detector (`scripts/framelog.py`) over two walks each: before 20 and 24 events, after 21 and 19, all 1-4
blocks (motion, mostly at 85-87 s), no scene-wide flash. Screenshots every 1200 presents: lighting, fog, shadows and grass as
before (the walks are not pixel-equal: presents fall at other moments at the higher frame rate). `HOS_SPEED_LOG=1`: 99.98-100.02%.

Not verified: other scenes (Kokiri, cutscenes, menus, loads), other GPUs, long sessions (the mirrors holding writes are kept:
VRAM use over time not measured), a CPU read without an invalidate (the counters would not see one).

## TICK-016: GPU cost of the translated frame (2026-11-09)

Hyrule Field, `run_bench.py --scene hyrule-walk --duration 115`, limit 120, RTX 5070. GPU time from `RX_VK_PASS_TIMING=1` (the last
600-present report of the run; inflated by the timestamps, read the shares).

GPU time a frame by kind, base build:

| kind | ms | spans | | whose |
|---|---:|---:|---|---|
| all | 9.78 | | | |
| pass (draws) | 4.89 | 160 | 2058 draws | the game's |
| dispatch | 1.78 | 127 | | the game's |
| msync (CPU pages -> mirror) | 1.29 | 56 | 1561 regions, 11.1 MB | engine (TICK-015) |
| barrier (recordings) | 0.56 | 311 | 112 the game's, 74 pass start, 87 dispatch, rest recording edges | game 0.24, engine 0.32 |
| wb (mirror -> guest) | 0.42 | 10 | 14 MB | engine (TICK-015) |
| rbrefresh | 0.41 | 25 | 546 pages, 2.1 MB | engine (PR 430) |
| xfer, event, ubupd, clear, upload barriers | 0.16 | | | engine |

Top passes are the game's: two one-draw 2048x1152 passes (B10G11R11 target + depth) at 0.82 and 0.56 ms (programs 4f0c0b40ab3a32cd and
739acd678ae08655, each with a storage buffer the replay cannot bind: "reads zeros"), then 0.17-0.07 ms passes and dispatches
(755a777ada52677d 41 runs 0.18 ms, ee7764a6b1ef459c 0.17 ms). So ~6.7 of 9.8 ms is the game's draws and dispatches, ~2.1 ms the
engine's copies, ~0.5 ms barriers.

Checked and not waste:
- Render target formats: `vk_formats.cpp` maps each NVN format to the same width (RGBX formats to their RGBA kind, which Vulkan needs).
- Load/store ops: every pass LOAD/STORE in GENERAL. The GPU here renders immediate-mode (no tile memory): load/store of a
  transient depth costs nothing like on a tiler, and a DONT_CARE would need knowing the next reader. Not changed.
- Clears: 1 a frame.
- Barriers: of 414 full barriers recorded a frame, 178 are already left out as repeats (TICK-010). Two experiments, off by default
  (`vk_texture.cpp` pass_start_barrier / dispatch_barrier, `vk_work.h` counting non-draw work by command buffer):
  `RX_VK_PASS_TRUST=1` leaves out the pass-start barrier after draws only (8 of 74 a frame: passes nearly always follow a copy, clear
  or update), `RX_VK_DISPATCH_TRUST=1` the dispatch barrier after draws and dispatches only (87 of 87). Together 414 -> 325
  barriers, 0.55 -> 0.42 ms of barrier spans, and no fps change: 72.6 / 72.6 against 72.6 / 73.9 (pass trust alone) and
  73.1 / 72.7 (defaults). The drains cost the GPU little; the frame is not held by them. Left off: no gain for an ordering the
  game might rely on.
- Robust buffers (`opts.robust`, device robustBufferAccess2, on by default for the device faults and flashes of 53caac5f): not
  measured in this ticket (an A/B with `RX_VK_ROBUST=0` was not run). It costs the driver's bounds checks on storage/uniform
  accesses, not the translated code.

| Run | fps | 1% low | p99 | > 8.33 ms |
|---|---:|---:|---:|---:|
| base (sprint figure) | ~73.6 | ~37 | ~20 ms | 100% |
| pass trust 1 / 2 | 72.6 / 73.9 | 40.7 / 39.5 | 20.1 / 18.9 ms | 100% |
| pass + dispatch trust 1 / 2 | 72.6 / 72.6 | 39.1 / 35.4 | 20.4 / 20.3 ms | 100% |
| final (defaults = base behaviour) 1 / 2 | 73.1 / 72.7 | 41.7 / 40.8 | 19.3 / 20.0 ms | 100% |

Conclusion: the engine-side GPU waste left is the copies (msync 1.3, wb 0.4, rbrefresh 0.4 ms), not barriers, formats or
load/store ops; the rest is the game's own shaders at 2048x1152. Next candidates: rbrefresh copies every readback page at each of
~25 submissions a frame, even unwritten by the CPU (a page the CPU did not write since could be skipped, but the capture's deferral
of CPU writes, `capture_owns`, makes that touch the exposure-meter ordering of TICK-006: not done); the msync regions; and the
resolution of the two big passes (the game's dynamic resolution resolved to 2048x1152).

Correctness (final build, experiments off): flash detector over two walks 19 and 9 events, all 1-2 blocks at 86 s (motion, as
TICK-015's 21 / 19); screenshots every 1200 presents show lighting, fog, grass and HUD as before (not diffed against a base walk);
`HOS_SPEED_LOG=1` 99.98%. The shader cache stamp is unchanged (no file of it touched). Not verified: the experiments' pictures
(flash detector not run with them on), other scenes, other GPUs, the cost of Robust buffers.

## TICK-017: CPU/GPU overlap (2026-11-09)

**Result: not reached. Letting the CPU run ahead of the GPU makes the GPU slower per frame, so the overlap loses. Behaviour unchanged
by default; the commit adds the experiment (off) and two diagnostics.** Hyrule Field, `hyrule-walk`, limit 120, RTX 5070.

Which reads need the GPU's results: the capture wait is never for the exposure meter's readback page. The deferral log (`vk: capture
defer: page ...`) names page 0x100d6c8000 (pool 0x100d000000 +0x6c8000), not in `rb_pages`; once the CPU runs ahead, 66-105 pages a
capture (pools 0x102b000000, 0x1030000000: the CPU-read pools TICK-015 still writes back, ~0.47 MB a submit), none in `rb_pages`.
So the wait is for a page the CPU and a write-back share, not for a value the CPU reads.

Tried:
1. The page copied again by a helper thread once the write-back is done, the next submission waiting on the GPU for a host-signalled
   timeline semaphore. Hung at the title on NVIDIA/Windows (a submit waiting on a not-yet-signalled host semaphore; the helper and
   the replay then blocked): dropped.
2. `RX_VK_CAPTURE_DEFER=1` (kept, off): such pages are left out of the capture and the mirror sync copies them from guest memory on
   the GPU, after the earlier write-backs (queue order): the same bytes the wait gave, no thread waits. `RX_VK_CAPTURE_SLACK=<k>`
   (with it) keeps a pacing wait for the submission k before.

| Run | fps | 1% low | p99 | > 8.33 ms | GPU busy |
|---|---:|---:|---:|---:|---:|
| base 1 / 2 | 73.0 / 72.9 | 38.7 / 39.6 | 19.5 / 19.9 ms | 100% / 100% | 75.8% / 75.6% |
| defer, no pacing wait | 56.1 | 18.6 | 51.7 ms | 100% | 89.1% |
| defer, slack 1 / 2 / 4 | 73.2 / 72.8 / 42.7 | 42.3 / 35.2 / 18.8 | 19.9 / 19.9 / 52.4 ms | | |

Why the fallback: with no wait the main thread waits in f_8042cd20's nvnSyncWait for the Presentation thread's previous-frame present
fence (`HOS_NVN_SYNC_LOG`: two syncs used in turn, lr 0x805903ac / 0x8042cde0, infinite timeout). New diagnostic `vk: fence waits`
(`RX_VK_FENCE_WAIT_LOG=1`: each one over 5 ms): the fence is 11 values (~1.5 frames) ahead of the GPU, already recorded and submitted,
no queue stopped, and the wait takes 14 -> 37 ms, growing: the GPU completes ~11 values in 35 ms where it does ~7 values a frame in
~13 ms with the wait. Pass timing (`RX_VK_PASS_TIMING=1`) with defer: 11.0 ms of timed GPU work a frame (base 9.8; the 2048x1152 pass
4f0c0b40 0.82 -> 1.34 ms) but nvidia-smi 89% busy at 56 fps (~16 ms busy a frame): ~5 ms of GPU-busy time a frame lies outside the
timed command buffers. So the host does not complete fences late (they wait on the GPU's own semaphore); the GPU itself slows when
CPU and GPU overlap. Not identified; candidates: contention on the imported host memory (guest pools the GPU reads and writes over
PCIe while the CPU writes them), WDDM residency work for the 1.8 GB of imports, the present's untimed blit. The base build's 0 game sync
waits a run are the wait's doing: the main thread drains the GPU each frame at its first submit.

Correctness: no build with the deferral on was kept or shipped; screenshots and flash detector not run with it (its fps loss made it
moot). Game folder: backup build restored. Not verified: other GPUs (the semaphore hang and the slowdown may be NVIDIA/WDDM-specific).
Next: find the untimed GPU time under overlap (Nsight or GPU timestamps around the present and imports) before any overlap scheme.

## TICK-019: GPU reads of imported host memory (2026-11-09)

**Result: the hypothesis does not hold. In Hyrule Field no shader, draw or dispatch reads imported guest memory: every bound pool
is served from its VRAM mirror already. What crosses PCIe is the engine's own, already-timed copies. No pool to move; the commit
adds the diagnostics only (behaviour unchanged).** `hyrule-walk`, limit 120, RTX 5070 (PCIe 5.0 x16).

What the GPU reaches in host memory, a frame (log lines every 600 submits; new ones in this ticket marked *):
- buffer bindings (uniform, storage, vertex, index, indirect, copy sources): all through `gpu_buffer_bind`; the existing
  "bound in guest memory, by pool" line never printed in a whole walk, and the lock-free path, which it did not count (*"GPU reads of
  imported guest memory": binds on the lock-free path), 0. Mirrors: 108 pools, 1696-1712 MB, i.e. every live import (63 live, 1744 MB).
- texel buffer views (`texel_buffer_view`, always the import itself, never a mirror) (*): 0 in the walk; GPU unswizzles from guest
  memory (*): 0 MB.
- the guest page table shaders follow pointers through (*`log_alloc_where`): 24 MB in memory type 4, heap 0, device-local
  host-visible (ReBAR): in VRAM. Recording streams (uniform/inline data, indices) (*): the same, VRAM.
- capture buffers, the mirror syncs' source (*): system memory, host-cached. The syncs copy ~11.6 MB a frame from them over PCIe
  (msync, 1.3 ms timed in TICK-016); rbrefresh ~2.1 MB from the imports; write-backs ~14 MB a frame into the imports (wb, 0.4 ms).
- `nvidia-smi dmon -s ut` (60-110 s): ~1.8-1.95 GB/s host->GPU and ~2.0-2.2 GB/s GPU->host, ~25 / ~28 MB a frame at 73 fps: the
  copies above plus the CPU's writes into the ReBAR streams. A few % of a PCIe 5.0 x16 link.

So there is nothing hot and read-mostly left in host memory to mirror; VRAM cost of the change: 0. The ~5 ms of untimed GPU-busy
time under overlap (TICK-017) is not reads of imports by shaders; still unidentified (candidates left: WDDM residency/paging for the
1.7 GB of mirrors + 1.1 GB of retired imports kept on Windows, the present's blit, copy throughput of the msync regions).

| Run | fps | 1% low | 0.1% low | p99 | > 8.33 ms | log mean 70-110 s | SM busy (dmon) |
|---|---:|---:|---:|---:|---:|---:|---:|
| base 1 / 2 | 73.8 / 72.4 | 37.4 / 28.5 | 20.4 / 8.7 | 20.0 / 21.2 ms | 100% | 73.6 / 72.3 | 74.0 / 70.3% |
| defer, slack 1, 1 / 2 | 73.4 / 71.4 | 37.3 / 37.4 | 15.1 / 22.2 | 19.5 / 20.9 ms | 100% | 72.9 / 71.5 | 74.2 / 73.0% |
| defer, slack 2, 1 / 2 | 72.2 / 73.5 | 36.6 / 42.9 | 17.0 / 31.6 | 20.3 / 20.1 ms | 100% | 72.1 / 73.5 | 72.8 / 73.0% |

(log mean: the "N presents, X fps" lines, time estimated from presents/fps.) Overlap (`RX_VK_CAPTURE_DEFER=1`, slack 1/2) is within
the runs' spread: neither helps nor hurts now, as in TICK-017 with slack. Left off.

Correctness (this build, diagnostics only): flash detector over two walks 18 and 16 events, all 1-2 blocks at 86 s (motion, as
TICK-015/016); screenshots every 1200 presents: lighting, fog, grass and HUD as in a base walk (sync5-2; not pixel-equal);
`HOS_SPEED_LOG=1` mean ratio 60.1 / 60.2 (the seconds of the walk, not the load). Shader cache stamp unchanged (no stamp input touched).
Not verified: other scenes (a pool over the mirrors' budget, or Kokiri, could still be read over PCIe: the new log line would show it),
other GPUs (a card without ReBAR puts the page table and the streams in system memory: `log_alloc_where` says where), the RX_VK_MIRROR=0
A/B (not run: with every pool mirrored there was nothing to compare against), Nsight.

## TICK-020: the whole frame in Nsight Systems (2026-11-09)

**Result: the 13.6 ms are accounted for. One Vulkan queue; the GPU is busy ~11 ms of a frame and idle ~4 ms, and every idle bubble
is the GPU waiting for the CPU to submit (none waits on a semaphore, the present or untimed work). The "untimed" GPU time of
TICK-016/017 is draw work: Nsight's per-workload timing does not split render passes, the in-engine timestamps do. The CPU side is
bound by the game's own threads plus one GPU drain a frame. The engine-side change tried (segments of the parallel replay in even
rounds) shortened the replay's recording wait but did not move the frame rate: not kept.** `hyrule-walk`, limit 120, RTX 5070.

How it was captured (Nsight Systems 2025.5.2 CLI, 12 s from 82 s after launch, the bench's environment):
`nsys profile --trace=vulkan,nvtx --vulkan-gpu-workload=individual --delay=82 --duration=12 --kill=true`. Without administrator
rights nsys cannot register its Vulkan layer (it writes HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers: "Failed to write to
registry", seen with an nvlog.config): no Vulkan event is collected. Passing its folder through `VK_ADD_IMPLICIT_LAYER_PATH`
loads both of its manifests (the plain and the system-wide one, same DLL) and crashes in vkCreateInstance; a folder holding only
`VkLayer_nsight-sys_windows.json` with an absolute `library_path`, plus `ENABLE_VK_LAYER_NV_nsight_sys=1`, both given with
`--env-var`, works (545k events). Not available without administrator rights: WDDM trace (queue packets, paging/residency, the
present's blit and DWM), GPU metrics (ERR_NVGPUCTRPERM), CPU sampling and context switches. CPU stacks came from the engine's
own sampler instead (`HOS_PROFILE=5 HOS_SAMPLE_ALL`, 12 s of the same walk, `scripts/crit.py`).
Under nsys the game ran at 66.4 fps (15.05 ms; ~73 without): read the shares.

GPU, a frame (default, nsys 15.05 ms; in-engine split from `RX_VK_PASS_TIMING=1` in the same walk, inflated by its timestamps):

| | ms | what |
|---|---:|---|
| queues | | one queue (the graphics+compute family's single queue); no copy or async-compute queue; 31 submissions a frame |
| GPU busy (union of submissions) | 10.97 | = nvidia-smi's ~74% |
| - replay submissions 0.3-2.5 ms | 6.12 | the game's passes and dispatches, segments of the parallel replay |
| - one long replay submission (~4.2 ms, once a frame) | 3.20 | render passes (the 2048x1152 scene passes); Nsight times only its non-draw items (~10 us) |
| - small replay submissions | 1.07 | |
| - submissions with no vkQueueSubmit2 of the app (empty, ~3 us each) | 0.53 | |
| - the main thread's memory-work submits | 0.05 | 2 a frame, ~22 us each |
| in-engine: passes / dispatches | 5.06 / 1.82 | game |
| in-engine: msync / wb / rbrefresh | 1.30 / 0.43 / 0.43 | engine (copies, TICK-015/016) |
| in-engine: barriers | 0.58 | game 0.24, engine 0.34 |
| present | ~0.02 GPU, 0.18 CPU | the f.done submit before vkQueuePresentKHR; the blit itself is not visible without the WDDM trace |
| GPU idle | 4.08 | |
| - waiting for a Presentation-thread submit | 2.11 | 4.5 gaps a frame (0.55 ms of them in gaps over 1 ms) |
| - waiting for a main-thread submit | 1.58 | 1.7 gaps a frame, mostly the one after the drain (below) |
| - work already submitted, GPU not running it | 0.07 | 17 gaps a frame under 10 us: no semaphore or present bubble |

The frame's shape (one frame of the capture): the GPU runs the long submission and the present's submit; the main thread waits in
the capture wait (vkWaitSemaphores, 1.8 ms a frame under nsys) until the GPU has finished everything up to the frame's last
submission; the GPU then has nothing until the main thread's memory-work submits (~1.3 ms later) and the Presentation thread's first
big segment (~3.2 ms after the drain). Every frame the GPU drains, then waits for the CPU, then the CPU waits for the GPU.

CPU, a frame (HOS_SAMPLE_ALL, 2877 instants of 2 ms, unprofiled 13.6 ms frame; nsys for the Vulkan calls):

| thread | running | blocked in |
|---|---:|---|
| sead::MainThread | 22% (3.0 ms) | the game's WaitLightEvent for its job threads (f_8007fe60) 43% (5.8 ms); the capture wait for the GPU (f_806668b0) 20% (2.7 ms); other game waits and SleepThread 9% |
| Presentation Thread | 16% (2.2 ms; vkQueueSubmit2 0.48, vkQueuePresentKHR 0.18) | the parallel replay waiting for its workers (run_submit) 23% (3.1 ms); idle in ReceiveLightMessageQueue 47% |
| replay workers (4) | ~1 ms a frame of Vulkan calls in all (vkCmdBindPipeline 0.48) | idle otherwise; while the Presentation thread waits for them ~1 of 4 records, the others idle |
| game job threads (BaseProcExecutor0-5, ModuleSystemWorker1-5) | 13-27% each | game |

23% of instants no game thread runs: the main thread is then in the capture wait (70%) and the engine's threads are idle: the
GPU-bound part of the frame (~3 ms). The rest of the frame the game's own threads run (game CPU work ~10 ms).

RX_VK_CAPTURE_DEFER=1 under nsys (66.7 fps, 14.99 ms): GPU busy 10.77 ms (6.22 / 3.04 / 0.98 / 0.47 / 0.05 as above), idle 4.22
(Presentation thread 2.43, main thread 1.44, GPU-side 0.06). The main thread still waits 2.25 ms a frame in vkWaitSemaphores: the
game's own nvnSyncWait for the present fence (TICK-017) takes the capture wait's place, and the frame keeps the same
drain-then-produce shape: that is why the deferral is neutral. The ~5 ms of untimed GPU-busy time of TICK-017 did not show: the GPU
did the same work a frame in both modes and no gap had submitted work waiting. It was measured at 56 fps without nsys; with nsys's
CPU overhead both modes are CPU-bound at ~66 fps, so whatever slowed the GPU when the CPU ran ahead (contention, WDDM
residency/paging) needs the WDDM trace or GPU metrics (administrator) to be seen.

Largest costs, and whose: the game's passes and dispatches ~6.9 ms of GPU (game); the game's job threads ~10 ms of CPU (game); the
drain a frame (~3 ms in which only the GPU works: the engine's capture wait by default, the game's fence wait with the deferral);
the engine's copies ~2.2 ms of GPU; the replay's wait for its slowest segment ~3.1 ms a frame on the Presentation thread,
overlapped with the game's threads.

Tried (engine, not kept): the parallel replay splits a command buffer into total/160 segments, at most 6 (often 5 on 4 threads: a
second round of one). Segments in two even rounds (8 on 4 threads, boundary points every 32 draws instead of 64): the recording of a
parallel command buffer 1.10-1.24 -> 0.99-1.00 ms, frame rate unchanged:

| Run | fps | 1% low | 0.1% low | p99 | > 8.33 ms | log mean 70-110 s |
|---|---:|---:|---:|---:|---:|---:|
| base 1 / 2 | 72.2 / 72.6 | 41.2 / 38.9 | 27.6 / 21.6 | 20.6 / 20.7 ms | 100% | 72.3 / 72.7 |
| even rounds 1 / 2 | 71.7 / 73.5 | 42.0 / 37.7 | 22.2 / 16.0 | 19.5 / 19.1 ms | 100% | 71.9 / 73.2 |

(A first variant, even rounds only when total/160 called for two or more segments, averaged 6.0 segments: 73.6 fps, one run.)
That wait is overlapped with the game's threads and not on the frame's critical path; the change adds submits and barriers and was
reverted (no build with it left in the game folder). What would move the frame: less GPU work before the drain (the engine's msync
copies, 1.3 ms, are its largest item: ~1570 small regions a frame from host-cached capture buffers, ~8.7 GB/s) or less game CPU
work; overlap (the deferral) only moves the wait into the game.

Not verified: WDDM-level events (paging, residency, the present's blit, other processes' GPU work), GPU counters, CPU context
switches (all need administrator rights); other scenes and GPUs.

## TICK-021: the mirror syncs' copies in one compute dispatch (2026-11-09)

**Result: engine copy GPU time (msync + wb + rbrefresh) 2.19 -> 0.81 ms a frame (-63%); msync 1.35 -> 0.29 ms, rbrefresh 0.41 ->
0.09 ms, wb unchanged. fps +0.9 (72.4 -> 73.0 log mean): the frame is held by the CPU side (TICK-020), not by these copies.**
`hyrule-walk`, limit 120, RTX 5070; A/B in one build with `RX_VK_MSYNC_SCATTER=0` (as before).

What it was: ~1650 copy regions a frame (11.5 MB, mostly 1-2 dirty pages each, scattered) in ~53 vkCmdCopyBuffer calls. At ~25 us
a call and ~0.8 us a region, the cost was per region, not per byte. Adjacent pages were already merged (TICK-015). Pages next to each
other in the pool are consecutive slots in their capture (slots follow the sorted written-page list), so merging across a gap
would copy pages the capture does not hold. Filling a gap from guest memory is wrong for lazy pools (the GPU's words are only in
the mirror) and for captured pools (guest memory is the game's next frame).

**Scatter copy** (`vk_memory.cpp`, "the mirror syncs' scatter copy"; `shaders/msync_scatter.comp`): each sync region is cut into
entries of at most 4 KB (source and destination buffer device addresses, size). The entries go into a 4 MB host-visible ring, and
one dispatch copies them, one 256-invocation workgroup an entry, 16 bytes an invocation. The presync
(`memory_sync_mirrors_before_segments`) batches every pool into one dispatch. A sync at a binding gets one of its own. Readback
refresh pages use the same path, after its existing barrier and still last of the uploads. Full barriers before and after each
dispatch. Capture buffers now carry STORAGE and SHADER_DEVICE_ADDRESS usage. A region not 16-byte aligned, or a batch over half the
ring (a load), goes through vkCmdCopyBuffer as before. The ring waits for its lap-before-last at wrap (~90 frames back, never
waited for). The order of the copies and what they copy is unchanged: the same pages from the same sources. The pipeline sits
outside the program cache. No shader-stamp input was touched (`vk_memory.cpp` and `shaders/` are not stamp inputs).

GPU time a frame by kind (`RX_VK_PASS_TIMING=1`, one walk each, last report; inflated by the timestamps):

| | before (`RX_VK_MSYNC_SCATTER=0`) | after |
|---|---:|---:|
| msync | 1.349 ms, 53 spans, 1650 regions, 11.47 MB | 0.291 ms, 31 dispatches, 2938 entries, 11.48 MB |
| rbrefresh | 0.412 ms, 544 pages, 2.12 MB | 0.093 ms, 544 pages, 2.13 MB |
| wb | 0.428 ms, 14.0 MB | 0.429 ms, 13.9 MB |
| upload-buffer barriers | 0.074 ms, 71 | 0.162 ms, 123 (the dispatches' own) |
| copies + their barriers | 2.26 ms | 0.98 ms |

| Run | fps | 1% low | p99 | log mean 70-110 s |
|---|---:|---:|---:|---:|
| before 1 / 2 | 72.9 / 72.6 | 41.5 / 39.6 | 19.4 / 20.2 ms | 72.4 / 71.9 |
| after 1 / 2 | 73.7 / 73.7 | 40.8 / 39.2 | 19.0 / 19.6 ms | 73.0 / 73.0 |

CPU cost: the entries are built where the regions were (the replay's recording, under pools_mu): ~3000 32-byte entries a frame plus
a ~100 KB memcpy into the ring. Not timed apart, and too small to show in the fps.

Not done, and why:
- Skipping unchanged pages (CPU compare against a shadow): unsafe as it stands. A page whose new CPU bytes equal the last synced ones
  may still have GPU words in the mirror: the exposure meter is the classic case (CPU writes 0, GPU adds, CPU writes 0 again). A
  skip would keep the GPU's sum (the TICK-006 failure). Making it safe needs the set of pages the GPU wrote, which is incomplete
  (bare table writes past 64 bytes). It would also add a ~11 MB compare a frame on the replay thread. After the scatter, msync is
  0.29 ms, so there is little left to win.
- Sub-page granularity: the write watch is per page.
- Write-backs: ~14 MB in ~140 runs, ~44 us a span: bandwidth, not per-region overhead (~32 GB/s, VRAM -> host). The runs also have
  8-byte cuts (`cut_cpu_words`), so they are not 16-byte aligned.
- Readback refresh limited to the pages actually read or written: its GPU cost is now 0.09 ms, and changing it touches the
  ordering of TICK-006.

Correctness: flash detector (`scripts/framelog.py`, whole walk) after 13 and 20 events, before 9: all 1-2 blocks at 86.0-86.9 s
(motion, as in TICK-015/016). Screenshots every 1200 presents: lighting, fog, shadows, grass and HUD as in the base walk (not
pixel-equal). `HOS_SPEED_LOG=1`: steady seconds 59.99 / 60.00 after, 60.03 before.
Not verified: other scenes (Kokiri's exposure, cutscenes, loads: the plain-copy fallback for big batches only seen at load),
other GPUs (AMD/Intel buffer-device-address reads of host-cached memory), long sessions.

## TICK-011: the game's CPU critical path (2026-11-09)

**Result: the CPU side of the frame is the game's own job graph, spread over many guest functions; neither host
synchronisation, the virtual cores, thread priorities nor the spin moves it. One host change kept: the dirty-page scan
of the submit-time capture skips 256 clean pages a load (main thread). Log mean 72.5 -> 74.6 fps over 4 + 4 runs (A/B
against the same tree without it: 73.4 -> 74.6); within the bigger single-run spread, but every run after is above every
run before.** `hyrule-walk`, limit 120, Ryzen 7 7800X3D (8 cores / 16 threads), RTX 5070. Lifted code unchanged (no -Lift).

The frame (~13.5 ms; `HOS_SAMPLE_ALL`, 25 s of the walk, ~6100 instants; ms a frame = share x 13.5 ms):

| thread | running | blocked in |
|---|---:|---|
| sead::MainThread | 21% (2.9 ms) | the job group's WaitLightEvent (f_8007fe60, group f_80070b20) 52% of its blocked instants (5.6 ms); the capture wait (f_806668b0 -> vkWaitSemaphores) 31% (3.3 ms); SleepThread (f_800998a0) 5%; f_83acfb90's event 5% |
| BaseProcExecutor0 | 24% (3.3 ms) | its worker loop f_8004ab40 |
| BaseProcExecutor1-5, ModuleSystemWorker1-5, NavMeshCAStepDepWorker1 | 12-19% each | the worker loops |
| Presentation Thread | 16% (2.2 ms) | ReceiveLightMessageQueue (f_834a3060) |
| replay workers | 8% | idle |

The chain: the main thread runs ~2.9 ms (of which the engine's submit path under f_806668b0 ~0.5 ms: `collect_in` 0.2,
`capture_make`, `refresh_pool_ranges_locked`, `take_capture`), hands a job group to the workers and waits 5.6 ms for it,
then waits ~3 ms for the GPU (TICK-010/020). While it waits for the job group, the number of job threads working
(spinning excluded): 0 for 13%, 1 for 34%, 2 for 20%, 3+ for 33% of the instants. The tail is one thread at a time:
BaseProcExecutor0 22%, NavMeshCAStepDepWorker1 17%, BaseProcExecutor1 11%, ModuleSystemWorker1 9% of those instants, in
the jobs under f_8004ab40 (f_800b3ce0, f_8016b320, f_819b8e20, f_83af6150) and under the main's own worker loop f_8007fe60
(f_820da77c). That is the game's job graph: a chain of dependent jobs, not a lack of cores.

Where the cost is: spread. On the busiest job thread no guest function has more than 6% of its running samples
(f_801d99a0 6.0%, f_81501400 2.5%, f_81a20b40 1.9%, ...); f_82bde830 (main, 9%) is game logic with atomics, not a libc
routine. The software FMA helpers (`a64_fpx_mul32_fma32`, `recomp_fpe_fma32`) ~3% of a job thread. All of these functions
are already in the hot list and the PGO profile (hot_funcs.txt, hookshot_pieces.prof): no lifter-level item stands out.
Host share of the running samples, all threads: `spin_until_set` 5.1-5.5% (the 20 us spin before an event or queue
sleep; 12% of a job thread's running time, mostly the workers waiting for their next job), `obj_lookup` + `obj` ~2.6%
(LockMutex from f_80364080 on the main thread: 3% of its time), `collect_in` 1.2-1.5%.

Synchronisation, measured (`HOS_WAKE_LAT=1`): signal to running again 5-7 us on average for the job threads and the main
thread (longest 120-260 us), ~40-50 wake-ups a frame per job thread: well under 0.5 ms a frame per thread, and the main
thread itself ~20 sleeping wake-ups a frame. Not the frame's limit.

Virtual cores (`HOS_VCORES`, six, the game's core masks): new diagnostic `HOS_VCORE_LOG=1` (hos_thread.c: per thread,
with the `vcores:` line, its waits for a core and the time in them). In the walk the game threads wait for a core 0.2-4 ms
a second each (sead::MainThread 1.5 ms a second); only AudioFrameworkThread (35 ms/s) and NavMeshDepWorker2 (22 ms/s)
wait more, neither on the frame's path. The total wait count varies a lot between runs (1.4k to 25k a 5 s) at the same fps.

Experiments (env only, each 2 runs; log mean 70-110 s; base 71.9 / 72.2 / 73.7 / 72.2):

| Run | fps | 1% low | p99 | log mean |
|---|---:|---:|---:|---:|
| `HOS_SPIN_US=0` (no spin) | 73.8 / 72.9 | 36.3 / 36.1 | 20.8 / 21.4 ms | 73.1 / 72.5 |
| `HOS_RESERVE_CORE=0` (game threads also on the last core) | 74.2 / 73.4 | 41.9 / 38.6 | 19.6 / 20.1 ms | 73.6 / 72.8 |
| `HOS_PRIO=MainThread=2,Presentation=2` | 72.4 / 73.4 | 36.3 / 39.3 | 21.1 / 19.1 ms | 71.8 / 72.9 |
| `HOS_PRIO` main, job threads, Presentation = above normal | 73.9 / 73.2 | 39.4 / 41.1 | 19.4 / 18.9 ms | 73.5 / 72.7 |

All within the spread: the 20 us spin, the core kept free (used only by the async replay, off by default) and Windows
priorities do not limit the frame. A larger per-thread object cache (1024 -> 4096) did not lower `obj_lookup`'s share
(the misses are not capacity): not kept.

Kept: `collect_in` (host/rx_backend.c, the soft write watch's dirty bytes read at each submit: twice a frame on the main
thread, before the capture and in memory_collect_written) first tests 256 pages with one 256-byte load while the range is
clean (before: 32). Same pages collected; only the scan is shorter.

| Build | fps | 1% low | p99 | > 8.33 ms | log mean 70-110 s |
|---|---:|---:|---:|---:|---:|
| backup build (game folder before) 1-4 | 72.6 / 72.9 / 74.2 / 72.9 | 38.3 / 36.0 / 43.9 / 36.9 | 20.9 / 20.8 / 19.1 / 21.2 ms | 100% | 71.9 / 72.2 / 73.7 / 72.2 |
| same tree without the change (A/B, interleaved) | 74.7 / 73.4 | 39.1 / 45.9 | 19.3 / 19.1 ms | 100% | 74.0 / 72.7 |
| with the change + 4096 cache (interleaved) | 75.4 / 76.0 / 74.7 / 74.5 | 41.6 / 40.8 / 41.1 / 32.5 | 19.3 / 18.5 / 19.0 / 19.2 ms | 100% | 74.7 / 75.4 / 74.1 / 74.1 |
| final (the change only) | 74.9 / 75.4 | 39.2 / 45.5 | 19.2 / 18.5 ms | 100% | 74.3 / 75.0 |

Correctness: no game logic touched. `HOS_SPEED_LOG=1` ratio 59.8-60.3 a second through the walk (both runs). Flash
detector over two walks: 13 and 9 events, all 1 block, darker, ~30-33 s into the log (motion; TICK-021: 13/20 after, 9
before). Screenshots every 1200 presents: picture as in the base walk.

What would move the frame: the job chain is the game's, so only faster guest code on it (a lifter-level gain across many
functions, e.g. the soft FMA helpers or the A64_POLL/A64_MARK overhead in hot loops, needs a -Lift build and was not tried
here) or fewer of the chain's waits for the GPU (the ~3 ms drain, TICK-020). 120 fps (8.3 ms) is not reachable on the CPU
side alone: the main thread's own work plus its job group is ~8.5 ms.

Not verified: Kokiri and other scenes; limit 144; other CPUs (fewer cores: the virtual cores and the spin may matter there);
the lifter-level options (no -Lift build).

## TICK-022: lifter-wide overhead in the lifted guest code (2026-11-09)

**Result: one lifter change kept: the software FMA calls are ~98% gone and the result bits are unchanged. Frame rate unchanged
(within the spread). The job chain is the same ~8.4 ms. The other candidates are already cheap, or need their current cost to be
correct.** `hyrule-walk`, limit 120, Ryzen 7 7800X3D, RTX 5070.

**What the software FMA was.** The FPX1 fast paths already use the host FPU: x86-64-v3 for all code, so `__builtin_fma` for binary64.
binary32 FMA went through binary64 and fell back at binary32 midpoints. The `recomp_fpe_fma32` exact body (and
`a64_fpx_mul32_fma32`, the lane fallback of vector FMUL) ran whenever a fast path refused. A probe build counted why it was called
(Hyrule walk, 12.6 M calls in ~100 s, FZ on, fpsr 0x9f):
quiet-NaN operand 98.5%, a tiny/zero result under FZ 0.7%, normal results 0.9% (midpoints, and the mul fast path's KEEP rules),
signalling NaN 0, infinity < 0.01%. Most of the calls came from vector FMULs over a lane that holds a NaN.

Changes (`third_party/a64-kit`):
- `a64_lift.py`: `a64_fpx_qnan_fma32` in front of the exact body in every `*_fma32` helper. It covers a quiet NaN operand with no signalling
  NaN, and returns the addend's NaN, else the first factor's, else the second's, with no flag (as the exact body does). It leaves to the body:
  signalling NaNs (IOC), a quiet NaN addend with inf*0 (default NaN + IOC), and under FZ any subnormal operand (flushed: IDC, and
  a flushed factor can make inf*0). FPCR.DN is clear by `RECOMP_FPX_OPEN`.
- `suyu/arm64_to_c.h`: FMADD/FRECPS/FRSQRTS binary32 fast paths use `__builtin_fmaf` (vfmadd, one rounding) when built with FMA3
  (`RECOMP_FPX_FMA64`), and no longer go through binary64 with a midpoint fallback.

FP equivalence (host tests, scratch):
- Hardware `fmaf` against the old binary64 route: 200 M operand triples (random, low-precision mantissas, subnormals, zeros,
  infinities, NaNs). 0 differences where the old route kept a result. NaN results are never kept. 0.2% of the triples were midpoints
  that the old route sent to the exact body. `fmaf` rounds them once, correctly, as AArch64 does.
- The quiet-NaN path against the exact body (`recomp_fpe_fma32` text from `sa/gen/a64_suyu.c`): 50 M triples, FZ on/off, IDC set/clear.
  10.8 M took the fast path, with 0 differences in value or fpsr. A first version kept FZ subnormals while IDC was set. That gave 49 k
  differences (a flushed factor makes inf*0 -> default NaN), so it now leaves them to the body.

A/B, 4 runs each, interleaved (base = TICK-011 build; fp = this change; mark = fp plus the experiment below):

| config | fps (4 runs) | mean | 1% low mean | p99 mean | log mean 70-110 s |
|---|---|---:|---:|---:|---:|
| base | 75.3 76.0 75.5 76.3 | 75.8 | 39.7 | 19.0 ms | 75.8 |
| fp | 75.1 75.1 74.1 75.8 | 75.0 | 40.0 | 19.8 ms | 75.3 |
| mark | 76.0 75.1 75.9 75.6 | 75.7 | 41.2 | 19.0 ms | 75.6 |

All within the run spread (~2 fps). The chain (`crit.py`, 12 s from 80 s, ~3000 instants): base main runs 21.8% and waits for the job group
41.6% of instants; fp 21.3% / 41.4%. That is ~8.4 ms either way. The FMA calls were a few us a frame spread over job threads, not on the chain.

Not kept:
- **A64_MARK only when crossing** (`RX_MARK_CROSS_ONLY` experiment): the second dirty-page store only when a store crosses into the next page.
  The exe is +26 MB (1.22 M marked store sites). +0.6 fps against fp, -0.1 against base: noise. The earlier note (MKW, -0.3 ms, +20 MB)
  does not show here.

Measured and left:
- **A64_POLL**: 440 k loop-header polls. Each is a relaxed load of `c->poll` plus a predicted-not-taken branch (`a64_poll` is out of line).
  On x86 that is a plain mov + cmp/jne on a line the thread owns. Hoisting it out of loops would break preemption of long guest loops
  (the scheduler's interrupt), for ~1 cycle an iteration.
- **Atomics**: LDAPR is an acquire load (a plain mov). LDAR is a seq_cst load (a mov). LSE ops and CAS are lock-prefixed RMWs, which x86 needs
  for any atomic RMW. STLR is a seq_cst store (xchg), which RCsc STLR->LDAR ordering requires on x86. No cheaper lowering keeps the semantics.
- **Compiler flags**: every piece is already built for `-march=x86-64-v3` (AVX2, FMA, BMI1/2, LZCNT, MOVBE; host_cpu.py picks it, RX_MARCH
  overrides) with `-ffp-contract=off` (needed: the exact bodies assume C rounding as written). `-march=native` would add AVX-512 (Zen 4), and
  the game would then stop on a CPU without it. It was not tried: the guest code is scalar and branchy, and the FMA was the only x86-64-v3
  feature the lifted code did not use.

Correctness: HOS_SPEED_LOG ratio 59.8-60.1 (base and fp). Screenshots at frames 1200..9600 (RX_VK_SHOT_EVERY=1200): the same walk, Link on
the same path. Shots are per frame, so the positions differ only by the fps at that frame. The flash detector over 2 walks (fp, 30 s
each): 13 and 12 small events (1-2 blocks, at the same spot ~20 s in), 0 on made frames. No base framelog in this ticket for comparison.
