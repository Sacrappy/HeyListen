# HeyListen native engine: plan, inventory, boundary

**The directive below is this project's main reference; the rest of this document serves it.**

Decided by the project owner on 2026-11-13, after the translated code's speed stopped moving (`docs/tickets/SPRINT-5.md`,
sessions 5 and 6): the game's engine is no longer optimised as translated ARM code. Its infrastructure is to be replaced,
one subsystem at a time, by native x64 C++ written for this project (`engine/native/`). The translated binary stays as the
functional reference, and as the way the gameplay still runs until its turn comes.

## The directive (project owner, 2026-11-13): the reference for everything below

### 1. Vision and final objective: a hybrid engine (scope set by the owner on 2026-11-13, late)

An engine 100% native was judged too long and too ambitious. The objective is a **hybrid HLE engine, in 3 to 4 weeks**:

| Native x64, in `engine/native` (`hl::*`) | Stays on the translated code |
|---|---|
| rendering | non-critical audio |
| memory | Havok's internal initialisation |
| jobs | the menus' state machines |
| streaming | the ~150,000 functions of actor and AI logic |
| the physics loops | |

Every heavy subsystem is native; what stays translated is what costs next to nothing. **Done means, from a player's
point of view: the game runs at 120+ fps and uses 4 GB of RAM.** Sections 2 to 4 below stand as written, read within this
scope: where they say "rewrite", the unit is the heavy part of a subsystem (its loops, its passes, its allocators), not
every function of it.

The replacement is **progressive and in production**: brick by brick, each native subsystem switched on in the game at
once, validated in the three scenes (picture, fps, RAM and VRAM) and committed before the next one starts.

### 2. Pillar 1: RAM and VRAM strictly apart (the break with ARM's unified memory)

On the console memory is unified: the game keeps its graphics resources in its heap because that heap *is* the video
memory. On a PC with a dedicated GPU the same model makes redundant copies (more than 12 GB of RAM and 10 GB of VRAM).
The x64 engine keeps **RAM for the CPU only** and **VRAM for the GPU only**:

1. **GPU pools without CPU access** (664 MB, step 1): 0 MB of host RAM, allocated in VRAM alone (device-local memory).
2. **Static GPU resources in CPU-cached pools** (3,770 MB of textures, geometry, shader code, step 2): once a resource
   is in VRAM its RAM is released (the guest pages decommitted), with a page trap as the safety net for the rare pages
   the CPU reads again. In the end (step 5) the native resource loader streams from the file to VRAM and these assets
   are never stored in the guest heap.
3. **No copy a frame** (the 46 MB a frame of NVN command memory and dynamic buffers): RAM is no longer copied to VRAM to
   show a frame. Dynamic buffers (uniform blocks, per-frame constants, streamed vertices) use memory that is both
   device-local and host-visible (Resizable BAR), written by the CPU in place. The capture of the NVN command lists
   goes: read in place at first, then Vulkan commands emitted directly by `hl::gfx` (step 4).
4. **The 2.8 to 5 GB "outside the game"** (this engine's caches and the GPU driver's arenas) are part of the footprint
   and are to be dealt with: audit and cap the engine's own RAM caches (the BC cache, the capture buffers, oversized
   staging rings) and correct the back end's Vulkan allocations that are host-visible or host-cached where device-local
   would do.

### 3. Pillar 2: native x64 performance (120+ fps)

Each brick rewritten in native C++20 uses the hardware fully: AVX2 and FMA, native threads without emulated locks, cache
locality.

1. **Native graphics front end (`hl::gfx`, steps 3 and 4)**: the recording pass `0x806602e0` (1.91 ms of CPU, 0.70 ms
   on the critical chain), then the graphics module job `0x8006cd60` and its sister jobs, emitting draw calls directly
   to Vulkan.
2. **Scheduler and critical chain (`hl::jobs`)**: the dispatcher and the stage registration answered natively (0.38 ms
   on the chain); no synchronous CPU-to-GPU wait on the chain in job `0x8007c120` (a non-blocking read of frame N-1);
   the actor stage job's six lanes balanced (1.25 ms on the chain) so that the frame does not wait for the slowest one.
