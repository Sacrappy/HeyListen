# The graphics recording job 0x8006cd60: specification for `hl::gfx`

What the module job at guest `0x8006cd60` is, what it reads, what it calls and writes, where its time goes, why it is on
the frame's critical chain, and how it is to be replaced by the native module `hl::gfx`. Read from the user's local
translation (`%LOCALAPPDATA%\HookshotBuilder\sa\gen`, `sa\image`) and re-aggregated from the three sampled castle-move runs
of `hot-functions.md` (`build/hotfn/r1..r3`). Addresses are guest addresses, offsets are in bytes. No translated code is
quoted (README, rule 4).

Conventions. **ms** is ms per frame, mean of the three runs, under the sampler (the sampled frame is ~2 ms longer than an
unsampled one: read shares, not absolute speed). *CPU* counts every non-waiting sample. *Chain J* is time on the critical
chain while the main thread waits in its job-group wait (lr `0x80080188`); *chain F* is time on the chain while the main
thread is not in that wait (its own phases). "Read" means read in the lifted code or the host source; *(inference)* marks
what is concluded, not read. The scratch scripts are listed at the end.

## The answers in short

1. **0x8006cd60 is one item body of the module job-group runner `0x8007fe60`**, called directly (not through a pointer):
   the runner hands out the items of a list with an atomic counter, and an item of kind 2 runs `0x8006cd60`. Each call
   records **one NVN command buffer** from Initialize to EndRecording and Finalize, the command buffer object living on the
   job's own guest stack.
2. **It runs serially.** In 94-95% of its samples no other thread is inside it at the same sampling round, against 4-6
   threads for the culling job `0x800998a0` and 3-5 for the recording job `0x80084de0`, measured the same way. Its
   largest part, `0x80a47660` (2.2 of its 3.05 ms), was never seen on two threads at once. It migrates between the main
   thread and module workers 1, 3, 4 and 5 from frame to frame.
3. **It is on the chain twice over**: 0.63 ms of chain J on the module workers (the figure in `hot-functions.md`) **plus
   0.64 ms of chain F on the main thread**, which runs the job itself inside the runner (the jobs table of
   `hot-functions.md` counts workers only). 1.27 ms of the sampled chain in all.
4. **Its CPU is 3.05 ms** (module workers 2.31, main 0.73) where the root is within the sampler's 16 frames; 0.72 ms more
   carries its inner functions without the root in reach, part of which is this job (90% of its samples hit the 16-frame
   cut). Bound: 3.05-3.7 ms.
5. **63% of it is one pass**: `0x80660320` (1.91 ms), reached through a registered callback (`0x80076cd0` -> `0x806602e0`,
   both reached only through pointers) **while a global guest mutex (`0x84eb5440`) is held**.
6. **The "per-object mutexes" are a slot table scanned under one mutex per slot** in `0x80364080` (0.90 ms): 0.83 ms of it
   is the host's `LockMutex`, and **0.69 ms is `obj_lookup`**, the OS layer's table probe for a guest mutex that missed the
   per-thread cache. That is two thirds of all `obj_lookup` time in the game (1.04 ms). No contention: waits under the
   whole job are 0.007 ms.
7. **`rx_nvn_invalidate` is not this job's problem.** Of 2.39 ms of CPU under it (module workers 1.89, main 0.50), this
   job holds 0.015-0.03 ms. 1.93 ms is the module job `0x8007c120` (`0x8034f440`, `0x80354060`, `0x80357060`). On any
   chain: 0.32 ms (J 0.06, F 0.25).
8. **The game's invalidate sites were undercounted**: `memory-boundary.md` counts 36 sites in 12 functions through the GOT;
   18 more functions load the proc global `0x84d67398` (`nvnMemoryPoolInvalidateMappedRange`) directly, among them every
   function under the sampled invalidates (and `0x84d67390`, FlushMappedRange, is loaded directly by 58).
9. The related rows of the profile are **siblings in the same runner, not callees**: `0x8009bb80` and `0x808226e0` are
   under the culling job `0x800998a0`; `0x8008fc00` is 93% under `0x80084de0`, 7% here (146 of ~2,050 samples).
10. **First unit to answer natively: the pass callback `0x806602e0`** (with `0x80660320` and below), installable through
    `a64_lookup_replaced` with no new lift. The whole job needs the lift's hook list (three direct callers).

## 1. Shape of the job

### How it is reached

| What | Read |
|---|---|
| Direct callers | `0x8007fe60` (the job-group runner, call at `0x80081830`), `0x80083580` (283 lines), `0x80083960` (1,534 lines). No pointer to it is formed anywhere: **it is not reachable through `a64_lookup_replaced`** |
| Sampled parents | module workers: thread loop `0x80df2680` -> `0x8007fe10` -> `0x8007fe60` -> job (2.08 ms); main thread: `0x802eacc0` -> `0x80070b20` -> `0x8007fe60` -> job (0.67 ms). Both through the runner; `0x80083580` / `0x80083960` were not seen in the samples |
| The runner's protocol (`0x8007fe60`, 3,338 instructions) | a work list on an object (call it R): an atomic counter at R+1248 hands out indices below the count at R+1272 into the pointer array at R+1280. Each item: +0 vtable (slot +16 checked against a known implementation, else the runner goes elsewhere), +16 kind, +24 the pass object, +32 the target object, +44 flags (bit 3 set when taken). **Kind 1** -> `0x8007d2e0`, **kind 2** -> `0x8006cd60`, **kind 3** -> `0x8007b520`. When the list is exhausted the runner moves to a second list (counter R+1244, count R+1256). Who builds the lists: not read |
| Arguments of `0x8006cd60` | x0 = item+32 (target object), x1 = item+24 (pass object), w2 = `0xffff + 0x102 * pass[+148]` (meaning not read) |
| Sister bodies | kind 3 `0x8007b520` (764 instructions) makes the same NVN command-buffer calls (17 sites, same procs) and is another recording item; kind 1 `0x8007d2e0` (456) calls the same pass body `0x8006e160` with no command-buffer life cycle of its own *(inference: records into a buffer it is given)*. Neither was more than 0.03 ms in the samples |
| Same runner, other cases | `0x800998a0` (culling / render list) at `0x80081268`, `0x80084de0` (render recording) at `0x8008154c`, `0x8007c120` (GPU memory invalidate) at `0x800816b0` |

### What it loops over

- **One item = one command buffer.** `0x8006cd60` (1,275 instructions) takes a 0x98-byte slot from the target object's
  command-memory manager (an atomic counter), locks the manager's mutex while it places the slot's memory in its rings,
  then Initializes an NVN command buffer object **on its own stack** (about sp+0x1b0), gives it control and command
  memory from the slot, a memory callback, begins recording, reports counters, records, ends, reads the memory used
  back into the slot, Finalizes the object and flushes the pool range it wrote.
