# D: the Vulkan replay's CPU cost on chain B (suspects)

Read-only study (no build, no game run). Sources: `engine/docs/perf-audit.md` (TICK-050/052/054/056/057), `docs/research/replay-cost.md`
(TICK-049), the code at `engine/gpu/vk/` (vk_replay.cpp, vk_draw.cpp, vk_work.h, vk_shader.cpp, vk_bank.cpp, segment_plan.h).
"measured" cites a file, "estimated" is my reasoning from the code and those numbers. Chain B gains are wall ms/frame on the
Presentation Thread (PT) unless said "summed" (CPU over all recording threads).

## 0. Where the 7.8 ms is, and the per-draw bill

- PT loop (measured, perf-audit TICK-054 "chainb"): dry passes 1.0-1.3, mirror pre-sync 0.78 (0.45 with MSYNC_FAST), setup 0.2,
  its own segment 2.35, wait for the others 0.68, **small command buffers recorded serially ~2.0**. Frames have 1,600 (walking)
  to 3,040 (standing) draws and 130-160 dispatches (perf-audit TICK-050; replay-cost.md).
- One draw costs ~3.6 us summed standing, ~5 us walking (replay-cost.md, TICK-050 fine profile): descriptors 1.26-1.4 us (textures
  0.7 of it), commands 1.38 (push call 0.22, vertex+index binds 0.30, bind pipeline 0.23, hazards/pass 0.13, push constants 0.12,
  dynamic state 0.10, draw call 0.05), program pick 0.61, pipeline lookup 0.13. Summed 11 ms/frame over 5 threads.
- Shape of the wall: ~830-draw command buffers cut in 5-6 segments (PT records one); ~500 draws and ~130 dispatches sit in
  command buffers under 320 draws that the PT records alone: **that is the 2.0 ms**, and it grows with any per-draw saving's value
  (a 10% cut of per-draw cost is ~0.2 ms there and ~0.25 ms on the segments).
- Hot-path hygiene is mostly fine: every getenv is a `static const`; no string ops per draw; `value_rec`/`value_queue` std::maps are
  touched once per recording/submit (~25 per frame, negligible); `ev_values` once per event; `ub_updates` is only ~80 a frame (56
  overlays; perf-audit log line) so the vector-per-update allocation is not a cost. Remaining per-draw maps/locks: `prog_slots()`
  (TLS unordered_map hit once per descriptor build, vk_draw.cpp:2490/2511), `picks[]` map on the 31% slow program path,
  ParLock ~0.7 takings per draw after TICK-052 (default on since TICK-054), `texture_of()` takes `reg_mu` on slot-cache misses.

## 1. Ranked table