3. **Math kernels, culling and physics (`hl::math`, `hl::physics`)**: the bounding-box kernels in C++20 with AVX2/FMA;
   for Havok (2.57 ms on the chain), without rewriting its whole state machine at once, the **compute-heavy leaf
   functions** called in loops by the solver (`0x819b8e20`) and the collision tests replaced natively, and the serial
   stretches (2.40 ms where one thread works alone) made parallel where their dependencies allow.

### 4. How the work is run

Work streams that touch different files run in parallel, as sub-agents:

- **Stream 1, RAM/VRAM and caches**: step 2 (pages released after upload, on the 3,770 MB), the audit and reduction of
  the 2.8-5 GB of engine caches and driver arenas, the 46 MB a frame.
- **Stream 2, the graphics front end `hl::gfx`**: step 3 (`0x806602e0` in native C++20), then direct emission to Vulkan
  (step 4).
- **Stream 3, the CPU critical chain**: the native dispatcher (0.38 ms), the GPU wait of `0x8007c120`, the bounding-box
  kernels in AVX2, a profile of the Havok solver's inner leaves.

### How a step is reported

Each step that lands comes with the same before/after table, from interleaved runs of both builds in the three scenes
(Kokiri Forest, the castle town, Hyrule Field): **the game's RAM (MB), the process's RAM (MB), VRAM (MB), the critical
chain (ms) and fps**. `tools/bench/steptable.py` makes it from the runs' logs.

### What this document adds to the directive: the measurements it has to be held against

The directive is the goal; these are the figures measured so far that bound it. They are kept here so that a step's
table is read against them, not to argue with the goal.

- **"What stays translated costs next to nothing" is the scope's assumption, and two measurements stand against it
  today**; both are to be re-measured as the native parts land, since they decide whether the two targets are met inside
  the scope:
  - time: the actor stage jobs, which are actor logic, hold 1.25 ms of the 5.8 ms critical chain (0.29 ms of it scattered
    logic in the instance the chain goes through, the rest animation pose, cloth and ray casts called from it);
  - memory: the game's own heaps outside GPU pools are 3.7 GB of RAM. "4 GB of RAM" for the game needs them near 3.7 GB
    with every GPU byte gone from RAM, or needs the memory and streaming modules to shrink what the heaps hold (resources'
    CPU side); for the whole process it also needs the driver's and this engine's own share (2.8-5 GB today) cut.
- **Game RAM**: 8.2 GB in the castle scene, of which 4.4 GB is CPU-side storage of GPU data (the directive's points 1
  and 2) and 3.7 GB the game's own heaps, which graphics work does not shrink ("The graphics front end and the memory
  target", below).
- **Driver arenas** are the GPU driver's memory, not this engine's: the engine can lower what it asks of the driver
  (fewer and smaller host-visible allocations, fewer imports), not free them itself. Vulkan memory the back end holds
  in system memory was 1.6 GB in the castle scene; the capture buffers 0.66 GB.
- **One build machine shares builds**: the game's build folder and the game folder are single, so streams that build
  and run the game take turns on the machine (the lock) even when their files differ; static work runs truly in
  parallel.
- **The critical chain**: 5.8 ms sampled. The directive's list (recording pass, dispatcher, the wait of `0x8007c120`,
  kernels, Havok leaves) is what `critical-jobs.md` and `gfx-record-job.md` size; under 3.2 ms needs the Havok part of
  it to deliver.