- **Inside, a loop over sub-passes** (count 1 or more, from the pass object's flags) calls the pass body `0x8006e160`
  (515 instructions), which runs fixed phases in order: `0x80070350`, `0x8006f9c0` (twice), `0x802702e0`, `0x8006fe10`,
  **`0x80a47660`** (the main recording, 1,649 instructions), `0x8027e840`, `0x806dc280`, plus about a dozen virtual calls
  on the pass object (devirtualised against known implementations: vtable slots +32, +72, +112, +120, +128, +144, +184).
- **`0x80a47660`** records barriers (5 sites), event waits (5), calls recorded command lists into this buffer (`0x806688c0`,
  `nvnCommandBufferCallCommands`, 10 call sites), and broadcasts to registered listeners through **`0x80076cd0`**: it
  locks the global mutex `0x84eb5440`, walks the circular list headed at `0x84eb5428`, calls each node's method (a known
  thunk `0x80076e60` that tail-jumps to the node's target, or a virtual call) and unlocks after the walk. The listener
  seen in the samples is **`0x806602e0`** (13 instructions; its address is formed by `0x80eeb4b0`, called from
  `0x831dd780`): by a mode at args+28 it calls `0x80660320` (mode 0) or `0x8065fe30` (mode 1). Sibling listeners
  `0x80076580` and `0x80076c10` (also pointer-only) run under `0x8027e840` and `0x80070350`.

### Call tree with sampled time (CPU, all threads; inclusive within the 16-frame stacks)

| Function (guest instructions) | inclusive ms | what is under it |
|---|---:|---|
| **`0x8006cd60`** (1,275) | **3.05** (MW 2.31, main 0.73) | self ~0.02; `n_nvnCommandBufferEndRecording` **0.22** (the host's flatten copy); AddControlMemory 0.02 |
| `0x8006e160` (515) | 2.78 | phases below |
| `0x80a47660` (1,649) | 2.22 | |
| `0x80076cd0` -> `0x806602e0` -> **`0x80660320`** (98 / 13 / 3,428) | **1.91** | self ~0.07, the rest below |
| `0x80364080` (649): slot scan | **0.90** | `LockMutex` 0.83 (of which `obj_lookup` 0.69, `mutex_lock` 0.08, `obj` 0.04), `mutex_give` 0.02, Unlock 0.02, `rx_nvn_invalidate` 0.014, guest 0.02 |
| `0x80255be0` (5,961): draws | 0.34 | `0x80681320` (2,206; render state) 0.15, `0x802640a0` (707; viewport / scissor) 0.12, self 0.04 |
| `0x8028ca00`, `0x8028b110`, `0x8027a310`, `0x8066bec0`, `0x802867f0`, `0x80664210`, `0x80299510`, `0x802adf20` and ~10 more | 0.06 each and less | barriers, small draws (`0x8066bec0`: 4 DrawArrays sites), container helpers (`0x8005ed00`, `0x8028a2a0`, `0x8027bfe0`) |
| `0x806688c0` (445): CallCommands | 0.15 (+0.04 through `0x806685b0`, `0x80668fc0`) | host `cmd_reserve_rec` 0.07, `inline_recording` 0.06 (the host copies the called recording in) |
| `0x8067d7a0`, `0x806790d0` | 0.03, 0.02 | |
| `0x8027e840` (668) | 0.27 | `0x80076580` listener -> `0x802740c0` 0.10; `0x8027e6c0` -> `0x8026a9e0` 0.09; `0x8027c330` 0.04; `0x8027f550` 0.04 |
| `0x80070350` (336) | 0.13 | `0x80076c10` listener -> `0x802771e0` 0.09 |
| `0x802702e0` (263) | 0.06 | |
| `0x8028f420`, `0x8047c660` | 0.04 each | both end in FP leaves `0x8156a8c0` / `0x815466c0` (through `0x8379bc90`, `0x802f3ae0`, `0x802f4100`) |

Leaf kinds under the job (CPU): lifted code 1.29; `obj_lookup` 0.69; EndRecording 0.22; `cmd_reserve_rec` 0.13;
`inline_recording` 0.09; `mutex_lock` 0.09; `obj` 0.06; `check_handle` 0.05; indirect-call and jump sites 0.09;
`tex_copy` 0.05 and `n_nvnTextureInitialize` 0.01 (textures made while recording); `free_dead_copies` 0.04.

**Kernels and sprawl.** One loop kernel: `0x80364080` (its cost is the host's lock lookup, not its own code). Small
leaves: `0x802640a0`, `0x8027bfe0`, `0x8028a2a0`, `0x8027c880` -> `0x81261320`, the FP leaves `0x8156a8c0` and
`0x815466c0`. Sprawling: `0x80660320` (150 direct callees, 39 indirect calls), `0x80255be0` (116 / 136), `0x80681320`
(11 / 99, 87 NVN sites), `0x80a47660` (95 / 37). No numeric kernel holds a large share: the job is call-heavy recording
logic.

**What the 16-frame limit hides.** 90% of the job's samples reach 16 frames. The tree above holds only samples whose
16 frames still contain `0x8006cd60`; 0.72 ms more contains `0x8006e160` or its callees without it (0.69 ms of it at the
16-frame cut). Part of that is this job seen too deep, part the sister kinds (`0x8007d2e0`, `0x8007b520`: 0.015 ms seen
with their root) and a few other callers of `0x8006e160` / `0x80a47660` (`0x835631b0`, `0x8006f8b0`, `0x83613900`).
Inclusive times of the outer functions above are lower bounds; the job's CPU is 3.05-3.7 ms.

## 2. Inputs

What the job reads, as far as read. **Who writes these structures, and when, was not read**, except the timing below.

| Structure | Offsets read |
|---|---|
| Work item (runner list R+1280) | +0 vtable, +16 kind, +24 pass object, +32 target object, +44 flags |
| Pass object (x1) | +0 vtable (devirtualisation checks at slots +32, +72, +112, +120, +128, +144, +184), +80 (a sub-structure passed down), +120 / +128 (a pair; +120 empty -> a default from a global), +136 u16 (mask 0x8004), +138 u16 (mask 0x21), +146 u8, +148 u32 (an index; also in w2), +184 and +552 (read by `0x8006e160`), +506 u8, +520, +528, +536, +864 (pointers), +2176 u32, +2184 |
| Target object (x0) | +8 u8, +16 pointer, +24 (sub-structure passed down), +1944 pointer to the command-memory manager, +2000 u32 flags (bits 5, 11, 15 tested), +2001 u8 (bit 3) |
| Command-memory manager | +8 mutex (held while a slot's rings are placed), +184 atomic slot counter, +192 count and +200 array of 40-byte ring descriptors read four at a time (+0 limit, +8 base, +36 size), +208, +216 slot array (0x98-byte slots), +225 / +228 / +232 / +233 / +234 (sizes and flags), +320 pointer to three rings of 40-byte entries (atomic counters at +24 / +48 / +72, divisors at +8 / +32 / +56, bases at +16 / +40 / +64) for the counter-report targets |
| Slot (0x98 bytes, written by the job) | +8 command memory, +16 its size, +24/+32, +40, +44 command memory used, +48, +56 the command buffer object (on the job's stack), +64 control memory, +72 its size, +76 control memory used, **+80 the command handle** (EndRecording's result), +88 flags, +96, +104, +108..+110 ring indices, +128 / +136 / +144 counter-report addresses |
| Slot table of `0x80364080` (object at x0) | +808 u8 (the scan runs only when 1), +208 slot count, +216 slot array (0x38-byte slots, a mutex at +0; +32 a 16-byte payload copied out, +44 a count, +48 a generation), +224 generation counter (compare-and-swap), +236 scan start, +240 (object with a vtable), +256 / +264 / +272 / +288 / +304 / +312 / +314, +528 / +536.. (sizes), +672 |
| Globals | the GOT targets `0x84e9f7ac` (a 32-bit slot passed to the SDK call `0x851095f0`, *(inference: a TLS slot)*), `0x84ea1b80`, `0x84ea1bd0`, `0x84ea1c80`, `0x84ea2d50`, `0x84e9fca8`, `0x84e9fcb8`, `0x84e9f6b8`, `0x84e9f6c8`, `0x84c17248`, `0x84c1aa38`; the memory callback pointer at `0x84cb4688`; the listener list `0x84eb5428` and its mutex `0x84eb5440`; the device-wide atomic at +3356 of the object at GOT `0x84cadc98`'s target (incremented at Initialize, decremented at Finalize) |

**When in the frame** (sample instants against the presents, deciles, three runs): the culling job `0x800998a0` runs 9-14 ms
after a present (the end of the frame before); `0x80084de0` at 11-15 ms and 0-1.5 ms; **this job 0-4.4 ms after the
present** (80% of its samples), together with `0x8007c120` (0.2-4.4 ms). *(Inference: it records the render lists the
culling job built and `0x80084de0` prepared; the item lists themselves were not traced to their writers.)*

**Read in place by gameplay**: not established. Nothing here shows gameplay functions reading these objects; it was not
searched.

## 3. Outputs

### NVN procedures (through the proc globals, call sites in the lifted code)

The game resolves its procs in `0x812be290` into globals at `0x84d66f00`-`0x84d685f0` and calls them either through a GOT
entry or by loading the global directly (the starred rows: missed by the GOT-only count of `memory-boundary.md`).

| Function | Procs (sites) |
|---|---|
| `0x8006cd60` | CommandBuffer Initialize, AddControlMemory, AddCommandMemory, SetMemoryCallback, SetMemoryCallbackData, BeginRecording, SetRecordingMode, ReportCounter (3), EndRecording, GetControlMemoryUsed, GetCommandMemoryUsed, Finalize, SetCommandMemoryCallbackEnabled; MemoryPool GetFlags, FlushMappedRange |
| `0x80a47660` | Barrier (5), WaitEvent* (5) |
| `0x80660320` | WaitEvent (10), SignalEvent (9), ShadingRateImagePalette (3), BindShadingRateImage, Barrier (2), SetScissor, SetViewport, SetDepthRange, MemoryPool GetFlags, FlushMappedRange |
| `0x80364080` | DispatchCompute; MemoryPool GetFlags (2), FlushMappedRange* (2), InvalidateMappedRange* (1) |
| `0x80255be0` | BindUniformBuffer (30), BindTexture (22), DeviceGetTextureHandle (3), BufferGetAddress (3), CallCommands (2), Barrier (2), SetScissor / SetViewport / SetDepthRange (2 each), BindProgram, Buffer builder and Initialize / Finalize, BindVertexAttribState, BindVertexStreamState, BindVertexBuffer, DrawElements, MemoryPool GetFlags, FlushMappedRange |
| `0x80681320` | 87 sites of render state: channel mask (16), blend, color, depth-stencil, polygon state setters and binds, stencil values, ReportCounter (4), ResetCounter (2), SetRenderEnable (2), line width, point size, geometry processing mode |
| `0x806688c0` | CallCommands (2) |
| `0x8066bec0` | Barrier (8), DrawArrays (4) |
| `0x8028ca00`, `0x8028b110`, `0x8027a310` | Barrier (4, 2, 1) |

**How many calls a frame**: not counted for this job. What exists: ~150-200 recordings (EndRecording) a frame and
1,700-2,050 draws a frame at Hyrule Field, an estimate of 15-25 k NVN calls a frame (`docs/research/suspects/C-nvn-capture.md`),
~2,450-3,050 draws a frame at the standing Market (`engine/docs/perf-audit.md`). By time, this job's EndRecording is 0.22
of the game's 0.66 ms (`0x80084de0` 0.29, the main thread's `0x802eb020` 0.11).

**Into which command buffers, on which threads**: one command buffer per call, its object on the job's stack, its memory
from the slot's rings; on the main thread (756 + 436 samples) and module workers 1, 3, 4, 5 (MW2 77 samples). The
resulting handle goes to slot+80, the memory used to slot+44 / +76. *(Inference: the main thread calls these handles
into its own buffers later: the main-only CallCommands under `0x807cd380` and the recording walk `0x802eb020` are the
candidates; not traced.)* Inside the job, `0x806688c0` calls other recordings into this buffer.

**Other writes later code reads**: the slot fields above; the slot table generation (+224) and scan start (+236) of
`0x80364080`; the device-wide live count (+3356); the pass object's flag bits; counter reports (GPU writes, *(inference:
GPU timing or statistics)*). The host writes a copy mark into control memory at EndRecording (`nvn_native.c`).

### What the host does with them (`engine/host/nvn_native.c`)

Every NVN call is a host call through a cached pointer (no per-call lock). Per recording: `BeginRecording` resets the host
segment; `EndRecording` **copies the whole recording** (`flatten`) and registers it under the exclusive `copy_lock`;
`CallCommands` **copies the called recording again** into the caller (`inline_recording`, after `check_handle` under the
shared `copy_lock`). `AddControlMemory` sweeps copies whose control memory is reused. Under this job these are 0.22 +
0.13 + 0.09 + 0.05 + 0.04 ms.

## 4. Locks and waits

### The mutexes

| Mutex | Where | Acquisitions | Cost seen |
|---|---|---|---|
| One per slot of the table at object+216 (0x38-byte slots) | `0x80364080`: a loop over every slot (count at +208, from +236 round), lock, compare the slot's count and generation, copy its 16-byte payload when they match (2,000 at most), unlock; then records a compute dispatch over what it collected | the slot count, each call; calls a frame not read | **0.83 ms in LockMutex**, 0.69 of it `obj_lookup` with the leaf in `obj_lookup` itself (the probe of the shared table, no system lock below it), 0.08 `mutex_lock`; Unlock 0.03 |
| `0x84eb5440` (global) | `0x80076cd0`: held while every listener runs, so **`0x80660320` runs under it** | once per broadcast | no wait sampled |
| Command-memory manager +8 | `0x8006cd60`, while a slot is placed | once per call | small |
| Others in `0x80a47660` (three lock sites) and `0x8005ed00` (container helper, many callers) | | | small |

**Contention**: none worth naming. Samples waiting under the whole job: 0.007 ms. The cost is the host's lookup of a guest
mutex: `LockMutex` misses the 1024-entry per-thread cache of `obj()` (`engine/host/nn_os_sync.c`) and probes the shared
table every time, while the `UnlockMutex` that follows hits it (0.03 ms). *(Inference: the scan touches more mutexes per
thread than the direct-mapped cache keeps between two scans; at an assumed 0.1-0.3 us a miss, 0.69 ms is a few thousand
locks a frame. Not counted.)* This is 66% of all `obj_lookup` time of the game (1.04 ms; executors 0.29).

### Every path into `rx_nvn_invalidate`

The only route is `nvnMemoryPoolInvalidateMappedRange` / `nvnBufferInvalidateMappedRange` (`nvn_native.c`), called by
the game's "flush or invalidate if the pool is CPU-cached" idiom: get the pool's flags, and with the CPU-cached bit set,
invalidate the range before reading GPU results. `rx_backend.c` gives the virtual core away and calls the Vulkan back
end's `invalidate` (`gpu/vk/vk_replay.cpp`): with the readback ticket it first waits for the replay section in progress;
for ranges that have write-backs it waits until the submission that writes them back is recorded and finished on the
GPU timeline; for ranges a texture the GPU wrote may be in, it catches the replay up.

| Caller (job) | CPU ms | chain ms | what it reads |
|---|---:|---:|---|
| `0x8034f440`, `0x80354060`, `0x80357060` (module job `0x8007c120`) | 1.93 (MW 1.47, main 0.46) | F 0.23, J 0.06 | not read; same function family (`0x8034xxxx`-`0x8036xxxx`) as `0x80364080`, whose compute dispatch and slot table *(inference: GPU-computed results read back one frame later)* |
| `0x807068e0` (under `0x80706470` / `0x80706610`; seen under kind 3 `0x8007b520`) | 0.24 | 0.01 | not read |
| `0x80365f00`, `0x80362ff0` (culling job `0x800998a0`) | 0.14 | 0.02 | not read |
| `0x80364080` (**this job**) | 0.015-0.03 | ~0 | the range its compute dispatch covers |
| `0x809f43c0` | 0.02 | 0 | |

Two back-end paths hold nearly all of it: a wait inside the driver (`nvoglv64.dll` -> kernel wait, 1.18 ms; *(inference:
the semaphore wait for the write-back value)*) and a wait inside `hookshot_vk.dll` ending in a kernel wait (0.78 ms;
*(inference: the readback ticket's wait for the replay section)*). The sampler cannot tell the two apart further (no
symbols for `hookshot_vk.dll` in the map).

**Plainly**: the GPU write-back wait is **not on this job's path** and is **mostly off the job-group chain** (0.06 ms J,
0.25 ms F, of 2.39 ms CPU). What would remove it safely belongs to `0x8007c120`, not to `hl::gfx`'s first unit: read what
those three functions read back and when it was written; if the game reads the results of a frame it already waited for
(the console's coherent memory makes that free), serve the read from the last completed write-back instead of waiting for
work in flight. The exposure meter case of the back end (`vk_replay.cpp`, "EvilBoyce") shows why this needs that reading
first: a stale read there made visible flashes.

## 5. Why it is on the chain

- **The main thread's job-group wait is in the same runner.** The main thread runs `0x8007fe60` itself (through
  `0x80070b20`), takes items like a worker, and when the lists are empty waits (lr `0x80080188`) for the items the workers
  still run. When this job is the bound, the main thread waits for the module worker that is recording this command
  buffer.
- **The job is a serial stretch.** It runs on one thread at a time in 94-95% of its samples (section "Method"); the
  1.91 ms pass runs under the global listener mutex; `0x80a47660` was never seen on two threads. Whether that is one item
  a frame or a few items that cannot overlap was not established *(inference: one kind-2 item carries the scene pass)*.
  A serial ~3 ms is longer than any of the parallel module jobs' share per thread, so it is the tail of its phase.
- **Its chain time**: J 0.634 ms (r1 0.69, r2 0.61, r3 0.61), all on module workers; F 0.669 ms, 0.635 of it the main
  thread running the job itself. On the J side: lifted code 0.31, host recording (`cmd_reserve_rec` 0.06, EndRecording
  0.06, `inline_recording` 0.04, `check_handle` 0.02) 0.18, `obj_lookup` + `obj` 0.05. Largest J paths: CallCommands
  `0x806688c0` 0.09, the job's own code 0.06, `0x80364080` 0.04, `0x80681320` 0.04.
- **Predecessors** *(inference from timing, not from the runner's dependency data, which was not read)*: the culling /
  render-list job `0x800998a0` and the recording job `0x80084de0` end before it in the frame; `0x8007c120` runs beside it.

## 6. Replacement plan

### The units

| Unit | Holds (CPU / chain) | Installable how | Boundary |
|---|---|---|---|
| **A. The pass callback `0x806602e0`** (-> `0x80660320`, `0x80364080`, `0x80255be0`, `0x80681320`, ...) | 1.91 of 3.05 ms; chain J 0.26, F 0.44 (0.70 of the job's 1.27) | **`a64_lookup_replaced`**: no direct caller; its address is formed only by `0x80eeb4b0` and it is reached through the listener list (`0x80076cd0`, itself pointer-only, tail-jumps to it through `0x80076e60`) | in: x0 the listener object, x1 the arguments (+28 the mode; mode 1 must still go to the translated `0x8065fe30`); out: 1 in w0, NVN calls into the command buffer the caller is recording, the slot table's generation and scan start, the compute dispatch; runs under `0x84eb5440` (the caller holds it) |
| **B. The whole item body `0x8006cd60`** | 3.05-3.7 ms; chain 1.27 | the lift's hook list (direct callers `0x8007fe60`, `0x80083580`, `0x80083960`); the sister bodies `0x8007b520` / `0x8007d2e0` with it, since they share `0x8006e160` | in: the item's two objects and w2; out: the slot fields (+44, +76, +80 and the rest), the stack command buffer's life cycle, the manager's counters, the device count +3356 |
| C. `0x80364080` alone (slot scan) | 0.90 ms | hook list (two direct callers `0x80660320`, `0x831f4490`) | the slot table and its mutexes |

**Proposed first unit: A.** It is the bulk of the job, single-threaded, needs no re-lift, and has a clean entry (one
pointer-reached function with two arguments). B follows once the hook list is available and A is proven; C is subsumed by
A.

What A must keep:
- **Guest calls it must still make** (crossing 2): `0x80660320` makes 39 indirect calls and 150 direct ones; the
  container and object helpers (`0x8005ed00`: 107 callers; `0x802737a0`: 160 callers) and the virtual calls on scene
  objects stay translated until read. A native A calls them on the guest context it runs on, and becomes native piece
  by piece below (render state `0x80681320` and the draw loop `0x80255be0` first: they are the NVN-heavy parts).
- **Structures in place** (crossing 3): the slot table of `0x80364080` and its mutexes are shared with whoever fills the
  slots (writers not read). Until the writers are native, the module **locks the same guest mutexes through the OS
  layer's object** but keeps its own array of resolved host mutex objects for the slots (the address-to-object lookup
  once, not per lock). That alone removes the 0.69 ms of `obj_lookup` without changing who holds what.
- **The NVN boundary**: phase 1 calls the host's NVN handlers in-process (same calls, same order, same arguments as the
  translation), so the back end sees the same stream. Phase 2 records the back end's records directly and removes the
  double copy (EndRecording's flatten, CallCommands' inline copy): ~0.35 ms of this job's CPU, ~0.12 ms of its chain J.
- **Threads**: it runs on whichever thread took the item (main thread or a module worker), under that thread's guest
  identity; it holds `0x84eb5440` for its whole run (the caller took it).

### Checking it against the translated pass

1. **Same NVN stream.** A switch in the NVN layer that logs, for one frame window and per command buffer, the sequence of
   (procedure, arguments) with guest pointers into the slot's command and control memory rebased to the slot, and
   counter-report targets masked. Run castle-move with the translated pass and with the native one (`HL_GFX=0/1`, one
   address answered or not) on the same input script, and compare the sequences per pass index (pass +148) and frame.
   Equal sequences are the acceptance; differences are listed by first divergent call.
2. **Same recordings.** At EndRecording the host already holds the flattened bytes: a digest per recording (with the
   same rebasing) compared between the two runs is a cheaper second check over a longer window.
3. **Same picture and speed.** Screenshots at fixed points of the script, the flash detector, game speed (the harness's
   checks), and the chain: `critpath.py` with the sampler, the job's J and F time.
Running both in the same process is not possible for A (the slot scan advances the generation and records GPU events), so
the comparison is between runs.

### Gain, honestly

Basis: the job's chain time is 1.27 ms (J 0.63 + main F 0.64), sampled. Of the job's CPU, lifted code is 1.29 ms,
`obj_lookup` and mutex overhead 0.8 ms, host recording 0.55 ms, the rest translation runtime. Unit A is 63% of the CPU
and 55% of the chain time (J 0.26, F 0.44).

| Step | What goes | Expected chain gain |
|---|---|---|
| A, phase 1 (native logic, same NVN calls, host mutex objects cached) | `obj_lookup` 0.69 ms CPU (~0.14 of chain); A's lifted code at an assumed 2-4x | 0.25-0.40 ms |
| B, phase 1 (the whole item body and its sister kinds) | the rest of the job's lifted code and translation runtime | +0.15-0.25 ms |
| Phase 2 (direct back-end records, no double copy) | EndRecording flatten, CallCommands copy, `check_handle` | +0.1-0.15 ms |
| Recording the pass's independent parts on several threads | the serial stretch itself (not designed: needs the pass's internal dependencies read) | not estimated |

