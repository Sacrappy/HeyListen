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