| id | location | what costs time and why | chain | est. gain ms/frame | evidence | risk | effort | verify with |
|---|---|---|---|---:|---|---|---|---|
| D1 | vk_replay.cpp:2108-2160 `replay_handle` (`segment_count`, `per`=160) | The ~500 draws in command buffers under 320 are recorded serially on the PT (~2.0 ms) while 4 workers idle. A 160-319 buffer gets 1 segment. Fix: header-only walk to count draws, split 2-3 ways from 96 draws up (PT records one), per-size cap so a segment is not under ~60 draws (first-32-draws cost is 7 us, not 3.5) | B | 0.3-0.6 | 2.0 serial: measured (TICK-054 chainb); split gain estimated: 150-draw buffer 0.54 ms serial vs ~0.35 split | low-med | M | `RX_VK_PARALLEL_SEG=96` env A/B first (the knob exists); chainb.py loop time; add a draws-per-command-buffer histogram line |
| D2 | vk_replay.cpp:2108, 1632 `record_segment` | Small buffers (one segment) run the **dry pass and then the worker-mode replay**: the stream is parsed twice, a 27 KB `NvnState` copied twice. Skip the dry pass when the header-walk count says < 2 segments (serial `replay()` already does reports/conditions/UBO inline, as with `RX_VK_PARALLEL=0`) | B | 0.15-0.25 | dry 0.34 us/draw measured (TICK-050: 0.28 ms/830 draws) x ~500 small-buffer draws; estimated | low-med | S | trace marks 13/14 for small buffers; picture shots (conditions, reports) |
| D3 | vk_replay.cpp:1471 `par_points.push_back({.., s, ..})`, `const NvnState start = state` | A full `NvnState` (~27 KB: 8 stages x 128 tex + 128 samp + 32 img handles, ubo/ssbo, 1 KB push) is copied every 32 draws: ~26 copies = ~0.7 MB a big command buffer, 3 per frame, plus a copy per segment start in the worker. Only ~5 points are ever used. Fix: keep points as (p, ub, cond, work) and snapshot state only at the planned boundary draws (last frame's plan exists: `pk_profile`), or RX_VK_PARALLEL_POINT=64 (halves it), or delta-store | B | 0.1-0.25 | size from vk_state.h (estimated); dry 0.28-0.5 ms/buffer measured, copy share estimated 15-30% | low | S | dry-pass ms in the `parallel replay` line; segment balance (`slowest`) must not worsen |
| D4 | vk_draw.cpp:6270-6420 index/vertex bind, `bind_index` (5750) | `vkCmdBindIndexBuffer` rebinds on nearly every draw (0.97 index binds/draw, TICK-050 fine) because `index_bind_same` needs equal offset. Bind the mirror buffer once at offset 0 and pass `firstIndex = offset/unit` (offset % unit == 0 always for aligned game IBs). Vertex+index binds are 0.91 ms summed | A-free, B | 0.1-0.25 | 0.91 ms summed measured (TICK-050); share of it that is index binds estimated 40-50% | low-med (robustness range: the bound size no longer clamps the index fetch; check `RX_VK_ROBUST`) | S | `FC_IBIND` count in the fine line; fine profile `vertex+index binds`; shots |
| D5 | vk_draw.cpp `program_for`/`still_valid`/`answer_handles` (vk_shader.cpp:1907-2075) | 31% of picks go the slow path (same program, other textures): recompute handles into a vector, compare, then `still_valid` re-resolves every answer's texture type. 14-16% of the workers' time; 1.4-1.85 ms summed. Fix: per-thread tiny cache (handle,slot_version) -> canonical answer type; drop the `picks[]` hash for the repeat program | B | 0.2-0.35 | 1.36-1.85 ms summed measured (TICK-050/052, game_log s11-castle-move) ; gain estimated as half | low-med (a stale type = wrong shader variant) | M | `programs` phase in the replay-time line, `program picks` counters; `RX_VK_CHECK_*` style compare mode |
| D6 | vk_work.h:48-70 (`note_work_cmd`, `note_other_work`) + vk_draw.cpp:3105 `checkpoint()` | Every draw, begin-rendering, barrier and dispatch does 2 `lock xadd`: one on the **single global `work_epoch` line** that all 5 recording threads write and `full_barrier` reads, one on `all_work_by_cmd[64]` (8 lines, false sharing; slots collide by handle hash). Each push constants call also does `++checkpoint_n` (global atomic) and writes `checkpoints[i]` (24 B, adjacent entries shared between threads); `VK_NV_device_diagnostic_checkpoints` is on by default (vk_main.cpp:249) so `vkCmdSetCheckpointNV` is also recorded per draw. TICK-052 split other counters for the same reason, not these | B | 0.1-0.25 | estimated: ~4 contended RMW per draw x ~3,500 draws x 30-60 ns = 0.4-0.8 ms summed | low (epoch skipping must keep the barrier dedupe correct: see note) | S | `RX_VK_PARLOCK_STATS`-style A/B: per-thread/skipped epoch in par mode; `draw CPU / k draws` (TICK-052 metric 3.4-4.2 us) |
| D7 | vk_replay.cpp:2210-2245 setup (before first segment starts) | The PT alone begins every segment's recording before releasing any job: `begin_segment_recording`, `vkResetCommandPool(upload_pool)`, `vkBeginCommandBuffer` x2 (upload, init), `gpu_time_begin` for 5-6 segments, plus `memory_rb_segments_begin`. Move the per-segment begins into `record_job` (parallel) | B | 0.1-0.2 | "setup after it 0.2 ms" measured (TICK-054 chainb); call count estimated | low | S | chainb.py setup step (marks 20 -> 17) |
| D8 | vk_replay.cpp:1497 `maybe_split` (RX_VK_CHUNK=120) | In the serial path every 120 draws the recording is cut: end_pass (a render pass ends, re-begins), 2 full barriers, `vkEndCommandBuffer` x2, `vkQueueSubmit2`, a new slot. ~4 cuts a frame for the ~500 small-buffer draws. It exists so the GPU starts early; the 5070 has 2.2 ms GPU/frame and the PT is the limit | B | 0.15-0.4 | cut count and cost estimated; `queue submit 0.20-0.22 ms/frame` measured (TICK-050) bounds it | low (GPU then starts later; barrier count falls) | S | `RX_VK_CHUNK=0` and `=480` env A/B, submit ms, `qsubmit_ms` in the frame trace |
| D9 | vk_replay.cpp:2255 per-segment cold start | A segment's first 32 draws cost 7 us, later ones 3-3.5 (measured, TICK-052): per-recording state (pipelines, full pushes, banks rebound, `texture_touch` under ParLock ~390 a command buffer, 0.86 touches/draw early). Fix: do the per-submission touches once in the dry pass / pre-segment step (TICK-052's own "next (2)"), then segments take the quick path | B | 0.15-0.3 | TICK-052 table (measured); gain estimated (excess ~0.13 ms per segment x 5 / parallel) | med (touch order vs upload ordering, TICK-006 class) | M | `RX_VK_PARLOCK_STATS=1` "why lookups went to the lock", touches per command buffer; slowest segment ms |
| D10 | `RX_VK_MSYNC_FAST`, `RX_VK_SEG_EARLY` (already built, off) | Pre-sync 0.86 -> 0.45 ms; segments started while the dry pass runs: PT waits 0.71 -> 0.28, frame prep -0.6 to -0.9 ms in every scene, fps unchanged because chain A ended next to chain B. Not a new idea; listed so it is counted when chain A shrinks | B | 0.6-0.9 (measured, no fps yet) | perf-audit TICK-056 | low-med | done | re-screen after any chain A gain |
| D11 | whole stream, new diagnostic (section 3) | **Repeat-rate instrument**: content-hash each dry-pass chunk (cut by a rolling hash on op bytes, not by draw count) and compare with 1, 2, 3 frames back. Gates D12/D13 | B | 0 (diagnostic) | no such instrument exists (replay-cost.md says the same) | none | S | one log line `chunks repeated from frame -1/-2/-3: x%`, standing and walking |
| D12 | dry pass memo (section 3) | Reuse the dry pass's output (the points/state snapshots, work count) for a chunk whose bytes and entering-state hash repeat; only the stream-order effects (reports, conditions, events, UPDATE_UB, written-marks) are replayed | B | 0.3-0.9 if hit-rate > 70% | estimated, hit-rate unmeasured | med | M | D11 first |
| D13 | recorded-segment reuse (section 3) | Secondary command buffers per pass-bounded chunk, keyed by stream hash + generations; avoids ~all per-draw CPU on a hit | B | 1.5-3 if hit-rate high, else 0 | estimated, unmeasured | high | L | D11 first; NVIDIA secondary-cb cost unknown |
| D14 | push descriptors (`bind_descriptors` 3005) | `vkCmdPushDescriptorSetKHR` ~5.95 writes/draw pushed (0.66 ms summed measured, TICK-050), driver encodes each descriptor into the stream each time. VK_EXT_descriptor_buffer would let a texture-set block be written once and reused by offset | B | 0.2-0.4 | 0.66 ms push call + 20% of 4.2 descriptor build measured; gain estimated | med-high (extension, new bank/stream paths) | L | `bind descriptors` fine line |
| D15 | vk_dlss.cpp:850, vk_fsr3_inputs.cpp:212, vk_draw.cpp:5797-5820 | With DLSS/FSR active, each draw into a scene-size B10G11R11 target takes `st().mu` just to find that "the largest of the frame already won" (plus `dlss_skin_check` checks). Hoist: an atomic `frame`/area check before the lock | B | 0.02-0.08 | estimated; zero with DLSS/FSR off | low | S | only matters for the DLSS bench |