So **0.4-0.8 ms of the sampled chain for A and B in phase 1-2**, less without the sampler (its frame is ~2 ms longer).
At the ~12 ms unsampled frame that is roughly 3-6% of the frame. The 2-4x for translated logic is an assumption, not a
measurement: it is what a native rewrite of call-heavy code typically gives over this translation (the translation's
FP-exactness and indirect-call overheads, `hot-functions.md`), and should be replaced by the first measured number of A.
The chain only shortens by as much as the next bound allows: the executors' jobs and the culling job are the other
branches of the same phase.

## 7. Brick 2.1 as built: the native pass and its check

### What is native and how the game reaches it

| Piece | Where | What it does |
|---|---|---|
| `hl::gfx::RecordPass` | `engine/native/{include,src}/hl/gfx/RecordPass.*` | the listener's own level: reads the mode, runs the scene pass (mode 0) or the second pass (mode 1), nothing for any other mode, returns 1 to the walk |
| `hl::bridge::GfxBridge` | `engine/native/{include,src}/hl/bridge/GfxBridge.*` | the addresses (`0x806602e0`, `0x80660320`, `0x8065fe30`), the argument block read in place (mode at +28), the phases run through the guest runtime on the calling thread's own context |
| The seam | `hl/gfx/Commands.h` | `Command` (a procedure of the OS layer's NVN table, x0..x5, s0..s3) and `CommandSink`: the one way output leaves the module. Its only user today is the check's recording; a native part will emit into it (NVN first, the Vulkan back end later) |
| The check | `hl/gfx/PassCheck.h` | below |
| C surface, OS layer | `hl/c/hl_gfx.h`, `engine/host/hl_gfx.c`; a hook in `nvn_native.c` (`nvn_native_proc`) | `HL_ANSWER(0x806602e0)`: no lifted code changes. The phases are the translated `0x80660320` / `0x8065fe30`, called with the listener's two arguments (both read x0 and x1 only, read in the lifted text) |

Switches: `HL_GFX=1` the native pass (the default since its validation; `HL_GFX=0`: the answer is not installed, the translation runs as before),
`HL_GFX_CHECK=1` the check (`=2` its control), `HL_GFX_LOG=1` the counters every 10 s.

### The check (step 1)

With `HL_GFX_CHECK=1` the OS layer hands the game a tap instead of each NVN procedure (decided when the game asks for its
first procedure; without the switch the procedures are untouched). While a pass runs on a thread, the tap adds every
NVN call of that thread to the pass's recording (procedure, x0..x5, s0..s3). Each pass takes the native or the
translated path at random (a fixed pseudo-random sequence, so passes that come in a fixed order in a frame are seen on
both paths). The check compares, inside one process, scene and minutes:

- the **shapes** of the streams (the sequence of procedures, by mode): how many passes of each path recorded a shape the
  other path never recorded;
- the **calls a pass of each procedure** called at least once a pass: the largest relative difference between paths.

What it proves: the native path records the same kinds of stream as the translated one, at the same rates, procedure by
procedure. What it does not prove: that a given pass would have recorded the same arguments on the other path (a pass
cannot be run twice: it advances the slot table's generation and records GPU events), nor anything about the pass's
writes to the game's own structures. For the shell this is enough, since the shell calls the same translated phases
with the same registers; for a native hot part (step 3) it is not: that needs a call-by-call comparison inside one pass,
the native part run first into a recording sink with its writes kept aside, then the translated part for real under the
tap (an entry gate can run its translated body itself), comparing calls with their arguments and the bytes written.

`HL_GFX_CHECK=2` is the check's control: the passes drawn for the native path run the translated pass too, so what it
shows between the two paths is the check's own noise. Measured on the main PC (110 s runs, all passes mode 0; mode 1 was
never seen in these scenes):

| Run | Scene | Passes (translated / native) | NVN calls a pass (translated / native) | Unmatched passes (translated / native) | Largest difference of a procedure's calls a pass (of 91 compared) |
|---|---|---|---|---|---|
| `HL_GFX=1 HL_GFX_CHECK=1` | kokiri-idle | 14,668 / 14,803 | 1,578 / 1,628 | 17.9% / 18.7% | 5.8% (`DrawElementsInstanced`) |
| | castle-move | 13,553 / 13,678 | 1,304 / 1,345 | 18.9% / 19.7% | 5.8% (`DrawElementsInstanced`) |
| | hyrule-walk | 17,317 / 17,324 | 1,473 / 1,501 | 18.5% / 19.0% | 4.4% (`DrawElementsInstanced`) |
| control, `HL_GFX_CHECK=2` | kokiri-idle | 15,658 / 15,742 | 1,597 / 1,635 | 17.6% / 18.2% | 4.7% (`DrawElementsInstanced`) |
| | castle-move | 17,616 / 17,595 | 1,411 / 1,437 | 18.2% / 18.8% | 4.6% (`DrawElementsInstanced`) |

Read: shapes vary from pass to pass even standing still (about one pass in six has a shape seen nowhere else), and the
fixed sequence of draws gives the "native" side a few percent more calls a pass and one huge pass (~41,000 calls, at
the start of a scene) in every run, the control included. The native shell's differences are of the control's size:
the check sees no difference between the shell and the translated listener. It would see a native part that changes
which procedures a pass calls, or how many, by more than ~5% for any procedure called once a pass or more; smaller or
argument-only differences need the call-by-call check described above.

### Validation (main PC, `build/s5/valid.sh`, 110 s a scene)

| Run | Scene | Ran | Game speed ratio | Native passes | Shots |
|---|---|---|---|---|---|
| `HL_GFX=1` | kokiri-idle | to the end, no crash | 59.74 | 34,731 | right (compared with `refshots-kokiri-idle`) |
| | castle-move | to the end, no crash | 59.71 | 35,531 | right |
| | hyrule-walk | to the end, no crash | 62.96 | 35,751 | right |
| `HL_GFX=1 HL_GFX_CHECK=1` | the three | to the end, no crash | 59.59 / 59.97 / 59.90 | half of the passes | right |

Memory with `HL_GFX=1` (game RAM / GPU pools / process RAM / commit / VRAM, MB): kokiri-idle 7,780 / 4,143 / 13,455 /
19,759 / 5,732; castle-move 7,816 / 4,155 / 13,814 / 21,451 / 6,812; hyrule-walk 7,818 / 4,171 / 13,666 / 20,654 /
6,274. The shell changes no memory: these are the game's figures.

### Where the pass's time is, and what step 3 should take first

Re-aggregated from `build/hotfn` (castle-move, three sampled runs, ms of CPU a frame under `0x80660320`, total 1.92):

| Under `0x80660320` | ms | Note |
|---|---:|---|
| slot scan `0x80364080` | 0.92 | 0.83 of it `LockMutex`, 0.69 of that `obj_lookup`: gone since the OS layer's mutex fix, ~0.2 ms left, nearly all host lock calls |
| draw loop `0x80255be0` | 0.34 | render state `0x80681320` 0.15 (guest 0.074, `check_handle` 0.03), viewport / scissor `0x802640a0` 0.12 (guest 0.097), own 0.04 |
| ~30 other callees | 0.01-0.06 each | barriers and small draws (`0x8028ca00` 0.06, `0x8028b110` 0.06, `0x8027a310` 0.06, `0x8066bec0` 0.05, `0x802867f0` 0.04, ...), most of their time in shared container helpers (`0x8005ed00`, `0x8028a2a0`, `0x8027bfe0`) |
| own code | 0.07 | |

Leaf kinds: guest code 0.74, host lock lookup 0.69 (now gone), the rest host NVN recording and runtime. After the mutex
fix the pass is about 1.2 ms and has no dominant part: the draw loop's two leaves are the largest native candidates.
Recommendation for step 3: **render state `0x80681320` first** (the NVN-heavy part: 87 of its 99 indirect calls are NVN
procedures, so it is the seam's natural first producer), behind an entry gate (5 call sites in 4 functions) and the
call-by-call check above; before writing it, read its callee `0x80078fc0` (3,159 lifted lines, 21 callees: what it is
decides whether the unit is small) and the two counters around it (`0x80078b10` / `0x80077d60`, 250+ callers each).
Then viewport / scissor `0x802640a0`. The slot scan is not worth a native rewrite for time now; it is for the mutex the
pass runs under (section 4).

## 8. Brick 2.2: the render-state function native, the NVN table, the exact check

### Reading: the draw function `0x80681320` and what is around it

**`0x80681320` is the draw function of one draw, not a render-state function.** (2,620 lifted lines; called from the
draw loop `0x80255be0`, `0x806688c0`, `0x8028a2a0` and `0x806811d0`.) Its arguments: x1 the recording context (its +216
is the NVN command buffer), x2 the draw's parameters (+24 a 64-bit flag word, +0..+56 counts and indices, +120..+132
floats), x3 the material (+8..+14 bytes and flags, +1096 / +1104 a table of 24-byte entries, +1216, +1224 polygon offsets
by slot, +1360..+1380 floats, +1384 a state callback object), x4..x7 more of the draw. It builds a **352-byte render-state
block on its own stack (sp+80)** from the draw and the material, records line width and point size, then chooses how to
draw by a key made of flag bits:

- a key other than 0, 64 and 128: one of **about 130 per-variant draw functions** through a table at `0x84c29878` (90-250
  lifted lines each, template instances: `0x80517700`, `0x835c3e90`, ...). Each loops over the draw's sub-meshes; for a
  sub-mesh whose state key differs from block+90 it calls **the render-state function `0x80077d60`**, for one whose
  render-enable key differs from block+91 the render-enable function `0x80078b10`, and draws through virtual calls on
  the sub-mesh's objects;
- keys 0, 64 and 128: its own loops over the sub-meshes, with **the render-state function's calls inlined** (the same 36
  NVN call sites, twice: 72 of its 87) and `0x80078fc0` to submit each sub-mesh.

