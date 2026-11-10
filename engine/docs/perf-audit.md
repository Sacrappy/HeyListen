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

## TICK-023: VRAM saturation (2026-11-09)

RTX 5070 12 GB, kokiri-walk (title -> load of the Kokiri Forest save, then the walk), 115 s, nvidia-smi every second.
A periodic line now gives the breakdown every 600 presents: `vk: VRAM breakdown: heaps U of A MB (P%); textures (ASTC decoded,
rendered live, rendered finalized, uploaded); mirrors; free in VMA blocks; other (staging, pipelines, streams, swapchain, upscaler)`.

| run | nvidia-smi peak | heaps (driver) at end | textures | mirrors | free in blocks | fps (60 s cut) |
|---|---:|---:|---:|---:|---:|---:|
| base | 10667 MB | 9600 / 10862 (88%) | 5704 (budget fixed 6144) | 1440 | 1678 | 64.7 |
| fix (budget only) | 10624 MB | 9740 / 10939 (89%) | 3643 (budget fell to 2654) | 1312 | 3960 | 79.2 |
| fix2 (+ big images dedicated) | 9972 MB | 8401-9068 (77-83%) | 5378-6003 | 1328 | 473-1468 | 82.1 |
| fix3 (+ mirrors under pressure) | 10507 MB | 8819-9494 (81-87%) | 5492-5875 | 1360 | 984-1665 | 81.3 |

Kept without need: (1) evicted textures' space stayed in VMA's 128 MB blocks (big render targets and arrays sharing blocks), so
eviction never lowered the driver's count (4 GB free in the blocks); (2) a fixed 6 GB texture budget whatever the rest held;
(3) render targets of a course left behind kept 3600 presents even over budget; (4) TICK-015 mirrors holding left-out write-backs,
never given back.

Fixes: images of 16 MB and up (4 bytes a texel estimate) get dedicated memory (RX_VK_BIG_IMAGE_MB, 0: as before); the texture
budget is min(old, 80% of the driver's allowance (RX_VK_VRAM_TARGET_PCT) - what the rest holds), block free space counted up to
512 MB only (counted whole, the first try ratcheted the budget down: each eviction moved bytes into "rest"); an image VRAM refuses
(placed in system memory) drops the budget to 7/8 of the textures at the next check (evict before the next refusal); over budget,
rendered textures that can be written back go after 600 unused presents instead of 3600 (written back first, as before);
idle lazy-held mirrors are listed, their left-out pages (minus the CPU's pages/words and texture storage) copied mirror -> guest at
the end of the next submission, then the mirror freed after that copy (RX_VK_KEEP_LAZY_MIRRORS=1: as before); past 80% of the
allowance, mirrors idle 600 submissions go as if their budget were short.

Result: the target (< 80% of the allowance) is NOT met steadily: 77-87% at the end of runs, smi peak 9.97-10.5 GB vs 10.67 GB.
What is left is live rendered textures (2-3.8 GB, not flushable or used within 600 presents, kept for correctness) and mirrors of
pools in use (1.3 GB). No "refused images", no mirrors given back in these runs (none idle long enough). fps 64.7 -> 79-82 (one
base run; variance between runs not measured). HOS_SPEED_LOG ratio ~60. Screenshots every 600 presents across title, load and walk:
normal. Not verified: long sessions, several area changes (Hyrule Field <-> Kokiri: no scripted path), the lazy-mirror write-back
path (did not trigger), flash detector.

## TICK-025: Castle Town Market, CPU-bound at ~57 fps (2026-11-09)

**Result: no host change. "OptimizeChangeBanc" is real guest work, not a spin, and it is not on the frame's path. The frame
is the game's job group (as in TICK-011) plus a GPU wait. The replay threads are idle when the main thread waits, so per-draw
replay cost does not limit this spot either.** kokiri-idle at the most recent save (Castle Town Market), limit 120, Ryzen 7
7800X3D, RTX 5070. Game-folder build as found (no build made, game folder untouched).

**OptimizeChangeBanc** (guest priority 27, core mask 3-5). Its loop is f_834a2e50 -> f_80df2680: `while (msg != quit) { calc();
msg = receive(+152 ? TryReceive : Receive); }` (f_834a3060 = ReceiveLightMessageQueue / TryReceive). The flag is set, so the
thread polls its queue without blocking and runs its calc continuously. This is how the game made it, a background thread
at low priority. On Horizon it would get only the time the priority-16 threads on cores 3-5 leave. Here it gets the same
from the virtual cores: the executors kick it, and BaseProcExecutor3-5 wait 0.2-0.5 ms a second for a core. The calc's
f_805aeea0 (82% self, 5,300 lifted lines) spreads its samples over many addresses. The hottest block (host 0x14196221e..,
guest ~0x805afbc0-0x805afda4) is a binary search over a sorted table with an inlined 8-byte-unrolled strcmp. It runs
no exclusives, no yield/WFE and no host primitive (ntdll is 2% of its leaves). So it is real lookups, not a wait. It also
records NVN command buffers (its thread logs `nvn:` recording lines).

**The frame** (slow1.txt, 4,797 instants; `crit.py`):

| thread | running |
|---|---:|
| OptimizeChangeBanc | 97.9% |
| BaseProcExecutor0-5 | 34-39% |
| sead::MainThread | 23% |
| NavMeshCAStepDepWorker1 | 23% |
| ModuleSystemWorker1-5 | 14-17% |
| Presentation Thread | 17% |
| replay workers | 10.5% (84.5% of their leaves in ntdll: idle) |

The main thread is blocked 77% of instants: the job group (WaitLightEvent in f_8007fe60) 63%, f_806668b0's capture/GPU wait
13%, f_83acfb90's event 10%, SleepThread 7%. In every instant where main waits in f_806668b0 (480) or f_83acfb90 (370), the
Presentation thread is idle in its message queue. The replay is not what main waits for. "f_805aeea0 innermost in 83%
of main-blocked instants" is OptimizeChangeBanc, which always runs. The job threads were all idle (waiting for their next
message) while main waited for the group in 209 instants (4.4%). The rest is the game's dependent job chain. Virtual cores
(`HOS_VCORE_LOG=1`): core 1 is contended (NavMeshCAStepDepWorker1, priority 22, waits 114 ms a second behind
ModuleSystemWorker1, priority 16; aglTextureDataMgr::Compaction 316 ms/s). That is Horizon's priority order, kept.

**Oversubscription.** About 5 logical CPUs busy on average (OptimizeChangeBanc 1, executors 6 x 0.37, the rest < 2) of the
14 the game threads may use (the last physical core is reserved). Not oversubscribed.

Experiments (env only, the game-folder build; fps / 1% low / p99 / log mean 70-100 s):

| run | fps | 1% low | p99 | log mean |
|---|---:|---:|---:|---:|
| base (HOS_VCORE_LOG=1) | 58.5 | 33.8 | 23.1 ms | 58.3 |
| base | 60.9 | 36.3 | 23.8 ms | 60.9 |
| `HOS_PRIO=OptimizeChangeBanc=-2` | 58.3 / 54.2 | 35.6 / 26.2 | 24.7 / 27.8 ms | 58.1 / 54.6 |
| `HOS_PIN=OptimizeChangeBanc=c000` (on the reserved core) | 59.7 | 34.6 | 24.6 ms | 59.6 |

The ticket's baseline was 56.6. The run-to-run spread here is 54-61 fps. Neither lowering OptimizeChangeBanc's Windows
priority nor moving it off the game's cores moves the frame. Nothing was kept.

Replay per-draw cost: across the workers, build_descriptors is the largest self function (~13% of their running samples),
then draw and gpu_buffer_bind. It is spread, and off the path at this spot (above), so it was not changed.

What would move this spot: the job chain's guest code (TICK-011/022: lifter-level gains, or the unmerged FMA fast path:
`a64_fpx_mul32_fma32` + `recomp_fpe_fma32` are ~9% of the leaves running while main waits here, more than in the Hyrule
walk) and the GPU wait in f_806668b0 (~2.3 ms a frame).

Not verified: a build with the FMA fast path at this spot; GPU-side cost at this spot (Nsight); Hyrule Field (nothing changed).

## TICK-026: hot-function map, native replacement framework, first replacement (2026-11-09)

**Map**: see `docs/hotmap.md` (Castle Town Market; `scripts/hotmap.py`). Nothing dominates the chain. The largest self
time is the module system (f_8009bb80 3.9%) and the executors' dispatcher (f_8004ab40 2.8%). After that comes a tail of
self-contained FP kernels at 1-2.5% each, plus ParallelDecompThread's decompressor (f_801102e0 13.5%, probably off the
frame's path).

**Framework**: `nx2/native_replace.py` plus `host/hos_native.c` (development.md, "Native replacements"). There is one
switch per replacement, `HOS_NR_<NAME>` = 0/1/diff, and every replacement defaults to off. The differential mode
samples 1 call in `HOS_NR_DIFF_EVERY`. The gate is added only when the lifted block's text hashes match.

**First replacement**: `HOS_NR_STRCMP_805AFC70`. It replaces the inlined strcmp of f_805aeea0's binary search (guest
0x805afc70-0x805afda0, inside the search loop 0x805afbc0-0x805afda4). The native version compares 8 bytes per step
where no page boundary is crossed and computes the guest's exact result registers and NZCV. The guest block writes no
memory. **Differential run**: one full 105 s bench run with DIFF_EVERY=16 made 14.9 M checks over 238 M calls, all on
OptimizeChangeBanc, with **0 differences**. HOS_SPEED_LOG ratio was 60.1-60.9.

A/B, kokiri-idle, 105 s, interleaved, same binary, env switch only (fps / 1% low / p99 / log mean 70-100 s):

