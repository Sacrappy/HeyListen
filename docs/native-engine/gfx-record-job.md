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

## Not read yet

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