Its other NVN sites: ReportCounter 4, ResetCounter 2, SetRenderEnable 2, BufferGetAddress 2, SetLineWidth, SetPointSize,
SetGeometryProcessingMode. Its guest calls are interleaved with its NVN calls: an SDK call `0x851095f0` twice at the start
(*(inference)* a thread-local read), the variant table, `0x80078fc0` per sub-mesh, virtual calls on scene objects, the
material's state callback, and after a variant returns it reads block fields the variant wrote (+139, +272, +280).

**Why it is not this brick's unit.** The exact check (step 3 of the brief) runs the native version first into a recording
with no NVN procedure called and nothing written; a function whose NVN calls depend on what guest callees did in between
cannot be run that way, and its draws are made by guest code (the variants, `0x80078fc0`) that would have to stay
translated inside it. It can be made native only in pieces between its guest calls. Its callee **`0x80078fc0`** (3,159
lifted lines, 21 callees) is the per-sub-mesh submission: CallCommands, SetGeometryProcessingMode, DrawMeshTasks, and mesh
preparation shared with the recording job `0x80084de0` (`0x8008fc00`, `0x8002a5f0`, `0x8008f840`, `0x800a8120`, ...): a
boundary to call back into, not part of a state unit.

**The unit taken: `0x80077d60`, the render-state function itself** (526 lifted lines, no direct callee, 36 NVN call sites
of 29 procedures, 254 callers with one site each: `0x80681320` and the per-variant draws). It is what the brief's "render
state" is: its 36 calls are the render-state calls `0x80681320` inlines, it has no guest callee in the case the game takes
(below), so it can be cut cleanly and checked exactly. Viewport / scissor `0x802640a0` was the fallback; it has 18 direct
callees (two SDK calls, `0x8005ed00`, `0x8025dae0` of 559 lines) around its 3 NVN calls, so it would not have been cleaner.
Its neighbour **`0x80078b10`** (146 lines, 256 callers) is the render-enable function: it resets block fields, and calls
SetRenderEnable or, when a global switch is set, writes the method into the command memory itself. Not taken (one NVN
call).

