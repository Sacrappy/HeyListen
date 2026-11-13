# Direct emission: how native draws reach Vulkan (specification, read from this engine's code)

What a draw goes through today from the game's NVN call to Vulkan, what the back end needs of one draw and where each
piece comes from, and how draws produced by `hl::gfx` are to reach the back end without the NVN procedure layer, its
recording copies and, for the data they own, the submit-time capture. Read from `engine/host/nvn_native.c`,
`engine/host/hl_gfx.c`, `engine/host/rx_backend.c`, `engine/gpu/render/rx_nvn.h`, `engine/gpu/vk/vk_api.h`,
`vk_replay.cpp`, `vk_draw.cpp`, `vk_memory.cpp`, `vk_state.h`, `vk_draw.h`, and `engine/native/include/hl/gfx/*`, at main's
latest commit; measurements are quoted from `docs/tickets/SPRINT-5.md`, TICK-048 / 049 / 050 / 052, `memory-boundary.md`
and `gfx-record-job.md` (no new measurement). Nothing was built or run. Functions are this engine's and are named
freely; guest addresses are the game's, as in the other documents; no translated code is quoted.

## The answers in short

1. **There is no NVN command memory in this engine's path, and no capture of it.** The OS layer's NVN procedures never
   write the game's command memory: each one appends a typed record (`nvn_op_hdr` + payload, `NOP_*` of `rx_nvn.h`) to a
   growable **host** buffer kept per command buffer (`cmd_rec`); the command memory the game hands over is only counted
   (`AddCommandMemory` stores its size; `GetCommandMemoryUsed` reports what a real driver would have used). The brief's
   premise ("bytes in NVN command memory in the guest, captured at submit, decoded a second time") holds only in a
   transposed form: the **records** are decoded once, by the replay; what the capture copies is the **data** the records
   point at by address (uniform blocks, vertex and index streams, storage buffers, indirect arguments, texture storage),
   ~46 MB a frame in the castle town, none of it command memory.