Gains overlap (D3/D2/D1 share the dry pass; D4-D6/D9 share the per-draw cost): sum of D1-D9 ~1.3-2.7 ms, realistically ~1.2-1.8 ms.

## 2. Top five

**D1 (small command buffers).** The PT records ~500 draws and ~130 dispatches alone, ~2 ms, because `segment_count` is
`total/160` and a buffer under 320 stays one segment. The knob exists (`RX_VK_PARALLEL_SEG`, min 32), so the first experiment is
free: 96 and 64 on castle-move, three interleaved runs, read frame prep and the PT loop in chainb. Expect some of the 0.5 ms
to be eaten by the first-32-draws penalty and by the fixed ~0.03 ms a segment (TICK-052), so the per-size rule should require
>= ~60 draws a segment. Needs a header-only op walk (8-byte headers, ~10 us per 2,000 ops) so the count is known before the
dry pass; this walk also serves D2.

**D2 (no dry pass for small buffers) and D3 (state snapshots).** Together they cut the dry pass by perhaps a third. For a buffer
that will be one segment, the dry pass is pure duplicate: `record_segment` then replays it from a copy of the start state in
worker mode. With the count known up front, run the old serial `replay()` (the `threads < 2` path, still the reference path
and used by every dump/profile mode). In big buffers, the dry pass's biggest avoidable cost is copying a ~27 KB `NvnState` every 32
draws for points of which ~5 are used; last frame's plan (already kept for SEG_EARLY) says where, or a point could store only
the stream position and the binding deltas since the last snapshot. Check: dry-pass ms in the `parallel replay` line.