| run | off (guest) | on (native) |
|---|---|---|
| 1 | 51.1 / 12.3 / 35.1 ms / 52.8 (hitch run, max 168 ms) | 57.7 / 33.3 / 25.1 ms / 58.2 |
| 2 | 57.7 / 39.0 / 23.4 ms / 57.5 | 61.8 / 42.7 / 21.6 ms / 61.8 |
| 3 | 55.3 / 28.1 / 29.2 ms / 55.2 | 60.0 / 32.7 / 24.8 ms / 60.3 |
| mean | 54.7 (56.5 without the hitch run) / 26.5 / 55.2 | **59.8 / 36.2 / 60.1** |
| extra off runs | profile run 56.6 / 32.4; differential run 55.1 / 28.5 | |

On won every pair, by about +3-5 fps. That is more than expected, because OptimizeChangeBanc is not on the frame's
path (TICK-025). A likely reason is that its lookups now take less core time from the executors on the virtual cores
it shares, but this was not proven. Three pairs fall within the known 54-61 spread, so this needs more runs before
the default is turned on. **The default stays off.**

Not verified: screenshots (RX_VK_SHOTS only writes in headless mode and no headless run was made). NPC behaviour was
checked only through the differential run and the speed ratio. Hyrule Field was not run. No profile was taken with
the replacement on.

## TICK-030: teal ground in Castle Town after TICK-023 (2026-11-09)

**Cause: TICK-023's "big images in dedicated memory", not the evictions.** kokiri-idle (Castle Town Market save), 110 s,
`HOS_HEADLESS=1` + `RX_VK_SHOTS` every 600 presents (headless reaches the same VRAM: heaps 77-80%, textures ~5 GB). The
main build showed the flat teal ground and the cyan glow from the scene's first frame (5400 presents), with no texture
evicted before it, so the user's "ASTC decoded" collapse (a low-draw phase: 263 draws, 273 MB used in 8 presents) was
not it. Bisected with `RX_VK_BIG_IMAGE_MB` on the same build: 16 (default) and 32 teal, 64, 300 and 0 right. Logging the
images given memory of their own (`RX_VK_LOG_DEDICATED=1`, new) showed the 32-64 MB set (by TICK-023's 4-bytes-a-texel
estimate) is the game's own block-compressed ground textures: BC5/BC6H/BC7 3064 x 2656, 2288 x 6112, 1144 x 3056 x 3
(8-14 MB really), created MUTABLE | BLOCK_TEXEL_VIEW_COMPATIBLE | EXTENDED_USAGE with storage use (shaders write their
blocks through raw R32G32(B32A32)_UINT views). In a dedicated allocation (NVIDIA RTX 5070) they render flat teal; in the
pool, right. Why the driver differs is not established; the images are not aliased anywhere in the engine.