- **Exactness**: a native kernel that replaces a translated one inside physics has to give the same bits (fused
  multiply-adds, NaN rules of min and max, flush-to-zero, ARM's reciprocal square root estimate), or the game's
  simulation diverges from the console's. Each replacement is compared on live inputs before it is switched on.

## State of the hybrid engine (kept current; last update 2026-11-13, midday)

What is native and switched on in the game today (each brick validated in the three scenes by function and picture;
speed is not being measured for now, by the owner's instruction: it comes back when the physics loops are touched):

| Subsystem | Brick | Switch (default) | What it does today | What is still the game's |
|---|---|---|---|---|
| Jobs | `hl::jobs::JobSystem`, `hl::bridge::JobBridge` | `HL_JOBS` (1) | each job thread's stage is a job of the native job system | the dispatcher that picks jobs (a native one is being written: `HL_DISPATCH`), stage begin and wait, the job group, the main loop |
| Memory | no-access GPU pools out of RAM | `HOS_NOACCESS_RAM` (1) | 0.4 GB of RAM | - |
| Memory | `hl::mem::ColdPages` | `HL_MEM_COLD` (1) | cold pages of CPU-cached GPU pools leave the working set: 2.1-2.5 GB of RAM (commit unchanged) | the heaps outside pools (3.5 GB, cold at 90%: being added); true decommit with restore from the game's files (specified: `memory-boundary.md` 9) |
| Memory | heap and regions committed on first touch | `HOS_LAZY_COMMIT` (1) | 2.9 GB of commit | - |
| Rendering | `hl::gfx::RecordPass` | `HL_GFX` (1) | the recording pass's listener is native and runs the pass's two phases | the phases themselves (translated); the first native producer of NVN calls is being written (`HL_GFX_STATE`); direct emission to Vulkan |
| OS layer | a mutex finds its host object from itself | `HOS_NO_MUTEX_IX` unset | 0.5 ms a frame of lookups gone from the chain | - |
| Streaming | `hl::stream` (`host/hl_stream.c`) | `HL_STREAM` (1), `HL_STREAM_CHECK` | the decode of a resource file (`0x80108da0`) native for files that fit one read (95% of the files, 40% of the bytes): zstd's own decoder with the game's dictionaries; its check found 0 mismatches over ~24,000 files in the three scenes; every decode recorded in a provenance table (`stream-boundary.md`) | files larger than one read (their chunked decode on a second thread), the loading threads, the requests, the file system |
| Physics loops | - | - | - | everything; specified in `physics-loops.md` (the step as a task graph, first units) |

Measured with all of the above on (main PC, validation runs, MB): the game's memory in RAM 5,040-5,515 (it was
8,200), the process's RAM 10,400-11,000 (it was 14,100), commit 19,900-21,600 (it was 24,900), VRAM 6,000-7,100.

Tools that measure it: `HOS_MEM_SPLIT` (RAM by part and by GPU pool flags), `HOS_HEAP_SPLIT[_COLD]` (the heaps as a tree,
their RAM and coldness), `HOS_POOL_TRAP` (who touches GPU pool pages), `RX_VK_RES_LOG` (the process and VRAM),
`tools/bench/steptable.py` (the table of a set of runs).

To look at when speed is measured again: in one validation run of the castle walk the cold-pages thread saw 222 MB come
back within 10 s while the scene streamed, and that run's frame count was low; whether the two are linked is not
established (other work was running on the PC).

## Rules

1. **The translator is frozen.** No change to how ARM64 instructions are translated or to how the generated code is
   compiled. (`docs/research/codegen-experiments.patch` and `executor-twins-experiment.patch` are the last experiments
   there; none is merged.)
2. **Replace, do not patch.** A subsystem of the engine is taken over whole by a native module; translated engine functions
   are not edited in place.
3. **Gameplay apart from infrastructure.** Gameplay logic and data may keep running from the translation for now. Rendering,
   streaming, memory, threads, audio and physics are the infrastructure that becomes native.
4. **Nothing of the game is distributed.** The repository and the releases hold this engine and the tool that transforms
   the user's own dump, nothing else:
   - no translated code: every user generates it locally from their own copy;
   - no keys and no routine that circumvents a protection: the tool reads a dump the user has already extracted and
     decrypted;
   - no asset of the game: no texture, model, compiled shader or sound.
   The native tree includes only its own headers and the standard library (`engine/tests/test_native_engine.py` checks it).
   Documents may name guest addresses and structure offsets of the supported build; they never quote translated code.

## What runs where today

Sizes are of the supported build, counted from the user's local image by the tool (`functions.json`).

| Layer | Runs as | Size | How it is reached |
|---|---|---|---|
| OS services: threads, locks, events, files, time, input, audio output, display | native C (`engine/host`, ~21,000 lines) | 747 SDK entry points answered, 218 more stubbed | the SDK's functions are answered at their addresses |
| GPU back end: NVN commands to Vulkan | native C++ (`engine/gpu/vk`, ~40,000 lines) | 427 NVN procedures | the game's NVN calls |
| The console SDK below its answered entry points | translated | 43,492 functions, 2.1 M instructions | called by the game |
| **The game's main module: engine and gameplay together** | **translated** | **249,484 functions, 15.4 M instructions** | - |

The main module has no symbols and no RTTI (its type names are not in the binary; the SDK's are). What is known of its
structure comes from its thread names, its strings, the call tree and what was read function by function in the tickets.

### Subsystems inside the main module

| # | Subsystem | What shows it | Threads it owns |
|---|---|---|---|
| 1 | Core library (sead): heaps, threads, containers, files | heap names (root, default, scene, actor instance, texture, frame and work heaps), `sead::MainThread` | main thread |
| 2 | Module system: the engine's modules and their job groups | `ModuleSystem`, the job group run and waited for by the main thread | ModuleSystemWorker x5 |
| 3 | Actor system (BaseProc): actor objects, their manager, links between actors, the job dispatcher | `BaseProcExecutor`, the dispatcher and stages read in sprint 5 | BaseProcExecutor x6, BaseProcCreateAndDeleteThread x6 |
| 4 | Main loop and frame timing | the frame step read in TICK-014 | main thread |
| 5 | Graphics front end (agl, gsys, the graphics module): scene, models, materials, culling, terrain, foliage, effects, UI, probes; ends in NVN command buffers | `agl::` heaps, texture compaction, model compose threads | Presentation Thread, PreActorModelCompose x6, texture compaction, effect loading x2, text box texture |
| 6 | Physics (phive, on Havok) | `phive::` names, physics run as an actor, Havok thread pool names | phive worker |
| 7 | Navigation (nav mesh, crowd avoidance, path finding, on Havok AI) | nav mesh manager and system names | NavMeshCAStepDepWorker, NavMeshDepWorker, path finding |
| 8 | Audio (aal over the SDK's renderer), rumble | `aal::` thread names | audio framework, audio user task, decoders, vibration |
| 9 | Resources and streaming: file resource manager, decompression, placement loading | `tera::FileResourceMgr`, loading threads | Resource_Control, Resource_Memory, ARes_Loading x4, ParallelDecompThread x2, GeneralFormatAsync x6, PlacementMgr, OptimizeChangeBanc |
| 10 | Game services: game data (flags, saves), events, sequences, online (stubbed) | manager names | event and movie threads, play report |
| 11 | **Gameplay**: each actor's logic, the AI node classes (hundreds of "execute", "query", "one shot" and sequence nodes), a script module, the game's managers | node and manager names | none of its own: runs as jobs of 2 and 3 |

Items 1 to 10 are the infrastructure this plan replaces. Item 11 is what keeps running from the translation.

## The boundary between gameplay and engine

There is no ABI between them: one statically linked C++ program, engine code inlined into gameplay functions. Three kinds
of crossing exist, and a native module has to honour all three for the subsystem it takes over.

1. **Calls** into engine functions that were not inlined. These can be answered natively at their address, the way the OS
   layer answers the SDK (a list of addresses given to the tool; the translation itself is not touched). A function that
   is reached only through a pointer (a job's body, a callback, a virtual function) needs no new lift at all: the
   runtime's lookup asks the OS layer first (`a64_lookup_replaced`, `engine/host/hl_host.c`).
2. **Calls back** from the engine into gameplay: job bodies, actors' virtual functions. The native module calls the
   translated function on a guest context.
3. **Structures read in place** by inlined engine code. No call to answer: the native module must keep those bytes as the
   gameplay code expects them, at the guest addresses it uses. As long as any gameplay runs translated, the guest address
   space exists and these structures live in it.

| Boundary | Status | Known crossings |
|---|---|---|
| OS and GPU services | **native today** | SDK entry points; NVN procedures |
| Jobs, threads, main loop | **mapped** (`jobs-boundary.md`); **the stages' work runs through the native job system** (switch `HL_JOBS`) | 18 entry points, 1 answered natively; worker set, worker, context and scheduler layouts; the identity table read in place at 217+ sites; per-core tables read by 193 functions |
| Memory | **mapped** (`memory-boundary.md`) | the global allocation functions and their call counts, the heap object and its virtual interface, thirteen heap classes, who makes which heap, the GPU pools; the resource manager's request functions are not found yet |
| Resources | to map | below: file calls (native). Above: the resource manager's requests |
| Graphics | to map above NVN | below: NVN (native). Above: what gameplay hands the scene (per-actor render information) |
| Physics, navigation | to map | Havok middleware underneath: the largest block |
| Audio | to map above the SDK | below: the SDK's audio renderer (answered natively) |

**How a boundary is mapped without symbols**: start from what is certain (thread entry points, functions that reference a
subsystem's strings, functions reached only through data tables), grow the set along the static call graph the tool
already has, and cut: a function of the set called from outside it is an entry point; a field the outside reads is a
shared structure. The job system's boundary was made this way.

## Target architecture

```
engine/native/
  include/hl/jobs    job system: workers, dependencies, lanes for guest jobs          <- first module (written, tested)
  include/hl/app     main loop: phases, frame step, pacing                           <- first module (written, tested)
  include/hl/bridge  per subsystem: guest entry points, shared layouts, guest calls  <- job bridge: table only
  include/hl/mem     address space and allocators (guest-visible and native)         <- next
  include/hl/res     files, decompression, streaming
  include/hl/gfx     render submission (above the existing Vulkan back end)
  include/hl/audio   mixing and voices
  include/hl/phys    physics and navigation services
```

- **One pool of native workers** runs everything. Native jobs use every hardware thread. Gameplay jobs run under a
  *lane*: one of the six job-thread identities the gameplay code is built around, taken by a worker for the job and given
  back. Six gameplay jobs at most run at once, each under its own identity, while the engine's own subsystems are not held
  to six (`hl/jobs/GuestLanes.h`).
- **The main loop** is native: a list of phases (input, gameplay step, render submission, present), each spreading its
  work on the pool.
- **Native state in native layouts.** A module keeps its own data outside the guest address space, in structures made for
  x64. Only what gameplay reads in place stays in the guest space, written by the module's bridge.
- **A bridge per subsystem** is the only place that knows guest addresses and layouts. The modules themselves do not.
- **The Vulkan back end and the OS layer are kept**: they are native already. The render module moves the boundary up from
  NVN calls to the scene's draw lists, one pass at a time.

## Order of work

| Step | Module | Switch means | State |
|---|---|---|---|
| 1 | Jobs and main loop | the 18 entry points answered natively, gameplay jobs run under lanes | **on in the game behind `HL_JOBS`**: each stage's work of a job thread is a job of the native job system, run by that thread (1) or by a native pool worker going on as that guest thread (2). 1 entry point of 18 answered; the dispatcher, the stages' begin and wait, the job group and the main loop are still the game's |
| 2 | Memory | heap entry points answered by a native allocator that owns the guest address space | boundary mapped (`memory-boundary.md`); nothing native yet |
| 3 | Resources and streaming | requests served by native loaders | first brick behind `HL_STREAM`: the decode of a file that fits one read (`stream-boundary.md`); the requests, the loading threads and the chunked decode are the game's |
| 4 | Render submission | the graphics module's passes produced natively | boundary to map |
| 5 | Audio | voices and mixing native | boundary to map |
| 6 | Physics and navigation | services native | boundary to map; middleware |

Each switch is checked against the translated subsystem it replaces (same inputs, same results, picture and game speed),
with a switch to go back, before it becomes the default.

## Where the frame's time is (measured 2026-11-13, `hot-functions.md`)

Three sampled runs of the castle scene, every sample resolved to its translated function:

- **Not in a few functions.** Of the job threads' guest time (35.8 ms of CPU a frame over the six executors and the
  five module workers), 73-75 functions make 50%, 380-395 make 80%, 763-832 make 90%; the largest one is 3.4%. On the
  chain the main thread waits for, 42 functions make 50% and 162 make 80%. A hundred native kernels do not make the frame.
- **In a few jobs.** By job (what the dispatcher calls), 6 jobs make half of the critical chain and 17 make 80%: the
  player's job, the module job that records graphics commands, two numeric families not identified yet, the model chain.
  The parallel bulk (models, animation: 2.4 ms of CPU) is hardly on the chain (0.08 ms).
- Self-contained numeric kernels in the top 100: 25 functions, 15% of the job threads' guest time, 13% of the chain.

So the unit to replace is a job with what it calls, starting with those on the chain, not a list of hot functions.

## Decisions (2026-11-13, after the measurements above)

The project owner set the targets: the critical chain under 3.2 ms, memory halved; the architecture is decided here.

1. **`HL_JOBS=1` stays the default.** The pool mode (2) is kept as an experiment until its stall is found; nothing below
   depends on it.
2. **The unit of replacement is a job, not a function.** A job's body is reached through a pointer, so it is answered
   natively with no new lift (`a64_lookup_replaced`); a function called directly goes through the lift's hook list.
   Order: the jobs that bound the chain first. Each replacement runs beside the translated job on live inputs and is
   compared before it is switched on (the differential mode of `host/hos_native.c` is the model).
3. **The graphics-command recording job (`0x8006cd60`) is where the graphics front end and the critical path meet**:
   it becomes `hl::gfx`, the first native module that produces NVN calls itself. Its specification is
   `gfx-record-job.md`.
4. **Gameplay jobs are not rewritten.** The player's job is gameplay logic: what is replaced inside it is its numeric
   kernels, and what is changed around it is when it runs. `critical-jobs.md` classes each job of the chain: replaceable
   whole, kernels only, or a matter of scheduling.
5. **Memory is cut from the host side first**, where nothing of the game has to be found:
   - the heap and the address regions are committed where they are first touched, not whole (`HOS_LAZY_COMMIT`, default
     on, Windows). Measured, castle scene, same build: the process's commit 24.9 GB -> 21.9 GB; the working set does not
     move (13.9-14.0 GB), since pages never touched were never in RAM;
   - RAM itself is the resident guest memory (9.2 GB in this scene) and the back end's copies. Next: measure how the
     resident guest memory splits (heaps, GPU pools, resources), then drop the guest copy of static GPU data after upload
     behind a page trap, and shrink the back end's caches (the BC cache, the texture shadows, the pool mirrors).

### Revised the same day, with `critical-jobs.md` and `gfx-record-job.md`

What the two specifications established changes the order of work:

- **The chain is physics.** The "numeric families" of the profile are Havok: Havok Physics with its driver is 2.24 ms of
  the 5.80 ms sampled chain, Havok Cloth 0.33 ms, the actor stage jobs 1.25 ms, the graphics module jobs 0.81 ms (1.27 ms
  counting the main thread's own share of the recording job), the job system 0.38 ms. 2.40 ms of the chain runs while
  every other job thread is idle.
- **The set decided in the morning does not reach 3.2 ms.** The 25 kernels and the two jobs that are replaceable whole
  hold 1.24 ms of the chain: at infinite speed they leave 4.56 ms. Under 3.2 ms needs Havok Physics and Cloth themselves
  shortened (3.12 ms left with both at zero time, 3.8 ms at four times their speed), or the serial stretches of the chain
  spread over threads, which needs the jobs' dependencies read first.
- **The player's job is an actor stage job on six executors**; the chain goes through the instance that ends last. The
  25 kernels are 3.5% of it.
- **The recording job's cost was partly this engine's own**: its slot scan locks one mutex a slot, and the OS layer's
  lookup of each mutex's host object was 0.69 ms a frame. Fixed in the OS layer (a mutex now carries its host object's
  number: `host/nn_os_sync.c`).

So, in this order:

1. **What is ours first**: costs of the OS layer and of the back end that sit on the chain (the mutex lookup, done; the
   dispatcher's and the stage registration's 0.38 ms through the native job system; the GPU write-back wait of module job
   `0x8007c120`).
2. **The recording pass `0x806602e0`** natively (`hl::gfx`): answered with no new lift, 1.91 ms of CPU, 0.70 ms of chain.
3. **The Havok solver task `0x819b8e20`** natively: reached through a table, runs alone for 0.375 ms of the chain. Weeks,
   not days: it must be bit-exact on Havok's internal formats, and a native version must reproduce the guest's fused
   multiply-adds, its min/max rules for NaN, flush-to-zero and the ARM reciprocal square root estimate.
4. **The model-bounds kernels** (`0x80ba6d80`, `0x800a8400`, `0x81746440`): small, fully read; they build the hook list
   for directly called functions and the differential check. Their gain is small (0.1-0.2 ms at best).

**The 3.2 ms target is not reachable in 7 to 10 days by these means.** A realistic reading of the numbers for that span:
steps 1 and 2 and the kernels, for about 1 ms of the chain if every estimate holds, none of them measured yet. Physics
as a native module is the plan's step 6 and the largest block of the game; it is what stands between 4.5 ms and 3.2 ms.

### The graphics front end and the memory target (owner's direction, 2026-11-13 night)

The owner's direction: the native graphics front end is what should remove the copies between RAM and VRAM, with the
game's RAM brought towards 2-4 GB. What the measurements allow, and how the front end gets there:

| What is in RAM today (castle scene) | MB | What removes it |
|---|---:|---|
| CPU-side storage of GPU pools the game declared without CPU access | 664 | the back end alone: it is the only one that touches them (in progress) |
| CPU-side storage of CPU-cached GPU pools (textures, vertex and index data, shader code) | 3,770 | a native resource path: file -> decompression -> VRAM, the bytes never placed in the game's memory; until then, pages dropped after upload where a page trap shows game code does not come back to them (being measured) |
| The game's own heaps, outside any pool | 3,662 | not the graphics front end: this is the gameplay's and the engine's CPU data; it shrinks only as subsystems leave the translation |
| Image, system region | 97 | - |

So the graphics front end, with the resource path under it, is worth up to about 4.4 GB of the 8.2 GB, which would
leave the game's memory near 3.8 GB: the upper end of the 2-4 GB aimed at. Below that needs the heaps themselves
(resources' CPU side, actors), which is the memory module and the resource module of this plan. The process holds more
than the game's memory: the GPU driver's own arenas (2.8-5 GB resident) and this engine's caches are counted apart in
`memory-boundary.md` and are not touched by any of this.

The front end is built from the bottom, each step on in the game before the next:

1. **GPU-only storage stays on the GPU** (the 664 MB): no write-back into memory the game cannot read.
2. **Static GPU data is owned by the native side after upload**: the game's pages are released once the data is in
   VRAM, and given back from VRAM if game code ever asks (the trap). This is the step that ends the double copy without
   the resource manager's functions being known.
3. **The recording pass `0x806602e0` natively (`hl::gfx`)**: the first native producer of NVN calls
   (`gfx-record-job.md`); then the whole recording job and its two sister items.
4. **Draw lists straight to Vulkan**: once the passes are native they stop going through NVN command memory and its
   capture (the 46 MB a frame the capture copies), which is where the back end's replay time and its mirrors go.
5. **The resource path**: textures and geometry decoded from the file into VRAM by native loaders. Needs the resource
   manager's request functions (not found yet).

## What is not known yet, said plainly

- The main module is 15.4 M instructions with no names. Items 5 to 7 are each tens of thousands of functions, part of
  them third-party middleware. Their boundaries are not mapped; their size is the plan's main uncertainty.
- Engine code is inlined into gameplay. Every subsystem has structures read in place, and they are found only by reading.
- Taking the job system over does not by itself shorten the frame: the game's dispatcher is 2.8% of the job threads'
  time. Its value is that native subsystems then run on all cores. The frame shortens when the work on its critical path
  (actor updates, animation, physics, render submission) is itself native.
- While gameplay jobs run translated they keep their six identities. More than six run at once only for native work.
