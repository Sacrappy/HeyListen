# The replay's cost: where it goes, how to measure it, how to cut it (TICK-049, 2026-11-11)

Study only. No game, build or benchmark was run for this document. The numbers are read from logs that are already on the
main PC (the Sync 11 runs, `build/bench/sync11/check*/game_log.txt` and `build/bench/sync11/ab/s11-*`, Ryzen 7 7800X3D,
RTX 5070, Market save, `--fps-limit 360`), from `engine/docs/perf-audit.md` and from the code at main 5457560
(`engine/gpu/vk/vk_replay.cpp`, `vk_draw.cpp`, `vk_shader.cpp`, `vk_memory.cpp`, `vk_internal.h`).

## Summary

- **The overlay's "GPU frame prep" is not CPU work.** It is `PROF_SUBMIT`: the wall time of `run_submit` on the submitting
  threads, mostly the Presentation Thread. That time includes waiting for the parallel replay's workers. Standing in the
  Market it is 7.1 ms a frame. The CPU work behind it is about 12.5 ms a frame, summed over the Presentation Thread and the
  four `mkw_vk replay worker` threads.
- **About 4.3 ms of the 7.1 ms is the Presentation Thread waiting for its segments.** Most of that wait is a second round:
  there are 4.7 segments on average for 4 workers, so the fifth segment runs alone after the first four. This is the
  cheapest lever, and it involves no caching.
- **The work is spread out per draw.** A draw costs about 3.6 us of CPU in total: descriptors 1.26 us, driver commands
  1.38 us, the program pick 0.61 us, the pipeline lookup 0.13 us, and the draw's own code 0.22 us. 88% of draws build
  their descriptors again, and 31% of program picks take the slow path ("with other textures").
- **Ranked designs:**
  1. Record each command buffer's segments in one round (no caching). Saves about 1.9-2.4 ms a frame on the Presentation
     Thread. Low risk.
  2. Resolve only the bindings that changed, and a cheaper program pick (a cache within each frame). Saves about 3 ms of
     summed CPU a frame, about 1 ms of it on the Presentation Thread. Medium risk.
  3. Reuse recorded command buffers across frames (secondary command buffers). Could save about 4-6 ms of summed CPU a
     frame, but only after bindless textures and frame-invariant uniform addressing exist. High risk. It is gated on a
     measurement of how often segments repeat.

## 1. Where the replay's time goes

### The pipeline from NVN to Vulkan

1. The game records NVN2 command buffers through `host/nvn_native.c`. Each call becomes a host-memory record
   `{op, size} + payload` (`gpu/render/rx_nvn.h`, `NOP_*`): state binds, draws, dispatches, copies, events and counters.
2. A queue submit hands the records' handles to `run_submit` (`vk_replay.cpp`). The first thing it does is the memory work
   (`submit_memory_work`: the capture, the mark scan and the writes collected; TICK-043).
3. `replay_handle` takes each command buffer:
   - **Small ones** (fewer than 2 × 160 draws and dispatches) are replayed serially on the Presentation Thread.
   - **Large ones** first get a **dry pass** on the Presentation Thread. It runs `replay()` in `Mode::Dry`, which tracks the
     state, keeps a `ParPoint` (a state snapshot) every 64 draws and dispatches, evaluates the render conditions and
     collects the uniform updates.
   - The command buffer is then cut into `clamp(total / 160, 1, kMaxBatch = 6)` segments. The 4 worker threads record
     them into command buffers of their own (`record_segment`).
   - The Presentation Thread waits for each segment in order and submits it as soon as it is done.
4. Per draw (`vk_draw.cpp` `draw()`), the steps are:
   - the targets (`targets` / `bare_targets`);
   - the colour types;
   - `program_for` (the variant pick, `vk_shader.cpp`);
   - the skip lists;
   - `graphics_pipeline` (a fast pair, then a per-thread map);
   - `build_descriptors`, unless the draw before used the same program and nothing it reads changed (`same_desc`);
   - `draw_hazards` (the pass and barriers);
   - `vkCmdBindPipeline` when the pipeline changes;
   - `bind_descriptors` (push descriptors, pushing only the writes that changed);
   - `push_constants`;
   - `dynamic_state` (diffed against the draw before);
   - the vertex and index binds;
   - the draw call.