**D4 (index buffer rebinding).** `bind_index` skips only an identical (buffer, offset, type) triple, and OoT's meshes sit at
different offsets in one big index pool, so the 0.97 binds/draw in the fine profile are nearly all real. Binding the buffer at
offset 0 and moving the offset into `firstIndex` (draw call argument, free) turns them into no-ops while the mirror buffer
stays the same. The `Vertex+index binds` line is 0.91 ms summed; the same trick for vertex streams is not possible without a
`vertexOffset` multiple of stride, so leave those. Watch the robust-access range (index fetch is clamped to the bound size).

**D6 (shared atomics per draw).** `vk_work.h` makes every draw and begin-rendering do two `lock xadd`, one on a global
`work_epoch` that the five recording threads write all the time (its own comment: "the other recording threads move it all
the time") and that `full_barrier` reads. TICK-052 found exactly this class of cost (shared counters one cache line, ~13% of the
workers in ParLock) and fixed `st_draws`/`prof_ns` but not these. Replace by a per-thread epoch (summed lazily where read) or,
in parallel mode, a thread-local counter with a "moved" flag the dedupe reads; keep dedupe off in parallel as it is now. The
checkpoint code (`++checkpoint_n`, `checkpoints[i]`) can use a per-thread index range. Do **not** turn checkpoints off: vk_main.cpp
says the device was lost every run without them.

**D8 (chunked serial recording).** `maybe_split` cuts the serial path every 120 draws, at a cost of a pass end, two full barriers,
two `vkEnd`, a queue submit and a slot. At 70 fps the GPU is 2.2 ms a frame and is not what the frame waits on, so early
start has little value. `RX_VK_CHUNK=0` is an environment switch: run it (and 480) before touching code. Barrier drains are
also GPU cost on weaker cards (TICK-057), so fewer cuts help the 3050 too.

## 3. Cross-frame reuse: what the NVN stream and the Vulkan recording depend on

**What changes every frame, from the code.**
- Uniform contents: never in the Vulkan commands. `uniform_range` (vk_draw.cpp:2237-2256) returns (mirror buffer, offset, size) via
  `gpu_buffer_bind(u.addr, ...)`; the GPU reads the bytes when it executes. So per-frame uniform *data* does not break a recorded
  command buffer. Only the 80 inline `UPDATE_UB` payloads a frame (56 overlays) are copied into the recording's stream buffer.
- Uniform *addresses*: per-object UBOs are bound for nearly every draw (prog_fast comment, vk_shader.cpp), and some game buffers are
  double-buffered (the water light buffer flickered "every other frame", vk_draw.cpp ~2820). So the NVN bytes of a static scene
  probably repeat with period 2, not 1: compare with frames -1, -2, -3. This is a guess; the instrument (D11) settles it.
- Draw list: culling, animation, LOD and particles change which draws exist; one inserted draw shifts every later fixed-size
  segment. Keys must come from **content-defined chunks** (cut where a rolling hash of op bytes hits a mask) so a shift only
  invalidates the chunks it touches.
- Per-recording stream data: driver banks c0/c2 (`stream_alloc`, deduped only *within* a recording via `BankCache`), c1 copies
  (`c1_at`), UBO overlays, quad-index buffers. Vulkan push descriptors and `vkCmdBindDescriptor` carry stream offsets, so a
  recorded buffer would point at a dead stream. To make them frame-invariant the bank bytes need a persistent
  content-addressed pool (same bytes -> same offset in a long-lived buffer), which is a self-contained change (S-M) and also
  removes bank uploads.