### The render-state function's inputs

It takes x0 the block and w1 the sub-mesh's state key (its low byte), returns nothing, and keeps its five NVN state
objects in its own 176-byte stack frame (blend at frame+8, polygon +60, depth stencil +64, color +72, channel mask +76).

| Block field | Read / written | What it is (as far as read) |
|---|---|---|
| +0 u64 flags | read | bit 2 forces depth writes, 3 stops them, 2-3 clear mode bit 1, 4 depth clamp, 5 depth test off (depth func always), 6 reversed depth (greater / greater-or-equal instead of less / less-or-equal), 12-16 blending on, **17 record nothing** (derive and write only), 18 mode bit 0 off |
| +24 material | read | +1224 + 8 x slot: polygon offset units and factor by slot (any 32-bit slot, signed); +1384 the state callback object |
| +40 recording context | read | +216 the command buffer (read only when something is recorded) |
| +57 depth mode | read | 4: not-equal, no writes; 5: writes on, the function by the channel masks; 8: the offset slot from +100 |
| +88 offset slot | read, written | the polygon offset slot applied last: the offsets are read from the material only when it changes |
| +90 key | written | the state key applied last: the per-variant draws skip the call when it is the sub-mesh's |
| +92 mode bits | read, written | bits 0 and 1 rewritten from the flags and the key |
| +97 | read | the targets blending is enabled on, when the flags or the key (bits 1, 2, 6) turn blending on |
| +100 | read | depth mode 8: the slot is this + 4 |
| +112 channel masks | read | 4 bits a color target, 8 targets; 0 also selects greater / less as depth function |
| +152 | written | all ones, every call |
| +160..+175 blend color | read | copied to +248 (the blend color call reads it there) |
| +240, +241, +340, +350 | written | depth test, depth write, depth function, depth clamp |
| +243, +272, +276, +343..+346 | read | stencil test, reference, value mask, operations, function |
| +244 | written | the blend targets enabled |
| +268 | written | the channel masks (a copy of +112) |
| +280..+327 | read | 8 targets x 6 bytes: blend factors and equations |
| +328..+339 | read, written | polygon offset factor, units, clamp (rewritten when the slot changes, clamp 0) |
| +341, +347, +349 | read | cull face, polygon mode, polygon offset enable (bit 0) |

**Who writes the block**: `0x80681320` builds it on its stack for one draw; the per-variant draws, this function and
`0x80078b10` update it between sub-meshes. **What it caches**: block+90 (state key), +88 (offset slot) and +91
(render-enable key), for the sub-meshes of one draw only. **Nothing is cached across draws**: every call records the
whole state again, blend and color defaults, the eight channel masks, depth-stencil and polygon objects, about 33 NVN
calls (measured below: 1,471,680 calls in 44,845 calls of the function). A later native unit that keeps the last state
per command buffer could skip most of them; that is a change of the stream (fewer calls), not an exact replacement.

**The state callback**: the material's +1384 is a function object; when its invoker (+16) is not the game's empty
function (`0x835c3bb0`, through the GOT slot `0x84cb5d98`), the function calls it, and possibly a member function
through material +1392..+1408, before recording: guest code whose result decides what follows. The native version
leaves those calls to the translation (the gate answers 0). In the three scenes it never happened (counts below).

### What is built