### The numbers (Market, Sync 11 build)

Times are per frame, from `vk: a frame's replay time (ms)`, the mean of the last windows of each run.

**Market standing** (kokiri-idle; `sync11/check`, 74.4 fps): 3,040 draws, 133 dispatches, 199 passes, 6 submits, 550 full
barriers recorded (108 left out).

| phase (`PROF_*`) | ms a frame | per draw | what it is |
|---|---:|---:|---|
| submit | 7.12 | | **wall** time of `run_submit` (Presentation Thread + the main thread's own submits), waits for workers included = overlay "GPU frame prep" |
| draw | 10.94 | 3.60 us | inclusive, summed over every recording thread |
| - descriptors | 3.83 | 1.26 us | `build_descriptors`: 2,675 builds a frame (365 draws keep their neighbour's), ~1.43 us a build; texture lookups 0.27 inside |
| - commands | 4.19 | 1.38 us | everything after the descriptors: hazards and the pass, binds, push descriptors, push constants, dynamic state, the draw call (the driver's share was 20-25% of the thread in the first audit) |
| - programs | 1.85 | 0.61 us | `program_for` (ParLock for mesh draws) |
| - pipelines | 0.41 | 0.13 us | `graphics_pipeline` |
| - the draw's own code | ~0.66 | 0.22 us | the remainder |
| dispatch | 0.54 | | 133 dispatches, timed exactly |
| memory sync | 0.38 | | `submit_memory_work` (both threads) |
| queue submit / present / slot wait | 0.20 / 0.10 / 0.04 | | |

**The parallel replay** (`vk: parallel replay, a command buffer`, every 240 command buffers, about 3.0 such command
buffers a frame):

- dry pass 0.28 ms;
- segments 1.44 ms (4.7 of them);
- the shared `ParLock` is taken ~1,070 times, with 0.29 ms of waiting over all threads.

For the Presentation Thread this makes:

| | ms a frame |
|---|---:|
| 3 × (dry pass 0.28 + waiting for segments 1.44) | ~5.2 (of which ~4.3 waiting) |
| serial command buffers, memory work, queue submits | ~1.9 |
| **total (= submit)** | **~7.1** |

**Why the segments take 1.44 ms.** A command buffer has ~830 draws and dispatches, which gives 5 (sometimes 6) segments of
~165. At ~3.6 us a draw, one segment is ~0.65 ms. Five segments on four threads take two rounds: about 2 × 0.7 ms = 1.44 ms.
In the second round one worker records while three idle, as TICK-020 already noted.

**Program picks** (`vk: program picks`, deltas over 200,000 picks):
- 68.6% kept by the fast pair `prog_fast` (of them 1/3 by the last picks' textures);
- **31.3% "with other textures"**: the answers' handles are recomputed into a vector, compared, and `still_valid` checks the
  types;
- 0 new.

**Market walking** (castle-move, `sync11/ab/s11-castle-move-r1`, 64 fps): 2,114 draws, 141 dispatches, 177 passes.

| phase | ms a frame |
|---|---:|
| submit | 8.91 |
| present | 1.38 |
| draw | 10.58 |
| descriptors | 4.86 (texture lookups 1.86) |
| programs | 1.47 |
| commands | 3.38 |
| memory sync | 1.08 |

The parallel replay: dry pass **0.93 ms** (textures and pools created while streaming land in it), segments 1.60 ms, lock
waits 0.81 ms, about 2.6 such command buffers a frame. Walking costs more per draw (5 us) because of texture creation and
first-use lookups. The frame's other costs (present 1.4 ms, memory sync 1.1 ms) grow too.

**GPU.** 2.2 ms a frame of the recordings' own timestamps ("GPU time"). It is not a limit for 120 fps.

### What this means at 120 fps (8.3 ms)

- Standing, the Presentation Thread is at ~7.2 ms (submit + present). That is 87% of the budget, so it becomes the limit as
  soon as the game's chain passes ~110 fps. Walking (~10.3 ms) it is already over the budget.
- The summed CPU (~12.5 ms a frame, ~1.5 cores at 120 fps) competes with the game's 12 job threads. On the i5-10400F
  bench (6 cores) that contention matters more than the wall time.
- Two things to cut:
  - the Presentation Thread's wall (the waits, the dry pass, the serial command buffers);
  - the per-draw CPU (descriptors, commands, the pick).

### What is not known yet, and why

- How `commands` (4.19 ms) splits between the driver calls and our bookkeeping. `RX_VK_PROF_FINE` gives exactly that, but no
  run with it exists.
- How `descriptors` (3.83 ms) splits between banks, c1, user uniform buffers, storage buffers and textures. Also from
  `RX_VK_PROF_FINE`.
- How uneven the segments are (the line gives their mean), and how long the Presentation Thread waits for the slowest one.
  No line gives either; one should be added (design 1, step 1).
- How often a segment's command stream repeats from one frame to the next. There is no instrument for it (needed before
  design 3).

### Caveats on the existing timers

- `prof_ns[]` (`vk_internal.h`) and `fine_ns[]` / `fine_n[]` (`vk_draw.cpp`) are plain `int64_t` arrays that 5 threads
  add to without atomics. Lost updates make the sums a little low: read shares, not exact sums.
- The per-draw phases are sampled, one draw in eight (`PROF_HOT`). `RX_VK_PROF_ALL=1` times every draw, at ~2% of the
  thread.
- `RX_VK_PROF_FINE=1` adds ~15 ns per timed site to every phase. Compare a run with it on only to another with it on.
- **`RX_VK_PASS_TIMING` (and `RX_VK_GPU_PROFILE`, dumps, `RX_VK_PROBE`) turns the parallel replay off.** `replay_handle`
  falls back to serial when `gprof_on()`. Never measure the CPU replay with the pass timing on.
- The program-pick counters (`st_hit`, `st_differ`, ...) are racy statics: their shares are good enough.
- The standing Market has two states from run to run (~3,050 or ~2,450 draws, TICK-037). Check the draw count before
  comparing runs.

## 2. Measuring each part on the main PC in one short run

One run of `kokiri-idle` (110 s, 66 s cut), with an optional second run for the sampler. Everything below already exists
in the code; nothing has to be built.

```bat
rem run 1: the replay's phases, finer phases and per-frame CSV (the base build, as installed)
set RX_VK_PROF_FINE=1
set RX_VK_FRAME_TRACE=1
python tools\bench\run_bench.py --exe <game>\HeyListen.exe --scene kokiri-idle --out build\bench\tick050\fine-idle --duration 110 --fps-limit 360
rem (castle-move with the same, if time allows: walking adds texture creation and first-use costs)
```

Expected log lines in `game_log.txt`. All are `[hos Presentation Thread] vk: ...` unless marked:

| Line | When | What it answers |
|---|---|---|
| `vk: a frame's replay time (ms): submit S, present P, draw D, descriptors, texture lookups, uploads, pipelines, programs, commands, dispatch, memory sync, queue submit, slot wait` | every 120 frames | the phases (S is the overlay's "GPU frame prep") |
| `vk: a frame (fine): ms: banks, c1, user uniform buffers, storage buffers, textures, hazards+pass, bind pipeline, bind descriptors, of it the push call, push constants, dynamic state, vertex+index binds, draw call` | every 120 frames | the split of descriptors (first five) and commands (the rest); "the push call" and "draw call" are the driver |
| `vk: a frame (fine): N draws; a draw: descriptor builds, descriptors, descriptor writes, bank calls, bank kept by its stage's generation, bank fills, ..., full pushes, pushes of changed writes only, pushes of nothing, writes pushed, descriptors pushed, pipeline binds, dynamic state from the draw before, dynamic state worked out, dynamic state commands, vertex stream binds, index buffer binds, push constants, pipeline keys from the fast pair, pipeline keys from the thread's map, ..., storage buffer descriptors built` | every 120 frames (wrapped over 2-3 lines) | what each draw does: how much is already reused |
| `vk: parallel replay, a command buffer: dry pass X ms, segments Y ms (n of them), lock taken L times, waited for W ms (all threads)` | every 240 parallel command buffers | the Presentation Thread's waits; n gives the rounds (n > 4: two rounds) |
| `vk: program picks: K kept (B by the last picks' textures, ...), N new, O with other textures (of T)` (`[hos host]` or any thread) | every 200,000 picks | the slow pick's share (O over T, as deltas) |
| `vk: a frame: R draws kept their neighbour's descriptors` | every 120 frames | descriptor builds = draws - R |
| `vk: <n> presents, <fps> fps; a frame: <draws> draws, <dispatches>, <passes>, <barriers>, <submits> ...` | every 120 frames | the denominators |
| `hookshot_frames.csv` beside the game exe | every frame | `draw_ms, desc_ms, cmds_ms, tex_lookup_ms, memsync_ms` per frame (spikes, the walking frames) |

Optional run 2, for the driver's share and the locks. This is the all-thread sampler (2 ms), the same scene:

```bat
set HOS_PROFILE=110
set HOS_SAMPLE_ALL=E:\OOT\HeyListen\build\bench\tick050\all.txt
rem start the sampling at ~72 s and stop it at ~104 s: it samples while <path>.go exists
start /b cmd /c "timeout /t 72 >nul & type nul > E:\OOT\HeyListen\build\bench\tick050\all.txt.go & timeout /t 32 >nul & del E:\OOT\HeyListen\build\bench\tick050\all.txt.go"
python tools\bench\run_bench.py --exe <game>\HeyListen.exe --scene kokiri-idle --out build\bench\tick050\sample-idle --duration 110 --fps-limit 360
python -I engine\scripts\vkprof.py build\bench\tick050\all.txt 40 "mkw_vk"
python -I engine\scripts\vkprof.py build\bench\tick050\all.txt 40 "Presentation"
```

- Name the samples with the linker maps of the build that made them (`RX_MAPS=<folder>`).
- `vkprof.py` prints, per thread prefix, the share of samples running and the functions by inclusive and self share. Look
  for `nvoglv64.dll` (the driver), `build_descriptors`, `program_for` / `still_valid`, `gpu_buffer_bind`, the ParLock mutex
  (`par_mutex()`), and the Presentation Thread's `run_submit` wait on `seg_cv`.

Do not set `RX_VK_PASS_TIMING`, `RX_VK_DUMP*`, `RX_VK_PROBE` or `RX_VK_PROF_ALL` in these runs. The first three switch the
replay to serial; the last changes the costs.

## 3. Designs, ranked by expected gain against risk

The estimates come from the standing Market numbers above (3 parallel command buffers a frame, ~3.6 us a draw). The
Presentation Thread's wall is the 120 fps ceiling. The summed CPU is the contention with the game's threads.

### Design 1: the segments in one round, and the Presentation Thread recording one of them (no cache)

**What changes.**
- The number of segments is capped at the number of recorders, not at `kMaxBatch` (6): `clamp(total / 160, 1, threads)`.
  A command buffer of ~830 then gives 4 segments of ~208, recorded at the same time.
- The Presentation Thread records one of them itself. Today it does nothing while it waits. With 4 workers and the
  Presentation Thread that makes 5 recorders and 5 segments of ~166.
- It records segment 0 and submits it at once. The workers take 1..n-1.
- After the segments, the existing `begin_recording(nr)` → `draw_begin_recording` resets the thread's per-recording state
  (`last_desc_valid`, `last_pipeline`, `pushed`, the stream `cur`), as it does today for the recording that follows.

**What is cached, and why it stays correct.** Nothing is cached:
- The segment boundaries move, but they were always arbitrary `ParPoint`s. TICK-006 proved the picture independent of the
  boundaries once `memory_rb_refresh` ran once per mirror epoch: one-thread, 640- and 1,000-draw segments all matched.
- The worker body is the same function, run on one more thread.
- The shared state is guarded as now (`ParLock`, `pools_mu`, the per-thread caches).

**Estimate.**
- Segments per command buffer go from 1.44 ms to ~0.80 ms (4 segments) or ~0.65 ms (5 recorders, plus the fixed cost per
  segment). Over 3 such command buffers a frame that saves **~1.9-2.4 ms a frame of the Presentation Thread's wall**. GPU
  frame prep goes from ~7.1 to ~4.9 ms.
- The summed CPU also falls a little: fewer segments mean fewer begin/end calls, fewer full barriers (2 each), fewer
  `vkQueueSubmit2` calls and fewer recording slots.
- Walking (1.60 ms segments, ~2.6 a frame) it saves about 1.5-2 ms.

**Risk: low.**
- Each recording thread does more work, so the slowest segment sets the time. Imbalance and a worker preempted by the
  game's threads at 120 fps would show as a longer wait. That is why step 1 of the plan adds the slowest segment to the log.
- The GPU starts on the first segment a little later (one segment of ~0.65 ms after the dry pass, as now).
- TICK-020's "even rounds" (8 segments on 4 threads) cut the wait from 1.10-1.24 to ~1.00 ms, but no fps came of it at 73
  fps. The wait was not on the frame's path then. It is the 120 fps path, so the acceptance is the thread's time and no fps
  loss, not an fps gain today.

**Later in the same direction (not in the first ticket).**
- Start segment 0 while the dry pass goes on. Its start state is known at once, and the dry pass's 0.28-0.93 ms becomes
  overlap. Workers would have to wait for the dry pass's conditions and uniform updates up to their segment's end
  (`ub_limit`, `cond_at`), so this needs a progress counter.
- Split the larger serial command buffers too (the `total < 320` rule).

### Design 2: resolve only what changed, and a cheaper program pick (a cache within each frame)

**What is cached, keyed on what.**

*Bindings.* Per recording thread and per `Program`, the last write list it built (`dw().writes`, `bufs`, `imgs`, `views`),
with an input signature per binding:

| Binding | Inputs (cache key) |
|---|---|
| driver banks c0/c2 | as now (`BankCache`: the stage's `tex_gen` / `ssbo_gen`) |
| c1 | as now (per recording) |
| user uniform buffers c3+ | `(stage, index, addr, size)` from `s.ubo`, a new per-stage/per-index generation `ubo_gen` bumped by `NOP_BIND_UBO` / `NOP_BIND_BUFS` when the value changes, `ub_generation()` (an overlay may be needed), the recording's stream (`cur`), `pools_gen` and the mirror epoch (what `gpu_buffer_bind` returned) |
| read-only storage buffers | the 16 bytes the descriptor reads from its constant buffer (`stage_cbuf_word` at `cbuf_offset`: re-read every draw, which is cheap, and compared), plus the signature of the uniform buffer they came from, `pools_gen`, the mirror epoch |
| written storage buffers | **never cached**: `gpu_buffer_bind(..., written=true)` has per-submission side effects (written stamps, write-backs, `texture_gpu_wrote`, `memory_note_gpu_write`) that must happen at each draw, as TICK-007 found |
| textures, samplers, images | the existing per-slot cache (TICK-007: handle + `texture_slot_version` + `sampler_slot_version` + `texture_destroy_generation`) |

A draw re-resolves only the bindings whose inputs changed and pushes only those writes (`bind_descriptors` already pushes
the changed writes only). OoT binds a new per-object uniform buffer for nearly every draw (`prog_fast`'s comment). The
common draw would then resolve one uniform buffer and the storage buffers read through it, not ~30 descriptors.

*Program pick.* For the 31% of picks "with other textures" (the same program, other texture handles), a per-thread map from
`(Program*, the answers' handles)` to the variant replaces recomputing `answer_handles` into a vector and calling
`still_valid`. Each entry carries the handles' `texture_slot_version`s. The variant depends only on the textures' types,
which a slot keeps while its version holds.

**How it stays correct when guest memory or state changes.**
- Nothing that the GPU reads from guest memory is cached. The descriptors still point at the pool or mirror as
  `gpu_buffer_bind` returns them.
- The bytes read at build time (constant-buffer words that hold addresses) are re-read and compared at every draw.
- Every generation the current `same_desc` test uses (`bind_gen`, `registrations_generation`, `ub_generation`,
  `texture_destroy_generation`) stays in each binding's key. Issue 1126 (views freed under a reused set) and #792 (a
  fallback's compute pushes) are the known traps. The `p.fallback` rule (`dw().prog == &p`) stays.
- A new recording (`draw_begin_recording`) empties the cache, because stream offsets are per recording.
- An A/B switch (`RX_VK_DESC_DELTA=0`) and a check mode that builds both ways and compares the writes (like the existing
  `RX_VK_CHECK_BANKS`) are part of the work.

**Estimate.**
- Descriptors 3.83 → ~1.8 ms and programs 1.85 → ~0.8 ms summed: **~3 ms a frame of CPU**.
- On the Presentation Thread, ~0.8-1 ms: shorter segments plus the serial command buffers.
- Walking gains more where texture lookups dominate (1.86 ms): only if the per-slot cache misses are the cause, which
  `RX_VK_PROF_FINE` must show first.

**Risk: medium.** A stale descriptor is a wrong texture or buffer for a frame (flashes, glowing plants, device faults). The
check mode and the flash detector are required. The gain also depends on the fine split: if the user uniform buffers and
storage buffers are not the bulk of the 1.43 us a build, the gain is smaller.

### Design 3: recorded segments reused across frames (secondary command buffers)

**What is cached, keyed on what.**
- What is cached: a secondary `VkCommandBuffer` per segment (or per pass).
- The key: a hash of the segment's NVN records with the per-frame fields normalised out (uniform-buffer addresses,
  `UPDATE_UB` data, counter addresses), plus:
  - the targets' identities;
  - the program serials;
  - the texture and sampler slot versions it resolved;
  - `registrations_generation`;
  - the pipeline set.
- On a hit it is replayed with `vkCmdExecuteCommands`.

**What it needs first, and why it is not possible today.**
- The recorded commands embed per-recording data: stream offsets for the driver banks, c1 and the uniform overlays; push
  descriptors with buffer offsets; OoT's per-object uniform buffers, whose addresses change every frame (rings).
- A hit needs these to become frame-invariant:
  - bindless textures (a shader translation change; every program prepared again; ~0.9 ms a frame on its own per TICK-003);
  - uniform data addressed through a per-frame table (buffer device address or dynamic offsets).
- The recording's side effects must still run every frame, in a "dry" form at roughly the dry pass's cost (~0.3 us a draw):
  texture write tracking, hazards and barriers, mirror bind marks, occlusion serials, the DLSS/FSR notes.
- On NVIDIA, secondary command buffers inside dynamic rendering need the `SECONDARY_COMMAND_BUFFERS` contents flag and
  their own inheritance. The pass boundaries would have to match the cached ones.

**Estimate.**
- If 70% of segments hit: commands + descriptors + programs (~9.9 ms summed) → ~3-4 ms, minus ~0.5 us a draw of key and
  validation: **~4-6 ms a frame of CPU**, ~1.5 ms of the Presentation Thread's wall after design 1.
- 0 if the streams do not repeat.

**Risk: high.** It takes weeks, changes the shader stamp, and adds a new class of stale-state bugs.

**Gate: measure first.** A diagnostic hashing each segment's normalised record stream and comparing it with the frame
before (`vk: segments repeated from the frame before: x%`). Below ~60%, drop it.

### Ranking

| Rank | Design | Presentation Thread (ms a frame) | CPU summed (ms a frame) | Risk | Size |
|---|---|---:|---:|---|---|
| 1 | one-round segments, the Presentation Thread recording one | -1.9 to -2.4 | ~0 (slightly less) | low | one ticket |
| 2 | per-binding delta resolution + pick map | -0.8 to -1 | ~-3 | medium | one or two tickets, after the fine profile |
| 3 | cross-frame secondary command buffers | ~-1.5 (after 1) | -4 to -6 if streams repeat | high | many tickets, gated on a repeat-rate measurement and bindless |

Designs 1 and 2 add up: together, the Presentation Thread falls to ~4 ms standing, about half the 120 fps budget. Do the
fine profile (section 2) first, in the same ticket as design 1: its numbers decide the shape of design 2.

## 4. Plan for design 1 (one ticket)

1. **Measure as it is** (section 2, run 1, `kokiri-idle` and `castle-move`). Keep the fine lines for design 2.
2. **Add an instrument.** Each segment job (`Job` in `replay_handle`) gets start and end stamps. The existing
   `vk: parallel replay, a command buffer` line adds:
   - the slowest and fastest segment (ms);
   - the rounds (`ceil(n / recorders)`);
   - the Presentation Thread's wait in `seg_cv` (ms).
   Log only; no behaviour change.
3. **A pure planning function in a header** (`gpu/vk/segment_plan.h`, as `target_overlap.h`). It is
   `segment_count(total, per, recorders, cap)`, used by `replay_handle`, with a unit test of the edge cases (total < 2 ×
   per, a cap below the recorders, `RX_VK_PARALLEL_SEG`, `RX_VK_PARALLEL_BOUNDARY` / `MINWORK` still honoured).
4. **`RX_VK_PARALLEL_ROUNDS=1`** (off by default): cap = workers (4) instead of `kMaxBatch`.
5. **`RX_VK_PARALLEL_SELF=1`** (with 4; off by default): the job body becomes a function `record_job(Job&)`. The
   Presentation Thread runs it for segment 0 before its in-order submit loop; the pool gets 1..n-1. Cap = workers + 1.
   - Check that nothing the replay thread keeps across recordings is left pointing at the segment's recording. The
     per-recording state is reset by `draw_begin_recording` (`last_*`, `cur`, `rec_cmd`) and `texture_begin_recording`.
     `texture_set_segment(++seq)` already follows.
   - Check that `texture_segment_worker(index)` accepts the replay thread.
   - Check that `occ_wserial` and `ub_limit` / `cond_at` are thread-local (they are set per segment in `record_segment`).
6. **A/B/C on the main PC**, with one binary and env switches only:
   - variants: base, ROUNDS, ROUNDS+SELF;
   - scenes: `kokiri-idle` and `castle-move`;
   - `--fps-limit 360`, frame generation off, 110 s runs, 66 s cut, 3 interleaved runs a variant (6 if close).
   - Report:
     - the new segment line (wait, rounds, slowest);
     - `submit` (GPU frame prep);
     - fps / 1% low / p99.
7. **Picture.**
   - `RX_VK_SHOTS` every 1200 presents, standing and walking, for each variant against base (exposure and shadows: the
     TICK-006 failure);
   - `scripts/framelog.py` flash detector over the walks;
   - `HOS_SPEED_LOG=1` ratio ~60;
   - no new `run_all` failure.
8. **The default.** Turn it on only if:
   - GPU frame prep falls by at least 1.5 ms standing;
   - the slowest segment shows no new tail;
   - fps is not lower beyond the 2-4 fps spread;
   - the shots and the flash detector are clean.
   Otherwise leave the switches off and write up why.

Acceptance in one line: GPU frame prep (`submit`) standing ≤ 5.5 ms (from 7.1) with the same picture. Record the fine
profile in `perf-audit.md` for design 2.