- Generations to keep in the key: `registrations_generation`, `texture_slot_version` of every handle used, `texture_destroy_generation`
  (GONE_RING), mirror epoch / `pools_gen` (`gpu_bind_stamp`), `ub_generation`, `rt_serial`, program serial, pipeline set.
- What cannot be skipped on a hit: `draw_hazards` (passes, barriers; depends on texture-written state), `texture_written` marks,
  `texture_touch`, occlusion serials, render conditions (CPU reads of GPU counters), written-storage-buffer side effects
  (`gpu_buffer_bind(.., written=true)` stamps, write-backs: TICK-007).

**Three levels, cheapest first.**
1. D11 diagnostic (hash chunks, compare with frames -1/-2/-3, and count how many of those chunks contain a dispatch, copy,
   clear, condition, event or UPDATE_UB, which are the ones that cannot be reused). 1-2 hours. Decide on the numbers.
2. D12, dry-pass memo. The dry pass's products are the state snapshots, `dry_work`, `par_conds`, `ub_updates` and the
   written-marks. For a chunk whose bytes hash and entering-state hash (27 KB hashed at ~10 GB/s = 3 us) are equal, the points
   and the exit state are equal and can be shared by pointer (that also deletes the 27 KB copies of D3). Reports, conditions,
   events and `UPDATE_UB` are replayed from a short side list. Gain only on the dry pass (~1 ms) and only if hit-rate is high;
   it does not touch the 3.6 us a draw.
3. D13, reuse of the recorded commands as secondary command buffers per pass-bounded chunk (a chunk with no barriers, copies,
   dispatches or clears inside a pass; the 199 passes a frame vary from a dozen draws to a few hundred). Needs render-pass
   continue + dynamic-rendering inheritance, the persistent bank pool above, and replaying hazards/marks as a thin "dry side
   effect" pass (~0.3 us a draw). Large, high-risk, and NVIDIA's cost for `vkCmdExecuteCommands` is not known to me. Not worth
   starting before D11 shows a repeat rate above ~60% (replay-cost.md's own gate).

A translation-level memo (cache of the built descriptor list keyed by its inputs) is weaker: 88% of draws already build, a
key costs ~0.3 us against a 1.4 us build, and the per-draw *commands* (4.2 ms) stay. `same_desc` already covers the neighbour draw.

## 4. Other findings

- **Fence-wait / invalidate readback** (TICK-059, someone else): nothing taken from `value_rec`/`value_queue`; they are touched per recording or submit only.
- `dry_mark_written` for dispatches calls `program_for` and `texture_view_of` per dispatch under ParLock (vk_draw.cpp:7943-7960):
  ~130 a frame, 0.9 ms dry pass walking vs 0.28 standing (replay-cost.md) suggests texture/target creation lands in the dry
  pass (measured only as the total). Not an easy win: creation must stay in stream order.
- Per-submission `vkQueueSubmit2` per segment is made by the PT under `present_mu` after `queue_flush_locked`; the ones before
  the last overlap the wait, so batching them would add GPU-start latency for no wall gain.
- `prog_slots()[&p]` (an unordered_map hit on every descriptor build, ~2,700 a frame) could be an index in Program: ~0.05 ms summed, not worth a row.
- Everything in `vk_jobs.cpp`, `target_overlap.h`, `keep_rendered.h`, `skip_code.h`, `split_count.h`: no per-draw cost found.
- Per-draw checks that look heavy but are not: `dlss_trace_draw`, `exposure_log_on`, `record_progs()`, `RX_VK_*` option parsing
  (all static), `stats_tick()` (static pointer test).

## 5. Suggested order

1. Environment-only A/B today: `RX_VK_PARALLEL_SEG=96/64` (D1), `RX_VK_CHUNK=0/480` (D8). No build.
2. D11 instrument (answers the user's caching question with data); D2 + D3 + D7 (small, same file, measurable in the dry-pass and setup ms).
3. D6 and D4 (small code changes in vk_work.h / bind_index), then D5.
4. D12/D13 only if D11 shows repeats. Re-screen D10 (SEG_EARLY + MSYNC_FAST) when chain A gains: it is already worth 0.6-0.9 ms of frame prep.