2. **The record stream is already a typed draw list.** A record is a fixed C struct per operation; the replay's
   `replay()` is a `switch` over them into `NvnState`. The costs around a draw are elsewhere: the NVN calling convention
   and the guest state objects on the producer's side, the recording's two host copies (EndRecording's `flatten`,
   CallCommands' `inline_recording`), the capture and the mirror syncs for data read by address, and per draw in the
   replay the program pick, pipeline lookup and descriptor build (~3.6 us a draw, TICK-049; the decode itself was never
   timed apart).
3. **A native pass runs inside a command buffer the translated code is recording**: the listener `0x806602e0` is called
   in the middle of `0x80a47660`'s recording into the job's command buffer, and its output must land between what the
   translated code recorded before and after it. So the direct path cannot be a second stream handed to the back end
   beside the NVN command buffers: it is **records written straight into the same command buffer's host segment**, in
   order, through a C entry of the NVN layer. Ordering, command handles, CallCommands and submission then stay exactly as
   they are, and the back end needs no new entry point for the first bricks.
4. **The data question is the real one.** What a native producer computes itself (a draw's uniform block) can travel in
   the record and be written by the replay into its own upload stream: no guest page, no capture, no mirror sync for it.
   What translated code still writes (most per-frame uniform data, streamed vertices, culling lists) can be read in place
   only if it is not rewritten before the replay reads it, and this engine's code holds the opposite as observed fact
   (data rewritten right after a submit; arrays refilled for the next frame as soon as the submit returns; culling list
   sets alternating). Static data (`0x024` pools) costs no capture already.
5. **Bricks**: (A) a record-level check sink beside the NVN path; (B) the native render state written as records, no
   NVN call; (C) uniform data a native producer computes handed inline; (D) a native draw loop emitting draws as
   records; (E) native-only recordings handed over without the flatten copy; then, only if measured worth it, (F) a
   resolved draw packet that skips the replay's state tracking. Each is usable alone and checked by function and
   picture.

## 1. The path of a draw today

### 1.1 The producer side: an NVN procedure answered by the OS layer

- **The call.** Game code (or, since brick 2.2, native code) calls an NVN procedure through the game's proc globals
  (`0x84d66f00`-`0x84d685f0`, resolved by the game in `0x812be290`); each global holds a host call into
  `nvn_native.c`'s handler `n_<procedure>` (the `NV()` macro), which reads its arguments from the guest registers of the
  calling context (`x0`-`x7`, floats in `v0`-`v7`) and guest pointers through `gw()` (checked against the guest window).
  Native code reaches the same handlers through `hl_gfx.c`'s `nvn_call`: it copies the arguments into the thread's
  guest context and calls the resolved host function (`hos_hostcall_target`), so a native caller pays the same calling
  convention and the same guest state objects as the game.
- **State objects** (blend, color, channel mask, multisample, polygon, depth-stencil, vertex attribute and stream
  states) are guest memory the game owns, packed in place by the `*Set*` procedures (`nvn_blend_st`, `nvn_ds_st`... in
  `rx_nvn.h`); binding one copies its bytes into a record (`bind_state`). The render-state function `0x80077d60` keeps
  five of them on its own stack frame and records about 33 calls a draw (`gfx-record-job.md` 8).
- **The command buffer object** (`nvn_cmd`, guest memory) holds only a slot number into a host table (`cmd_rec`:
  `owner`, `seg`, `cur`, `end`); records go to `seg`, a block from the layer's size-class pool (`pool_get`), grown by
  doubling and copying (`cmd_reserve_rec`, 64 KB first). `BeginRecording` resets `cur`; every recording procedure calls
  `rec()` / `cmd_alloc()`: an 8-byte header `{op, size}` and the payload, 8-aligned. **Nothing is written into the
  game's command memory**: `AddCommandMemory` stores its size, `GetCommandMemoryUsed` reports `hw_used`, an estimate of
  the real encoding (`hw_fix`) that the game sizes its heaps from. NVN's inline fast path (write pointer at +0x20, end at
  +0x28) is kept at `~0 / 0` so that it never fits and every call takes the API path.

What a record holds, by kind (`rx_nvn.h`):

| Kind | Records | What is copied at record time | What is kept by address (read later) |
|---|---|---|---|
| Fixed state | `SET_RT`, `VIEWPORTS`, `SCISSOR`, `DEPTH_RANGE`, `BIND_STATE`, `BIND_VATTR`, `BIND_VSTREAM`, `STENCIL`, `POLY_OFFSET`, `BLEND_COLOR`, `PRIMITIVE_RESTART`, `DEPTH_CLAMP`, `RENDER_ENABLE`, `RASTER_DISCARD` | all of it; render targets as pointers to **immutable host copies** of the texture objects (`tex_copy`: one per distinct content, kept for the session) and their views by value | nothing |
| Program | `BIND_PROGRAM` | nothing | **the guest `nvn_prog` object**, dereferenced by the replay (`s.prog = *b.prog`): its stage code addresses and control sections |
| Buffers | `BIND_UBO`, `BIND_SSBO`, `BIND_VBUF`, `BIND_BUFS` (deferred) | the GPU address and size of each range | the bytes at those addresses |
| Textures, samplers, images | `BIND_TEX`, `BIND_SAMP`, `BIND_IMG` | the handles (texture and sampler ids into the pools the game registered: `rx_nvn_register_texture` / `_sampler`) | deferred binds: the game's handle array, re-read at submit |
| Draws | `DRAW`, `DRAW_IDX`, `DRAW_INDIRECT`, `DRAW_MESH`, `DISPATCH`, `DISPATCH_INDIRECT` | counts, firsts, instances, primitive, index type | index buffer, indirect arguments and counts |
| Data in the stream | `UPDATE_UB` (up to 64 KB), `PUSH_CONST` | **the bytes** (the caller may change them afterwards) | nothing: written in GPU order by the replay |
| Side effects | `SIGNAL_EVENT`, `WAIT_EVENT`, `REPORT_COUNTER`, `RESET_COUNTER`, `RENDER_COND`, copies, clears, `BARRIER` | the addresses and values | the counters and events in guest memory |

- **EndRecording** appends `NOP_END`, copies the whole recording (`flatten`, into a pool block with a `copy_prefix`) and
  returns that copy's **host address as the command handle**. The copy is registered by the position a console driver's
  descriptor would take in the control memory (`rec_buckets`, 64 KB buckets, under the exclusive `copy_lock`), a
  16-byte mark is written there (`COPY_MARK` and the copy's address), and the handle joins the live set
  (`live_handle_add`). Copies die when that control memory is added again (`release_copies`), or when a sweep finds the
  mark overwritten (`sweep_overwritten_copies`, every 25,000 recordings); a dead copy is freed once the replay has passed
  every submit made before (`free_dead_copies`, against `rx_nvn_replayed`).
- **CallCommands / CopyCommands** check each handle against the live set (`check_handle`, shared `copy_lock`) and copy
  its records, without `BEGIN`/`END`, into the caller's segment (`inline_recording`): a called recording is copied a
  second time, and a submitted command buffer is one flat stream with no call records left in it.
- Cost on the producer side (castle, sampled, `gfx-record-job.md`): under the recording job, EndRecording 0.22 ms,
  `cmd_reserve_rec` 0.13, `inline_recording` 0.09, `check_handle` 0.05, `free_dead_copies` 0.04; EndRecording is 0.66 ms
  a frame over the game.

### 1.2 The submit

`nvnQueueSubmitCommands` (`nvn_native.c`):

1. copies the handle array (64 at most), checks every handle, and **resolves the deferred binds**: for each
   `BIND_TEX/IMG/SAMP` and `BIND_BUFS` record with a source array, the array's current contents are copied into the
   record (`resolve_deferred`), because the game refills those arrays for the next frame as soon as the submit returns;
2. with `HOS_NVN_HOLD=2` (the default, TICK-054), a submit from a thread other than the presenting one (the main thread's
   two a frame) is **held**: its handles are queued, and the "hos held submits" host thread (woken at each hold), the
   Presentation Thread before its own next submit or present, or whoever waits first (a fence wait, a finish, an event
   poll) flushes the held submits in order (`held_flush_why`, under `flush_cs`). Otherwise the submitting thread replays
   at once. Either way the call into the back end is `rx_nvn_submit_q(handles, n, queue)` (`rx_backend.c` forwards it to
   the DLL's `nvn_submit_q`), with the virtual core given away meanwhile (`hos_vcore_away`).

### 1.3 The capture: what it copies, why it must be a copy

`submit_q` (`vk_replay.cpp`, synchronous mode, the default since `RX_VK_ASYNC` is off) takes `sync_order_mu`, pins the
thread to the core kept for the replay (`ReplayPin`) and calls **`memory_capture_submit`** (`vk_memory.cpp`) before the
replay. `capture_make`:

- **Which ranges**: the union of three lists (`cap_ranges_mu`): the pools that have a VRAM mirror (`cap_ranges`,
  published when mirrors are made), the storage of textures the replay uploads from (`tex_cap_ranges`), and every pool of
  at most 64 MB below 1 TB (`pool_cap_ranges`, `RX_VK_CAPTURE_POOL_MB`, refreshed when the pool list changes).
- **Which pages**: those of these ranges written since the last capture, from the OS layer's soft write watch (the lifted
  stores mark the pages they write; `host->collect_written_in` reads and clears the marks of those ranges). Pages
  outside these ranges are collected by the replay itself after the capture (`memory_collect_written`), for textures and
  the other pools.
- **The copy**: each page whole (4 KB) into a host-cached `VkBuffer` (`Capture::buffer`, its mapping), by the
  submitting thread and the copy helpers; with `RX_VK_CAPTURE_PRECOPY` / `RX_VK_PRECOPY_KICK=2` (on) most pages are copied
  ahead in one parallel round when the main thread leaves its end-of-frame wait, and the take merges them
  (`capture_merge.h`). Pages a submission still on the GPU writes back ("late" pages: the light meter's) are copied
  again once that submission is done (`take_capture`: waited, deferred with `RX_VK_LATE_DEFER`, or merged).
- **What is kept**: at the take (`memory_take_capture`, in the replay's memory work) each page is stamped with the
  capture's generation and entered in a flat table by page (`cap_table`: generation and slot); the last `kKeep` (24)
  captures' buffers are kept (and at most 768 MB) because a pool not bound by this submission is synced at a later one,
  which must copy the page as it was at its own submit; pages of a texture's storage get a **shadow** (a page each, up to
  256 MB) for the uploads. RAM: the capture buffers are 402-525 MB after `RX_VK_CAP_FIT` (619-716 before;
  `memory-boundary.md` 11.2).
- **How much and how long**: ~11,900 pages a frame (46 MB) in the castle town, ~9,000 of them in one 256 MB region
  (`0x104e000000`-`0x105f000000`); 51% are byte-identical to their last copy and ~1 MB of 8-byte words differs a frame
  (SPRINT-5 session 2). 1.3-1.9 ms a frame on the render chain at the memory bus's ~60 GB/s (1.29 ms in session 1's
  trace); the granule capture (session 3: 37% of the bytes written, 6.4 -> 2.5 MB a submit) cost more in the extra store
  marks than it saved and was not adopted.
- **Why a copy and not a read in place** (each reason is a comment of this code, with the failure it fixed): the game
  rewrites a frame's data in place right after its submit, since on the console the GPU has read it by then (MKW's
  terrain constants, "submit-time capture"); the replay runs after the submit (held, chunked, parallel), so a pool synced
  from guest memory at replay time read a later frame's bytes: OoT's GPU culling alternates two list sets, and without
  the capture "every other frame its setup read the next frame's records" and foliage, grass, characters and particles
  were missing (`RX_VK_SYNC_CAPTURE`, "EvilOOT"); a texture's storage rewritten after its submit gave another frame's
  heights (texture storage in the capture); a CPU-side wait for a GPU write-back is why late pages exist (the exposure
  meter, "EvilBoyce"). The asynchronous replay (`RX_VK_ASYNC=1`) was dropped for the same reason: double-buffered data
  rewritten after a submit with no fence, white or black frames.

### 1.4 The replay: decode, state, descriptors, pipelines, mirror syncs, draws

`run_submit` (`vk_replay.cpp`, under `replay_mu`), per submission:

1. **Memory work** (`submit_memory_work`): `memory_sync` (the pool maps), `memory_take_capture` (above),
   `memory_collect_written` (the other written pages: textures re-uploaded, pools marked dirty).
2. **A recording** (`begin_recording`: a slot's command buffer, its descriptor pool and upload stream reset),
   `memory_begin_submission`, a full barrier, the queue's own state (`use_queue_state`: the game's queues replay on one
   Vulkan queue, ordered by their events: a `WAIT_EVENT` not met stops the submission, `QueueRun`, until the other
   queue's work signals it).
3. **The decode**, per handle (`replay_handle` -> `replay(cmd, NvnState&, p)`): a `switch` over the records. State
   records update `NvnState` (`vk_state.h`: program, 8 stages x 16 uniform and storage ranges, 128 texture and sampler
   handles a stage, blend, depth-stencil, vertex formats, streams and buffers, targets, viewports...), bumping
   `bind_gen` / `tex_gen` / `ssbo_gen` only when a value changes; `UPDATE_UB` appends its bytes to `ub_updates`; reports
   and small clears are written to guest memory in stream order on the CPU; render conditions are read on the CPU.
   Large command buffers are **replayed in parallel**: a serial **dry pass** walks the stream once, keeping the state every
   64 draws (`par_points`), the render conditions' results (`par_conds`) and the uniform updates, then **segments** start
   from those states on the replay workers (`record_segment`, `Mode::Worker`), each into a secondary recording; TICK-050
   measured a big command buffer's dry pass 0.33 ms, its segments 1.51 ms (4.65 a command buffer), the slowest 1.25 ms,
   the Presentation Thread waiting 1.42 ms.
4. **A draw** (`draw()` in `vk_draw.cpp`): the targets resolved (`targets`, once per `rt_serial`); the program picked
   (`program_for`: the stages' code translated from guest memory by Eden's recompiler, cached by key; 0.61 us a draw,
   31% of picks on the "other textures" path); the pipeline (`graphics_pipeline(p, s, targets, topology, attribute
   types)`: keyed by the program and the state it reads; 0.13 us); the **descriptors** unless nothing they read changed
   (`same_desc`: program, `bind_gen`, the uniform updates' generation, the registrations', the textures destroyed),
   `build_descriptors` (1.26 us a draw, 2,675 builds a frame; textures 2.08 ms a frame of 4.23, TICK-050): uniform buffers
   from `ub_overlay` (the stream's `UPDATE_UB`s over the range), else the pool's mirror (`gpu_buffer_bind`), else a copy
   into the recording's stream from `memory_read_as_submitted` (the captures' pages over guest memory); textures through
   their registration to a `VkImage`, uploaded at first use from guest storage or its shadow; the driver's constant banks
   (`driver_bank_fill`) into the stream; hazards and barriers (`draw_hazards`, `begin_pass`); pipeline and descriptor binds,
   push constants; **vertex buffers and the index buffer through `gpu_buffer_bind`** (the pool's VRAM mirror, kept per
   binding stamp); `vkCmdDraw*`. Commands are 1.38 us a draw (bind descriptors 1.02 ms a frame, vertex and index binds
   0.91, bind pipeline 0.70).
5. **Mirror syncs**: `gpu_buffer_bind` of a pool with a mirror not synced in this submission (`synced_epoch`) runs
   `mirror_sync`: the pool's pages dirty since its last sync are copied into the mirror on the GPU, from the kept capture
   that holds each page's current generation (`cap_table`), else from guest memory. ~30 parts a frame, ~0.86 MB a part
   (TICK-048); 22 MB / 0.5 ms of GPU a frame on the main PC; 85% of the synced bytes unchanged at 256-byte grain (the chunk
   diff pays on a slow link only).
6. **The end** (`finish_recording`): `end_pass`, the uniform updates flushed, `memory_end_submission` (GPU writes copied
   back to guest memory through the write-back shadows, for the pools the CPU reads), a full barrier, the queue submit
   with its timeline value. Long submissions go to the GPU in parts of 120 draws (`maybe_split`, `RX_VK_CHUNK`).

### 1.5 Threads, and where the time goes

| Step | Thread | Measured (castle town, main PC) |
|---|---|---|
| NVN calls, records, flatten, inline copies | the recording thread: main thread, module workers (the recording job migrates between them) | host recording ~0.55 ms of the job's CPU (`gfx-record-job.md` 6) |
| Submit, deferred binds | the submitting thread (main thread, Presentation Thread) | - |
| Held flush | "hos held submits", the Presentation Thread, or a waiter | ~1.5 ms a frame moved off the main thread (TICK-054) |
| Capture | the replaying thread + copy helpers; pre-copy at the main thread's end-of-frame wait | 1.3-1.9 ms, 46 MB (SPRINT-5) |
| Decode, state, draws (serial part, dry pass) | the replaying thread, pinned to the reserved core | the replay 6.5-7 ms a frame with the switches, 7-9 without; ~3.6 us a draw over ~2,200-3,000 draws |
| Segments | replay workers (`par_worker_main`) | 1.51 ms a big command buffer, 3 a frame (TICK-050) |
| Mirror syncs, write-backs | GPU copies recorded by the replay | 0.5-0.6 ms of GPU a frame (main PC) |

The render chain of SPRINT-5 ("the GPU's tail -> the main thread's meter readback -> its hold -> the capture -> the
replay -> the last submit") is bound by the GPU's tail, the 46 MB the capture copies and the replay's ~4 us a draw.

## 2. What one draw needs to reach Vulkan, and where each piece comes from

What `draw()` reads, grouped by what it becomes in Vulkan. "Record" means copied into the record when the procedure was
called; "guest as submitted" means read from guest memory at replay time through the captures (`memory_read_as_submitted`,
the mirror syncs from `cap_table`), which is what the capture exists for; "held" means state the back end keeps across
draws and submissions.

| Piece | Vulkan form | Comes from | Read when |
|---|---|---|---|
| Program (stage code and control sections) | `Program` (translated SPIR-V, cached by key and variant), its layout | the guest `nvn_prog` object, dereferenced at replay (`BIND_PROGRAM` keeps the pointer); the code bytes at the stages' GPU addresses (`0x064` pools, static) | at replay; the code at first use of a key (held after) |
| Pipeline | `VkPipeline` from `graphics_pipeline` | `pipeline_key`: program key and variant, topology class, samples, vertex attribute formats and divisors (those the vertex stage loads), target formats, and where the device lacks extended dynamic state 3: blend per target, write masks, polygon mode, depth clamp, alpha-to-coverage; the rest is dynamic state | record (state), held (pipelines) |
| Dynamic state | viewport, scissor, depth range and bias, stencil values, blend constants, cull and front face, depth and stencil tests, primitive restart... | record | at replay |
| Render targets | the pass's attachments (`Targets`, `begin_pass`) | record (immutable texture copies and views) -> the back end's `Texture` for that storage | at replay (images held) |
| Vertex streams | `vkCmdBindVertexBuffers2` (buffer, offset, size, stride) | record (address, size, stride) -> the pool's VRAM mirror, synced from the captures at the pool's first binding in the submission | **guest as submitted** for the pages written since the pool's last sync; static pools: held |
| Index buffer | `bind_index` | record (address, type) -> mirror as above; quads and `uint8` without the extension: read as submitted and converted in the stream | **guest as submitted** |
| Uniform blocks (NVN slots 0-15 = Vulkan bindings 3-18) | descriptor: the stream (overlay or copy) or the mirror | record (address, size) -> `ub_overlay` if the stream updated the range (`UPDATE_UB`), else the pool's mirror, else a copy from guest as submitted | **guest as submitted**, or the record's bytes for `UPDATE_UB` |
| Storage buffers, images | descriptors | record (address, size; image handles) -> mirror / texture | guest as submitted; GPU writes copied back at the submission's end where the CPU reads them |
| Textures and samplers | image views and samplers in the descriptor set | record (handles) -> the registrations the game made in its texture and sampler pools (`register_texture_copy`, in submit order) -> `VkImage` uploaded from the storage (or its shadow taken by the capture), ASTC transcoded | registrations: held, in order; storage: at first use and when the CPU wrote it, as submitted |
| Driver constant banks (c0, c2) | uniform data in the stream | computed from `NvnState` (`driver_bank_fill`: texture and storage handles, grid, push constants) | at replay |
| The draw | `vkCmdDraw[Indexed][Indirect[Count]]`, mesh tasks | record (counts, firsts, instances, base vertex); indirect arguments and counts by address | indirect: **guest or GPU as submitted** |
| Order around it | barriers, pass breaks, events, counters, conditions | record; reports and conditions are read and written on the CPU in stream order | at replay |

**From command memory: nothing**, since the engine has none (section 1.1). From the records, everything that is a value;
from guest memory as submitted, every byte reached through an address: uniform blocks, vertex and index streams,
storage buffers, indirect arguments, texture storage, the program object. What "as submitted" means is the same for each
kind: the bytes the game had written when it called `QueueSubmitCommands` (the capture of the pages written since the
previous submit), because the console's GPU reads them after the submit and before the game's next writes, and the
replay runs later than that. Two kinds are read even earlier: deferred binds' handle and range arrays are read at the
submit call itself (`resolve_deferred`), and `UPDATE_UB` / `PUSH_CONST` data at the call. From state the back end holds:
programs, pipelines, the texture and sampler registrations and their images, mirrors of static pools, the queue's
`NvnState` between submissions of the same queue.

## 3. The direct path's interface

### 3.1 Where the seam goes, and why there

Three facts from the code fix the place:

1. **The native pass records into a command buffer the translated code is recording.** `0x8006cd60` initializes a
   command buffer on its stack, `0x80a47660` records barriers, waits and called recordings into it, and in the middle of
   that calls the listener `0x806602e0`, whose output (the scene pass) must come between them (`gfx-record-job.md` 1). A
   frame stays mixed at a finer grain than a command buffer or a pass: inside one command buffer, translated records,
   then native ones, then translated ones again; and below the pass, the render-state function `0x80077d60` (native)
   is called from translated draw functions between their translated calls.
2. **Command handles are host addresses of recordings, and the game keeps and reuses them**: the job writes the handle
   to its slot (+80), the main thread calls or submits it later, possibly in later frames, and `CallCommands` copies it
   into another command buffer. A native recording must be such a handle, or the translated callers cannot use it.
3. **The back end consumes records already**: `replay()` decodes typed C structs; the decode is a switch, and the costs
   measured are in what a draw does (section 1.4).

So the direct path writes **records into the command buffer's own host segment**, in the order of emission, on the
recording thread, through the NVN layer's C entry points. The back end sees one stream per command buffer, as today;
handles, `CallCommands`, held submits, queues, events, the parallel replay and the captures stay as they are. What
leaves the path: the NVN calling convention (guest registers set and read), the guest state objects written and read
back, the per-call `gw()` checks and host-table lookups (`cmd_host` a record), and, for data the producer owns, the guest
page, its capture and its mirror sync (section 4). A second stream handed to the back end beside the NVN command buffers
(a `nvn_submit`-like entry taking a typed list) is not proposed for the mixed period: it cannot be placed inside a
command buffer the translated code records, and it would need its own handles for the game to call.

### 3.2 The structures

**The record layouts, shared.** The native tree includes only `hl/...` and the standard library, and `rx_nvn.h` is the
OS layer's. The layouts native code writes go in a C header of the native tree, `engine/native/include/hl/c/hl_gfx_rec.h`:
the header `{op, size}`, the op numbers the native side writes, and those payloads (`bind_state`, `stencil`,
`poly_offset`, `blend_color`, `flag`, `depth_range`, `viewports`, `scissor`, `bind_buf`, `bind_handles`, `bind_vbuf`,
`bind_vattr`, `bind_vstream`, `draw`, `draw_idx`, `barrier`, plus the new ones of 4.2), each with its size. `nvn_native.c`
includes it and `_Static_assert`s, for every one, that `sizeof` and each `offsetof` equal those of `rx_nvn.h`'s struct and
that the op numbers are the same: a change to either breaks the build, not the picture. No change to `rx_nvn.h`'s
existing records.

**The writer** (OS layer, `nvn_native.c`), a C surface handed to native code with the other host functions
(`hl_gfx_host` in `hl/c/hl_gfx.h` gains a `rec` member; `hl_gfx.c` fills it):

```
typedef struct hl_gfx_rec_host {
    /* the command buffer object at guest address cmd: its record segment, for appends by the calling thread
       (0 when cmd is not a command buffer that is recording) */
    void *(*open)(void *user, uint64_t cmd);
    /* a record of op with payload bytes of payload, appended in place: its payload, zeroed (the size and alignment
       as rec(): header included, 8-aligned); the command buffer's accounting (cmd_used, hw_used) kept */
    void *(*append)(void *user, void *seg, uint32_t op, uint32_t payload);
    /* the immutable copy of the texture object at guest address tex (tex_copy's table): for SET_RT and copies */
    const void *(*tex_pin)(void *user, uint64_t tex);
    /* hw_fix: what the real driver would have used for the last record, in bytes (the game sizes heaps from it) */
    void (*hw_bytes)(void *user, void *seg, uint64_t bytes);
    void *user;
} hl_gfx_rec_host;
```

`open` returns the `cmd_rec` (checked: magic, `recording`, the slot's owner) once per native unit of work, not per
record; `append` is `cmd_alloc` + `put_hdr` + zeroing without `cmd_host`'s lookup. These are the only new things the OS
layer exports; they live beside `rec()` and share its code.

**The native side** (`engine/native`, `hl::gfx`):

- `RecordWriter` (`hl/gfx/Records.h`): typed methods over one open segment, one per record kind
  (`bind_state(kind, bytes)`, `stencil(face, which, value)`, `poly_offset(f, u, c)`, `bind_ubo(stage, index, addr, size)`,
  `bind_textures(stage, first, handles)`, `bind_vertex_buffers(first, ranges)`, `draw_indexed(...)`,
  `ubo_inline(stage, index, bytes)`...), each writing the record the NVN handler would have written for the same call,
  including its `hw_bytes`. This is the layer producers that are written for it call directly (the draw loop, brick D).
- `RecordSink : CommandSink` (the "second sink" of the brief): takes the `Command`s existing native code emits (NVN
  procedures with their registers: `RenderState::emit` today) and turns them into `RecordWriter` calls. For the
  procedures that set state objects (`BlendStateSet*`, `ColorStateSet*`, `ChannelMaskStateSet*`, `DepthStencilStateSet*`,
  `PolygonStateSet*`), it applies the setter to a native copy of the object keyed by its guest address (as the handler
  does in place), and a bind appends that copy's bytes; nothing is written to the guest object. The code that emits does
  not change, which is what `Commands.h` promised ("a sink can write the Vulkan back end's records directly, with no
  change to the code that emits").
- The NVN numbering (`Nvn.h`'s `Proc`) stays the commands' language; the record numbering is `hl_gfx_rec.h`'s.

### 3.3 Ownership, lifetime, threads, order

| What | Owned by | Until | Thread |
|---|---|---|---|
| The open segment (`cmd_rec`) | the command buffer object, as today | `EndRecording` (flattened into the handle's copy), `Finalize` | the one thread recording that command buffer (the NVN layer's rule: "a command buffer records on one thread at a time"): the thread that runs the native unit, the same that runs the translated code around it |
| The native copies of state objects (`RecordSink`) | the sink, per native unit (a call of the render-state function, a pass) | the unit's end; a state object bound in a later unit is set again by its code first (the render-state function sets every object it binds on each call: `gfx-record-job.md` 8) | the recording thread |
| A record's payload, inline data included | the recording, then its flattened copy (the handle) | the copy's death: its control memory added again or its mark overwritten, **and** the replay past every submit made before (`free_dead_copies`); a handle called again in a later frame keeps its data with it | written by the recording thread; read by the replaying thread, the dry pass and the segments' workers |
| Texture copies (`tex_pin`) | the NVN layer's table | the session (as today) | any |

**Order with the rest of the frame.** Within a command buffer, records are in emission order: what translated code
records before the native unit precedes it, what it records after follows, because both append to the same segment on
the same thread. Between command buffers nothing changes: the game's submits, holds, queues, events and called
recordings order them as today. A native producer must therefore never record from another thread into a command buffer
the game is recording (it would need the segment's ownership handed over; not needed by any unit planned).

**Handles kept consistent across both paths:**

- *Command handles*: produced by the same `EndRecording` (flatten, live set, control-memory mark) whether the records
  came from NVN calls or from `RecordWriter`; nothing distinguishes them afterwards.
- *Textures and samplers*: handles are the game's ids into its own texture and sampler pools; native code reads them
  where the game keeps them (material tables, bind arrays) and writes the same values; registrations stay the game's
  calls (`RegisterTexture` / `RegisterSampler`, `rx_nvn_register_*`, queued in order with the submits). A deferred bind
  keeps its source array address so that `resolve_deferred` reads it at the submit, as for the game's.
- *Buffers*: GPU addresses, which below 1 TB are guest addresses (virtual pools above resolve through the NVN layer's page
  table); a native producer passes the game's addresses unchanged.
- *Programs*: the record keeps the guest `nvn_prog` pointer, as `nvnCommandBufferBindProgram` does (the replay copies the
  object at replay; `rx_nvn_program_released` orders releases after the work that uses them).
- *Render targets*: through `tex_pin`, the same immutable copy table as `SetRenderTargets`.

### 3.4 What the back end gets, against its real entry points

For bricks A to E: **no change to `MkwVkRenderer` / `MkwVkHost`** (`vk_api.h`, `MKWVK_API_VERSION` stays 12). The back
end receives native records through `nvn_submit_q` inside ordinary handles. The one back-end change is the new record
kind of 4.2 (`replay()`'s switch and `build_descriptors`' uniform path, plus `vk_state.h`'s `NvnState`). A back-end entry
point would be needed only for brick F's variant that writes uniform data into device-local host-visible memory at record
time (4.3), which is not proposed for the mixed period.

## 4. The data question: what the back end still reads "as of the draw"

### 4.1 By kind

| Kind | Written by | What the code shows of its lifetime | What a native producer does |
|---|---|---|---|
| Static vertex, index, texture and shader data (`0x024`, `0x064` pools) | loading (decompression into the pool) | written once at load; game code touched under 1 MB of 3.35 GB of `0x024` in 35 s (`memory-boundary.md` 7); unwritten pages are not captured: **no per-frame copy today** | nothing to change: bind the game's addresses (mirror held) |
| Per-draw uniform blocks computed by the producer itself | the native producer (once the draw loop is native) | its own | **hand the bytes in the record** (4.2): no guest page, no capture, no mirror sync; the replay copies them into its upload stream |
| Uniform blocks and per-frame constants written by translated code (`0x022` pools: the draw loop `0x80255be0` is the largest identified writer, 7-8 MB of distinct pages in 35 s; `0x802a8fc0` 5, `0x80364ee0` 4.3, `0x80658660` 3.4, `0x8572f300` 1.5-2.4, `0x8575cdc0` 1-2) | translated code | **rewritten after the submit, observed** (the reasons of 1.3); deferred constant pools filled just before the submit (`nvn_native.c`'s header comment), i.e. possibly after the draw was recorded | as today: bind the address, the capture and mirror syncs serve it. Copying the bytes at record time is right only if nothing writes the range between the draw's recording and the submit: checkable per draw (brick C's check), not provable from this code |
| Streamed vertices, instance data | translated code | as above (instance data and transforms read a later frame's when synced late: the "one-frame flashes" comment of the captures' table) | as above |
| GPU culling lists, indirect arguments, counters | the GPU (and the CPU's resets) | two list sets alternating frame to frame (`RX_VK_SYNC_CAPTURE` comment); reports written in stream order | as today: GPU-written ranges stay mirrors with write-backs |
| Deferred bind arrays | translated code | refilled for the next frame as soon as the submit returns | as today: the record keeps the array's address, read at the submit |
| The program object | translated code (`SetShaders`) | released through `rx_nvn_program_released`, ordered after the work | as today |

**Read in place** (no copy at all) is possible only for memory nobody writes between the draw and the GPU's read: static
data (already so), and data a native producer owns and promises not to rewrite until the submission is done, which is
easier to give as bytes in the record than as a guest address with a promise. For memory translated code writes, this
engine's code records the opposite as fact, and no structure was read that bounds those writers' rings (their sizes,
how many frames ahead they write, `gfx-record-job.md` "Not read yet"): **nothing about their lifetime can be proven from
the code**; what can be done is measured, per range: `HOS_NVN_HOLD_CHECK=1` already counts, for the held submits, pages
written again between the submit and the flush and those whose bytes differ (`hos_hold_check`), and brick C adds the
same check per uniform block between record time and submit time.

### 4.2 Uniform data in the record: the new record kind

`NOP_UBO_INLINE` (appended to `rx_nvn.h`'s enum before `NOP_COUNT_`; payload `{int32 stage, index; uint32 size, pad}`
followed by `size` bytes, at most 64 KB as `UPDATE_UB`):

- **Replay** (`vk_replay.cpp`, `replay()`): in every mode (serial, dry, worker) it sets the stage's slot in `NvnState` to
  "inline" with a pointer to the bytes in the recording and their size, and bumps `bind_gen` when the bytes differ from
  the slot's last (a pointer compare first, then the bytes). The slot's address is cleared, so nothing reads guest memory
  for it. The pointer stays valid for the replay of that submission (the copy outlives it, 3.3), and the parallel replay's
  saved states (`par_points`) carry it as they carry the rest of `NvnState`.
- **Descriptors** (`vk_draw.cpp`, the uniform path of `build_descriptors`, NVN slots 0-15): an inline slot is served as
  the driver bank c1 is (`stream_alloc` and a copy into the recording's stream, which is device-local host-visible memory
  behind the BAR: `memory-boundary.md` 11.4); no `ub_overlay`, no `gpu_buffer_bind`, no `memory_read_as_submitted`.
- **What reads `s.ubo` addresses elsewhere** must accept an inline slot: the DLSS and FSR 3 scene notes read the vertex
  stage's slot 0 address (`dlss_note_scene_draw`, `fsr3_inputs::note_scene_draw`, by address in guest memory), the exposure
  log, dumps. Each is to be listed and given the inline bytes or skipped; this is the change's main risk.
- **Data a producer computes and the game also reads back** (if any) cannot leave guest memory: the producer then writes
  both (guest bytes for the game, the record for the GPU). Whether the game reads any per-draw uniform block back was not
  established.

`UPDATE_UB` is not the same thing and is not reused: it writes a guest GPU range in stream order, and every later bind of
that range in the submission (and in submissions still in flight) sees it through `ub_overlay`, which is the console's
meaning; an inline block belongs to one bind.

### 4.3 What is not proposed, and why

- **A ring in device-local host-visible memory written at record time** (the directive's "written by the CPU in place"):
  it would skip the replay's copy of inline bytes, but a ring slot must be retired by the GPU's completion of every
  submission that uses it, and a recording can be called and submitted again in later frames (handles live as long as
  their control memory). The inline record ties the bytes' life to the recording's, which the NVN layer already manages;
  a ring would need per-recording reference counts. Worth it only once whole frames are native.
- **A resolved draw packet that skips the replay's state tracking**: the decode is not where the measured per-draw time is
  (descriptors 1.26 us, commands 1.38, program pick 0.61, pipeline 0.13, TICK-049), and the replay's caches already skip
  work when state does not change (`same_desc`, `pipe_fast`, `vertex_set`). It is kept as brick F, conditional on a
  measurement of the decode's share once speed is measured again.
- **Reading translated-code data in place**: `RX_VK_MIRROR=0` (the GPU reading imported guest memory) gave 5.5 fps and 143 ms
  of GPU a frame (SPRINT-5 session 6); data read in place has to be in VRAM, which for written data means a copy anyway.

## 5. The plan in bricks

Each brick has a switch (`HL_GFX_*`, 0 off meaning the previous behaviour), is usable in the game alone, and is checked
by function and picture (the three scenes, shots compared with the reference, the brick's counters clean). No speed
measurement in this phase.

### A. Record check: native records beside the NVN path (no behaviour change)

- **What**: `hl_gfx_rec.h`, the writer's C surface in `nvn_native.c` (3.2), `RecordWriter` and `RecordSink`. With
  `HL_GFX_REC_CHECK=1`, at each call of the native render-state function (brick 2.2 on: `HL_GFX_STATE=1`), the commands are
  emitted twice: into `RecordSink` writing into a scratch segment (not the command buffer's), and into `NvnSink` as today;
  the records the NVN handlers appended to the command buffer's segment over the call (the segment's cursor before and
  after) are compared byte for byte with the scratch records. Mismatches counted, the first eight logged in full.
- **Files**: `engine/native/include/hl/c/hl_gfx_rec.h` (new), `engine/native/{include,src}/hl/gfx/Records.*` (new),
  `engine/native/include/hl/c/hl_gfx.h` and `src/c/hl_gfx.cpp` (the `rec` member, the check's calls),
  `engine/host/nvn_native.c` (the three functions and the static asserts), `engine/host/hl_gfx.c` (the switch, the cursor
  read), `engine/native/tests/gfx_test.cpp` (records for every kind written, state-object setters on native copies).
- **Check**: 0 mismatched records over a run in each scene, with the call count reported (both sides non-empty). The
  game's behaviour is unchanged (the NVN path still records).
- **Risks**: the handlers' small rules (`hw_fix` sizes, `BOOL` truncation, the channel mask's bit packing, the setters'
  defaults) reproduced wrong: that is what the check finds.
- **Before B**: zero mismatches in the three scenes.

### B. The render state written as records, no NVN call

- **What**: `HL_GFX_REC=1`: the native render-state function emits into a `RecordSink` on the calling command buffer's
  open segment instead of `NvnSink`. The guest state objects in its frame are no longer written (they are the
  function's own, dead after it returns: `gfx-record-job.md` 8); the 352-byte block is written as before (the game reads
  it).
- **Files**: `engine/host/hl_gfx.c` (the sink chosen, `open` on the context's command buffer: block +40 -> +216),
  `engine/native/src/c/hl_gfx.cpp`.
- **Check**: brick A's comparison kept available in this mode as `HL_GFX_REC_CHECK=2` (the records written for real,
  the NVN calls made into a scratch command buffer object and compared); the call-by-call check of 2.2 still passes;
  validation runs in the three scenes with shots.
- **Risks**: a command buffer that is not recording when the gate runs (the gate then falls back to `NvnSink`, counted);
  the pass check of `HL_GFX_CHECK` taps NVN procedures and would no longer see these calls (to be run with `HL_GFX_REC=0`).
- **Before C**: B clean in the three scenes. B by itself is small (the 33 calls a draw stop paying the guest calling
  convention and the setter round trips); its value is that the seam is proven on live command buffers.

### C. Uniform data handed inline: first the check, then the switch

- **What**: `NOP_UBO_INLINE` in `rx_nvn.h`, `vk_replay.cpp`, `vk_draw.cpp`, `vk_state.h` (4.2), and
  `RecordWriter::ubo_inline`. A producer is needed that computes uniform bytes: until the draw loop is native, the first
  use is a **copy at record time** of a block the translated draw loop has written just before binding it (the native
  draw loop of D will compute them instead). So the brick starts with a check: `HL_GFX_UBO_CHECK=1` records, beside each
  `BIND_UBO` of the scene pass, the block's bytes at record time in a side table, and the replay compares them with
  what it reads as submitted (`memory_read_as_submitted` at the draw), counting blocks equal, different, and by which
  NVN slot and pool. Only the slots found equal in every scene become candidates for `HL_GFX_UBO_INLINE=1`.
- **Files**: `engine/gpu/render/rx_nvn.h`, `engine/gpu/vk/vk_replay.cpp`, `vk_draw.cpp`, `vk_state.h` (and the readers of
  `s.ubo` listed in 4.2), `engine/host/nvn_native.c` (the side table for the check), `engine/native/src/gfx/Records.cpp`;
  tests: a back-end test that decodes an inline record into `NvnState` (the tests cut functions from the sources: the
  existing replay tests are the model).
- **Check**: the comparison's counts (equal must be 100% for a slot to be inlined); the picture in the three scenes with
  the switch on; the capture's page count (`RX_VK_CAPTURE_WHERE`) shows whether those pages stopped being written (they
  stop only when nothing writes the guest block any more, i.e. with D).
- **Risks**: blocks filled after the bind and before the submit (the deferred constant pools): the check says which;
  readers of `s.ubo` addresses (DLSS, FSR 3 inputs); a block the game reads back.

### D. The draw loop native, emitting draws as records

- **What**: the scene pass's draw loop `0x80255be0` (5,961 lifted instructions; BindUniformBuffer 30 sites, BindTexture
  22, a DrawElements, vertex state and buffer binds) or a part of it, native, writing records through `RecordWriter`
  directly (program, vertex state, vertex and index buffers by the game's addresses, textures by the game's handles,
  uniform blocks it computes inline), with the call-by-call check of brick 2.2 extended to records (run native into a
  scratch segment with its guest writes kept aside, then the translated body for real, compare records and bytes
  written). This is step 3 of `gfx-record-job.md` reaching the loop; its reading (what the 136 indirect calls reach) is not
  done.
- **Files**: `engine/native/{include,src}/hl/gfx/DrawLoop.*` (new), the bridge's layouts (`hl/bridge/GfxBridge.h`),
  `engine/host/hl_gfx.c` (its gate), `engine/nx2/hl_gates.py` input (its hash), tests.
- **Check**: records equal call for call in the check mode; the uniform blocks it computes equal to what the translated
  loop wrote to guest memory (bytes compared at the end of each call); the picture; the capture's pages in the draw loop's
  pools fall (that is where the 46 MB would start to shrink).
- **Risks**: guest calls in the middle of the loop (it is call-heavy: 116 direct callees), structures read in place, the
  guest pages it stops writing being read by someone else (to be found with `HOS_POOL_TRAP` on that pool before the switch).

### E. Native recordings handed over without the flatten copy

- **What**: when a whole command buffer is recorded natively (unit B of `gfx-record-job.md`, the item body `0x8006cd60`),
  `EndRecording`'s copy of the segment is skipped: the segment itself becomes the handle's copy (the segment handed over
  to the copy list, a new segment taken for the next recording), saving one copy of every byte recorded.
  `CallCommands`' inline copy stays (a call record followed by the replay would need the called copy kept alive until the
  caller's replay, which `release_copies` does not guarantee: a copy released before its caller is submitted is freed
  once the replay passes the submits made before the release, not the caller's).
- **Files**: `engine/host/nvn_native.c` (`EndRecording` taking ownership of the segment for writers that opened it).
- **Check**: the handle digests of `gfx-record-job.md` 6 (a digest per recording) equal between `HL_GFX_REC_HANDOFF=0/1`
  runs on the same scene start; picture.

### F (conditional). A resolved draw packet

Only if the decode's own share turns out to matter when speed is measured again: a record carrying a draw with its
complete state already resolved (pipeline key inputs, bindings by slot, inline uniform data), consumed by a `draw()`
entry that takes it without updating `NvnState` field by field. It would need the parallel replay's dry pass to keep
points across such records, and the descriptor and pipeline caches keyed from it. Not specified further: nothing measured
says it pays.

### Order and what each needs

| Brick | Needs | Changes the game's behaviour |
|---|---|---|
| A | brick 2.2 (`HL_GFX_STATE`) | no |
| B | A clean | yes (records instead of NVN calls for the render state) |
| C check | the record writer; the back end's comparison | no |
| C switch | C's check at 100% for a slot | yes |
| D | B, C, and the reading of `0x80255be0` (its callees, the structures it reads) | yes |
| E | unit B of `gfx-record-job.md` native | yes (no byte changes in the stream) |
| F | a measurement | - |

## 6. What this does not solve

- **The capture is data, and most of it stays.** The ~46 MB a frame are pages written by game code in the mirrored pools,
  the texture storage the replay uploads from, and the pools of 64 MB or less; ~9,000 of the ~11,900 pages lie in one
  256 MB region. Which pools and which writers make those pages was not attributed (the trap counted first touches over
  35 s, not bytes a frame). The draw loop under the native pass is the largest writer identified among `0x022` pools; the
  rest (`0x802a8fc0`, `0x80364ee0`, `0x80658660`, `0x8572f300`, `0x8575cdc0` and the unidentified) is not graphics front-end
  code that this plan makes native. Direct emission removes only the pages a native producer stops writing in guest memory.
- **Command memory**: none to remove (section 1.1). What the producer side does lose is host work: the calling
  convention and setter round trips (B), EndRecording's copy for native recordings (E); `inline_recording` stays.
- **The replay's per-draw work** (program pick, pipeline, descriptors, barriers) is the same for a native draw as for a
  translated one; F is the only brick aimed at it, conditionally.
- **Everything still recorded by translated code**: the other passes of `0x80a47660`, the sister items, `0x80084de0`, the
  culling job's compute, the UI, effects: all of it stays NVN calls, records and captured data.
- **The render chain's other bounds**: the GPU's tail and the light meter's readback (SPRINT-5), mirror write-backs for
  GPU-written ranges the CPU reads, the late pages.
- **The capture buffers' RAM** (402-525 MB) shrinks only as the pages to capture do.

## 7. Not established

- Whether the decode (`replay()`'s switch and `NvnState` updates) is a measurable share of the replay: never timed apart;
  TICK-049 / 050 split descriptors, commands, program pick and pipeline only.
- Which pools and writers make the capture's ~11,900 pages a frame, and how many of them a native draw loop would stop
  writing.
- The lifetime of any per-frame buffer the translated code writes (its ring size, how many frames ahead it writes):
  nothing read bounds it; only measurement can say, per range.
- Whether the game reads back any uniform block it writes for a draw (decides whether a native producer must still write
  guest memory).
- Whether the uniform blocks the translated draw loop binds are complete at bind time (brick C's check measures it).
- Whether the render-enable function `0x80078b10`'s other path (writing a method into the command memory itself when a
  global switch is set, `gfx-record-job.md` 8) is ever taken: with this NVN layer such a write reaches nothing, since the
  command memory is never read.
- How the parallel replay's dry-pass points and the segments' cost behave with inline records (C): by reading, the state
  copy carries a pointer; not checked in code.
- Where the recording job's handles (slot +80) are called and submitted (`gfx-record-job.md`), which matters for E.
- The readers of `NvnState::ubo` addresses outside the descriptor path: two found (DLSS, FSR 3 scene notes); the full
  list was not made.

## 8. Bricks A and B as built

### What is built, and how the game reaches it

| Piece | Where | What it does |
|---|---|---|
| The shared layouts | `engine/native/include/hl/c/hl_gfx_rec.h` | the header `{op, size}`, the op numbers native code writes, the payloads (`bind_state`, `stencil`, `poly_offset`, `blend_color`, `flag`, `depth_range`, `viewports`, `scissor`, `bind_buf`, `bind_handles`, `bind_vbuf`, `bind_vattr`, `bind_vstream`, `draw`, `draw_idx`, `barrier`), the six NVN state objects a bind copies, the NVN values of their defaults; `nvn_native.c` static-asserts every size, offset, op number and value against `rx_nvn.h` / `nvn_api.h` |
| `RecordWriter` | `hl/gfx/Records.h`, `src/gfx/Records.cpp` | one typed method a record kind, each writing the record and the accounting (`hw` as the procedure's `hw_fix`) the NVN procedure would; for producers written for it (brick D) |
| `RecordSink` | same | a `CommandSink`: the 29 procedures of `Nvn.h` turned into records for one command buffer; the 18 setters applied to native copies of the state objects, keyed by guest address, each seeded from its guest bytes at first use (the depth-stencil and polygon objects have bits no setter covers: they keep what the stack held, as with the procedures); a bind appends the copy's bytes; nothing is written to guest memory |
| `ScratchSegment` | same | a segment in host memory keeping the command buffer's accounting as the NVN layer does: the check's other side, and the tests' |
| The sink chosen | `src/c/hl_gfx_rec.cpp` (`hl::c::gfx_rec_emit`), one line in `hl_gfx_state_apply` (`src/c/hl_gfx.cpp`) | at each answered call of the render-state function: `NvnSink` (as before), `RecordSink` on the live segment, or both compared |
| The OS layer's surface | `nvn_native.c` (`nvn_rec_open / _append / _mark / _bytes / _rewind / _guest`, beside `rec()`), `engine/host/hl_gfx_rec.c` (hands it over at start, reads the switches, logs) | below |
| Tests | `engine/native/tests/records_test.cpp` | each record kind's bytes and accounting against the layout; the sink against an independent model of the 29 handlers (bit fields rewritten as shifts on the guest bytes) over ~350 inputs of the render-state function, in both orders; seeds, strays |

The game reaches it through brick 2.2's gate: `0x80077d60` answered natively (`HL_GFX_STATE=1`, the default),
`hl_gfx_state_apply` derives and writes the block, then hands the commands to `gfx_rec_emit`, which asks the OS layer
once for the switches.

### The host surface, against section 3.2's proposal

| Proposed | Built | Why |
|---|---|---|
| `open(cmd)` checked on magic, `recording`, owner | `open(cmd)`: the object's slot is its own (else 0, and the call goes through the NVN procedures, counted "left to NVN") | that is the one case where the procedures do something else (`cmd_host` adopts a new slot and logs); the procedures record whether or not the buffer is recording, so requiring it would only send more calls to the fallback |
| `append(seg, op, payload)` | `append(cmd, op, payload, hw)`: the NVN layer's own `rec()` (and `hw_fix` when `hw` is not 0) | the same code as the procedures, so the accounting cannot drift; `hw_bytes` folded in |
| `tex_pin(tex)` | not built | none of the 29 procedures pins a texture (no `SET_RT`, no copy): it belongs with the first producer that records targets |
| `hw_bytes(seg, n)` | `append`'s `hw` | none of the 29 procedures calls `hw_fix`; the other record kinds' sizes are in `RecordWriter` |
| - | `mark`, `bytes`, `rewind` | the check: where the recording stands (length, `cmd_used`, `hw_used`, `last_rec`), its bytes, and a rewind to drop the NVN procedures' copy in `HL_GFX_REC=1 HL_GFX_REC_CHECK=1` |
| - | `guest(addr, n)` | the guest reads the procedures make (`gw`: the game stops on an address outside the window, as there): the blend color at bind time, the state objects' seeds, an object bound but not set |
| - | `mode`, `log` | switches read and logged by the OS layer, once |

### What the 29 procedures did besides their record, and what happens to each

| Side effect | Procedures | With `HL_GFX_REC=1` |
|---|---|---|
| the state object packed in place in guest memory (the render-state function's own stack frame, dead after it returns) | the 18 `*StateSet*` | applied to the sink's native copy; the guest frame is not written. Nothing reads it but the binds of the same call (`gfx-record-job.md` 8) |
| out-of-range target index ignored | `ColorStateSetBlendEnable`, `ChannelMaskStateSetChannelMask` | the same guard |
| `HOS_NVN_CHMASK_LOG` (the first 40 calls' arguments, debugging) | `ChannelMaskStateSetChannelMask` | not reproduced: a debugging print |
| the command buffer looked up per record (`cmd_host`; a slot that is not the object's own: a new one adopted, logged) | the 11 `CommandBuffer*` | looked up once (`open`); not its own slot: the whole call goes through the procedures, which do what they always did |
| the segment grown (`cmd_reserve_rec`: pool blocks, the segments' byte and count statistics) | the 11 | the same function, through `rec()` |
| **the accounting the game reads back**: `cmd_used`, `hw_used`, `last_rec` in the guest command buffer object (`GetCommandMemoryUsed` / `Free`; the game sizes its heaps from it) | the 11 (none calls `hw_fix`: the record's size counts) | the same `rec()`; compared by the check after every call |
| guest pointer arguments checked (`gw`: the game stops outside the window) | the binds (the object), `SetBlendColor` (16 bytes) | the same check (`guest`) |
| the bound object's bytes and the blend color read at the call | the five binds, `SetBlendColor` | read at the same point of the call: the block is written before the commands, as before |
| the guest context's argument registers set (x0-x5, s0-s2) by the native caller (`nvn_call`) | all | not set: they are caller-saved scratch registers at the function's return, which nothing may read |
| texture pins, control-memory marks, events, counters, reports | none of the 29 | - |

The pass check's tap (`HL_GFX_CHECK`) sees NVN calls; with it set the gate compares the translated function instead of
answering, so the records path is not used then. The parallel replay and its dry pass read the same bytes; no consumer
was found that keys on anything the procedures did beyond their records.

### Switches

`HL_GFX_REC=1` the render state written as records (the default since the runs below; `HL_GFX_REC=0` is the NVN path as before);
`HL_GFX_REC_CHECK=1` the record check (with `HL_GFX_REC=1`: the native records live, the procedures' compared and
dropped, the spec's `HL_GFX_REC_CHECK=2` folded into the same switch). Both act only where the render-state function is
answered natively (`HL_GFX_STATE=1` and no `HL_GFX_CHECK`). The line `hl gfx rec: calls written as records N (records R),
left to NVN F, commands not written S; check: calls compared C, records, bytes, mismatched calls M (accounting A)` every
10 s; the first eight mismatches in full (both sides' first differing record, in hex).

### The check and the runs

PC 3050, 110 s a scene (function only; last counter line of each run):

| Run | Scene | Calls compared | Records | Bytes | Mismatched calls (accounting) | Left to NVN | Commands not written |
|---|---|---:|---:|---:|---:|---:|---:|
| `HL_GFX_REC_CHECK=1` (brick A) | castle-move | 40,857 | 325,274 | 10,722,968 | 0 (0) | 0 | 0 |
| | kokiri-idle | 62,359 | 497,132 | 16,393,176 | 0 (0) | 0 | 0 |
| | hyrule-walk | 57,711 | 459,565 | 15,150,784 | 0 (0) | 0 | 0 |
| `HL_GFX_REC=1 HL_GFX_REC_CHECK=1` (brick B, checked) | castle-move | 51,955 | 413,282 | 13,621,800 | 0 (0) | 0 | 0 |

| Run | Scene | Ran | Calls written as records (records) | Left to NVN | Shots |
|---|---|---|---:|---:|---|
| `HL_GFX_REC=1` (brick B), PC 3050 | castle-move | to the end, save unchanged | 46,420 (369,038) | 0 | right after loading |
| | kokiri-idle | to the end | 65,851 (525,004) | 0 | right |
| | hyrule-walk | to the end | 64,993 (518,147) | 0 | right |

About eight records a call: of the ~33 commands, the 18 setters write no record (they set the objects the binds copy).

Main PC (`build/s5/valid.sh`, 110 s a scene):

| Run | Scene | Ran | Game speed ratio | Calls (records) | Mismatched calls (accounting) | Shots |
|---|---|---|---|---:|---:|---|
| `HL_GFX_REC_CHECK=1` (brick A) | kokiri-idle | to the end, no crash | 59.63 | 64,773 compared (514,257; 16,942,992 bytes) | 0 (0) | right (`s8a-*-shots`) |
| | castle-move | to the end, no crash | 59.89 | 54,354 compared (428,552; 14,098,256 bytes) | 0 (0) | right |
| | hyrule-walk | to the end, no crash | 59.54 | 16,276 compared (125,675; 4,115,544 bytes) | 0 (0) | right |
| `HL_GFX_REC=1` (brick B) | kokiri-idle | to the end, no crash | 60.18 | 65,217 written (517,793), 0 left to NVN | - | right: the same picture as `refshots-kokiri-idle` |
| | castle-move | to the end, no crash | 59.63 | 37,015 written (292,292), 0 left | - | right (`s8b-*-shots`) |
| | hyrule-walk | to the end, no crash | 59.64 | 153,432 written (1,222,285), 0 left | - | right |

Memory with `HL_GFX_REC=1` (game RAM / GPU pools / process RAM / commit / VRAM, MB): kokiri-idle 2,998 / 1,528 / 7,318 /
19,054 / 5,833; castle-move 3,528 / 1,716 / 7,780 / 19,875 / 7,246; hyrule-walk 3,093 / 1,591 / 7,589 / 19,341 / 6,121.
The brick changes no memory: what the segments hold is the same bytes.

### For brick C

- A record of any size is one `append`: `NOP_UBO_INLINE` needs no new host surface, only its op number and layout in
  `hl_gfx_rec.h` (and `rx_nvn.h`), checked the same way.
- **The accounting the game reads back must not grow with the inline bytes.** Every record counts its own size in
  `hw_used` unless `hw_fix` replaces it, and the game sizes its command memory from `GetCommandMemoryUsed`: an inline
  block of up to 64 KB must be appended with `hw` set to what the bind it stands for costs (20 bytes, `BindUniformBuffer`),
  or the game's heaps would be sized for bytes a console never writes. The record check compares this accounting
  already and would show it.
- The check's machinery (`mark`, `bytes`, `rewind`) compares any native unit's records with the NVN procedures' in place,
  call by call; brick D's check can reuse it as it is.

## 9. Brick C, step 1: are uniform blocks final at their bind, and what would an inline take out of the capture (measured)

### What was built (`HL_UBO_CHECK=1`, off by default; a measurement, no behaviour change)

| Piece | Where | What it does |
|---|---|---|
| The check record | `rx_nvn.h` (`NOP_UBO_CHECK`, `nvn_p_ubo_check`, `nvn_ubo_hash`) | appended after the last op; stage, slot, address, size and three hashes of the block's bytes (at most 64 KB) |
| The host side | `engine/host/nvn_native.c` (`ubo_check_on/_hash/_end`, `bind_buf`, `resolve_deferred`, `EndRecording`, `inline_recording`) | every `BindUniformBuffer(s)` (whoever calls: translated or native code) is followed by a check record with the block hashed at the bind; `EndRecording` adds the hash at the recording's end (a called recording's records keep their own), `QueueSubmitCommands` the hash at the submit. The record is outside the accounting the game reads: `cmd_used`, `hw_used`, `last_rec` are put back after it, and a new `cmd_rec.extra` / `copy_prefix.extra` keeps it out of what `CallCommands` adds to the caller |
| The back end | `engine/gpu/vk/vk_ubo_check.cpp` (new), hooks in `vk_replay.cpp` (`replay()`: `NOP_BEGIN`, `NOP_UBO_CHECK`, `NOP_UPDATE_UB`, `ubo_check_refs`; `present()`), `vk_memory.cpp` (`capture_make`, `memory_nvn_pool_of`, `memory_tex_capture_ranges`) | the replay thread's pass over the records (the serial replay or the parallel replay's dry pass, never a segment's worker: each record once) reads the block as submitted (`memory_read_as_submitted`, what the mirror sync and the descriptor's stream copy give the GPU) and compares; counts by stage and slot, size class, pool (flags, size); for the different ones, where the hashes first differ; whether an `UpdateUniformBuffer` of the stream overlaps the range; whether the same range was bound before in the same command buffer. And each capture's pages (`capture_make`) against the ranges the frame's records read, classified at each present |

Lines every 10 s: `hl ubo check: ...` (cumulative), `hl ubo check stage N by slot`, `by size`, `by pool flags:size`, `hl ubo check
capture the last 10 s ...` and `since the start`, and the pages read by no record by pool and 16 MB region. The renderer interface
(`vk_api.h`) is unchanged.

### The runs

Main PC, `bash build/s5/valid.sh s8c1 s8-ubo1 HL_UBO_CHECK=1` (the three scenes, 110 s each; logs in `build/s5/s8c1-*`), and one
castle run of the next build for the pages' pools (`s8c2-castle`). All three ran to the end with no crash line; the game speed
ratio 59.78 / 60.37 / 59.22 (kokiri / castle / hyrule). **The check is heavy**: it hashes every bound block three times on the
game's threads and reads it again on the replay's (29-57 GB hashed a run), and the runs' fps were 41-45, the castle's 22 (its
script marked the run "stopped early" for its few frames measured; the log shows it running to the end). It is a measuring
mode only. Shots looked at (the last of each scene): the castle town market, complete and right, as the reference.

Binds counted over the whole run (the load included):

| Scene | Binds | Equal | Different | of them changed bind -> end of recording | end of recording -> submit | after the submit | Updated in the stream | Re-binds of a range bound before in the command buffer (equal / different) |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| kokiri-idle | 21,350,119 | 58.0% | 41.3% | 48,579 | **8,770,198** | 2,696 | 139,355 | 8,337,037 / 4,627,692 |
| castle-move | 10,948,561 | 62.7% | 36.5% | 24,443 | **3,964,604** | 4,980 | 90,248 | 4,864,727 / 2,112,801 |
| hyrule-walk | 20,051,677 | 61.0% | 38.3% | 43,621 | **7,633,912** | 2,119 | 146,613 | 8,381,874 / 4,032,304 |

No block was unreadable. `UpdateUniformBuffer`s into a range bound already: 233,979 / 160,972 / 243,535.

By pool (flags : pool size), the same in the three scenes:

| Pool | Binds (castle) | Equal (kokiri / castle / hyrule) |
|---|---:|---|
| `0x22`, pools of 1 MB or less | 7,146,037 | 43.2% / 44.1% / 45.2% (every different bind is here, but ~2,000 a run) |
| `0x22`, pools of 1-16 MB | 1,452,865 | 99.9% / 99.8% / 100.0% |
| `0xa2`, 1-16 MB | 603,803 | 100% |
| in no pool the back end has flags for | 1,745,856 | 93.8% / 94.8% / 93.2% (the rest: updated in the stream) |

By size: 256 bytes or less 55-63% equal; up to 1 KB 72-74%; up to 4 KB 78-86%; **up to 16 KB 11-13%**; up to 64 KB 84-88% (the
rest updated in the stream, none different).

By stage and slot (vertex stage 0, fragment 1, the others 2-5): slots 5-8 and 10-13 of stages 0 and 1, slots 3-8 and 10-13 of
stage 1 (slot 3: 99.9%, a few hundred different a run), every slot of stage 2 but 9, and compute's slots 1-3 are **100% equal in
the three scenes**; they are bound about once or a few times a frame. The slots that carry the per-draw data are not: stage 0
slot 1 38-43% (the most bound: ~2.5-5.4 M a run), slot 2 74-76%, slot 3 18-22%, slot 0 33-37%, slot 4 73-84%; stage 1 slot 1
37-46%, slot 2 71-75%, slot 0 35-41%; slot 9 of stages 0-2 33-49%; compute slot 0 63-73%.

**What the different blocks are.** 99.3-99.4% of them changed between the end of their recording and the submit: the game
records a command buffer (binding ranges of its ring pools, `0x22` of 1 MB or less) and then writes the per-draw blocks it
bound, before it submits. A copy at the bind is the previous use's bytes for them. 0.6% of the different blocks (0.2% of all
binds) changed between the bind and the recording's end, and 0.01-0.05% of all binds after the submit call (the held submits:
bytes written between `QueueSubmitCommands` and the flush that captures). Re-binds of a range bound earlier in the same command
buffer are 52-53% of the different binds: the rings are reused within a command buffer, and the console's GPU too sees such a
range with its submit-time bytes at every draw.

The capture, pages a frame (the last 10 s of each run):

| Scene | Captured | Read only by equal uniform binds | by uniform binds, one at least different | by uniform binds and other records (all storage) | only by other records | texture storage read by no record | **read by no record** |
|---|---:|---:|---:|---:|---:|---:|---:|
| kokiri-idle | 9,414 (36.8 MB) | 168 | 238 | 87 | 111 | 35 | **8,774** |
| castle-move | 11,078 (43.3 MB) | 137 | 205 | 109 | 106 | 49 | **10,473** |
| hyrule-walk | 9,864 (38.5 MB) | 101 | 169 | 100 | 109 | 26 | **9,359** |

"Read by" means a record of the same frame names a range over the page: uniform and storage binds (immediate and deferred),
vertex buffers, index buffers (count x size; an indexed indirect draw's first 64 KB), indirect arguments and counts, buffer
copies' sources, render conditions. The pages read by no record (castle run `s8c2-castle`, a frame): ~9,500 in no pool the back
end has flags for, the twelve busiest 16 MB regions all in the 256 MB region `0x1050000000`-`0x1060000000` that SPRINT-5 found
holding most of the capture (1,280-1,360 a frame in `0x1059000000`), ~560-600 in `0x22` pools of 1 MB or less, ~340-500 in
`0x24` pools. No
record names those pages: they are read, if at all, through addresses the GPU finds in other data (storage buffers and uniform
blocks holding pointers: the global memory Eden's recompiler tracks, the culling lists), or by later frames, or not at all;
this measurement cannot tell which.

### Step 2: built for the one class equal everywhere, not for the per-draw blocks, and why

**Why not for the per-draw blocks (the numbers).** The slots that carry per-draw data (vertex stage slots 0-4 and 9, fragment
slots 0-2 and 9, geometry slot 9, compute slot 0: with the few mixed slots, about 90% of all binds) are equal at the bind for 18-84% of their binds, slot by
slot, and their different binds (36-41% of all binds, 4-9 million a run) changed between the end of the recording and the submit
in 99.3-99.4% of cases: the game records the command buffer, binding ranges of its small ring pools (`0x22`, 1 MB or less, where
every different bind lies), and only then writes the blocks it bound, before the submit. A copy at the bind would draw them with
the ring slot's previous bytes, and whether a bind is one of those cannot be known at the bind (the same slot is equal or not draw
by draw). **What would make the inline possible for them** is the writer: the code that fills those ring blocks after
`EndRecording` (not identified by this measurement; a `HOS_POOL_TRAP` / write watch on one `0x22` ring pool between a
recording's end and its submit names it) writing them before it records the bind, which only a native draw loop (brick D) that
computes the block and binds it in one step can do. Even then the gain in the capture is small: the pages read only by equal
uniform binds are 101-168 a frame (0.4-0.7 MB of 37-43 MB), every page any uniform bind reads 4-5% (~450-500 pages).

**The class equal everywhere, inlined (`HL_UBO_INLINE=1`).** Every bind of these slots was equal to what the replay read as
submitted in the three scenes (0 different, 0 updated in the stream, 15,796-451,954 binds a slot a run): vertex stage slots 5-8,
10, 11 and 13; fragment slots 4-8 and 10-13; geometry slots 0-8 and 10-13; compute slots 1 and 2. (Vertex slot 12 had one
different bind in hyrule, fragment slot 3 44-186 a run: left out. Compute slots 3 and up were not in the log, whose slot line was
cut by the log's line limit in those runs: left out.) They are ~10.5% of the binds (castle: 1.15 M of 10.9 M), bound once or a
few times a frame: per-frame
and per-pass constants. Built:

| Piece | Where | What it does |
|---|---|---|
| The record | `rx_nvn.h` (`NOP_UBO_INLINE`, `nvn_p_ubo_inline`: stage, slot, address, size, n, then the bytes) | appended after `NOP_UBO_CHECK` |
| The host | `nvn_native.c` (`ubo_inline_slots`, `ubo_inline_on`, `ubo_inline`, `bind_buf`) | a `BindUniformBuffer(s)` of those slots, at most 64 KB and contiguous in host memory, is recorded as the inline record instead of the address record; `cmd_used` and `hw_used` move as the 40-byte address record hw_fix'd to 20 would make them, the rest of the record is in `cmd_rec.extra`, which `CallCommands` leaves out of the caller's accounting (so `GetCommandMemoryUsed` reads the same numbers as without the switch) |
| The replay | `vk_replay.cpp` (`NOP_UBO_INLINE`, `NOP_BEGIN`, `NOP_BIND_UBO`, `NOP_BIND_BUFS`), `vk_state.h` (`ubo_bytes`, `ubo_bytes_n`) | in every mode (serial, dry, worker) the slot gets the bind's address as before and a pointer to the bytes; `bind_gen` moves when the address, size or bytes change; a bind by address drops the bytes; every recording's `NOP_BEGIN` drops them too (the bytes live in the recording that bound them, which may be freed after its submission: a later recording drawing without a rebind reads the slot by address, as before). The parallel replay's saved states carry the pointer with the rest of `NvnState` |
| The descriptor | `vk_draw.cpp` `uniform_range`, `vk_replay.cpp` `ub_overlay_onto` | an inline slot is copied from the bytes into the recording's upload stream, the stream's `UpdateUniformBuffer`s over the range applied as `ub_overlay` does, zeros past the bytes carried; no mirror, no read as submitted |
| The other readers of a slot's address (DLSS and FSR 3 scene notes, the bank and handle reads, the program pick's constant reads, dumps) | unchanged | they read the slot's address in guest memory as before: the measurement says the same bytes |
| The WebGPU replay | `rx_nvn.cpp` | the inline record as a bind by address |

The capture is not changed: 93-95% of the captured pages are read by no record at all and are still needed (they are read, if at
all, through pointers in other data), so "needed by no record" is not a safe test, and the pages only these slots' blocks hold
are a few at most.

**Validation (main PC, function and picture).** `valid.sh s8i1 s8-ubo3 HL_UBO_INLINE=1`: castle-move and hyrule-walk ran to the
end, no crash line, game speed ratio 60.18 / 59.75; kokiri-idle's first run spent its 110 s compiling the predicted pipelines
again (0 frames: the new `NvnState` fields change the replay's state stamp, which keys the pipeline cache, so the first start
after this build re-predicts ~25,500 pipelines) and was run again (`s8i2-kokiri-idle`): to the end, ratio 59.87. Shots looked
at (the last three to four of each scene): complete and right, the same market scenes as the reference, no flicker between
shots. Inlined in a run: 2.4-2.8 million binds, 9.6-11.3 GB of block bytes (~4 KB a block) copied into the records.
`HL_UBO_INLINE=1 HL_UBO_CHECK=1` in the castle (`s8i3-castle`, the check compares the inline bytes, hashed at the bind, with what
the replay reads as submitted): **every inlined slot 0 different** (e.g. fragment slot 4: 482,653 equal of 482,653; vertex slot 11
181,484 of 181,484); compute slots 3-8 and 10-13 were also all equal in that run (a fourth scene run, not inlined). Memory with
the switch (game RAM / GPU pools / process RAM / commit / VRAM, MB): castle 3,279 / 1,535 / 7,791 / 20,414 / 6,905; hyrule 2,454
/ 1,499 / 6,775 / 18,349 / 5,281; kokiri (game RAM / pools) 2,283 / 1,299.

**Default: off** (`HL_UBO_INLINE=0`). It is clean in the three scenes on the main PC, but it buys nothing yet: the capture is
unchanged and these blocks' pages are not what it copies; it adds ~100 MB a second of block bytes copied into the recordings
(not timed, rule 6); the PC 3050 was not run with it; and the slot list is a measurement of three scenes, not a property of the
game. It stays as the record kind and descriptor path brick D will use for blocks a native draw loop computes.

### What this says for the rest of the plan

- **A copy at the submit instead of the bind would match what the replay reads for all binds but 0.01-0.05%** (and the ~0.7%
  updated in the stream, which neither copy serves): `resolve_deferred` already walks every submitted record at the submit. It
  is not a gain either while the capture's bulk is the region above, and it puts every bound block's bytes (1.8 MB a frame on
  average over the castle run) onto the submitting thread; it is noted, not proposed.
- **The capture's 37-43 MB a frame are not uniform data.** ~85% of them (castle) are one 256 MB region that no record reads directly. What
  writes it and what reads it (a pointer table walk, `RX_VK_PROTECT_BARE`, `HOS_POOL_TRAP` on that range) is the next question for
  the capture, before any brick of this plan can reduce it; brick D's premise ("the capture's pages in the draw loop's pools
  fall") holds only for the ~1,000 pages a frame in the `0x22` and `0x24` pools.
- Section 7's line "whether the uniform blocks the translated draw loop binds are complete at bind time" is answered: no, for
  the per-draw slots; they are complete at the submit.
- **The next brick proposed instead of more inlining: the capture's attribution.** Name the writer and the readers of the 256 MB
  region `0x1050000000`-`0x1060000000` (85-95% of the capture): which pool it is (it is in no pool the back end has flags for),
  which guest functions write it between submits (`HOS_POOL_TRAP` over the region, as `memory-boundary.md` 6 did for the
  no-access pools), and whether the GPU reads it through pointers (the programs' global-memory descriptors that point into it,
  logged at the descriptor build). That decides whether the capture can be cut by range (a region the GPU never reads, or reads
  only through a known table) rather than by record. Second, for the per-draw uniform blocks: find the ring writer that fills
  them after `EndRecording` (a write watch on one `0x22` ring pool between a recording's end and its submit), which brick D must
  replace for an inline of those blocks to be right.
