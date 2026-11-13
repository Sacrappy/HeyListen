# HeyListen native engine: plan, inventory, boundary

Decided by the project owner on 2026-11-13, after the translated code's speed stopped moving (`docs/tickets/SPRINT-5.md`,
sessions 5 and 6): the game's engine is no longer optimised as translated ARM code. Its infrastructure is to be replaced,
one subsystem at a time, by native x64 C++ written for this project (`engine/native/`). The translated binary stays as the
functional reference, and as the way the gameplay still runs until its turn comes.

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
   layer answers the SDK (a list of addresses given to the tool; the translation itself is not touched).
2. **Calls back** from the engine into gameplay: job bodies, actors' virtual functions. The native module calls the
   translated function on a guest context.
3. **Structures read in place** by inlined engine code. No call to answer: the native module must keep those bytes as the
   gameplay code expects them, at the guest addresses it uses. As long as any gameplay runs translated, the guest address
   space exists and these structures live in it.

| Boundary | Status | Known crossings |
|---|---|---|
| OS and GPU services | **native today** | SDK entry points; NVN procedures |
| Jobs, threads, main loop | **mapped** (`jobs-boundary.md`) | 18 entry points; worker set, worker, context and scheduler layouts; the identity table read in place at 217+ sites; per-core tables read by 193 functions |
| Memory | partly seen | heaps reached through a virtual interface (allocation at slot 6 of a heap's table), a global allocation function; the heap classes, free and resize, and heap creation are to map |
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
| 1 | Jobs and main loop | the 18 entry points answered natively, gameplay jobs run under lanes | native side written and tested without the game; bridge not on |
| 2 | Memory | heap entry points answered by a native allocator that owns the guest address space | boundary to map |
| 3 | Resources and streaming | requests served by native loaders | boundary to map |
| 4 | Render submission | the graphics module's passes produced natively | boundary to map |
| 5 | Audio | voices and mixing native | boundary to map |
| 6 | Physics and navigation | services native | boundary to map; middleware |

Each switch is checked against the translated subsystem it replaces (same inputs, same results, picture and game speed),
with a switch to go back, before it becomes the default.

## What is not known yet, said plainly

- The main module is 15.4 M instructions with no names. Items 5 to 7 are each tens of thousands of functions, part of
  them third-party middleware. Their boundaries are not mapped; their size is the plan's main uncertainty.
- Engine code is inlined into gameplay. Every subsystem has structures read in place, and they are found only by reading.
- Taking the job system over does not by itself shorten the frame: the game's dispatcher is 2.8% of the job threads'
  time. Its value is that native subsystems then run on all cores. The frame shortens when the work on its critical path
  (actor updates, animation, physics, render submission) is itself native.
- While gameplay jobs run translated they keep their six identities. More than six run at once only for native work.