| Piece | Where | What it does |
|---|---|---|
| `hl::gfx::RenderState` | `engine/native/{include,src}/hl/gfx/RenderState.*` | `decide()`: the function's derivations, pure (StateInputs to StateOutputs: depth, blend targets, mode bits, offset slot); `emit()`: its NVN calls, in its order with its arguments, into a CommandSink |
| The NVN table and its sink | `hl/gfx/Nvn.h`, `src/gfx/Nvn.cpp` | `Proc`: the 29 procedures by NVN name, with each one's argument count (ints, floats); `NvnEntryPoints` (the OS layer's call); **`NvnSink`**: a CommandSink that calls each command's procedure. The seam's first real use, and still its only way out |
| The call-by-call check | `hl/gfx/StateCheck.h`, `src/gfx/StateCheck.cpp` | compares one call's two lists of NVN calls entry by entry (procedure and each argument register the procedure reads, pointers as values) and the block's bytes after the call; counts, and logs the first eight mismatches in full |
| The bridge | `hl/bridge/GfxBridge.h` | `guest::RenderStateBlock` (every field above, `static_assert(offsetof)`), the material, the recording context, the callback; `kApplyRenderState`, the frame's layout, `proc_global()` (the game's global of each procedure, read from the lifted function's call sites); `StateBridge`: reads a call, writes the outputs in place (and tells the OS layer's write tracking), builds the expected bytes |
| C surface | `hl/c/hl_gfx.h`, `src/c/hl_gfx.cpp` | `hl_gfx_nvn_*` (the table's names and globals), `hl_gfx_host` (NVN call, write tracking, log), `hl_gfx_state_apply`, `hl_gfx_state_check_begin / _call / _end`, `hl_gfx_state_report` |
| OS layer | `engine/host/hl_gfx.c` | **`HL_GATE(80077d60, "31664901fd8bc782")`**; the NVN entry points: each procedure resolved by name through the NVN layer (`nvn_native_proc`) to its host function, and **checked once against the game's own global** (the same host call, or the gate turns itself off); the tap now also feeds the call-by-call check with each procedure's native number |
| Tests | `engine/native/tests/gfx_test.cpp` | the derivations (depth function and writes by flags, key and depth mode; blend targets; the slot in 32-bit arithmetic), the calls' order and arguments, the block read and written in place, the sink's argument counts, the check's comparison and log |

**How the game reaches it.** `nx2/hl_gates.py` (run by the build scripts) puts an entry gate at the head of the lifted
`f_80077d60` in `code_000.c` when its text hashes `31664901fd8bc782` (both machines' build logs: `hl_gates: f_80077d60
in code_000.c: gated`; `code_000.c` is 12.5 MB and recompiles when the gate goes in or out). The gate is on from the start;
its first call reads the switches, checks the build, resolves and checks the NVN table, and turns itself off for good when
the brick is off. Then, on the thread running the call: `HL_GFX_STATE=1` reads the block, derives, writes the block,
and calls the NVN procedures through the table on that thread's guest context, with the state objects at the same frame
addresses as the translated function; `HL_GFX_CHECK=1` runs the native version into a recording, then the translated body
itself (the gate's own call, which the gate lets through) with the tap recording, and compares.

Switches: `HL_GFX_STATE` 0 (the translated function) or 1 (native); `HL_GFX_CHECK=1` (or 2) checks instead of answering;
`HL_GFX_LOG=1` adds the line `hl gfx: render state: native calls N, left to the translation M` every 10 s.

### The check and the runs (PC 3050, 110 s a scene; function only, no speed)

| Run | Scene | Calls compared | NVN calls compared | Mismatched calls | Left to the translation (callback) |
|---|---|---:|---:|---:|---:|
| `HL_GFX_CHECK=1` | kokiri-idle | 69,757 | 2,292,361 | 0 | 0 |
| | castle-move | 44,845 | 1,471,680 | 0 | 0 |
| | hyrule-walk | 23,473 | 767,094 | 0 | 0 |
| `HL_GFX_STATE=1` | castle-move (twice) | 54,414 native calls, 0 left | | | |

Both sides are non-empty and equal call for call (an empty or partial tap would show as mismatched lengths), and the
block's 352 bytes match after every call. The native runs went to the end with no crash, and the castle shots are
right. **Not done: the validation runs on the main PC** (three scenes with `HL_GFX_STATE=1`, shots, memory table)
and native runs in kokiri-idle and hyrule-walk. The main PC's lock was held by other streams for more than two hours,
so `HL_GFX_STATE` stays off by default until those runs are done.

## 9. Brick 2.3: the sub-mesh submission `0x80078fc0`

### Reading: what it is

`0x80078fc0` (3,159 lifted lines, 1,329 guest instructions, 336-byte frame) **submits one sub-mesh of a draw**. It is a
virtual method (slot +0x58 of the drawable's vtable): its callers compare the slot with `0x80078fc0` (the GOT slot
`0x84cb5dc0`) and call it directly when it matches, else call the slot. Direct call sites: `0x80681320` (3), `0x80078490`,
`0x80078800`, `0x80078ce0` (the draw function and three of its siblings) and the recording job `0x80084de0` (1).
Arguments and result:

| Register | What |
|---|---|
| x0 | the drawable D (the object whose vtable is checked) |
| x1 | the render-state block B (the 352-byte block of section 8, on the caller's stack) |
| w2 | the sub-mesh index i (signed) |
| w3 | a mask M of draw kinds, ANDed with D's flags |
| result | none |

**What it reads** (offsets in bytes; "read" means read in the lifted code; who writes D and R was not read: they are
the scene's drawable and its model resource):

| Structure | Field | What |
|---|---|---|
| D | +0x1e u16 | bits 4-7: the number of sub-meshes (i at or past it: the function returns at once) |
| | +0x54 u32 | the draw-kind flags F (bits 0, 3, 4, 5, 6, 7 select paths, with M) |
| | +0x5c u16 | the instance count n |
| | +0x68 | the model resource R |
| | +0x78 | a table of 24-byte cluster entries (meshlet paths) |
| | +0x80 | an object handed to the indirect and query paths |
| R | +0x8 -> +0x18 | the sub-mesh descriptors: 56 bytes each (below) |
| | +0x638 | the sub-mesh records: 0x270 bytes each (S below) |
| | +0x69e u8 | the geometry processing mode the model needs |
| | +0x69f u8 | the mesh kind: 0 indexed draw, 1 and 2 mesh-shader tasks, other: no draw |
| | +0x6a4 u32 | the mesh-shader task count (kind 1), or its thread count (kind 2: rounded up to 32s) |
| Descriptor (56 bytes) | +0x0 | the index ranges: 8-byte entries (+0 byte offset, +4 count) |
| | +0x10 | the index buffer's holder (+8 its NVN buffer) |
| | +0x24, +0x28, +0x30 u32 | primitive, index type (also the index size's log2), base vertex |
| | +0x34 u16 | the number of index ranges |
| S (0x270 bytes) | +0x50 | a pre-recorded command handle (the CallCommands path) |
| | +0x58 u8 | bit 0: the sub-mesh has a pre-recorded handle |
| | +0x268 u32 | its triangle count (added to the block's counter) |
| B | +0x28 | the recording context (its +0xd8 is the NVN command buffer, its +0x8 the draw helper's view of it) |
| | +0x38 u8, +0x63 u8, +0xe8 | the cluster entry's index, a culling slot, the culling context (meshlet paths) |
| | +0x3b u8 | the geometry processing mode applied last (written) |
| | +0x50 u32 | the triangles submitted (written: += S+0x268) |
| | +0x5c u32 (the mode bits of section 8) | bit 5: pre-recorded handles allowed; bit 14: rewritten from the geometry mode |
| Globals | `0x84ca2dd8` -> u32 | a switch: set, the CallCommands path writes the call into the command memory itself |
| | `0x83e79c60`, `0x83e7a994` | two u32 tables: the game's primitive and index-type enums to NVN's (read by `0x812c3470` / `0x812c34e0`) |

**Its order**, by path (every path is one call; K = F & M):

1. **Prologue**: return when i is not below the sub-mesh count. When R+0x69e differs from B+0x3b: B+0x3b takes it,
   **SetGeometryProcessingMode**(command buffer, mode), and B+0x5c's bit 14 becomes (mode == 1).
2. **The callback**: `0x8008fc00`(B, D, 0) (7,134 lifted lines, 263 callers, 145 NVN call sites: program, uniform and
   storage buffers, textures, the whole render state again; it writes block fields and command memory). Returns when its
   bit 0 is clear. **Shared preparation**: called back into, not part of the unit.
3. K bit 3: `0x83607fc0`(D+0x80, context, B+0x38, 0) (168 callers; a false result returns); F bit 5: `0x83607730`(D, B).
   Both guest code before the rest; F is read again after each.
4. **The direct paths** (K & 0x51 = 0): with n at most 1, S+0x58 bit 0 and B+0x5c bit 5 set: the byte at command
   buffer +0x10 is set to 1 (a mark in the game's NVN object; the preparation writes the same byte at many of its call
   sites, *(inference: a "commands recorded" flag)*), then **CallCommands**(command buffer, 1, S+0x50) (or, with the global switch, the call written
   into the command memory directly: copies through `0x8572f020`). Otherwise by R+0x69f: kind 0, the indexed draw
   `0x8029fef0` (below) with n instances (1 when n is 0); kind 1 or 2, **DrawMeshTasks**(command buffer, 0, count);
   other kinds draw nothing. Then B+0x50 += S+0x268.
5. K bit 4: the indirect draw `0x83607e80` (DrawElementsIndirect), then the counter.
6. K bit 0 or 6: the cluster (meshlet) paths: a cluster entry from D+0x78 by B+0x38; either one cluster list
   (`0x835b4c50`, `0x814e6dc0`, then `0x83607840` a range), or, with K bit 7, a loop over 32-byte cluster bounds with a
   frustum test (`0x80029240`), a hierarchical-depth occlusion test against the culling context's depth pyramid
   (B+0xe8: its matrices at +0x328..+0x354, the pyramid's levels at +0x6b0..+0x6c8, a tile grid at +0x5c0 / +0x668; with
   box projection `0x800a7f00` when +0x6df is 1), merging runs of visible clusters into one indexed draw each
   (`0x8002a5f0` + `0x8008f840`); without a culling context, an iterator `0x800a8120` hands out the runs. The counter
   gets the triangles of every run (count / 3).

**The indexed draw `0x8029fef0`** (44 lines; NVN-only, part of the unit) and its two helpers: from the descriptor, the
first range's offset and the last range's offset and count give count = last count + ((last offset - first offset) >>
index size); **BufferGetAddress**(the NVN buffer) (`0x8002a5f0`, which stores the address and a zero on the caller's
stack); `0x8008f840` maps the primitive and the index type through the two tables (`0x812c3470`, `0x812c34e0`;
`0x812c3500` is an identity) and tail-calls **DrawElementsInstanced**(context+8's +0xd0 = the command buffer, mode, type,
count, address + first offset, base vertex, base instance 0, n instances).

**NVN calls of the unit, in order**: SetGeometryProcessingMode (when the mode changes), then after the callback one of:
CallCommands; BufferGetAddress and DrawElementsInstanced; DrawMeshTasks; nothing.

**What it writes**: B+0x3b, B+0x5c (bit 14), B+0x50; the command buffer object's byte +0x10; on the stack below the
caller's sp, its own frame and the draw helper's address pair (scratch, read by nobody after the call). The cluster
paths write their own frame and the iterator's state (not in the unit).

**Callees, by kind** (21 call sites): NVN-only helpers that become the unit: `0x8029fef0`, `0x8008f840`, `0x8002a5f0` (and
`0x812c3470`, `0x812c34e0`, `0x812c3500` below them). Shared preparation called back: `0x8008fc00`. Other paths, left
to the translation: `0x83607fc0`, `0x83607730` (guest calls before the draw), `0x83607e80` (indirect draw),
`0x80029240`, `0x800a7f00`, `0x800a8120`, `0x835b4c50`, `0x814e6dc0`, `0x83607840` (clusters), `0x8572f020` (a 7-line
leaf with 13,242 call sites: the copy of the inline-write path).

### The unit taken, and how it is cut

The function cannot be native whole in one session: its cluster paths are a culling and occlusion kernel with its own
callees, and a guest call (`0x8008fc00`) sits between its first NVN call and the rest. **The unit is the function on its
direct paths**: prologue, the callback through the guest runtime, and the direct paths natively. Which path a call takes
is decided from D's flags, M and the global switch at the function's entry (none of them is written by the prologue);
a call on any other path (K bit 3, F bit 5, K & 0x51, the switch set) is left whole to the translation. The callback
could in principle change what the decision read: the check counts that case (below), and it is zero in the runs.

Native, the gate at `0x80078fc0`'s entry: decides the prologue (`hl::gfx::SubmeshDraw::prologue`) and emits it; calls
`0x8008fc00` on the thread's own guest context with the stack pointer lowered by the function's own 336-byte frame (so
the callback's stack objects, whose addresses appear in its NVN calls, are where the translation puts them); then
decides the submission (`SubmeshDraw::decide`, on what the callback left) and emits it.

**The check in segments** (`HL_GFX_CHECK=1`): the gate runs the native prologue into a recording, then the translated body
for real under the tap. A second gate, at `0x8008fc00`, armed only for the call made by a checked `0x80078fc0` on the
same thread, is the checkpoint: it closes segment A (its NVN calls and the block's bytes compared), runs the translated
`0x8008fc00` itself with the tap paused, then runs the native submission on what the callback left into a recording and
lets the translation go on; at the end of the body segment B is compared (its NVN calls, the block's 352 bytes and the
command buffer's byte +0x10). The path decided at the entry is decided again at the checkpoint: a difference is counted
as a mismatch of segment B.

### What is built

| Piece | Where | What it does |
|---|---|---|
| `hl::gfx::SubmeshDraw` | `engine/native/{include,src}/hl/gfx/SubmeshDraw.*` | `prologue()` / `emit_prologue()`: the range check and the geometry processing mode; `path()`, `decide()` / `emit()`: the way (pre-recorded list, indexed instanced draw, mesh tasks, nothing), the index count, the instance and task counts, the counter, the mark, and the calls in the function's order with its arguments |
| The NVN table | `hl/gfx/Nvn.h`, `src/gfx/Nvn.cpp` | five procedures more (34): SetGeometryProcessingMode, CallCommands, DrawMeshTasks, BufferGetAddress, DrawElementsInstanced. A `Command` now carries x0..x7 (the indexed draw takes eight). BufferGetAddress is marked a **query**: native code asks it through the entry points while deriving (`NvnEntryPoints::ask`, which returns x0) and the stream carries it for the comparison; `NvnSink` does not call a query a second time |
| The check | `hl/gfx/StateCheck.*` | the same comparison, now named per use (its log lines begin "render state", "draw prologue", "draw submission") and with a path flag: the path decided at the entry no longer holding is a mismatch whatever the calls |
| The bridge | `hl/bridge/GfxBridge.*` | `guest::Drawable`, `Model`, `Mesh`, `SubmeshDescriptor`, `IndexHolder`, `SubmeshRecord` and the block's +0x3b / +0x50 (each field `static_assert(offsetof)`), the addresses, `SubmitRoute` and **`DrawBridge`**: the route, the reads (in the function's order: the model only past the range check, the descriptor only on the indexed path, the two enum tables read from the game's memory), the writes in place (told to the write tracking), the bytes compared (the block's 352 and the command buffer's mark byte) |
| C surface | `hl/c/hl_gfx.h`, `src/c/hl_gfx.cpp` | `hl_gfx_draw_start`, `_begin` / `_end` (native, around the callback), `_check_begin / _segment / _prepared / _call / _end`, `hl_gfx_draw_report`; the NVN entry points and callbacks shared by both producers |
| OS layer | `engine/host/hl_gfx.c` | **`HL_GATE(80078fc0, "329b54a385248a34")`** (the function) and **`HL_GATE(8008fc00, "4df7e218373859d0")`** (the checkpoint, on only with the check); the preparation called natively with `a64_icall` on the gate's own context, sp lowered by 0x150, x30 the address after the function's own call, x29 / x30 put back after; the NVN table resolved once for both producers |
| Tests | `engine/native/tests/gfx_test.cpp` | the prologue (signed range, mode bit 14), the paths and the counts (negative span, 32-bit counter, task rounding), the calls' order and arguments, the bridge's reads and writes in place against the expected bytes, the route, the sink's query and eight-register procedure, the check's path flag |

Both gates are in `code_hot_7.c` (one recompile of that file when they go in). Switches: `HL_GFX_DRAW` 0 (the
translated function) or 1 (native on the direct paths; **the default** since the main PC's validation below); `HL_GFX_CHECK=1` (or 2) checks instead of answering
(it checks the render state too); `HL_GFX_LOG=1` adds the line `hl gfx: draw: native pre-recorded N, indexed N, ...;
left to the translation: query N, callback N, clusters N, inline N` every 10 s.

### The check and the runs (110 s a scene; function only, no speed)

Counts at the run's last report (10 s before its end). PC 3050:

| Run | Scene | Prologue: calls / NVN calls compared | Submission: calls / NVN calls compared | Mismatched | Left to the translation |
|---|---|---:|---:|---:|---:|
| `HL_GFX_CHECK=1` | castle-move | 117,326 / 100 | 93,580 / 93,580 | 0 | 6,464 (clusters) |
| | kokiri-idle | 131,499 / 0 | 104,935 / 104,935 | 0 | 12 (clusters) |
| | hyrule-walk | 119,723 / 52 | 94,045 / 94,045 | 0 | 320 (clusters) |

The prologue's calls include the sub-meshes out of range (23,746 / 26,564 / 25,678: the callers ask for more sub-meshes
than a drawable has, and the function returns at once). Main PC (the same build, `HL_GFX_CHECK=1`):

| Scene | Prologue: calls / NVN calls compared | Submission: calls / NVN calls compared | Mismatched | Left (clusters) | Render state: calls / NVN calls, mismatched |
|---|---:|---:|---:|---:|---|
| kokiri-idle | 319,701 / 0 | 245,085 / 245,085 | 0 | 20 | 62,136 / 2,031,538, 0 |
| castle-move | 271,888 / 52 | 216,726 / 216,726 | 0 | 3,836 | 48,598 / 1,580,214, 0 |
| hyrule-walk | 278,207 / 136 | 219,734 / 219,734 | 0 | 516 | 142,446 / 4,679,303, 0 |

In all six runs **every submission took the pre-recorded path** (CallCommands): the indexed draw, the mesh tasks and the "nothing" kind were never taken, so they are checked by
the tests only, not in the game. No call took the query, callback or inline paths; the cluster paths (left whole to the
translation) are 0.01-6% of the calls. The render-state check ran in the same runs: 0 mismatches (castle-move 53,902
calls, 1,767,096 NVN calls; kokiri-idle 61,777 / 2,029,526; hyrule-walk 56,788 / 1,865,084).

Native (`HL_GFX_DRAW=1`, PC 3050, three runs a scene, the last with shots every 600 frames): every run went to the end
with no crash and the save unchanged; in the shot runs 91,743 / 104,121 / 110,817 native submissions (castle-move /
kokiri-idle / hyrule-walk, all pre-recorded), 6,228 / 16 / 20 left to the translation (clusters). The last shots of
each scene show the scene whole (geometry, shadows, characters, text), as in the reference runs. The game-speed ratio
was not logged on the PC 3050 (the runs lacked `HOS_SPEED_LOG=1`).

**Validation on the main PC** (`build/s5/valid.sh s2c-draw gfx-draw HL_GFX_DRAW=1`, 110 s a scene):

| Scene | Ran | Game speed ratio | Native submissions (last report) | Left to the translation | Shots |
|---|---|---|---:|---:|---|
| kokiri-idle | to the end, no crash | 60.35 | 255,120 | 24 | right (the Market square, characters, shadows, HUD; as `refshots-kokiri-idle`) |
| castle-move | to the end, no crash | 60.15 | 198,147 | 128 | right (as `refshots-castle-move`; the camera is in another place of the same street at the same frame number) |
| hyrule-walk | to the end, no crash | 60.41 | 225,609 | 20 | right (the fountain, water, speech text) |

Memory (game RAM / GPU pools / process RAM / commit / VRAM, MB): kokiri-idle 2,692 / 1,490 / 6,901 / 19,278 / 5,965;
castle-move 3,350 / 1,766 / 7,910 / 19,715 / 6,267; hyrule-walk 3,174 / 1,632 / 7,810 / 19,520 / 6,117 (the brick
changes no memory: these are the build's figures with the other streams' memory bricks). `HL_GFX_DRAW` is on by
default since these runs.

### Where it leads

- **Skipping unchanged state** does not apply here: the unit's one state call (the geometry mode) is already skipped
  when unchanged, by the block's +0x3b.
- **Direct emission**: what a sink handing commands to the back end would need from this unit is small. Nearly every
  submission is a CallCommands of a list the game recorded once (S+0x50): today the OS layer copies that recording
  into the caller's at every call (`inline_recording`, section 3). A back-end sink could reference the recording by
  its handle instead (the back end already registers each recording at EndRecording), keyed on the handle and the
  command memory it lives in, and invalidated when that memory is reused (`AddControlMemory`'s sweep is the place).
  The indexed draw needs the index buffer's GPU address, mode, type, count, base vertex and instances: all in the
  command as it stands.
- **The rest of the function**: the cluster paths (culling with the frustum and the depth pyramid, merging runs) are a
  numeric kernel; they are 0.01-6% of the calls in these scenes, so they come after the draw function `0x80681320`.

## 10. Brick 2.4: the shared preparation `0x8008fc00`

### Reading: what it is

`0x8008fc00` (7,134 lifted lines, 5,733 guest instructions, a 224-byte frame, one return) **prepares a draw**: it picks the
drawable's material entry for the block's pass, then applies to the command buffer whatever the block says was not
applied yet: the render state, the program, the program's buffers and textures, a per-drawable uniform block, and the
entry's pre-recorded command list (or guest code that binds the vertex streams). It is called directly from 263
functions (266 sites): the sub-mesh submission `0x80078fc0`, the recording job `0x80084de0` (2 sites), `0x800955a0`
(3 sites, 4,652 lines) and about 260 small functions (100-200 lines each, `0x8366d820`..., one site each: template
instances of the per-variant draws, *(inference)*).

| Register | What |
|---|---|
| x0 | the render-state block B (section 8: the 352-byte block on the draw function's stack) |
| x1 | the drawable D (section 9) |
| w2 | not read (the sub-mesh submission passes 0) |
| result | w0: 1 when the material entry is not empty (every path that gets past the start), 0 when it is (the function then does nothing at all) |

**Its order** (each step the first branch of the function that can leave the step before):

1. **The material entry M.** The slot is B+0x62, or, when that is 0xff, the drawable's pass slot D+0x2c[B+0x39] (an
   unsigned byte index; B+0x39 is the byte section 8 calls the depth mode). The slot is a signed byte index into
   D+0x3b[]: an entry of -1 takes the game's default entry (the GOT slot `0x84cb5d10`), any other e takes
   M = D+0x60 + e x D+0x5f (the entries' stride). M+0 = 0: return 0.
2. **The render state.** When D+0x58 (the drawable's state key) differs from B+0x59, or (with B+0x5c bit 0) the material
   state's id ([D+0x70]+0x154) differs from B+0x98, the function copies the material state into the block (blend color
   +0xf8, the eight targets' blend factors and equations +0x118..+0x147, the blend targets +0xf4, depth test and writes
   +0xf0 / +0xf1, depth function +0x154, cull face +0x155, polygon mode +0x15b / +0x15c, polygon offset enables +0x15d,
   each compared first) and, when anything changed and B's flags bit 17 is clear, **records the whole render state
   again** (36 NVN sites: blend and color defaults, logic op, the enabled targets' blend, color bind, blend color, eight
   channel masks, depth-stencil, stencil values when stencil test is on, polygon state, offset clamp, depth clamp): the
   render-state function's set (section 8) with the logic op and front face fixed.
3. **The program.** M+8 is the program object P; when it differs from B+0xd8: **BindProgram**(the recording context's
   +0x188, P+0x20, P+0x28), B+0xd0 = 0, B+0xd8 = P; then the program's interface, each part compared with the block's
   copy of its id: [P] (four 32-bit words: an id against B+0x90, and three binding words against B+0x78, +0x7c, +0x80),
   [P+8] (an id against B+0x94, binding words at +0x28 / +0x2c against B+0x84 / +0x88), [P+0x10] (an id against B+0x8c,
   a 30-bit mask at +4, a binding word per texture slot from +8). A binding word holds a slot per shader stage group
   in its low three bytes (0xff: none) and a tag in its top byte (0xff, 0xfe, 0xfd, 0xfc, 0xfb: which stages, through
   the tables at `0x83f0f7c8` and `0x83f49888`). **Uniform blocks and storage buffers** are bound from buffer objects
   (B+0x30, and the material's +0x538, +0x540, +0x588): the object's alignment and bind are virtual methods (+0x28,
   +0x50), devirtualized against the game's two known classes (`0x8002a580` / `0x80365940` for the alignment;
   `0x8353e5d0` binds a uniform buffer, `0x834bc160` a storage buffer), else called; the address is base +0x80 plus an
   aligned stride (+0x40, +0x48, +0x4a, +0x88) times the index. **Textures and samplers**: per slot of the mask, with B's
   mode bit 7 from the handle table at B+0xb8 (**BindTexturesDeferred**), else from the texture objects at B+0x20 (an
   object not ready yet is waited for and registered through guest code: `0x802a6e80`, `0x802a6cc0`, a texture view
   with swizzle and `0x800634b0`; then **DeviceGetTextureHandle** and **BindTexture** per stage). With the global switch
   `0x84ca2dd8` set, every bind is written into the command memory directly (method words, no NVN call).
4. **Pending bits.** When B+0xb0 is not 0: the bits less D+0x14 that B+0xb2 does not have yet are applied (a path at
   `0x80094654`, not read in detail); else B+0xb2 takes them.
5. **The per-drawable uniform block.** When the material's +0x518 table exists, D+0x5a differs from B+0x60 and B+0x80
   is not -1: the entry (0xc8 bytes at the material's +0x598, bounded by +0x590) is made ready once through `0x808a3050`
   (an atomic flag at its +0xc0) and bound like the program's blocks; B+0x60 = D+0x5a.
6. **The list or the streams.** k = [D+8]+0x14 & 3. With B+0x5c bit 3 and the entry's list k present (M+0x18+16k bit
   0): when its bit 1 is set, the command buffer's mark byte is set and **CallCommands**(command buffer, 1, M+0x10+16k)
   (with the global switch, the call is written into the command memory, as in section 9). Otherwise guest code binds
   the vertex streams: `0x802a03f0`(the model, the context, the slot) and `0x83604500` (2,315 lines, 57 indirect calls),
   after a CallCommands of a pre-recorded handle the model keeps per slot, when B+0x5c bit 4 and D+0x59 bit 2 say so.
7. **Clusters.** With B+0x38 (the cluster index): `0x83606290` when D+0x54 bit 2, and `0x835c2690` (787 lines) when
   D+0x16 is not 0.

**The structures it reads** (who writes them: the block is the draw function's, section 8, rewritten field by field by
this function and the render-state function; D, its material entries, the material and the program are the scene's
and the shader system's, written before the frame's recording: not traced):

| Structure | Fields |
|---|---|
| B (written) | +0x59 state key applied, +0x5c mode bits (bits 12, 13, 15 rewritten from the program), +0x60 drawable block applied, +0x78..+0x94 the interface ids and binding words applied, +0x98 material state applied (**the render-state function sets it to all ones at every call**, so a draw that called it re-applies the material state at its next preparation), +0xb2 bits applied, +0xd0 / +0xd8 program applied, +0xf0..+0x15d the render state copied from the material state |
| B (read) | +0x0 flags (bit 17: record nothing), +0x18 the material (its +0x518, +0x538, +0x540, +0x588, +0x590 / +0x598), +0x20 texture objects, +0x28 the recording context (+0xd8 the command buffer, +0x188 the object BindProgram takes), +0x30 a buffer object, +0x38 cluster, +0x39 pass, +0x62 slot, +0xb0 pending bits, +0xb8 handle table |
| D | +0x8 (its +0x14: the list selector), +0x14 pending mask, +0x16 cluster count, +0x2c pass slots, +0x3b material slots, +0x54 kinds, +0x58 state key, +0x5a drawable block, +0x5f stride, +0x60 entries, +0x68 model, +0x70 material state (+0xe0..+0x159) |
| M | +0 (0: no material), +8 program, +0x10 + 16k four lists (+0 handle, +8 flags) |
| P | +0 interface ids and binding words, +8 more, +0x10 the texture table, +0x20 / +0x28 the NVN program and its stages, +0x30 u16 flags (bits 7, 8: default ids from the globals at `0x84ea2848`..`0x84ea2858`) |
| Globals | `0x84cb5d10` default entry, `0x84ca2dd8` inline-write switch, `0x84cb56e0` / `0x84cb56e8` / `0x84cb4fa8` / `0x84cb4fb8` the known buffer classes' methods |

**What it caches.** Everything it skips is cached in the block, so for the life of one draw function's block: the
state key and material state (step 2), the program and each part of its interface (step 3), the pending bits (4), the
drawable block (5). Nothing is cached across draws, and the list (step 6) is called at every call.

**Callees**, by kind: **guest logic that must be called back**: `0x808a3050` (the drawable block made ready once),
`0x802a03f0` and `0x83604500` (vertex streams), `0x83606290` and `0x835c2690` (clusters), `0x802a6e80`, `0x802a6cc0`,
`0x800634b0` (a texture registered and its view made), the buffer objects' virtual methods; **SDK imports** (the
`0x83c6xxxx` stubs: a timed wait while a texture becomes ready, `0x83c62370` a copy), `0x80e5a620`; **NVN-only**:
`0x8572f020` (the inline-write copy). Indirect calls: 159, of which **117 are NVN procedures** and 42 the buffer
objects' methods and their alignment calls. (The brief's "145 NVN sites" counted the virtual binds whose register had
been loaded from a procedure's GOT entry earlier: a per-site reading of the register's last load gives 117.)

**Its NVN sites by what they bind** (117): render state 36 (setters 25, binds and values 11), program 1, uniform
blocks 47 (BindUniformBuffer 46, BufferGetAddress 1), storage buffers 12, textures 18 (BindTexture 11,
BindTexturesDeferred 4, DeviceGetTextureHandle 1, texture view 2), CallCommands 3.

### The cut, and why

The function cannot be cut at a point in its middle and the rest left to the translation: an entry gate answers the
whole call or none of it. So the native version takes **whole calls on one path**, decided before anything is written,
and leaves every other call whole to the translated body (as the sub-mesh submission does with its paths). The path
taken is the **cached path**: the material entry not empty, the state key and the material state already applied, the
program already bound, the pending bits already applied, the drawable block unchanged (or none), the list present.
That call reads about twenty fields, writes at most B+0xb2 and the mark byte, and makes at most one NVN call (the list's
CallCommands): it has no guest callee, so it is exact by construction and fully checkable. Every other route is a
step of the order above that it leaves to the translation, and the route is counted (`hl::gfx::PrepareRoute`: state,
program, pending bits, drawable block, streams, inline, clusters). An empty material entry (return 0, nothing else) is
answered natively too.

What comes next, by the histogram below: the state step (36 calls that `RecordSink` already writes as records), then the
program step, whose binds (uniform blocks: the procedures stream 8 is turning into inline records) and guest callees
(buffer objects' methods) need the bind handlers' side effects read first.

### What is built

| Piece | Where | What it does |
|---|---|---|
| `hl::gfx::Prepare` | `engine/native/{include,src}/hl/gfx/Prepare.*` | `decide()`: the route, in the function's order, and on the cached path the result, the bits written and the list call; `emit()`: the CallCommands |
| The bridge | `hl/bridge/GfxBridge.*` | `guest::MaterialEntry`, the drawable's and the block's new fields (each `static_assert(offsetof)`), `PrepareBridge`: the material entry chosen, the reads in the function's order (nothing past the point where a call leaves the path, as the function), the writes (told to the write tracking), the bytes compared |
| C surface | `hl/c/hl_gfx.h`, `src/c/hl_gfx.cpp` | `hl_gfx_prep_start / _apply / _check_begin / _check_call / _check_end / _report / _shapes`; the paths' histogram |
| OS layer | `engine/host/hl_gfx.c` | the gate `HL_GATE(8008fc00, "4df7e218373859d0")` (already in `code_hot_7.c` as section 9's checkpoint: no new gate, no recompile of the lifted file) now also answers the preparation; the tap now tells each procedure's kind |
| Tests | `engine/native/tests/gfx_test.cpp` | each route in order, the bits, the call; the bridge's reads (the slot through the pass, a signed slot to the default entry, the list), its stop points, its writes against the expected bytes |

Switches: `HL_GFX_PREP` 0 (the translated function; the default) or 1 (native on the cached path); `HL_GFX_CHECK=1`
checks every call instead (the route counted, the cached path compared call by call: the NVN calls with their
arguments, the block's 352 bytes, the mark byte and the result) and files every call's translated NVN calls by kind
in the histogram; `HL_GFX_LOG=1` the lines `hl gfx: preparation: ...` and `hl gfx: preparation shapes ...` every 10 s.
No new NVN procedure: CallCommands is in the table since section 9. `RecordSink` is not extended: the cached path makes
no bind, and its one call (CallCommands) copies the called recording in the NVN layer (`inline_recording` under the copy
lock), which is not a record a sink can write without that host surface.

## Not established

- Who builds the runner's item lists (R+1244..R+1280), the pass and target objects, the slot table of `0x80364080`
  (who fills slot +32 / +44 / +48), and when; whether gameplay reads any of them in place.
- How many kind-2 items a frame, and whether the serial behaviour is one item or a chain of items.
- The second callback mode `0x8065fe30`, the other listeners on `0x84eb5428`, `0x80eeb4b0`'s registration and
  `0x831dd780`.
- What the 39 indirect calls of `0x80660320` and the 136 of `0x80255be0` reach (scene objects, materials: the
  gameplay-facing part of the boundary).
- What `0x8007c120`'s readbacks read and how old the data may be; the meaning of the counter reports.
- Where the command handles (slot +80) are called and submitted.
- The meaning of w2 (`0xffff + 0x102 * pass[+148]`) and of the pass object's flag words.
- How many locks the slot scan takes a frame (needs a counter in the OS layer, e.g. per caller lr in `LockMutex`).
- The runner `0x8007fe60`'s dependency data between its lists (`jobs-boundary.md` lists it as not read).
- Unsampled numbers; other scenes.

## Method

- **Samples**: `build/hotfn/r1..r3.samples.txt` re-read whole (waits included), every frame resolved through
  `build/s5/bin/rel8/HeyListen.map`; ms a frame from each run's sampling period and presents (`r?.json`). Chain values
  from `r?.recs.json` (the J/F marks `analyze.py` made from the job traces), at each run's chain scale.
- **Serial test**: for each sample inside a function, the number of distinct threads with a sample inside the same
  function within 3.5 ms (one sampling round); controls `0x800998a0`, `0x80084de0`, `0x8007c120` measured the same way.
- **Static reading**: the lifted functions named above, read for structure only; proc calls mapped by following each
  indirect call's target back to a proc global (through a GOT entry or loaded directly) using the name-to-global table
  built from `0x812be290`.
- Scratch scripts (untracked, the session's scratchpad `gfx` folder): `agg.py` (samples with the job or the invalidate),
  `tree.py` (call tree), `leaf.py`, `chain.py`, `inv.py` (per host function by caller and job), `hidden.py` (16-frame
  limit), `conc.py` (serial test), `phase.py` (instants against presents), `fnprocs.py` (NVN procs per function, direct
  loads included), `stores.py`, `rd8.py`; and the earlier session's `index.pkl`, `q.py`, `show.py`, `nvnprocs.pkl`.