Fix: `create_image` (vk_vma.cpp) no longer forces dedicated memory for BLOCK_TEXEL_VIEW_COMPATIBLE images; the big ASTC
textures re-encoded as BC3 (written by buffer copies, the 1365 x 1365 x 90 terrain arrays and the 128 x 128 x ~390 arrays)
ask for it from vk_texture.cpp by their real size (a byte a texel). Budget (texture_end_frame): VMA's block free space now
counted as reusable whole (TICK-023's 512 MB cap: `RX_VK_BUDGET_FREE_CAPPED=1`); the budget never falls below what the
last 8 presents used plus an eighth (the user's log had 6.2 GB of textures against a 5.25 GB budget, 630 retired at once);
`RX_VK_EVICT_MIN_UNUSED` clamped to >= 3. Mirrors (RX_VK_KEEP_LAZY_MIRRORS) unchanged: not implicated by the bisection.

| run | ground | nvidia-smi peak | heaps at end | textures (ASTC decoded) | retired | fps in Castle Town |
|---|---|---:|---:|---:|---:|---:|
| pre-TICK-023 (backup-tick023) | right | 10188 MB | 8317 / 10753 | 5459 | - | 58.1 |
| main (TICK-023) | teal | 9844 MB | 8334 / 10757 (77%) | 5330 (2323) | 714 | 56.6 |
| main, RX_VK_BIG_IMAGE_MB=0 | right | - | 8281 / 10781 (77%) | 5450 (1903) | 799 | 56.8 |
| fix run 1 | right | 9880 MB | 8354 / 10779 (78%) | 5948 (2966) | 676 | 60.8 |
| fix run 2 | right | 9730 MB | 8407 / 10707 (79%) | 5097 (2251) | 682 | 57.6 |
| fix run 3 | right | 10012 MB | 8455 / 10692 (79%) | 5060 (2234) | 683 | 57.1 |
| fix, RX_VK_VRAM_TARGET_PCT=50 | right | 8625 MB | 6614 / 11175 (59%) | 3308 (1804) | 2423 | 56.8 |

fps: mean of the log's 120-present windows from present 6000 (Castle Town), one run each, variance ~2-4 fps. Under forced
pressure the budget sat at the recent-use floor (3.3 GB) and the ground stayed right. Not verified: windowed (non-headless)
play, walking/area changes, other GPUs (AMD/Intel) for the dedicated BC case, the lazy-mirror free path (no mirror freed
while drawn was seen, not proven safe either), why NVIDIA's dedicated allocation breaks raw-view-written BC images.

## TICK-027: Castle Town walking and turning, 60 -> 30 fps (2026-11-09)

**Cause: VRAM over-commit from render targets that the engine kept after the game had already drawn new targets over their storage. It is not occlusion and not pipeline creation.** New scene `castle-move` (tools/bench/run_bench.py) loads the most recent save (Castle Town Market). B at 72-75 s closes the talk that the last title A presses start next to the dancing couple. From 76 s it walks forward and back with right-stick camera turns, five times. Per-120-present windows (base build): draws stay about the same while moving as while standing (~2,150-2,300). Pipelines made: 0 (3,170 cached). Uploads: 2-9 a frame, 0.5-2.6 MB. Once the moves start, the texture count jumps by ~1,100 in one window (about +2.7 GB, from 6 to 8.8 GB, budget 6,144 MB). VRAM then reads **12.6 GB used of 11.0 GB allowed**, so the driver pages. About 25 textures a frame are made and retired (standing: 7-12). Replay submit time goes from 7.5 to 12-17 ms a frame, and GPU time from 2.2 to 3-9 ms. The all-thread profile during the turns (crit.py, 3,844 instants) shows no game thread running in 40% of instants, with main waiting in f_806668b0 / f_81207a50, the GPU/replay waits. In those instants the Presentation thread runs texture_frame (19%), VMA CreateBlock (5%) and targets() (4%). Over all its samples, vmaCreateImage -> BindImageMemory takes 9.5% and destroy_image 7.6%.

A per-kind count of the textures (logged every 600 presents, `vk: textures by frames unused`) showed ~4 GB of render targets unused for 8-59 frames. These were mostly 1280x720 NVN 0x29/0x25 and 2560x1440 0x3f/0x34 sets, 50-90 copies of each. OoT's transient targets move through a pool: each frame it makes a new full-screen set at other addresses that overlap the previous sets, never at the same address. So `drawn_over` (same start address only) missed them, and they stayed for the 60-frame minimum. An env-only check with `RX_VK_EVICT_MIN_UNUSED=10` (for everything) brought moving to 53-55 fps, which confirmed the cause.

**Fix** (vk_texture.cpp, texture_frame): a render target goes once it has been unused for `RX_VK_EVICT_OVERWRITTEN_UNUSED` frames (default 8; 0 restores the old behaviour) if one of these holds:
- another target has been drawn at its address (drawn_over);
- a target drawn after its last use covers its first byte. A target inside it (a layer or level) does not count, and neither does a neighbour that only reaches into its end.

This check runs every 10 presents, under the budget too, and never touches finalized-kept textures or ones the GPU may still read. Textures while moving fall from 6-8.8 GB to 4.4-4.5 GB, and VRAM stays under the limit (8.5-8.7 of 10.9 GB).

castle-move, 3 runs each, same binary, A = `RX_VK_EVICT_OVERWRITTEN_UNUSED=0` (as before). Windows are by time after launch: standing 70-84 s, moving 88-110 s.

| run | standing fps / 1% low / > 33 ms | moving fps / 1% low / > 33 ms | moving: textures MB, made a frame, replay ms |
|---|---|---|---|
| A1 | 51.1 / 6.2 / 4.1% | 40.1 / 5.9 / 10.4% | 6434, 14.2, 12.6 |
| A2 | 50.1 / 7.7 / 4.4% | 43.5 / 7.0 / 9.1% | 5940, 12.6, 10.9 |
| A3 | 40.9 / 6.9 / 14.2% | 31.2 / 6.3 / 24.0% | 7929, 21.1, 14.4 |
| B1 | 58.0 / 18.9 / 1.1% | 50.9 / 10.7 / 5.3% | 4391, 11.3, 9.5 |
| B2 | 64.3 / 32.0 / 0.2% | 56.7 / 18.8 / 1.8% | 4498, 12.4, 9.5 |
| B3 | 60.2 / 31.1 / 0.2% | 51.7 / 11.0 / 4.2% | 4389, 11.5, 9.5 |

kokiri-idle (Castle Town Market save, standing), 75-110 s, fps / 1% low / > 33 ms: A 48.9 / 8.7 / 6.4%, 65.1 / 31.8 / 0.2%,
62.6 / 14.0 / 1.5% (mean 58.9); B 64.4 / 30.5 / 0.3%, 64.0 / 33.4 / 0.2%, 64.1 / 31.5 / 0.3% (mean 64.2).

Draws per frame did not change (~2,100-2,300 in both). Moving is still ~8 fps below standing: ~12-25 targets a frame are still made (VkImage create and bind) as the pool moves. Recycling images of the same create-info is the next step and was not done here. HOS_SPEED_LOG ratio while moving was 56-62, around 60.

**Real occlusion queries (tried, dropped).** A one-frame-late implementation was built and measured:
- a ring of 4096 occlusion queries, one per draw between a samples-passed reset and its report;
- reports written in stream order with the address's last completed count, and 4096 until it had one.

It produced counts (800-1,900 reports a frame, ~80% of them 0) and fewer draws (1,200-2,000), but Castle Town rendered washed-out mint/white, with buildings and meshes missing or flickering (framelog: dark/bright events of up to 111 of 144 blocks). This is the known breakage of enabling occlusion in this game (Hookshot issues 1002/1004, Lichii). The code was reverted. Nothing of NX2navi was kept, so no THIRD_PARTY_NOTICES row. The engine's answer stays as it was (every samples-passed report 4096; `HOOKSHOT_OCCLUSION_OFF=1` patches the game's queries out).

Also seen, and not caused by this ticket: the teal/white over-bright ground and some missing objects in Castle Town while walking. They show up in headless shots at presents ~6000-7500 and in windowed framelog frames, including with the unmodified backup build (backup-tick027). The sun glare drawn through walls when the camera is inside geometry comes from the fixed "visible" answer.

Not verified: Hyrule Field; other GPUs; the windowed picture beyond framelog thumbnails; whether the faster eviction makes rendered textures come back empty in other scenes (the rule only takes targets whose storage a later target covers).

## TICK-031: VRAM, decoded ASTC as BC7 / BC1, spare images (2026-11-09)

RTX 5070 12 GB, which cannot sample ASTC (`textureCompressionASTC_LDR = false`).

**Setup**
- Save: Castle Town Market, near the fountain.
- Scenes: `castle-move` now uses 0.7 s legs with 1.9 s of camera turn in place (the old 1.5 s legs walked Link out through the gate into Hyrule Field). `kokiri-idle` runs on the same save.
- Runs are windowed: 6 of 7 `HOS_HEADLESS=1` runs hung at boot on a guest mutex (`nn.npln.Worker mutex 0x100ac110c8 ... waiting 10 s`). The old texture path hangs the same way, so this is a separate issue.
- A = the same binary with `RX_VK_ASTC_BC3=1 RX_VK_SPARE_FRAMES=0` (the previous behaviour). B = the new defaults.

**Where the VRAM went (A, castle-move, breakdown line at the end of the run)**

| Source | Size |
|---|---|
| Heaps (driver) | 7938-8630 MB of 10.7-11.2 GB (71-80%) |
| Textures, total | 4.5-5.1 GB |
| of which decoded ASTC | 3.0 GB |
| of which rendered, live | 0.8-1.2 GB |
| of which rendered, finalized | 0.1-0.3 GB |
| of which uploaded | 0.54 GB |
| Mirrors | 1.71-1.76 GB |
| Free in VMA blocks | 0.45-1.6 GB |
| Other (staging, pipelines, swapchain, ...) | 0.8 GB |

A new per-kind line, `vk: ASTC kept: ...`, is logged every 600 presents. It splits decoded ASTC by block size, kept format and shape:

| ASTC block | kept as (A) | textures | MB (A) | MB (B) | kept as (B) |
|---|---|---:|---:|---:|---|
| 5x5, 2D + mips | RGBA8 | 551 | 1750 | 522 | BC7 |
| 8x8 cube array (2048, 8 cubes) + mips | BC3 | 1 | 262 | 262 | BC7 (has alpha) |
| 8x6, 8x5, 6x6, 8x8, 2D + mips | RGBA8 | 454 | 597 | ~215, both rows | BC1 (no alpha) or BC7 |
| 8x5, 8x6, 6x6, 2D + mips | BC3 (2 MB and up) | 147 | 167 | (in the row above) | BC1 or BC7 |
| 4x4 2D; 10x8, 10x10, 12x12 + mips | RGBA8 | ~125 | ~130 | ~40 | BC7 or BC1 |
| all ASTC | | | **3024** | **1120** | 472 made as BC1, 838 as BC7 |

The line also gives two bounds: everything as BC7 would take 999 MB, and BC1 for every 6x6-and-larger block (alpha ignored) 723 MB. So a better target format saves about 2 GB of the 3 GB. This ticket does not touch rendered targets or the mirrors (1.76 GB: one VRAM mirror per guest pool).

**Transcode** (vk_texture.cpp, `shaders/astc_bc_encode.comp`; the encoder is new code written for this engine, no third-party code).

Which textures go through it: every 2D ASTC texture of 64 KB and up as RGBA8 (`RX_VK_ASTC_BC_MIN_KB`) that fits the 2048 scratch.

How: the existing path is kept. Eden's GPU ASTC decoder writes into the 2048 x 2048 x 8 RGBA8 scratch, one layer and level at a time, in the upload command buffer. The new compute shader then encodes the scratch, in place of the BC3 encoder.

Formats chosen:
- **BC1** (4 bits a texel), when both hold:
  - the blocks are 6x6 texels or larger (3.6 bits a texel or less);
  - no ASTC block can carry alpha.
  - The alpha check reads the blocks' colour endpoint modes on the CPU when the texture is made (`astc_block_alpha`). It covers void-extent alpha, CEM 4/5/10/12-15, and multi-partition CEMs (parsed using the weight-bit count). It costs 40-60 ms over a whole scene load.
  - Encoder: principal axis, then 3 rounds of index choice and least-squares endpoints. Texels under half alpha use 3-colour mode with transparent black.
- **BC7** (8 bits a texel, the same size as ASTC 4x4) in all other cases.
  - Mode 6: one RGBA line, 7-bit endpoints plus a p-bit, 4-bit indices. For opaque blocks the p-bits stay 1, so alpha stays 255.
  - For blocks whose alpha varies, mode 5 is also tried (RGB and alpha encoded apart). The lower-error result is kept.
  - Same refinement rounds as BC1.
- **sRGB**: the image is UNORM (MUTABLE). Views are BC1/BC7 sRGB where the NVN format is sRGB, as for BC3 before.
- **Mips**: each level is encoded from its own decoded level (the game's mips, as before).
- **BC4/BC5 not used**: in these ASTC textures no NVN format or swizzle tells normal maps or masks apart safely.

Edge cases:
- **Alpha arriving later.** If a texture kept as BC1 gets an upload with alpha blocks, it is retired at the next present once the GPU is done with it, and made again as BC7. The check reads the source of the staging copy. The `made again as BC7` count in the log line was 0 in these runs.
- **Copies between formats.** A copy between two decoded-ASTC textures of different BC formats is decoded by a blit into the scratch and encoded again (vk_xfer.cpp, texture_copy_into_bc).
- **BC3 is kept** for `RX_VK_ASTC_BC3=1` and for the AMD uneven-level path.

Why no worker threads or disk cache:
- The encode is a GPU dispatch in the same upload command buffer as the decode before it (where the BC3 encode was).
- So there is no frame where the texture is missing or a fallback is shown.
- There is nothing worth caching: the decode had to run anyway.
- The cost is GPU time at upload, mostly during scene loads. While moving, uploads are 0-3 a frame.

A bug fixed along the way:
- The first version read the staging copy back for the alpha re-check.
- The staging memory is write-combined (VRAM behind the BAR), so a 1024 x 1024 BC1 upload took 40 ms.
- Result: 60 "slow upload" lines a run, and 1% lows of 13-17 fps while moving.
- After switching to the source: 5 slow uploads a run, none of them ASTC, the same as A.

**Spare images (render-target churn)**
- Images that are let go (destroy_later, once the GPU is done with them) wait for a new texture with the same VkImageCreateInfo.
- They wait up to 6 presents (`RX_VK_SPARE_FRAMES`; 0 turns this off), with at most 256 MB held (`RX_VK_SPARE_MB`).
- A matching new texture takes a spare instead of calling vkCreateImage and binding memory. It then gets the same layout change and clear as a new image.
- All spares are freed when the heaps pass 85% of what the driver allows.
- castle-move: ~9000 kept, ~5300 taken (58%), the rest freed unused. Spares held at the 600-present log lines: 0-30 MB.
- Its fps effect on its own is within the run-to-run spread (run D below).

| run (castle-move) | standing 70-84 s fps / 1% low / >33 ms | moving 88-110 s | nvidia-smi peak MB | mean 80-110 s MB | heaps at end MB |
|---|---|---|---:|---:|---:|
| A3 (shots on) | 63.3 / 27.2 / 1.4% | 59.3 / 26.5 / 1.6% | 10084 | 9335 | 8630 (80%) |
| A4 | 62.2 / 26.8 / 1.5% | 62.7 / 32.6 / 0.8% | 10265 | 9286 | 8278 (77%) |
| A5 | 62.3 / 27.9 / 1.4% | 60.5 / 33.0 / 0.4% | 9872 | 8772 | 7938 (71%) |
| D (BC3, spares on) | 62.5 / 37.9 / 0.5% | 57.9 / 28.3 / 1.1% | 10092 | 9300 | - |
| B6 | 63.5 / 36.7 / 0.4% | 57.8 / 29.6 / 1.0% | 8583 | 7501 | - |
| B7 | 64.3 / 39.0 / 0.1% | 58.9 / 29.4 / 1.0% | 8719 | 7578 | 6204 (56%) |
| B8 (shots on) | 64.9 / 37.6 / 0.0% | 57.5 / 29.1 / 1.1% | 8578 | 7691 | - |

kokiri-idle (same save, standing):

| | fps / 1% low | nvidia-smi peak | decoded ASTC | heaps |
|---|---|---:|---:|---:|
| A | 62.9 / 30.1 | 8964 MB | 2479 MB | 62% |
| B | 62.5-63.9 / 34-35 | 7602-7603 MB | 941 MB | 50% |

HOS_SPEED_LOG ratio was 60.0 in every run.

**Result (castle-move)**
- Decoded ASTC: 3.0 GB -> 1.1 GB (-63%).
- nvidia-smi peak: 10.07 -> 8.63 GB (-14%).
- Steady VRAM: 9.13 -> 7.59 GB (-17%).
- Heaps at the end: 71-80% -> 56-59%.
- The ticket's -30% peak target is NOT met. What is left:
  - the mirrors (1.76 GB);
  - rendered targets (0.8-1.6 GB; with more room, the budget now keeps more of them);
  - about 0.8 GB other.
- Standing fps is the same or higher, and 1% lows are higher (36-39 vs 27-28).
- Moving fps averages 58.1 against 60.8 for A. That is within the spread between runs here (A 59-63, D 58), but equality is not shown.

**Quality**
- Compared: shots at the same present (5400, the scene's first frame) from all-RGBA8 (`RX_VK_ASTC_RGBA8=1`), A and B.
- Crops at 3x, side by side: a house front, a barrel and sack, Link's back (build/shots/cmp, not committed).
- At the 1280 x 720 shot size: no visible difference, and no block artefacts, banding or alpha loss.
- Not measured: PSNR, close-up surfaces.

**The white/mint ground in Castle Town is not a VRAM shortage.**
- It is there from the Market's first frame in all three variants. The mint share of the frame is 0.31-0.36 at presents 5400-6600 in A, B and all-RGBA8.
- Meanwhile the heaps are at 56-80%, nvidia-smi reads 8.2-10.3 of 12 GB, no image was refused, and no texture was evicted before it appeared.
- It is not the ASTC re-encode either: all-RGBA8 shows it too.
- Its cause stays open, and this ticket does not fix it.

**Safe degradation**
- Unchanged and already in place: an image VRAM refuses goes to system memory (create_image) instead of being left unmade.
- The texture budget evicts before the next refusal (TICK-023).
- Spare images are dropped first under pressure.
- No lower-mip fallback was added: no image was refused in any run.

**Not verified**
- The BC1 -> BC7 remake path (never triggered).
- Copies between BC1 and BC7 textures (none seen).
- Hyrule Field.
- Other GPUs (AMD's uneven-level path keeps BC3).
- Long sessions.
- PSNR of the encoder.
- Headless runs.

## TICK-036: white/mint ground in Castle Town Market (2026-11-09)

**Cause: a new pool mirror took over a retired pool's mirror before that mirror had been filled.** This happens under the parallel replay (TICK-006). It has nothing to do with VRAM, textures, occlusion or an unbound storage buffer.

Setup: the user's Market save near the fountain, `kokiri-idle` and `castle-move`, windowed, RTX 5070.

**What was missing**
- A frame dump (`RX_VK_DUMP=6000`) showed the depth prepass with no ground at all: depth stays at the far plane over the bottom third of the frame. The fountain basin, the plaza's trees and the cobblestone streets near Link were missing too.
- What showed there instead was the scene's background fill (the mint), brightened by fog and bloom (the white). The green box was the fountain's water volume, with no basin under it.
- `RX_VK_DRAW_STATS` listed the ground draws: forward programs `8b007d458ae6d820` (it samples the game's baked ground textures, BC6H/BC7 1532x1328x3 and BC5 3064x2656) and `1023e9b9927eced0`. They ran, but every primitive was clipped: 999 primitives in and 0 out, and 6432 in and 0 out.
- `RX_VK_DUMP_ARGS` showed that their vertex and index buffers live at `0x7fd1c470fa`, `0x7fd21d3800` and `0x7fd3...`. Those addresses are in pools that were "taken in" at the scene load (`pool 0x7fd1000000..0x7fd3000000 took in 2 pools: mirror made with the GPU's contents of 2 retired pool(s)`).

**Leads checked and ruled out**
- `HOOKSHOT_OCCLUSION_OFF=1`: still white.
- The empty "camera within 1 m" list of the mesh culling (record 4 of `0x117b40b000`, programs `9d8403e2e566c773` and `c36a4f3f667c886a`): that list is empty by design, because no object's centre is that close to the camera.
- The heightmap the culling reads (`#206`, R16 1775x1175x8): it decodes correctly.
- No `storage buffer NOT bound` lines in the Market. The two programs that log it in Hyrule (TICK-016) are not the ground here.

**A/B**
- `RX_VK_NO_INHERIT=1` (new mirrors filled from guest memory only): cobblestones, the fountain and the trees are back (`build/shots/noinherit/vk_006600.png`).
- `RX_VK_LOG_INHERIT=<address>` (new, diagnostic) reads back 64 bytes at an address when a new mirror takes over a retired one. At the ground's vertex page it showed:
  - retired mirror: all zero;
  - guest memory: the vertices;
  - new mirror after the takeover: all zero.
- The page was stamped only once, at gen ~3900 (long before). The retired mirror had been made in the same submission (epoch) as the takeover.

**Mechanism** (`vk_memory.cpp` `mirror_make`)
- Each segment of a parallel recording has its own upload command buffer (`vk_texture.cpp` `up_cmd`, thread_local), and the queue runs them in segment order.
- A segment-4 worker bound the old 16 MB pool and made its mirror: the fill copy went into segment 4's uploads.
- A segment-2 worker then needed the larger pool. It imported that pool, retired the old one, made the new mirror, and copied the old mirror over it.
- That copy went into segment 2's uploads, which run before segment 4's. So it copied an empty mirror, and the vertices and indices became zeros in VRAM for the rest of the scene.
- Which pools are hit depends on the order the workers reach them. That explains why the bug looked tied to a place (the Market from this save) and came and went (TICK-027's walking ground).

**Fix**
- `mirror_make` records the epoch and segment that filled each mirror (`made_epoch`, `made_seg`).
- A retired mirror that a later segment of the same submission fills is not taken over. It only holds guest memory as of this submission, which the new mirror's own copy already has.
- The takeover now copies only the pages the CPU has not written since the retired mirror's last sync (`mirror_inherit.h`, the same rule as `mirror_sync`, `RX_VK_PROTECT_BARE` pages kept). Before, it copied the whole range, so a retired mirror whose sync of this submission sits in a later segment (or that was never synced after a CPU write) put back what it held before the CPU wrote.
- What the takeover exists for is unchanged: GPU results that only the old mirror holds, such as Kokiri's foliage records.
- `RX_VK_INHERIT_ALL=1` restores the whole-range copy (the later-segment rule stays). Test: `tests/test_mirror_inherit.py`.
- Log line: `mirror made with the GPU's contents of N retired pool(s) (... MB kept, ... MB the CPU wrote since taken from guest memory; N retired mirrors left out, filled by a later segment)`. In the Market runs, ~30 retired mirrors were left out at the load, and 31.7 MB of CPU-newer pages were taken from guest memory.
- Shader stamp: unchanged (no translator file touched). The program and pipeline caches are kept.

**Result**
- Ground, fountain and plaza trees are drawn from the Market's first frame, standing and walking. Shots:
  - `build/shots/fix-idle/vk_006600.png` (standing);
  - `build/shots/fix-move/vk_005700.png`, `vk_007200.png`, `vk_007800.png` (castle-move);
  - `build/shots/fix-hyrule/vk_007200.png` (running down the street to the gate).
- Before, for comparison: `build/shots/base/vk_006000.png`.

FPS (`run_bench.py`, 60 s warm-up, one run each, no shots; before = the game folder's previous build, `backup-tick036`):

| scene | before fps / 1% low / p99 | after fps / 1% low / p99 |
|---|---|---|
| kokiri-idle (Market, standing) | 65.3 / 9.5 / 29.1 ms | 67.3 / 9.9 / 26.9 ms |
| castle-move (Market, walking) | 64.1 / 27.2 / 27.6 ms | 63.6 / 26.7 / 28.6 ms |

The ground, fountain and trees now drawn cost nothing measurable: both rows are within the run-to-run spread (2-4 fps).

**Not verified**
- Hyrule Field and Kokiri: the walk to the gate reached the loading screen at the 115 s limit, so the foliage records that the takeover was made for were not re-checked in Kokiri.
- Other GPUs.
- `RX_VK_PARALLEL` off (the bug needs segments, so it should not occur there).
- Whether other pools taken in at other loads were broken in the same way elsewhere. This is likely, given the mechanism; the floating trees and missing meshes of Hookshot #994 are candidates.

## TICK-037: what holds the frame after Sync 9, and the capture's streaming copy (2026-11-10)

**Result: the frame is held by the game's CPU chain: the main thread's own work plus its waits for the game's job threads,
~80% of a frame. The engine's largest piece on that chain is the submit-time capture on the main thread (~1.1-1.3 ms a frame
of copying, the other game threads mostly idle meanwhile), and that copy was bound by memory bandwidth. Copying it with
streaming stores (`RX_VK_CAPTURE_STREAM`, on by default) gave +2.9 fps standing and +3.3 fps walking in the Market (every
walking run with it above every walking run without it), +2.4 fps on the walk to the gate. The GPU (~8-10 ms a frame), the
replay threads and the present/fence waits do not hold the frame.** Market save by the fountain, frame limit off
(`--fps-limit 360`), frame generation off, Ryzen 7 7800X3D, RTX 5070, windowed `run_bench.py`.

### Attribution (before the change)

Main thread, `HOS_SAMPLE_ALL` 88-104 s after launch (after the scene's late-load hitches; ~3,800 instants), per frame
(sampled frame 14.3 ms standing, 15.0 ms walking; the sampler itself costs ~1 ms). Method: `crit.py`'s per-thread states,
then the main thread's state split by what it waits in, and what every other thread does at those instants (ticket
scratch scripts, not committed).

| main thread | kokiri-idle (standing) | castle-move (walking) | what it is |
|---|---:|---:|---|
| waits for the job group (f_8007fe60 -> WaitLightEvent) | 6.4-6.8 ms | 7.1 ms | game: the job threads' chain (BaseProcExecutor0-5 45-50% busy each meanwhile; no job thread working in 8% of it) |
| runs guest code | 1.9 ms | 1.9 ms | game |
| waits in f_83acfb90 (an event the job threads set) | 1.2 ms | 1.0 ms | game; the Presentation thread is idle in its queue then |
| SleepThread poll in f_800998a0 (20 us steps) | 0.7 ms | 0.8 ms | game: polls the ModuleSystemWorkers, which run then |
| **capture copy in n_nvnQueueSubmitCommands** | **1.1 ms** | **1.3 ms** | **engine**: no other game thread runs in 41% of these instants |
| rest of the submit path (collect_in scans, take_capture, stamps, recording, the helpers' wait) | ~0.6 ms | ~0.8 ms | engine |
| host runtime (obj_lookup from LockMutex in f_80364080, NVN command copies) | ~0.5 ms | ~0.5 ms | runtime |
| GPU waits (vk_timeline_wait, the driver's submit/begin) | < 0.1 ms | ~0.15 ms | sync |

A note on the sampler: `crit.py` counts a thread as running only when its leaf is in the exe or the dll. The capture's copy
runs in ucrtbase's memcpy, so it shows as "blocked in guarded_copy". It is work: the reverse of the earlier mistake (the
driver's vkWaitSemaphores counted as a spin). Also: name a sample file only with the maps of the build that made it (a
rebuild moves the dll's functions).

Other threads: the Presentation Thread (the game's render thread, which runs the replay) is idle in its message queue 39%
of the time and waits for the parallel replay's workers another 29%: ~5 ms a frame of slack, off the main thread's path.
The replay workers are mostly idle. Virtual cores: ~5,500 waits in 5 s over all threads, ~0.1 ms each (~1.5 ms a frame
summed over every thread, spread).

GPU (`RX_VK_PASS_TIMING=1`, kokiri-idle, inflated by its timestamps): 10.0 ms a frame in all: passes 5.4, dispatches 2.0,
barriers 0.8 + 0.2, msync 0.73 (32.5 MB, 8,300 entries), wb 0.30, rbrefresh 0.19. An unprofiled frame is ~14 ms, so the GPU
idles a few ms of each frame. It is the next limit, at ~8-10 ms (100-125 fps), after the CPU chain. Nsight Systems was not
used: it would have to launch the game outside run_bench.py.

What the capture copies: 1,200-1,750 pages (4.6-6.8 MB) a submit, 6 submits a frame, 25-40 MB a frame, written by the game
since the submit before. A diagnostic during the ticket showed 0.0% of a capture's pages in the capture before it (rings of
fresh data, nothing copied twice); ~75% of it is synced to the mirrors.

Why the copy was slow (an in-game probe, and a scratch microbenchmark of the same shape: ~2,400 pages in runs, the source
just written by six other threads, 37 rotating 16 MB destinations):
- In game, 9.7 MB copies took 0.35 ms for the main thread's 1/5 share and 0.36 ms on each helper (started 11 us after being posted): ~27 GB/s in all.
- Eight helpers instead of four: the same time (0.37 ms for 6.8 MB). Bandwidth-bound, not thread-bound.
- The destinations (25 kept captures + 12 spares) are never in the cache, so a plain copy reads each line from DRAM before writing it.
- Microbenchmark: memcpy 0.43 ms, streaming stores 0.31 ms, a destination kept in the cache 0.22 ms.

### Change: the capture's copy with streaming stores (vk_memory.cpp)

`copy_part` copies each run of pages with `stream_memcpy`: AVX2 loads and `_mm256_stream_si256` stores, 128 bytes a step,
memcpy for a destination not 32-byte aligned or a size not a multiple of 128 (never in practice: pages into page slots).
Same SEH guard (`guarded_copy_stream`), same per-page retry with memcpy after a fault. The stores are fenced (`_mm_sfence`)
at the end of each part, before the helper counts it done and so before the capture is handed on. The GPU and the CPU's
later reads (take_capture's texture shadows, the replay's reads as submitted) see the same bytes. Built only with AVX2
(x86-64-v3, the default target); else memcpy. `RX_VK_CAPTURE_STREAM=0`: memcpy, as before. Not a shader-stamp input: no
`--prepare-shaders`. Test: `tests/test_capture_stream_copy.py` (the function against memcpy over sizes and alignments).
`RX_VK_CAPTURE_THREADS` now accepts up to 15 helpers (it was 0-4), for experiments; the default is unchanged.

Capture time a submit (log, `capture: a submit`): kokiri-idle 0.31-0.37 -> 0.27-0.29 ms, castle-move 0.42-0.45 -> 0.32-0.34 ms.

A/B, the same binary, interleaved, env switch only (fps / 1% low / p99 with the 66 s cut, and in brackets fps 80-110 s, the
steady part after the late scene-load hitches):

| scene | run | off (`RX_VK_CAPTURE_STREAM=0`) | on (default) |
|---|---|---|---|
| kokiri-idle | 1 | 66.5 / 35.4 / 23.0 ms (67.6) | 71.8 / 38.2 / 21.9 ms (73.4) |
| | 2 | 66.4 / 32.8 / 24.3 ms (67.3) | 69.8 / 35.1 / 21.9 ms (71.0) |
| | 3 | 69.4 / 36.5 / 21.7 ms (71.0) | 69.4 / 33.6 / 22.8 ms (70.4) |
| | mean | **67.4 / 34.9 / 23.0 ms** (68.6) | **70.3 / 35.6 / 22.2 ms** (71.6) |
| castle-move | 1 | 62.5 / 24.7 / 30.5 ms (61.6) | 66.7 / 26.5 / 26.1 ms (66.7) |
| | 2 | 64.3 / 27.8 / 26.4 ms (64.2) | 66.1 / 23.9 / 30.4 ms (65.0) |
| | 3 | 62.2 / 24.7 / 30.2 ms (60.9) | 66.2 / 25.3 / 28.7 ms (65.4) |
| | mean | **63.0 / 25.7 / 29.0 ms** (62.2) | **66.3 / 25.2 / 28.4 ms** (65.7) |
| hyrule-walk (the street to the gate) | 1 | 62.8 / 14.7 / 41.5 ms (62.5) | 65.1 / 7.4 / 75.4 ms (72.7; a hitch after 66 s) |
| | 2 | 63.9 / 15.5 / 38.7 ms (62.5) | 66.5 / 28.6 / 27.7 ms (66.6) |

The standing Market has two states from run to run (~3,050 draws a frame, or ~2,450 with fewer people about; idle-off-3 and
idle-on-1 had the lighter one). In the same state: 3,050 draws 66.4-66.5 -> 69.4-69.8, 2,450 draws 69.4 -> 71.8. The gain is
larger than the ~0.5 ms of copy the main thread saves: the copy's DRAM traffic also competed with the job threads. 1% lows
are unchanged (they are the scene's hitches, not the copy).

Picture: shots every 1200 presents (`RX_VK_SHOTS`, windowed; the folder must exist), on and off, standing and walking: `build/shots/tick037-idle-on`, `tick037-idle-off`, `tick037-move-on` in the worktree (not committed). Ground, fountain, trees, people, shadows and HUD as in the off run (not pixel-equal: the frames are at other moments).

### Tried, not kept

- **Two-stage capture** (copy into a staging slot that stays in the cache, then two drain threads copy it into the buffer):
  no gain in this form. The capture is taken (take_capture) ~0.01 ms after it is made, on the same thread, so the drain was
  always waited for, and 120 of 600 captures were bigger than the 8 MB slot. Making it pay needs the drain's wait moved to
  the GPU (a host-signalled timeline semaphore before the submission) and the CPU's reads of capture buffers gated. That
  touches the TICK-006/036 ordering, so it was not done.
- **More capture helpers** (8 instead of 4): the same time, bandwidth-bound.

### What is next (ranked)

1. The game's job chain (~6.5-7 ms of the main thread's waits plus its own ~2 ms), spread over many guest functions. The
   largest self time while main waits is f_801d99a0, 4.5% of the job threads' samples (entangled actor/scene code, not a
   replacement candidate), then f_8009bb80 3.6%, f_800a8400 2.5%, f_81501180 2.0% (docs/hotmap.md). Native replacements of
   the FP kernels (f_800a8400, f_81501180, f_80ba6d80, f_820e2520) are worth ~1-2% each at best.
2. The capture copy is still ~0.8-1 ms a frame on the main thread: the two-stage form above with a GPU-side wait, or copying
   only the pages a submission's bindings can read (needs the read sets).
3. obj_lookup from LockMutex in f_80364080 on the main thread (~0.2 ms a frame): a cache kept in the guest's MutexType would
   need proof that the game never reads those fields.
4. Then the GPU (~8-10 ms with timestamps): the game's passes and dispatches; the engine's copies are ~1.2 ms of it.

Not verified: other CPUs (a smaller L3 or another memory system changes the copy's balance; without AVX2 it is memcpy), long
sessions, Kokiri Forest, AMD/Intel GPUs. No Hyrule Field save exists (hyrule-walk is the Market street to the gate). No
Nsight run.

## TICK-033: real occlusion counts, measured (2026-11-10)

**Real counts are right; only same-frame counts give a correct picture, and waiting for them costs more than culling saves.**
Ryzen 7 7800X3D, RTX 5070, windowed `run_bench.py`, `--fps-limit 360`, frame generation off, Market save, 66 s cut.

Instrument (vk_draw.cpp "real occlusion counts", hooks in vk_replay.cpp): `RX_VK_OCC_REAL=<mode>`, default 0 (off: no
queries, the fixed 4096 as before). Each recorded draw runs inside a precise occlusion query whose slot is the draw's serial
number in the stream (counted on the replay thread in serial and dry passes, carried into each parallel segment), in a
64K-slot host-reset ring (4K chunks reset ahead of use once their last lap's submission is done). A report's count is the
sum of its window's slots, read when its submission is done (polled at each present). Modes: 1 log only; 2 / 3 one frame
late by report address (zero-or-fixed / the count); 4 same frame: a render condition on a report not read yet stops the
dry pass (or the serial recording) there, what is recorded is submitted and waited for, and every pending report's count
is written over the fixed value in guest memory; 5 one frame late keyed by the bound program, the window's draws and the
place among the frame's reports. `RX_VK_OCC_LOG=<dir>` writes reports, conditions and each window's draws with pipeline
statistics for `RX_VK_OCC_LOG_FRAMES` frames every `RX_VK_OCC_LOG_EVERY` presents. Not a shader-stamp input.

What OoT does: ~1,840 samples-passed reports a frame (windows of 1.1-1.2 draws); a block of ~900 proxy tests (plain indexed
draws) first, then ~920 render conditions, all "render if count != 0", each reading a report of the same frame and stream
557-2,170 draws earlier. ~880 reports a frame read 0: ~150 empty windows, ~720 plain draws whose primitives reach the
clipper and whose fragments all fail depth (really hidden). Report addresses never repeat in consecutive frames.

| Market | fixed (default) | mode 4, same frame | mode 5, late by place | mode 2, late by address |
|---|---|---|---|---|
| kokiri-idle fps / 1% low / p99 | 70.2 / 34.8 / 20.8 ms | 61.7 / 33.6 / 24.2 ms | 68.6 / 34.9 / 22.5 ms | 70.3 / 33.9 / 21.9 ms |
| castle-move | 65.4 / 25.1 / 29.0 ms | 59.7 / 24.8 / 30.0 ms | - | - |
| picture | right | right (standing, walking) | buildings missing for frames | washed out, meshes missing |

GPU time (the log's "GPU time", serial replay `RX_VK_PARALLEL=0`, where every submission is timed; the parallel replay's
segments are not), kokiri-idle: fixed 8.5-8.6 ms a frame (63.9 fps serial), mode 4 8.1 ms (57.1 fps serial): the hidden
objects are worth ~0.4 ms of GPU (~5%).

Decision: default unchanged. The wait (~2 ms a frame of the CPU chain) is not paid back: the GPU is not the limit and the
hidden objects are one cheap draw each. A no-wait correct form would be `VK_EXT_conditional_rendering` (a predicate word
per report copied from its query slot at the next pass end, the gated draws recorded under it); it saves no CPU (the
draws are still recorded), so it is not worth it now.

Not verified: other scenes than the Market (no Hyrule Field save; Kokiri not reached), other GPUs (precise queries,
NVIDIA's counts), long sessions with mode 4, the sun's behaviour under mode 4 at sunset/night.

## TICK-038: resource use (RAM, VRAM, CPU, GPU) at the Market (2026-11-10)

**Result: the process's working set 14.2 -> 11.9 GB standing and 15.5 -> 13.6 GB walking (-1.9 to -2.3 GB), private bytes -2.0 to
-2.5 GB, start-up shaders 8.5 -> 2.8 s; fps and 1% lows not worse (standing +1.7 fps). Both changes are in what the precompile kept
in the NVIDIA driver's memory: 7808 compute pipelines and their shader modules made at every start, of which the Market binds ~110.
VRAM: the driver's untracked part -120 to -190 MB; the total is within run-to-run noise (render targets vary by up to 1 GB between
runs).** Ryzen 7 7800X3D, 32 GB, RTX 5070 12 GB, windowed `run_bench.py`, `--fps-limit 360`, frame generation off, 66 s cut, Market save.

### Instruments

- `RX_VK_RES_LOG=1` (vk_vma.cpp, vk_memory.cpp, vk_draw.cpp; off by default: a mutex per allocation and per pipeline bind). Every
  600 presents, beside the VRAM breakdown: `vk: process memory:` (private, working set, the C heaps' use (HeapSummary), the
  captures' kept and spare buffers, the texture-storage shadow, the capture table), `vk: pipelines:` (graphics made, pipelines
  ever bound, compute modules let go), `vk: compute pipelines built in play:`, and `vk: allocations by kind` (every VMA allocation
  tagged at its call site: textures, render targets (images with attachment usage), mirrors, write-back shadows, captures,
  streams, staging, readback, scratch, other; by memory class: VRAM, VRAM the CPU maps (BAR), system cached, system uncached).
- `tools/bench/ressample.py <dir>` beside run_bench: private / working set / threads / CPU each second, CPU by thread name,
  nvidia-smi, and a scan of the address space with residency (QueryWorkingSetEx) at 12, 30, 45 and 95 s.

### Where it went (before; kokiri-idle at 95 s, castle-move in brackets)

| category | committed | resident | what |
|---|---:|---:|---|
| guest window (the game's memory) | 11.2 GB (12.3) | 7.75 GB (9.2) | committed as the runtime hands it out; ~3.5 GB never touched |
| HeyListen.exe image (lifted code) | 1.03 GB | 0.27 GB | |
| **5 private regions of ~1 GB** | **5.18 GB** | **5.0 GB** | the NVIDIA driver's arenas (the C heaps hold only ~0.4 GB); 4.8 GB by the end of the precompile (12 s) |
| 165-210 private regions of 32-63 MB | 5.5 GB (6.9) | 0.4 GB (0.6) | come with the scene's VRAM (the driver's commit beside video memory); not resident |
| captures (host-cached Vulkan memory) | 0.47 GB (0.61) | | 25 kept 270-330 MB + 12 spares 195-385 MB |
| C heaps | 0.45 GB | | the engine's own objects |
| process: private / working set | 23.7 GB (25.7) | 14.2 GB (15.5) | |

Experiments on the arenas: without the precompile (`RX_VK_PRECOMPILE=0`, title only) they hold 1.8 GB (one ~1 GB region is likely
the 1.07 GB pipeline cache read at start); with it 5.1 GB. Letting the compute programs' modules go: -0.7 GB; not building the
compute pipelines at start: a further -0.65 GB (and -0.2 GB of VRAM, the "other" part of the breakdown). In play the Market binds
1,108-1,119 pipelines (106-117 of them compute) of the 10,635 made.

VRAM by kind (kokiri-idle; castle-move): textures 1.27 GB (1.6-1.7), render targets 1.0-1.1 GB (0.96-2.2: the dynamic resolution's
sets, kept up to 3600 presents while there is room), mirrors 1.52-1.54 GB (1.7), write-back shadows 224 MB, BC scratch 160 MB,
staging 76-140 MB and streams 96 MB in BAR VRAM, free inside VMA blocks 0.4-1.9 GB (varies), the driver's other ~1.03 GB. None of
these was changed: the BC scratch give-back (`RX_VK_BC_IDLE_S`) brought upstream issue 993's garbled textures; the render-target and
finalized-texture rules each fix a past picture bug (rainbow sky, black pause blur, issues 107/386/993); the mirrors and the
write-back shadows are the design (the shadows are a ~20% fps feature on some GPUs).

CPU (72-102 s, both builds ~5.9 cores busy): replay workers 4 x ~20% (real recording; they block on a condition variable, no spin),
capture helpers 4 x ~7% (the bandwidth-bound copies of TICK-037), the game's job threads 25-42% each, nn.npln.* (the online library)
~19% of a core in all. Spinning: the guest's 20 us SleepThread polls (lr 0x8009a478, ~17,000 a second, `HOS_SLEEP_LOG=1`) are
yielded away (`sleep_ns`: SwitchToThread under 1 ms), ~0.35 of a core; they are the job chain's waits on the critical path
(TICK-037), where a timer would add latency: left as they are. GPU: 55-67% busy (nvidia-smi) in both builds; redundant passes were
not looked for in this ticket.

### Changes

1. **Compute programs stay on disk until dispatched** (`RX_VK_LAZY_COMPUTE`, vk_draw.cpp precompile_run): when the pipeline cache
   began with this device's and driver's data, the precompile no longer loads every kept compute program (7808 texture variants)
   and builds its pipeline. A dispatch reads its program from the program cache (`program_cache_load`, as graphics draws already
   did) and builds the pipeline on a worker, the dispatch waiting up to 50 ms as before. From the warm caches: ~2.5 ms each,
   106-117 at the Market's load; 0 dispatches left out and 0 slow compiles in all runs. A start whose cache data is refused (a new
   driver) precompiles all of them as before. 0: always precompiled; 2: lazy even without the cache's data. A build under 30 ms
   (NVIDIA leaves the creation feedback's cache-hit bit clear) does not count toward the 32 that write the 1 GB cache file again in
   play; the exit save writes it whatever the count.
2. **Compute shader modules let go once their pipeline is made** (`RX_VK_CS_MODULE_DROP`, default on): nothing reads a compute
   program's module after `compute_pipeline` is set (the dispatch builds only while it is null); the driver kept each module's SPIR-V.
3. Tried, off by default: `RX_VK_CAP_SPARE_AGE=<n>` frees capture spares not taken for n captures. With 600 the spares still held
   89-411 MB (they are taken again, not left over from the load) and 1.5-2x as many capture buffers were made on the game's thread.

No shader-stamp input changed (vk_shader.cpp, vk_api.h, vk_state.h, vk_draw.h untouched): no `--prepare-shaders`.

### A/B (one binary; off = `RX_VK_LAZY_COMPUTE=0 RX_VK_CS_MODULE_DROP=0`), means of the runs

| scene (runs off/on) | fps off -> on | 1% low | p99 ms | working set GB | private GB | VRAM "other" MB | start-up shaders s |
|---|---|---|---|---|---|---|---|
| kokiri-idle (3/3) | 69.7 -> 71.4 | 30.7 -> 35.2 | 23.1 -> 21.2 | 14.20 -> 11.90 | 23.68 -> 21.23 | 1033 -> 840 | 8.5 -> 2.8 |
| castle-move (5/5) | 66.6 -> 66.2 | 26.3 -> 26.7 | 28.0 -> 29.2 | 15.51 -> 13.59 | 25.70 -> 23.74 | 1041 -> 918 | 7.7 -> 4.3 |
| hyrule-walk (2/2) | 65.2 -> 66.1 | 17.9 -> 27.2 | 36.5 -> 29.9 | 14.85 -> 12.98 | 25.11 -> 23.19 | 1048 -> 918 | 8.2 -> 5.2 |

Runs: kokiri-idle off 68.8 / 69.7 / 70.6, on 71.1 / 70.8 / 72.2; castle-move off 65.8 / 66.9 / 68.9 / 65.5 / 65.7, on 68.1 / 65.6 /
65.5 / 63.8 / 67.9; hyrule-walk off 65.8 / 64.6, on 65.4 / 66.7. The driver's arenas at 95 s: 5.02 -> 2.83 GB resident standing,
5.02 -> 2.85 GB walking. Peak working set 14.2 -> 11.9 GB standing, 16.0 -> 13.8 GB walking. nvidia-smi peak and the process's
dedicated GPU memory: within noise (7.6-8.7 GB both ways). CPU 5.9 cores both. Shots every 1200 presents, on and off, all three
scenes (`build/tick038/ab/r1-*/shots` in the worktree, not committed): the same picture (ground, fountain, trees, people, shadows,
HUD), not pixel-equal (frames at other moments). Saves identical after every run.

Not verified: other areas (a compute program first dispatched elsewhere builds then, from the warm caches), a driver update's
first start (falls back to the full precompile by design), AMD/Intel, long sessions.

Next: the pipeline cache object (~1 GB of the driver's arenas); the ~1,700 precompiled graphics pipelines the Market never binds
(needs a rebuild path without pop-in); render targets of old dynamic-resolution sets kept 3600 presents while VRAM has room
(needs the write-back path proven first); VMA free space inside blocks (0.4-1.9 GB, the 128 MB image blocks' fragmentation).

## TICK-039: the job-group wait, reconstructed (2026-11-10)

**Result: the main thread's wait for its job group (f_8007fe60 -> WaitLightEvent, lr 0x80080188; 7.0 ms a frame standing,
7.8-8.1 walking) is ~97% job execution on the dependency chain. Waking (signal -> the woken thread running again) is ~3%
(0.18-0.30 ms a frame), virtual-core waits and SleepThread polls on the path ~0.1% each. Of the chain's work, ~80-85% is
guest code itself, ~8-10% the runtime under it (obj_lookup, the soft-FP helpers, mutex_lock) and ~7-10% the system (mostly
the wake syscall inside the signaller's SignalLightEvent). The host's sync primitives, thread placement and spin are not
what the frame waits for; no host switch moved fps beyond the spread, and every one tried was slower. No default changed.**
Market save by the fountain, `--fps-limit 360`, frame generation off, Ryzen 7 7800X3D, RTX 5070, windowed `run_bench.py`.

### Instruments (kept, off by default)

- `HOS_JOBTRACE=<file>` (host/hos_jobtrace.c): for a window (`HOS_JOBTRACE_AT`, default 90 s; `HOS_JOBTRACE_SECS`, 3) every
  guest thread's blocking waits with object and guest caller (events / light events, condition variables, contended mutexes,
  message queues, semaphores, multi-waits, the kernel's ArbitrateLock / WaitProcessWideKey / WaitForAddress), how each ended
  (ready, after the 20 us spin, after sleeping), every signal (and, for the kernel, whom it woke), SleepThread polls and waits
  for a virtual core. Outside the window a record point is one load and a branch. ~1 M records a second (4 M cap: ~4 s).
  NavMeshCAStepDepWorker1 alone signals ~750 k condition variables a second that nobody waits on (lr 0x80044344..0x800457f0).
- `HOS_SAMPLE_NS=1`: each HOS_SAMPLE_ALL sample also carries its own instant (`;@<ns>`), so samples can be matched to the
  trace's 50-250 us segments (the line's ms is 1 ms coarse: matching on it put half the "work" samples in waits).
- `tools/bench/jobtrace.py <trace> --present <present_trace.csv> [--samples <file> --maps <dir>]`: per-thread work / waits /
  spin / polls / vcore waits; the main thread's waits by caller; the critical path of every blocked job-group wait, walked
  back from the main thread's wake-up (the signaller, what it did since its own blocked wait ended, who woke that, ...) as
  work / wake / poll / vcore segments; the job threads' concurrency during the group's waits; wake latency by thread; the
  samples that fall on the path's work, by leaf and guest function. Work is what a thread does between blocking waits; a wait
  that did not block counts as work; the spin before a block counts as waiting; a memcpy or a driver call counts as work.

### The frame (main thread, ms a frame; trace windows of 3-5 s at 86-91 s)

| | kokiri-idle (standing, 71-74 fps in the run) | castle-move (walking, 63-64 fps) |
|---|---:|---:|
| job group wait (lr 0x80080188) | 7.0 | 7.8-8.1 |
| f_83acfb90's event (lr 0x83acfcf4) | 1.1 | 0.7-0.9 |
| SleepThread polls (lr 0x8009a478, 20 us each, ~3 k a second) | 0.9 | 1.0-1.2 |
| other waits | < 0.1 | 0.4 |
| main thread working (its guest code and the engine's submit) | ~4.5 | ~5.9 |

### The job group's critical path

| | kokiri-idle | castle-move |
|---|---:|---:|
| work on the path | 6.78 ms (96.8%) | 7.49-7.82 ms (95.8-96.7%) |
| wake (signal -> running) | 0.21 ms (3.0%) | 0.24-0.30 ms (2.9-3.8%) |
| virtual-core waits on the path | 0.01 ms | 0.02 ms |
| SleepThread polls on the path | 0.01 ms | 0.01 ms |
| path work by thread | BaseProcExecutor0-5 0.78-1.17 ms each, ModuleSystemWorker1 0.37, MSW2-5 0.05-0.17 | the same shape |

Wake latency of every blocked wait (not only on the path): 5.7-6.5 us mean after a sleep (p90 8-9 us, p99 10-24 us) for the
executors, module workers and the main thread; waits ended in the spin: < 1 us. Virtual-core waits: < 2 ms a second per job
thread (NavMeshDepWorker2 ~20 ms a second, off the path).

What the path's work runs (HOS_SAMPLE_ALL with HOS_SAMPLE_NS in the same run; 579 samples standing, 428 walking): guest code
84.8% / 80.1%; runtime 8.1% / 10.3% (obj_lookup + obj 1.9-2.4%, the soft-FP helpers a64_fpx_mul32_fma32, a64_fpx_fma32_fma32,
recomp_fp_estimate ~1-3.5%, mutex_lock 0.7%); system 6.9% / 9.6% (ntdll under event_signal and under f_8004ab40's signals:
the wake syscall of a SignalLightEvent whose waiter sleeps, 3-5%; a copy in n_nvnTextureInitialize). The largest guest
functions on it: f_801d99a0 3.7-4.0%, f_800a8400 2.2-3.3%, f_80ba6d80 1.2-2.9%, f_81c86720, f_81b8c000, f_81501180,
f_8004ab40 (the dispatcher) ~1.5-2% each: the same flat profile as docs/hotmap.md.

The job threads during the group's waits (share of that time): all six executors working 41-44%; one or none 36-39%; 2-5
the rest; 3.45 executors on average. The group is two phases: a parallel one where the six executors are all busy, and a
serial tail where one job (on one executor or a module worker) is the chain. Neither is a wake or scheduling problem.

Attribution asked by the ticket: (a) job code execution ~97% of the wait (of which ~80-85% guest code); (b) wake latency of
the guest sync primitives ~3%; (c) host overhead inside the lifted code ~8-10% of the path's work (~0.6-0.8 ms a frame),
spread over obj_lookup, soft-FP helpers, mutex_lock, plus ~3-5% wake syscalls; (d) host scheduling (virtual cores, polls,
priorities, core count) < 0.5%.

### Tried (env only, castle-move, 110 s runs, 66 s cut; fps / 1% low / p99)

| config | runs | mean fps |
|---|---|---:|
| default (spin 20 us) | 65.4 / 28.4 / 28.9 ms, 67.8 / 30.2 / 27.3, 66.0 / 26.6 / 29.3 | **66.4** |
| `HOS_SPIN_US=0` | 63.8 / 26.1 / 29.6, 64.9 / 27.3 / 27.9 | 64.4 |
| `HOS_SPIN_US=5` | 62.4 / 24.2 / 31.8, 62.7 / 27.5 / 30.5 | 62.6 |
| `HOS_SPIN_US=100 HOS_SPIN_ALWAYS=1` | 61.9 / 25.1 / 31.5 | 61.9 |
| `HOS_PIN_BY_CORE=1` (game threads on logical CPU 2 x core) | 36.4 / 7.8 / 115.8 | 36.4 |
| `HOS_CORES=8` (kokiri-idle, with the trace) | 70.2 / 36.8 / 20.4 (default with the trace: 71.3) | the game still makes 6 executors and 5 module workers |

The 20 us spin is the best of these: shorter spins put the wake syscall and the ~6 us wake on more hand-offs, longer ones
hold the spinning thread's virtual core (and its SMT sibling) away from the work. Fixed affinity is far worse than letting
Windows place threads. More virtual cores change nothing: the executor pool is the game's (f_80b5e9e0 builds it, with
per-core slots for six cores), so more executors would need a game patch of the job system's per-core tables.

Not changed, and why: a faster wake (WaitOnAddress, spin-then-block): the path's wake is 0.2-0.3 ms a frame and the spin
already covers the short hand-offs; thread priorities (TICK-011: within the spread); obj_lookup (~0.15 ms a frame on the
path; a cache in the guest MutexType needs proof the game never reads it); native replacements: no guest function carries
more than ~4% of the path, the self-contained FP kernels 1-3% each (at a 2x native speed-up ~0.1 ms a frame each).

### What is next (ranked)

1. The serial tail of the group (36-39% of its time with one job thread working): which job it is per frame (the trace
   names the thread, the samples the guest function) and whether it can start earlier or be split. This is the game's job graph.
2. More executors: the parallel phase (41-44% of the group's wait, all six executors busy while ~10 logical CPUs idle) would
   shrink with 8-12 executors, ~1 ms a frame at best. Needs the job system's per-core slots (f_80b5e9e0 and its users)
   widened and the virtual cores raised with them: a game patch, high risk.
3. Lifter-level speed of the guest code (80-85% of the path): what TICK-011/022 measured (A64_POLL, memory marks, atomics,
   FMA) did not move it; a new approach (e.g. keeping hot guest registers in host registers across calls) is a large project.
4. The runtime on the path (~0.6-0.8 ms a frame): the soft-FP helpers' remaining fallbacks and obj_lookup's misses, each
   ~0.1-0.25 ms.

Build check (kokiri-idle, interleaved, trace off): the TICK-038 build 74.0 / 71.1 fps (mean 72.6 / 36.9 / 21.2 ms), this build
70.2 / 74.8 (72.5 / 39.6 / 20.7 ms): the same. Picture and game speed: no default path changed (record points only), so no
shots and no HOS_SPEED_LOG run were made for this build.

Not verified: other CPUs (fewer cores make the virtual cores and the spin matter more), Kokiri / Hyrule Field, long sessions.

## TICK-040: the hot set and the PGO profile on the Market job chain (2026-11-10)

**Result: before this ticket, 72-78% of the Market job path already ran in `code_hot_*` (-O2 + upstream's profile), not in the
-O1 chunks. Two variants stay inside the 2-4 fps spread. The first is a hot set extended with our scenes' functions (849 -> 2949
functions, 93-96% of the path hot) with a profile recorded at the Market. The second is that set with upstream's back-edge polls
and their own profile. -O3 is no faster. No default changed; the game folder keeps the Sync 10 build.** Market save by the
fountain, `--fps-limit 360`, frame generation off, Ryzen 7 7800X3D, RTX 5070, windowed `run_bench.py`, 110 s runs, 66 s cut.

### What dev builds use

`scripts/dev-build.ps1` sets `A64_HOT_FUNCS=%LOCALAPPDATA%\HookshotBuilder\hot_funcs.txt` and
`RX_PGO=%LOCALAPPDATA%\HookshotBuilder\hookshot_pieces.prof` when they exist. build.py links with that folder's `hot_order.txt`.
All three are the builder's play profile (`versions\v0.1.0-rc1\HookshotBuilder\tools\work`, from upstream's builder parts
through `scripts/release_parts.py profile`, byte-identical):
- 849 hot functions (83 MB of lifted C in 8 pieces);
- a 6.6 MB profile of 1587 records;
- 3682 functions in the link order.

A test compile of one piece against the current lift: 17 of 260 functions have a mismatched profile (ignored), the rest use it.

### Hot and cold on the path (HOS_JOBTRACE + HOS_SAMPLE_NS, `tools/bench/jobtrace.py`, now with a hot/cold split)

Each sample on the job group's critical path is charged to its innermost guest function, by the object that function was linked
from: `code_hot_*.o` (hot) or a -O1 `code_NNN*.o` (cold), read from the map's last column.

| | kokiri-idle before | after (h2) | castle-move before | after (h2) |
|---|---:|---:|---:|---:|
| path samples under hot code | 77.9% | 95.8% | 71.5% | 93.0% |
| guest-code leaves that are hot | 78% | 96% | 72% | 94% |
| path work, ms a frame (one 4 s trace each) | 7.09 | 6.63 | 8.08 | 8.09 |

All game threads (36 s of samples, 72-108 s): the main thread and the job threads were 80-82% hot before. The cold remainder
was a long tail of more than 1,700 cold functions, none above 0.7% of the samples:
- walking, in one trace: f_81738440 3.4% and f_817327a0 3.2% of the path;
- f_801102e0 4.6%, on a streaming thread off the path;
- 80% of the cold weight takes ~775 functions.

Moving ~20% of the path from -O1 into the hot pieces took 0.46 ms off the standing path and nothing off the walking one. So
compiling this code at -O2 with a profile instead of -O1 is worth a few percent at most. The lifted code's cost is its memory
traffic (guest registers in the context, the runtime calls), and neither level changes it.

### Variants (all from the same sources; `build/tick040/bin/<variant>` in the worktree, not committed)

- **base**: the game-folder build (Sync 10): upstream's hot list and profile, loop-header polls.
- **h2**: 2949 functions, 123 MB of hot C:
  - upstream's 849;
  - 917 cold functions from our three scenes' samples (kokiri-idle, castle-move, hyrule-walk, equal weight): the ones covering
    80% of the cold weight, each at most 250 KB of C;
  - 1183 one-level callees of at most 1.5 KB, so they inline.

  Link order: our sampled functions by weight, then upstream's. Profile:
  1. an `RX_PGO=gen` build of that lift;
  2. one run of each scene with `HOS_PGO` (counters reset at 72 s, written at 106 s);
  3. `llvm-profdata merge`;
  4. merged with upstream's profile at weight 6:1 (ours 1.6e11 counts, theirs 4.5e11).

  A test compile of two pieces: no mismatched or unprofiled function.
- **h2o3**: h2 with `RX_HOT_OPT=O3`.
- **p2**: h2's list with upstream's back-edge polls (from upstream 0d6ed03: `nx2/poll.py`, `rtgen.py`, build.py's
  `-DA64_POLL_CTX` for the DLL) and a profile recorded on that code the same way (683,147 polls in 113,685 functions).
- **h1**, dropped: the same 80% of the cold weight with no size cap, 3418 functions. Its giant new functions joined upstream's
  three 2 MB ones in one piece:

  | that piece | time | peak RAM |
  |---|---:|---:|
  | instrumented | 1281 s | |
  | -O2, compiled alone | 337 s | 1.9 GB |
  | upstream's heaviest piece, for comparison | 133 s | 1.4 GB |

### A/B, interleaved (fps / 1% low / p99 ms; two sessions of 3 rounds, base and h2 in both)

| scene | base (6 runs) | h2 (6) | h2o3 (3, session 1) | p2 (3, session 2) |
|---|---|---|---|---|
| kokiri-idle | 70.8 / 38.3 / 21.3 | 71.8 / 37.2 / 21.7 | 70.9 / 35.4 / 22.2 | 72.4 / 36.1 / 21.7 |
| castle-move | 65.7 / 30.4 / 27.3 | 64.9 / 28.7 / 30.1 | 63.8 / 28.0 / 30.8 | 67.2 / 30.5 / 27.5 |
| base in the same session, kokiri-idle | | | 70.3 / 36.0 / 22.3 | 71.2 / 40.6 / 20.3 |
| base in the same session, castle-move | | | 65.4 / 30.1 / 28.0 | 65.9 / 30.6 / 26.7 |

The runs (fps):

| | kokiri-idle | castle-move |
|---|---|---|
| base | 71.2 71.1 68.6 69.7 73.2 70.8 | 63.1 65.5 67.6 65.2 66.3 66.2 |
| h2 | 75.7 68.6 71.1 71.0 72.8 71.5 | 65.6 65.4 65.2 64.4 64.3 64.4 |
| p2 | 73.0 71.7 72.6 | 66.0 65.6 70.1 |

- h2 is +1.0 fps standing and -0.8 walking. Its walking p99 was above base's in every run (29.1-31.2 against 26.1-28.6 ms).
- p2 is +1.2 / +1.3 fps against base in its session.

None is beyond the spread, so none became the default.

The back-edge polls: Sync 10 measured them at -3 fps standing and -2 walking with upstream's profile. With a profile recorded on
their own code they are at least even (+1.2 / +1.3, inside the spread). So the Sync 10 loss was the profile no longer matching
the functions whose loops changed. With these polls:
- `native_replace` reports `strcmp_805afc70 ... NOT applied`: the region hashes change. The replacement is off by default, and
  its recorded text would need updating.
- The text-speed block still applies (`oot_patches.py` matches both forms).

Adopting them needs the profile shipped with them: a lift without it costs ~3 fps.

### Behaviour

- Game speed: `HOS_SPEED_LOG=1`, castle-move, the last 40 one-second lines. Ratio: base 59.98 (58.61-61.12), h2 60.02
  (58.93-60.86), p2 60.01 (59.36-60.86).
- Picture: shots every 1200 presents (`RX_VK_SHOTS`), walking, in `build/tick040/shots/{base,h2,p2}-move` in the worktree.
  The Market, people, shadows, the HUD and text boxes are drawn alike (the frames are at other moments).

No variant is the default, so no test run was needed for them. The only code committed is in the tools and a script.

### Build cost (8 jobs, this PC; clang's working set summed over all its processes)

| | time | peak RAM |
|---|---|---|
| lift (-Lift) | 169-179 s | |
| hot pieces, upstream's list, -O2 + profile | heaviest piece 133 s alone | 1.4 GB (that piece) |
| hot pieces, h2, -O2 + profile | 98-212 s a piece, the build 217 s | 7.3 GB for all eight compiles |
| hot pieces, h2, -O3 + profile | 101-280 s a piece, the build 285 s | 7.4 GB |
| `RX_PGO=gen` build (developer only) | 731-736 s with the lift's chunks | 21.9 GB (the instrumented giant functions) |

The `RX_PGO=gen` build also plays at ~10 fps.

A changed hot list also moves the chunk boundaries: a chunk-ending function that is now hot ends no chunk. So on an update every
cold chunk compiles again once, as for any lifter change.

### Fixed on the way

`scripts/dev-build.ps1 -Lift` kept the earlier lift's `code_*.c`. After a lift with fewer chunks (a longer hot list ends fewer
of them), the earlier lift's last chunks stayed beside the new ones. The link then failed on duplicate symbols (f_85392450 in
code_047 and code_051). The script now deletes them first, as `make_game.py` does. The objects stay cached by content.

### What is next (ranked)

1. A lifter that keeps guest registers in host registers across calls (the large project TICK-039 ranked third). The compile
   level is not the lever (above): the guest code's cost is its traffic through the context. Upstream's back-edge polls were a
   step in that direction, and they are even here once profiled.
2. The job graph: the serial tail (TICK-039) and more executors.
3. If the back-edge polls are taken later:
   - ship a profile recorded on them with the same lift (the builder's `tools/work`);
   - update `native_replace`'s recorded text;
   - measure again over more runs.

Not verified:
- other CPUs and memory systems (the profile's branch weights are the game's, the code layout is the PC's);
- Kokiri Forest and Hyrule Field (no saves);
- more than 6 runs a variant.
