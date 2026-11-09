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
