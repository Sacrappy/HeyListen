# The physics loops: map and specification (castle-move, 2026-11-13)

What one physics step of the game is, as a graph of tasks; what loops each task runs, as code; what bit-exactness asks of
a native x64 loop and how the translated C answers it today; and the first native units to build, specified. Static
reading of the user's local translation (`%LOCALAPPDATA%\HookshotBuilder\sa\gen`) and re-aggregation of the three sampled
runs of `hot-functions.md` / `critical-jobs.md` (`build/hotfn/r1..r3`, pooled as in `critical-jobs.md`). Nothing was run.
No translated code is quoted (`README.md`, rule 4): addresses, offsets, sizes, call relations and plain-words algorithms
only.

Status of this document: **sections 1, 2 (solver and narrow phase) and 4 are written; in section 6, U1 and U2 are
specified completely, and U3 too (second session); the cloth loops, the ranked list of every unit and the scheduling
study are partly written. What is left is listed in section 7. Section 8 says what is native now (U1, U2 and two
kernels of the actor stage, checked bit for bit in the game).**

Conventions, as in `critical-jobs.md`: **read** (established from the lifted code or the data), **likely** (shape plus
strong context), **guess**. "J" is the sampled job-group chain in ms a frame (mean of three runs, 5.80 ms in all),
"CPU" the same samples on every job thread, "alone" the part of J during which no other executor or module worker ran.
"insns" are guest instructions (`functions.json`). Function names in quotes are Havok's own signature strings that the
function forms (assert and profiler strings kept in the binary), so they name the function with high confidence.

Scratch scripts of this study (untracked; session scratchpad `s5/`): `agg.py` (per job root and per function: self and
inclusive J / CPU / alone, call edges), `roottab.py` (one table a root), `census.py` (per function: insns, loops,
indirect calls, FP idioms), `sk.py` / `ops.py` (reading views), `dref.py` (where an address is stored in the image),
`upper.py` (what runs a job).

## 1. The step as a graph

### 1.1 Who runs it

- **Every Havok Physics task runs on the six BaseProcExecutor threads, as a job of the game's own dispatcher**
  `0x8004ab40` (read: every sample of every hknp task has the dispatcher, then `0x8004aae0 / 0x80df2680 / 0x80df25c0 /
  0x834a2e50`, above the task's `process`; no Havok thread pool runs in these scenes). Havok's own queue
  (`hkDefaultTaskQueueEx`, `0x816f8fc0` and its three processing loops) is present in the binary but never sampled.
  The dispatcher itself contains the call of `hkTask::processTask` (`0x8004fee0`, "void hkTask::processTask(const
  hkTask::Input &)"; called from `0x8004ab40` and `0x8007fe60`), which calls the task's virtual `process` (vtable slot 3,
  +24). So an hkTask is one kind of job of the game's scheduler (read), and a native job system that answers the
  dispatcher answers the Havok tasks too.
- **phive (Nintendo's layer over hknp) drives the step through a job graph of its own**: 64-byte job nodes allocated
  from a pool by `0x8349a510` (read: pool +0 capacity, +8 node array, +304 allocation counter (atomic), +32 / +40 the list
  of created nodes, +312 its counter; node +0 the callable, +8 index, +12 16-bit priority, +14 byte, +16 / +20 two
  words, +24 zero, +32 a self-linked list head of successors (pushed with a compare-and-swap at +40), +56 16-bit 1, +60
  the count of predecessors still running, incremented atomically). The graph is built every frame by
  `0x806a48f0` ("DynamicsFramework::updateWorldSingleThreadExt"; phases "[Phive] updateWorldPreImplExtCallPhase1..3_",
  "...PostImplExtCallPhase1..3_", "phive::JP::AfterUpdateWorldPostImplExtCallPha..."), which forms the address of the
  world-step job `0x803749e0` and pushes it (read, `critical-jobs.md`).
- The Havok tasks are created as hkTask objects in an `hknpTaskGraph` (`hkTaskGraph::addTask` `0x817009dc`, 72 call
  sites; dependencies through `0x8170160c`, likely `addDependency`), then handed to the scheduler as jobs. Which function
  turns an `hknpTaskGraph` into the game's jobs is **not established** (candidates: the phive node functions above,
  `0x80372400`, `0x839febe0` "[Phive] hkTask").

### 1.2 The world step `0x803749e0` (phive, one instance, 0.478 J, 0.795 CPU)

Read (control flow and calls; the field offsets are of the phive world object in x0):

1. Runs the world's pre-step listeners (a list at +496..+504, virtual slot +32 or +16 of each, under the lock at +520,
   `0x851047f0` / `0x851048a0` are the SDK mutex lock / unlock).
2. Walks a second listener list under the lock at global `+112` (slot +48 callbacks; filters by a body table at
   +1120).
3. `0x80374d80` on the world's +136 object (per-step bookkeeping), then **`0x803799c0`** ("next_center_of_mass_local":
   the per-body **write-back** of the previous step's results into phive's bodies; it calls `0x8037adc0`, below).
4. By the step mode in the 64-bit word at +100 / +108 (bit 0: simulate; bit 2: copy a per-substep value from +216
   +1832; bit 3: run `0x80374ec0` on +232) it calls **`0x803776c0`** (pre-collide jobs: 653 lines, creates phive nodes
   with `0x8349a510`) and **`0x80372000`** (the collide and solve phase, below) with the time step from +36, the task
   queue from +1128 and the body manager from +1120 / +1184; or, when not simulating, it bumps the step counter through
   the 16-byte ring at the queue's +24 (an atomic increment of +308 and a compare-and-swap push at +40).

`0x80372000` ("[Phive] post collide", "post collide 1 / 2 / 3", "post solve"): allocates two phive job nodes (pool in
x1 +0 / +8, counters +304 / +312), wires them as successors of the current node (compare-and-swap on +40, counter +60),
then calls **`0x80372ad4`** ("hknpWorld::generateCollideTasks(const hknpStepInput &, hknpTaskGraph &)", phive's
override of the hknp lambda) which calls **`0x817ee880`** ("hknpWorldEx::preCollide": add bodies, pre-collide
signal, update caches of mutated shapes, prepare the simulation context) and **`0x819923b8`** (forms every collide task
name: hknpPrepareNarrowPhaseTask, hknpPrepareNarrowPhasePairsTask, hknpProcessNarrowPhasePairsTask,
hknpNarrowPhasePairsPrepareGatherTask, hknpNarrowPhasePairsGatherThreadOutputsTask, hknpProcessNarrowPhaseGridTask,
hknpBroadphaseOptimizeTask, hknpPrepareConstraintsTask, hknpBuildConstraintJacobiansTask; 1,261 insns, 37 indirect
calls, 88 calls: it builds the collide task graph). Then `0x80372400` (437 insns, 23 atomics, no FP: likely the hand-off
of the graph to the job nodes). The solve tasks are generated by tasks themselves (below).

`0x803799c0` -> `0x8037adc0` (801 insns) is two loops (read): first over phive's motion records (iterator
`0x838d8920`), with virtual calls per record (+256, +968 of a body's controller: callbacks into actor code); then over
phive's rigid bodies (iterator `0x838d8b30`): for each body record whose +240 body has a +120 Havok body, it locks,
calls **`0x804f5030`** (3,467 insns, 48 indirect calls, 102 calls; "next_position", "next_rot": the per-body
write-back of hknp's motion into phive's next position / rotation and velocities), stores the result at record +256,
and unlinks finished records with atomic list operations. **0x804f5030 is the largest single physics leaf on the chain
(0.341 J inclusive, 0.132 self)**, but it is phive code with 48 virtual calls: class B.

### 1.3 The tasks of one step, in order

Task classes read from their `process` strings and vtables (`dref.py`: the address stored in a vtable whose slot -1 is
the offset-to-top 0xffffffffffffffe0, so all are `hkTask` overrides):

| order | task (`process`) | vtable slot | what (from its strings and calls) | J | CPU | alone | instances seen |
|---:|---|---|---|---:|---:|---:|---|
| 0 | `0x803749e0` phive world step (not a task: a phive job) | data, pushed by `0x806a48f0` | listeners, write-back of the previous step, generation of the collide graph | 0.478 | 0.795 | 0.019 | 1 |
| 1 | `0x8199f7c0` hknpPrepareNarrowPhaseTask | - | prepares the pair streams | 0.024 | 0.049 | 0 | 1 |
| 2 | broad phase: `0x81a14200` CopyAabbsAndMarkActiveBodiesRecollide, `0x81a42cd8` -> `0x81a363c0` hkcdBuildRangesTask (SimdTree build), `0x81a191a0` CollideTrees, `0x81a19f80` Finalize, `0x8199d5b8` hknpBroadphaseOptimizeTask | `0x848da9f8` (CollideTrees), `0x848db208` | tree rebuild of moved bodies' AABBs, tree-versus-tree pair collection (`0x81a33440`, integer) | 0.073 + 0.058 + 0.024 + 0.015 | 0.12 + 0.08 + 0.05 + 0.12 | 0.024 + 0.024 + 0.010 + 0 | up to 3 |
| 3 | `0x8191ee00` hknpProcessNarrowPhasePairsTask | `0x848d7bb8` | the narrow phase over body pairs (section 2.2) | 0.365 (+0.219 deep stacks) | 2.19 (+~1.5 deep) | 0.034 (+0.06) | **6** (72% of rounds with 2+) |
| 4 | `0x819ab300` PrepareGather, `0x819ad000` GatherThreadOutputs, `0x819affa0` SortThreadOutputs | - | merge the per-thread output streams | < 0.01 each | | | 1 |
| 5 | `0x819aeae8` hknpNarrowPhasePairsBuildJacobians | `0x848d9d20` | second-round results, deferred manifolds -> contact Jacobians | 0.112 | 0.225 | 0.107 | **1** |
| 6 | `0x819b25e0` hknpNarrowPhasePairsFinalize; `0x819a9348` PrepareConstraints, `0x819aa220` BuildConstraintJacobians | - | constraint (joint) Jacobians | small | | | |
| 7 | `0x81997f00` hknpGenSolverTasksTask -> `0x819b657c` "BuildTaskGraphHelper::buildTaskGraphDependencies" ("CreateSolverTask", "BuildGraph") -> `0x819b7700` | - | creates the solver tasks of this step and their dependencies (4 `addTask`, 7 dependency calls, 6 virtual calls) | small | | | 1 |
| 8 | `0x819b8e20` the solver task ("gatherSolverVelocities", "hknpStepConstraintJacobianProcess", subIntegrate / subIntegrateLastStep) | `0x848d9e80` | section 2.1 | 0.399 | 0.550 | **0.375** | **1** |
| 9 | phive post-solve in `0x80372000`'s second node, `0x817f4aa0` "hknpWorldEx::postSolve" | | events, deactivation | 0.02 | | 0.02 | 1 |
| 10 | `0x82bcfae0` point queries (a phive delegate job, record `0x84b37ed0`: hknpWorld::queryPoint / getClosestPoints through `0x803aa320`) | data | gameplay queries against the stepped world | 0.112 | 0.298 | 0.098 | up to 3 |

The order is read from the creation code (1-8: `0x819923b8`, `0x819b7700`) and confirmed by the sample positions in the
job-group wait (`critical-jobs.md`, tenths 0-1 and 5-8). The step appears **twice in a frame's wait** (tenths 0-1 and
5-8, each with narrow phase, solver and write-back): whether these are two worlds or two steps of one world is **not
established** (the step-mode word at +100 / +108 and the per-substep table at +216 +1832 suggest a world stepped in
two halves; likely, not read).

### 1.4 Why the solver and the Jacobian build run alone

- `0x819b8e20` is a **32-byte task object** (read: `0x819b73a0`, an `hkArray` reserve with 32-byte elements, called by
  the graph builder; the task's own reads stop at +31): +0 vtable, +8 the solver context, +16 mode byte (0: solve
  pass; 1: a path guarded by an assert loop; other values trap), +17 / +20 / +21 bytes (passed as solver flags), +18
  16-bit (substep index into a table of 4-byte entries), +22 flags (bit 0 / bit 1: call subIntegrate for the first /
  second half, bits 2 / 3: call subIntegrateLastStep for the first / second half), +24 and +28 32-bit counters
  (iterations done, incremented at the end of a pass).
- The builder makes **several such tasks, chained**, one per solver pass (likely: each is "one Gauss-Seidel pass of
  substep s, then optionally integrate"), and each depends on the previous one: the sampled task never had a second
  instance, and it runs with no other job thread working in 77 of 82 chain samples. **The serialisation is a data
  dependency, not an artefact of how the list is built**: Gauss-Seidel passes over shared body velocities must run in
  order within an island. Two islands could run side by side; whether this scene's solver works on one merged island
  set, or the builder chains independent islands anyway, is **not established** (it decides the scheduling gain in 5.2).
- `0x819aeae8` (BuildJacobians) has one instance and runs right before the solver: it consumes every narrow-phase
  thread's output (read: it is after the gather / sort tasks), so it waits for the whole narrow phase.

### 1.5 Havok Cloth (summary; details are a later section)

- A cloth world's step `0x8016b320` -> `0x8016b620` ("hclWorld::stepSingleThreaded") is a job whose address is stored at
  `0x84c70b28` (a delegate record formed by `0x8016cb20`, which builds the cloth world: hclWorld, hclTransformSet,
  collidables). It is started and waited for by `0x80168320` from the player's actor stage job `0x800b3ce0`
  (`jobs-boundary.md`, rule 5): the player's job waits 0.289 ms for it; it runs alone 0.147 of its 0.245 J (tenth 4).
- The other cloth worlds `0x80166100` (record `0x84b46418`) run in parallel bulk (2.38 ms CPU, 0.08 J).
- Inside: `0x81b47080` hclSimulateOperator::_execute -> `0x81b56de0` solve (constraint sets' `apply`: standard link
  `0x81c86720`, local range `0x81c67fa0` -> `0x81c7bd00`, bend stiffness `0x81c7ecc0` -> `0x81c7f880`), collide and solve
  `0x81b58720` -> `0x81b57a80` "_iterateCollidables" -> `0x81b5a3c0` (4,803 insns) -> `0x81b8c000` (17,427 insns),
  integrate `0x81c66a60`, skinning `0x81db9a40` -> `0x81cdff40`.

## 2. The loops, as code

Columns: inclusive / self J and CPU, alone (inclusive), insns, indirect calls ("ic"), back edges ("loops"), FP idioms
from `census.py` (counts of guest operations in the lifted body, fast and fallback paths together, so read them as
proportions: vmul / vadd = 4-wide float multiply / add-sub, unfused; sfma = scalar fused multiply-add; smul / sadd =
scalar; d64 = double; minmax = FMIN / FMAX lanes; rsqrte / steps = estimate and Newton step; cvt = integer conversions).

### 2.1 The solver task `0x819b8e20` (0.399 J, 0.550 CPU, 0.375 alone; one instance)

Loop structure (read from its control flow; offsets of the task object in 1.4):

- Entry: x0 = task, x1 = hkTask::Input (+8 a thread context or zero; when zero the thread-local Havok context at
  tpidr+0x20 is used, created on first use by `0x816e1700`). `0x819212c0(task+8)` returns the simulation thread
  context; its +1880 holds the solver step info, +1216 the per-thread solver data.
- Mode 0 (the only one sampled): **for each pass** (counter at task +28 against the count at +24 of the step info):
  `0x819ba420` "hknpStepConstraintJacobianProcess(threadCtx, int, int, bool, int, ...)" with the task's flags, then by
  flag bits 2 / 3 `0x8182f860` -> `0x8182dc20` "hknpSolverIntegrator::subIntegrateLastStep", by bits 0 / 1 `0x8182f520`
  -> `0x8182d2c0` "subIntegrate", then the per-substep counter update. The monitor-stream writes around each part
  (`0x816e09e0` timer reads, "Tt..." records into the thread's stream when the global flag at a fixed address +944 is
  set) are Havok's profiler: they cost little but are part of the translated body.
- Mode 0 tail: `0x817d8300` (542 insns) over the step info's +64 / +80 / +104 / +128 arrays indexed by the substep:
  the velocity gather ("gatherSolverVelocities" is inlined in the task: its string is formed by `0x819b8e20`).

| function | role | incl J | self J | incl CPU | alone | insns | ic | loops | FP |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| `0x819ba420` | hknpStepConstraintJacobianProcess: walks the Jacobian ranges of the pass | 0.180 | 0 | 0.243 | 0.165 | 1,254 | 2 | 62 | none of its own |
| `0x8177b2a0` | "hknpStreamContactSolver::solveJacobians": the **contact solver kernel**, over contact Jacobian blocks of 4 | 0.175 | 0.097 | 0.238 | 0.165 | 3,969 | 7 | 84 | vmul 689, vadd 675, minmax 82, smul 61, sadd 82, cvt 6, div 2, sqrt 1; **no fused op** |
| `0x81785c60` | under solveJacobians: a second Jacobian type (likely friction / angular, 23 ic) | 0.044 | 0.010 | 0.068 | 0.039 | 3,522 | 23 | 50 | vmul 320, vadd 330, d64 90, div 10, sqrt 10 |
| `0x81775a00` -> `0x80171a90` | under it: modifier callbacks (7 ic in 0x8177b2a0 are "FireModifiers") | 0.019 | | | | 602 | 14 | | |
| `0x8182dc20` | subIntegrateLastStep: **for each active motion**: integrate, then update the body (below) | 0.175 | 0.020 | 0.229 | 0.175 | 1,591 | 7 | 44 | vmul 64, vadd 56, smul 30, sadd 27, minmax 8, div 3, sqrt 3, d64 3 |
| `0x817d9d80` | per body after the last substep: new world AABB (shape AABB under the new transform, expanded by the motion) | 0.141 | 0.068 | 0.170 | 0.141 | 736 | 1 | 6 | vmul 47, vadd 72, d64 30, minmax 32, sqrt 4, cvt 8 |
| `0x8173e2ac` -> `0x81746440` | convex shape AABB (unit U1, section 6) | 0.024 | 0.024 | 0.024 | 0.024 | 21 + 124 | 0 | 1 | |
| `0x81a040bc` -> `0x8170341c` | world AABB of a body's oriented box (unit U2; data-only, reached through `0x84cbfcc8`) | 0.024 | 0.020 | 0.029 | 0.024 | 128 + 44 | 0 | 0 | d64 12 |
| `0x80168130`, `0x8174bb00`, `0x818556f0` -> `0x80825860` | small vector helpers of the body update | 0.025 | | | | 38, 33, 64, 143 | | | |
| `0x8182d2c0` | subIntegrate (non-last substeps): per motion, velocity -> transform | 0.029 | 0.024 | 0.059 | 0.029 | 599 | 1 | 26 | vmul 24, vadd 32, smul 21 |
| `0x817d8300` | velocity gather / scatter | 0.010 | 0.010 | 0.015 | 0 | 542 | 3 | 16 | vmul 62, vadd 54, sqrt 7 |

Shares of the 0.399 J: contact solving 0.18 (of which pure kernel self time 0.10), the last-substep integration and body
update 0.18 (of which the AABB part 0.14), the other substeps 0.03. All of it runs alone.

**Pure numeric kernels on plain arrays** (no virtual calls, no Havok container calls): `0x81746440`, `0x8170341c`,
`0x81a040bc`, `0x8174bb00`, `0x80168130`, `0x80825860`. **Kernels with callbacks**: `0x8177b2a0` (7 indirect calls:
modifier hooks, taken only for Jacobians flagged for them), `0x817d9d80` (1: the shape's virtual AABB for non-convex
shapes). **Havok structure walkers**: `0x819ba420`, `0x8182dc20`, the task itself.

### 2.2 The narrow-phase task `0x8191ee00` (0.365 J + 0.219 deep, 2.19 CPU + ~1.5 deep; six instances)

Structure (read from the call tree of the samples and the functions' strings; the loop itself not read line by line):
`0x8191ee00` "hknpProcessNarrowPhasePairsTask::process" takes a range of body-pair work items and calls
`0x81928de0` "mergeAndCollide2StreamsIntern ... collide_bodyPairs" (3,457 insns, 110 loops): **for each body pair** of
its range, merging the active and inactive cache streams, it dispatches on the pair's collision-detector type and
writes manifolds and caches to its thread's output streams ("ThreadOutput::m_activeCacheStreams",
"m_inactiveCacheStreams", "m_deferredManifoldsOut", "m_perPairOutput"). Each instance owns its output streams: the
instances are independent (read from the strings: per-thread outputs, merged later by the gather tasks).

Dispatch targets on the chain:

| path | what | incl J | incl CPU | notes |
|---|---|---:|---:|---|
| `0x8197a620` -> `0x81855ca0` -> `0x8197af20` -> `0x8197a168` -> `0x81979e40` -> `0x819783e0` -> `0x8176c980` -> `0x81730760` | convex-convex: GSK (`0x8176c980` 3,386 insns, minmax 167, d64 54; `0x81730760` 2,040 insns, vmul 464, vadd 563: the GSK iteration, likely) | 0.132 (+0.22 deep) | 0.88 (+1.5 deep) | the largest CPU of the whole step |
| -> `0x81738700` -> `0x8173b340` -> `0x81738fa0` / `0x8173a5c0` / `0x8173a830` / `0x8173ae00` -> `0x8173ab70` | penetration depth (expanding polytope: `0x8173a830` one face of the polytope, `0x8173ab70` double geometry) | 0.019 (+0.21 deep) | 0.29 (+1.45 deep) | d64 everywhere |
| `0x8197a22c` -> `0x81979e40` / `0x81979780` -> `0x8197be18` -> `0x8173e2ac` -> `0x81746440` | convex vs composite (child AABBs of a compound or mesh, then the same convex routines) | 0.112 | 0.43 | `0x81746440` here 0.049 J |
| `0x81902240` -> `0x81782ca8` -> `0x8177b020` -> `0x81779980` | contact Jacobian setup for new manifolds (`0x81779980` 1,413 insns, vmul 269, d64 59), with phive's modifier `0x80042950` ("LinearCastVsTriggerVolumeModifier::postContactJacobianSetup") | 0.064 | 0.46 | 52 indirect calls in 0x81902240 |
| `0x817472e0` | support of a 1-4 vertex convex (jump table on the count) | 0.029 | 0.08 (+0.23 deep) | |

The task runs with 3.9 other job threads busy on average: shortening it shortens the chain only where it is the last
instance to finish (the "strict" columns of `critical-jobs.md`).

### 2.3 The other physics loops (summary)

- `0x819aeae8` BuildJacobians -> `0x819ae440` -> `0x8192f8a0` (1,652 insns, 19 ic) -> `0x81973300` (2,107 insns) ->
  `0x817738e0`, `0x8176a780` (2,172 insns, minmax 163, div 13): the second narrow-phase round for deferred manifolds,
  then their contact Jacobians. One instance; 0.107 alone.
- `0x82bcfae0` point queries -> `0x803aa320` (phive, sfma 102: scalar fused FP is phive's compiler, not Havok's) ->
  `0x803aaef0` / `0x803a9020` -> hknpWorld::queryPoint `0x818f5998` -> `0x804ed080` / `0x81a44f80` (shape point
  queries), `0x805621c0` (phive filter, cvt 39).
- Write-back `0x804f5030` (2.1 above): FP is scalar and **fused** (sfma 140, and fma64 60: double FMAs), unlike Havok's
  vector code.
- Broad phase: integer tree code (`0x81a33440`, `0x81a363c0`), no FP of note.

## 3. The data (as far as section 6 needs it)

- **Transforms passed to the AABB kernels** (`0x81746440`'s x1): three float4 rows at +0, +16, +32 (the rotation,
  applied as row0 * x + row1 * y + row2 * z, so the rows are the rotated basis vectors), then seven floats: a
  translation high part at +48, +52, +56, +60 (four lanes) and a low part at +64, +68, +72 (three lanes; the w lane
  reuses +72). Position = high + low computed in double: Havok's "precise transform" (likely; the field use is read).
- **Convex shape** (x0 of `0x8173e2ac`): +56 a relative array of vertices (32-bit signed offset from +56, then a 32-bit
  count; offset 0 means "no array"), vertices packed as three floats (12 bytes); +104 a float and +108 a 16-bit value
  that, placed in the high half of a float, gives a second float: their product is the shape's convex radius.
- **AABB out**: 32 bytes, min float4 at +0, max float4 at +16; w lanes are computed, not cleared.
- Alignment: the 4-wide float code assumes nothing beyond 4-byte alignment of the vertex array (loads are 8 + 4 bytes);
  transforms and outputs are 16-byte aligned (pair loads of 16 bytes).
- Temporary buffers of the solver (Jacobian and temp streams, "hknpSolverData::ThreadData::m_jac*Stream",
  "m_solverTempsStream") come from Havok's block stream allocator (strings in `0x817f3ce0` "PdBlock stream allocator"),
  per thread, per step (likely). Their layouts are **not read** in this session.

## 4. Exactness

### 4.1 How the translated C computes floating point today (read: `a64_suyu.h`, `a64_fpx.h` in the lifted folder)

- **Every guest FP operation keeps its own rounding.** Single-precision add / sub / mul / div / sqrt run natively in SSE
  when a fast-path test says the native result equals Arm's (finite, normal, and the sticky FPSR flags it could raise
  already set); otherwise they fall back to exact soft-float bodies (`recomp_fpe_*`) that also keep FPSR. Vector
  operations do the same per lane (`a64_vfpx_*`), and straight-line runs are grouped into speculative blocks
  (`A64_BK`: the whole block in SSE, then one test; a failed test re-runs the block's ops one by one).
- **The fast path is taken only when FPCR is plain** (round to nearest, DN clear, no other control bit but FZ) **and
  FPSR.IXC is already set** (`RECOMP_FPX_OPEN`); the host runs with MXCSR round-to-nearest, DAZ and FTZ clear.
  Flush-to-zero (FPCR.FZ, bit 24) is emulated by the tests: a subnormal operand or result sends the op to the exact body.
- **Fused multiply-add** (FMADD / FMLA): single precision in double then one rounding to single, falling back when the
  double is a single-precision midpoint; double precision with the hardware FMA (built with `-mfma`, x86-64-v3) or the
  exact body. A guest FMUL is computed through the same helper with a zero addend (`mul32_fma32`): it is **not** a fusion.
- **FMIN / FMAX**: inline per lane, Arm's rules (a signalling NaN gives the quieted NaN and IOC; a quiet NaN operand is
  returned (quiet); min of +0 and -0 is -0, max is +0; under FZ subnormal operands are flushed first, with IDC; under DN
  a NaN result becomes the default NaN).
- **FRECPE / FRSQRTE**: `recomp_fp_estimate(c, bits, width, rsqrt)` (runtime, `a64_rtgen.py`: Arm's tables, exact);
  **FRECPS / FRSQRTS**: fused (2 - a*b) and (3 - a*b) / 2 with one rounding (helpers `recps32` / `rsqrts32`).
- **Conversions**: FCVT single <-> double inline (exact, with the FZ flag rules), integer conversions through
  `recomp_fp_to_int` / `recomp_fixed_to_fp` (toward zero for FCVTZS, Arm's saturation on overflow and NaN -> 0).
- **FPCR at run time.** Havok's per-thread init (`0x816e1700` -> `0x816e1944`) sets or clears FZ by a mode argument
  (`0x8169d350`: FZ = bit 0 of its argument; `0x816e1944` passes "mode == 2"); `0x80ec0ea0` sets FZ unconditionally on
  the thread that runs it. Which value the executors carry while they run Havok tasks is **not established** statically
  (it is one log line: read `c->fpcr` at a task's entry in the bridge). The lift has seven FPCR writes in all (read):
  **Settled at run time (stream 7, section 8): every call of the Havok units (u1, u2) in the three scenes had FPCR =
  0x1000000 exactly (FZ set, nothing else); the actor stage's units (affine, sphere) saw both 0 and 0x1000000 (FZ set
  on some threads or at some times, never another bit).** The lift has seven FPCR writes in all (read):
  the two FZ setters above, three in the SDK's context save / restore (`0x851109b8` and its neighbours), and two in
  `0x8575d1bc` / `0x8575d228` that replace the rounding-mode and FZ bits with an argument (a C library `fesetround`-like
  helper, likely; its callers `0x8338d510`, `0x83c62290`, `0x85739e50`, `0x8581d2f0` are not physics). Default NaN
  (bit 25) is set by none of them except through the saved context: likely never set.
- FPSR is read back by the game only in the SDK's context save / restore (read: no `mrs fpsr` in the physics
  functions censused). A native loop need not produce FPSR flags, but **it must leave the guest's FPSR as the translated
  code would** if the fast path's "IXC already set" test is to stay true afterwards: setting IXC is harmless (it is
  sticky and set almost always); clearing nothing is required.

### 4.2 What each loop relies on

| loop | fused ops | FMIN / FMAX | estimates | double | conversions | FZ sensitivity |
|---|---|---|---|---|---|---|
| contact solver `0x8177b2a0`, `0x81785c60` | **none** (Havok's 4-wide code is FMUL then FADD: the vector multiply-add of hkVector4 is not fused on this build) | yes (82 lanes: impulse clamping, likely) | none | `0x81785c60` yes | 6 (int) | yes if FZ is on (impulses near zero) |
| integration `0x8182dc20`, `0x8182d2c0` | none | 8 | none | 3 | 2 | idem |
| body AABB `0x817d9d80`, `0x81746440`, `0x81a040bc`, `0x8170341c` | none | yes | none | yes: positions in double, outward rounding by bit adjustment (6.1) | single <-> double | double -> single of tiny values |
| narrow phase GSK / EPA (`0x8176c980`, `0x81730760`, `0x8173a830`, `0x8173ab70`) | none | many (167 lanes in `0x8176c980`) | none | yes | 7 | yes |
| phive write-back `0x804f5030`, `0x8014aad0`, point queries `0x803aa320` | **yes, scalar single and double** | | | yes | | |
| cloth `0x81b8c000`, `0x81b5a3c0`, `0x81c7bd00`, `0x81c86720`, `0x81c7f880`, `0x81db9a40` | none in vectors | some | **FRSQRTE + FRSQRTS** (36 in `0x81b5a3c0`, 15 + 30 steps in `0x81b8c000`, 4 in `0x81c7bd00`) | little | `0x81db9a40` 308 | yes |

So for **Havok Physics' own loops the x86 rules are simple**: plain AVX (`_mm256_mul_ps`, `_mm256_add_ps`) with **no
FMA**, in the guest's order; min / max must be Arm's (x86 `minps` returns the second operand when either is NaN and does
not order -0 / +0: emulate with a compare and blend, or check lanes and fall back); no estimates; doubles where the
guest uses doubles. Cloth needs Arm's FRSQRTE table (call `recomp_fp_estimate` or a 256-entry table lookup
reproducing it) and a single-rounded step (with FMA: `fnmadd(a, b, 3) * 0.5` is exact for normal results, the same
argument as the runtime's helper). Phive's write-back needs FMA exactly where the guest fused.

**FZ.** If the executors run Havok with FZ set, a native loop under MXCSR FTZ+DAZ is not the same as Arm FZ in every
case: Arm flushes a result to zero when it is tiny *after* rounding, x86 FTZ *before* rounding (results just below the
smallest normal that round up to it differ), and Arm's FZ also flushes inputs (DAZ does). The safe rule for a native
unit: run in the host's default MXCSR (no FTZ / DAZ) and detect, per block of lanes, any subnormal input or result (one
compare on the exponent field); such lanes go to the translated helpers. This is what the runtime's fast path does, and
it keeps the native loop exact whatever FPCR.FZ is.

## 5. Replaceable units and scheduling (partial: physics, this session)

Chain effect = J x (1 - 1/s), "upper" counting all chain samples, "strict" only the alone ones (as `critical-jobs.md`).

| rank | unit | reached | J (alone) | 2x upper / strict | 4x upper / strict | work (days) |
|---:|---|---|---:|---|---|---:|
| 1 | U1 `0x81746440` convex vertex AABB (+ `0x8173e2ac` radius) | direct call (2 sites: `0x8173e24c`, `0x8173e2ac`): HL_GATE | 0.112 (0.029) | 0.056 / 0.015 | 0.084 / 0.022 | 2 |
| 2 | U2 `0x81a040bc` + `0x8170341c` oriented-box AABB | **pointer** (`0x84cbfcc8`): HL_ANSWER | 0.029 (0.024) | 0.015 / 0.012 | 0.022 / 0.018 | 2 |
| 3 | U3 `0x817d9d80` body AABB update (U1 and U2 inside) | direct (1 site in `0x8182dc20`): HL_GATE | 0.141 (0.141) | 0.071 / 0.071 | 0.106 / 0.106 | 4-6 |
| 4 | `0x8177b2a0` contact solveJacobians (+ `0x81785c60`) | virtual (stream contact solver's vtable): HL_ANSWER | 0.175 (0.165) | 0.09 / 0.08 | 0.13 / 0.12 | 10-15 |
| 5 | `0x8182dc20` subIntegrateLastStep whole (contains U3) | direct from `0x8182f860` | 0.175 (0.175) | 0.09 / 0.09 | 0.13 / 0.13 | +5 after U3 |
| 6 | the solver task `0x819b8e20` whole (4 + 5 + `0x8182d2c0` + gather) | vtable `0x848d9e80`: HL_ANSWER | 0.399 (0.375) | 0.20 / 0.19 | 0.30 / 0.28 | 20-30 |
| 7 | narrow-phase convex-convex (GSK `0x8176c980` / `0x81730760`, EPA `0x8173b340` and leaves) | direct | ~0.35 with deep stacks (0.04) | six-wide: ~0.1 upper, ~0.02 strict | | 20+ |
| 8 | BuildJacobians `0x819aeae8` | vtable `0x848d9d20` | 0.112 (0.107) | 0.056 / 0.054 | 0.084 / 0.080 | 15 |
| 9 | phive write-back `0x804f5030` | direct | 0.341 (0.019) | 0.17 / 0.01 | 0.26 / 0.014 | 10 (48 virtual calls to keep) |
| 10 | cloth `0x81b5a3c0` + `0x81b8c000` (collide and solve) | direct | 0.108 (0.108) | 0.054 / 0.054 | 0.081 / 0.081 | 10-15 |

Scheduling (partial):

- **Solver islands across threads.** The solver's 0.375 ms alone is the largest serial stretch of the step. If the
  builder's chain of passes covers several independent islands, a native solver could run islands on separate threads
  (native work is not held to six lanes) with each island's Gauss-Seidel order unchanged: bit-exact, since islands
  share no body. Needed first: read `0x819b7700` (which bodies and Jacobian ranges each pass covers) and count islands
  in the scene at run time.
- **Point queries `0x82bcfae0` beside the solver**: they read the world after the step (they run at tenth 8, after the
  second solve); moving them earlier would change their results. Not available without changing gameplay timing.
- **Write-back `0x804f5030` per body in parallel**: each body's write-back looks independent (per-record lock), but it
  calls 48 virtual functions of phive and actor code: not safe to split without reading every callee.

## 6. The first three units

Common to all three:

- **Bridge**: a C function in `engine/host/hl_physics.c` registered with `HL_GATE(addr, hash)` (direct calls) or
  `HL_ANSWER(addr, fn, on, ...)` (pointer-reached), reading the arguments from the guest context (x0.., v registers)
  and guest memory in place through `A64_PTR`; the native kernel in `engine/native` (`hl::phys`), plain C++20 with no
  `-ffp-contract` (the build forbids contraction) and **no FMA** in these three.
- **Switch** `HL_PHYS_AABB` (0 off; 1 native; 2 differential). In mode 2 the gate runs the translated body first on the
  real state, then the native kernel on the same inputs into a scratch buffer, and compares the 32 output bytes (and x0,
  which none of these return); counts mismatches, logs the first ten with their inputs, and logs once the FPCR value seen
  (`c->fpcr`, which settles section 4.1's FZ question). Zero mismatches in castle-move, kokiri-idle and hyrule-walk before
  1 becomes the default.
- **Exact rules**: per lane, in the guest's order; FMUL and FADD separately rounded (single); FMIN / FMAX with Arm's NaN
  and signed-zero rules; single -> double conversions exact; double -> single in round-to-nearest-even; **fallback**: if
  any input or intermediate lane is NaN, infinite or subnormal (single or double), or if `c->fpcr` has any bit other
  than FZ set, call the translated body instead (the gate keeps it). This keeps the native path to the cases where IEEE
  on x86 and Arm agree bit for bit, and makes FZ irrelevant (no subnormal ever enters or leaves the native path).

### 6.1 U1: `0x81746440`, AABB of a convex vertex set under a precise transform (and `0x8173e2ac`)

- **Reached**: directly, from `0x8173e2ac` (the convex shape's AABB function: also the slot reached from the shape's
  vtable) and from `0x8173e24c`. Gate `0x81746440` itself (124 insns, leaf, no calls); `0x8173e2ac` (21 insns) can stay
  translated, or be gated in the same unit (below).
- **Arguments**: x0 = address of a relative array header: int32 `off` at +0, int32 `n` at +4; the vertices start at
  x0 + off (when off == 0 the base is 0 and n must be 0). x1 = transform (layout in section 3). x2 = output (32 bytes).
- **Constants**: one float4 `BIG` loaded from guest address `0x83eb0c20` (read from the image: bits 0x7f7fffee in each
  lane, Havok's "large float", 17 units of the last place below FLT_MAX). `min = BIG`, `max = -BIG` (sign bit flipped in
  each lane).
- **Loop** (i = 0 .. n-1, vertex p = (x, y, z) three floats at base + 12 i):
  `t = R0 * x` (4 lanes, R0 = transform +0..+15), `u = R1 * y`, `w = R2 * z` (each a separately rounded multiply), then
  `v = (t + u) + w` (two separately rounded adds, in that order), then `min = FMIN(min, v)`, `max = FMAX(max, v)` per
  lane (FMIN(a, b) with a = the running min, b = v; FMAX likewise).
- **After the loop** (all in double, each operation separately rounded):
  `Hx, Hy, Hz, Hw` = floats at transform +48, +52, +56, +60; `Lx, Ly, Lz` = floats at +64, +68, +72.
  `Px = Hx + Lx`, `Py = Hy + Ly`, `Pz = Hz + Lz`, `Pw = Hw + Lz` (the w lane adds +72 too: read).
  For each lane k: `a = P_k + (double)min_k`; then **outward rounding**: take the 64-bit pattern of `a`; if `a > 0`
  (or unordered: the guest's compare-and-select takes NaN this way too, read again by stream 7) subtract 0x10000000
  from it, otherwise (a <= 0, either zero) add 0x10000000; convert the adjusted double to
  float (round to nearest) -> out.min_k. For max: `b = P_k + (double)max_k`; if `b >= 0` add 0x10000000 to its bit
  pattern, otherwise (b < 0 or NaN) subtract it; convert -> out.max_k. (2^28 units of a double's last place is half a
  float's last place: the nudge turns round-to-nearest into rounding away from the box's inside. Note the asymmetry at
  zero: for min, +0 and -0 both get +0x10000000 added; for max, +0 gets +0x10000000, -0 compares >= 0 too and gets it.)
- **Output**: out +0 = (min_x, min_y, min_z, min_w), +16 = (max_x .. max_w). Nothing else is written; no return value.
- **`0x8173e2ac`** (if gated with it): x0 = shape, x1 = transform, x2 = out. Calls U1 with x0 + 56. Then `r = f104 *
  g`, where f104 is the float at shape +104 and g is the float whose bits are (the 16-bit value at shape +108) << 16;
  one rounded multiply. Then out.max = out.max + r and out.min = out.min - r, each lane (r broadcast to the 4 lanes),
  separately rounded.
- **Native form**: each v is computed exactly as above (per vertex, no reordering inside it); the min / max reduction
  may be reordered, because Arm's FMIN / FMAX are associative and commutative on non-NaN values, signed zeros included
  (min(+0, -0) = -0 and max(+0, -0) = +0 whatever the order). So two vertices per 256-bit register, reduced at the end,
  is **bit-exact under the fallback rule** (no NaN input; a NaN vertex sends the call to the translated body).
- **Check**: mode 2 as above, every call for the first 600 frames then 1 in 64. Expected volume: ~0.38 ms CPU a frame in
  the translated form; called from the solver (alone) and the narrow phase (six-wide).
- **Expected chain effect**: J 0.112 (0.029 alone): 2x -0.056 / -0.015, 4x -0.084 / -0.022 (as `critical-jobs.md`).

### 6.2 U2: `0x81a040bc` (with `0x8170341c`), a body's world AABB from its oriented box

Read whole (128 + 44 insns, no indirect call, no loop). `0x81a040bc` is stored in data at `0x84cbfcc8` (the shape-type
table that `0x817d9d80` calls through, slot +24: see 6.3) and has no direct caller in the samples: answer it with
HL_ANSWER, no new lift.

- **Arguments**: x0 = body B, x1 = a precise transform A (layout of section 3: rows +0, +16, +32; high translation +48
  .. +60; low translation +64, +68, +72), x2 = out (32 bytes). Uses 64 bytes of stack for a temporary transform T.
- **Step 1, `0x8170341c(T, A, B + 128)`: compose A with the body's transform.** Let `Q0, Q1, Q2, Q3` be the four float4
  at B +128, +144, +160, +176. For each `q` of `Q3, Q2, Q1, Q0` (in that order, results to T +48, +32, +16, +0):
  `T_q = (A0 * q.x + A1 * q.y) + A2 * q.z`, 4 lanes, each multiply and add separately rounded, single precision
  (A0, A1, A2 = A +0, +16, +32; q.x etc. = lanes 0, 1, 2 of q broadcast). Then the precise translation: `L = (A64,
  A68, A72, A72)`, `H = A +48 .. +60`, `s = T48 + L`, `hi = H + s` -> T +48; `lo = s - (hi - H)` (two operations, in
  that order) -> lanes 0, 1 to T +64, lane 2 to T +72. All single, unfused.
- **Step 2, half extents.** `r` = float at B +104 (broadcast); `e = (B140 + r, B156 + r, B172 + r, G80 + r)` (B140 =
  float at B +140, the w lane of Q0; B156, B172 likewise; G80 = float at G +80 where G is the pointer stored at guest address
  `0x84ca4680` (page `0x84ca4000` +1664); read it at run time; e.w is computed but not
  used). `C = (G80, G84, G80, G84)` (the 8 bytes at G +80 repeated).
  `a0 = |T0 * e.x|`, `a1 = |T16 * e.y|`, `a2 = |T32 * e.z|` (multiply rounded, then the sign bit cleared), then
  `E = (C + a0) + (a1 + a2)`, 4 lanes, single, in that order of operations (the two inner sums first: `C + a0` and
  `a1 + a2`, then their sum).
- **Step 3, centre in double**: `P = (d(T48) + d(T64), d(T52) + d(T68), d(T56) + d(T72), d(T60) + d(T72))` (the w lane
  again adds +72), double adds. For each lane k: `mn = P_k - d(E_k)`, `mx = P_k + d(E_k)` (double), then the outward
  nudge of U1 (min: bits - 0x10000000 if mn > 0, else bits + 0x10000000; max: bits + 0x10000000 if mx >= 0, else
  bits - 0x10000000), convert to single (nearest). Out +0 = (mn_x .. mn_w), +16 = (mx_x .. mx_w). Nothing else is
  written but the 64 bytes of stack (not visible to callers).
- **Exactness**: no FMA; abs is exact; the only rule beyond plain IEEE is the nudge, done on the integer bits as the
  guest does. Fallback as in the common rules.
- **Check**: the 32 output bytes. **Expected chain effect**: J 0.029 (0.024 alone): 2x -0.015 / -0.012, 4x -0.022 /
  -0.018. Work: 2 days.

### 6.3 U3: `0x817d9d80`, the per-body AABB update of the last substep

**Read whole (stream 7, second session; native as `u3`, section 8).** 736 insns, called once per body by `0x8182dc20`
(subIntegrateLastStep, one site, `0x8182ecb4`). Arguments: x0 = simulation thread context (or 0), x1 = motion M, x2 =
solver record R (104-byte entries: +32 flags, +76 a float), x3 = a box P (min 4 floats, max 4 floats), x4 = the body B
(192 bytes, written), x5 = motion state S (+0 orientation quaternion x, y, z, w; +16, +24, +32 the centre of mass in
binary64; +40 .. +84 floats: +64 .. +72 linear and +76 .. +84 angular velocity). G = the object at `*0x84ca4680` (+80
four floats, +96, +336, +512, +640 float pairs). All single precision unless said, each operation separately rounded,
no fused operation anywhere. Lanes the function computes and never uses are left out below.

1. **Speeds.** If B +92 (16-bit flags) bit 10 is clear: B +94 (16 bits) and B +76 .. +79 are zeroed; go to 3.
   Otherwise, with a, b = M +96, +100: `q = s56² + s60²`; `l = ((s40 a)² + (s44 b)²) + (s48 a)²`; `w = (s52² + q) +
   G640`; `mn = FMIN(sqrt l, G96)`; `aw = a · sqrt w`; `sc = ((u8 B156 · k255) + 0) · h(B154)` (k255 = bits
   0x3b808081; h(x) = the float whose high half is the 16-bit x); `v4 = mn · sc`; then with m = M +64:
   B +78 = signed-min(trunc(m · v4 + k), 0xffff), B +94 = unsigned-min(trunc(K · mn + k), 0xffff), B +76 =
   unsigned-min(trunc(m · (v4 + aw) + k), 0xffff) (k = bits 0x3f7fbe77, K = bits 0x4722f8e0; trunc = FCVTZS, Arm's
   saturation; the low 16 bits stored).
2. **Transform (moving bodies).** The rotation is S's quaternion times the conjugate of B's own (B +160), a Hamilton
   product computed by lanes: with m1 = m[0,2,3,1], m2 = m[3,1,0,2], c1 = c[0,3,1,2], c2 = c[3,0,2,1] (c the
   conjugate), `d = m2 c2 - m1 c1`, `e = m2 c1 + m1 c2`, then `d` reversed (lanes 3,2,1,0) minus `e`, lanes 0 .. 2
   negated. Rows by `0x80168130`: with t = q + q, xx = t0 q0, yx = t1 q0, zx = t2 q0, yy = t1 q1, zy = t2 q1, xw = t0 q3,
   yw = t1 q3, zw = t2 q3, zz = q2 t2: row0 = ((1 - zz) - yy, yx + zw, zx - yw), row1 = (yx - zw, (1 - zz) - xx, zy +
   xw), row2 = (zx + yw, zy - xw, (1 - xx) - yy). The rows' w lanes (B +12, +28, +44) are the local centre of mass c;
   `cm = row2 c.z + (row0 c.x + row1 c.y)` per lane; position: `p = S.com - (double)cm` (binary64), high = (float)p,
   low = (float)(p - high). Stored: three rows of 12 bytes at B +0, +16, +32; high x, y, z at +48 .. +59; low at +64 ..
   +75. B +60 (the high w) is not written.
3. **Margin.** `t = M48 · G336` (two lanes), `f = FMAX(FMAX(B60 - t, t), R76)`.
4. **The shape's box** under B's transform (B +0 .. +75 is a precise transform, section 3): unless B +140 bit 3, the
   shape at B +120 through the shape-type table (`*(*0x84ca78c8)` + (shape +24 byte) · 512, function at +24), called
   with (shape, B, a stack box). Seen in the three scenes: `0x8173e2ac` (convex vertex set: U1, then the radius
   shape+104 · h(shape+108) subtracted from the min and added to the max), `0x81a040bc` (U2), `0x8174bb00` (a two-vertex
   convex: each end rotated, `e0 = row2 z0 + (row0 x0 + row1 y0)` and `e1 = (row0 x1 + row1 y1) + row2 z1`, then
   `high + (e0 + low)` and `high + (low + e1)`, FMIN / FMAX of the two minus / plus shape +104; single precision, no
   outward step), and `0x81a011b0`, `0x818556f0` (they call further helpers: not native). With bit 3, inline: half extents
   `e_k = (G512_(k&1) · h(B154)) · u8 B(157+k)`; per lane `E = |row2 e2| + (|row0 e0| + |row1 e1|)`, clamped to h(B154)
   by a signed integer minimum of the bit patterns; centre `cl = row2 B44 + (row0 B12 + row1 B28)`; `P = ((double)high +
   (double)low) + (double)cl` (w lane: low z); min = outward(P + (double)(-E)), max = outward(P + (double)E) (section
   6.1's step).
5. **Growth.** `up = (f0, f1, f0, f1) + max`, `dn = min - (f0, f1, f0, f1)`. "Still" when the three linear and three
   angular velocity components are all zero. If not still: `lin2 = s72² + (s64² + s68²)`, `ang2 = s76² + (s80² + s84²)`.
   If lin2 > 0: `pad = FMIN(M120 · sqrt lin2, G96) · sc` (two lanes), `reach = f0 + h(B154)`; the box `com ∓ reach` in
   binary64 (w lane 0), rounded outward; `up = FMIN(up + pad, that max)`, `dn = FMAX(dn - pad, that min)`; then f += pad;
   else f += G80 (two lanes). With `v6 = (M120 a)` lane-wise times the angular velocity in lanes (s80, s84, s76): if R
   +32 & 0x380 == 0x80, `s = sqrt(v6z² + (v6x² + v6y²))`, `up += G336 v6 + G336 s`, `dn += G336 v6 - G336 s`; else if
   R +32 bit 17 and `lin2 > k_c · M104²` (k_c = bits 0x3c33abb1): the swept cast `0x817d8c90` (not native: declined,
   0.1% of calls); else `up = FMAX(up, v6 + up)`, `dn = FMIN(dn, dn + v6)`.
6. **Stores.** B +148 = sat16(trunc((M120 a · sqrt ang2 + f0) · M64)) (0 + f0 when still), B +150 = sat16(trunc(f0 ·
   M64)) (signed to unsigned 16-bit saturation). Then, if not still and M +80 > 0, and the threshold `M104 · M80` is
   above both |S +56, +60, +52| and |angular velocity| lane-wise: if the new box lies in B's stored box (B +96: max x,
   y, z and min z; +112: min x, y) and in P, return; otherwise grow both by M80. Then (thread context needed: 0 runs
   a Havok context lookup, declined natively) unless the stored box holds the shape's box, append a 16-byte record
   (tag 0x0000000403000010, then B +88) to the block at context +1568; unless the grown box overlaps P, append (tag
   0x0000000d04000010, then B +176) to the second stream's block (context +1888 +677: a signed index, -1 none; entry
   context +1896 + 80 · index, its writer at +32, the block its first word). A block's first word: bytes used (low half,
   records from +32) and records (high half); a full block (used >= 4048) calls the allocator `0x816ef7bc` (declined
   natively). Finally, still bodies whose box equals the stored one return; otherwise B +92 |= 0x10 and the box is
   stored (max with min z in its w lane at +96, min x, y at +112).

**Callers' registers.** The only caller sets every general and vector register it reads after the call, and the flags
(`cmp` before its branches): a native answer that leaves the caller-saved registers as they were on entry is not
visible.

Expected after the three, at 4x: about -0.13 ms of the chain in the strict reading (U1's solver share and U2 counted
once inside U3), -0.19 in the upper one. Small: their value is that they build the gate, the bridge, the exactness
rules and the differential check that units 4-6 (the contact solver, the integration, the whole solver task: 0.30 ms
strict at 4x) need.

## 7. Not established

- The function that turns an `hknpTaskGraph` into the game's jobs, and how task dependencies are expressed to the
  game's scheduler (the phive node layout is read; the bridge from hkTaskGraph to nodes is not).
- Why the step appears twice in a frame's wait (two worlds or two half-steps).
- The number of solver tasks a step creates, what each covers (`0x819b7700` not read), and the number of islands: so
  the scheduling gain of running islands in parallel is not a number.
- (Settled, section 8: FZ is set, and only FZ, whenever the Havok units ran.)
- U3's swept cast `0x817d8c90` and the shape box functions `0x81a011b0`, `0x818556f0` are not read (6.3).
- The meaning of U2's inputs (which transform x1 is, what the w lanes of the body's rows and G +80 hold) is inferred;
  the specification does not depend on it.
- The contact solver's Jacobian stream layout (`0x8177b2a0`), the body / motion layouts (offsets beyond those named
  above), the block-stream allocator's buffers.
- Section 2 for cloth (only summarised in 1.5 and 4.2), the broad phase and the point queries in loop detail; the full
  ranking of section 5 beyond physics (cloth units, kernels of the actor jobs).
- "Calls a frame" for each loop body: the samples give time, not call counts; no counts were taken.
- One scene, one machine (castle-move, main PC), as in `critical-jobs.md`.

## 8. What is native (stream 7, `hl::math`)

Four kernels answer their guest functions natively, bit for bit, in the game (`engine/native/include/hl/math`,
`engine/native/src/math`, the layouts in `hl/bridge/MathBridge.h`, the OS layer's side and the check in
`engine/host/hl_math.c`). Section 6's plan is kept with two changes: the file and switch are `hl_math.c` / `HL_MATH` (not
`hl_physics.c` / `HL_PHYS_AABB`), and the check computes the native answer first, into scratch, then runs the translated
function for real.

| unit | guest | reached | what (specification) |
|---|---|---|---|
| u2 | `0x81a040bc` (+ `0x8170341c`) | pointer (`HL_ANSWER`) | 6.2 (read again: right, but for NaN in the outward step, 6.1) |
| affine | `0x80ba6d80` | direct calls (`HL_GATE`, hash `fc814e67d0dad1eb`) | `critical-jobs.md` #3 (read again: right; the two multiply-adds are fused, the first product is not) |
| u1 | `0x81746440` | direct calls (`HL_GATE`, hash `851bc99bf31be697`) | 6.1 (read again: right, the NaN case corrected) |
| sphere | `0x800a8400` | direct calls (`HL_GATE`, hash `42b7877d24ec9c57`) | `critical-jobs.md` #1, completed below |

**The exactness rules as code** (`hl/math/Exact.h`, tested by `engine/native/tests/math_test.cpp`): every guest operation
is one call of a `Guard`, in the guest's order, which computes it in SSE (binary32 / binary64, round to nearest, host
MXCSR untouched) and records whether operands and result are ones on which x86 and the translation agree: inputs finite
and not subnormal; sums finite and above the smallest normal or exactly zero; products likewise, a zero only from a zero
operand; quotients and square roots likewise; narrowing to binary32 above the smallest normal or zero. These are the
translated code's own fast-path tests (`a64_fpx.h`), so the native path is exact under FPCR.FZ set or clear. The guest's
fused multiply-adds go through binary64 (the product is exact there) and are given up on a binary32 midpoint (the only
case where rounding twice differs; a test builds one). FMIN / FMAX are Arm's (truth table tested, NaN propagation
included); the outward step on the double's bits is the guest's, unordered compares included. A call whose FPCR has a bit
other than FZ, or any value outside the rules, is **declined**: the translated function runs.

**The sphere, completed from the code**: x0 = bounds object (+0 descriptor D, +56 the 32-byte record, none when 0), x1
+32 = the bone palette (64-byte matrices, four float4 columns). D +90 byte: 0 = one sphere under the bone at D +84; else
the bone list at D +32 (16-bit), the count at D +88 (the first sphere is used whatever the count). Spheres at D +64 (16
bytes: centre, radius). Per sphere: centre' = fma(fma(m0 cx, m1, cy), m2, cz) + m3 (4 lanes), the column lengths
sqrt((z^2 + x^2) + (w^2 + y^2)), their largest by compares (first, then a larger second, then a larger third), radius' =
r times it. The first sphere is the record. Each next one: d = centre' - C (4 lanes), d2 = (dz^2 + dx^2) + (dw^2 + dy^2),
g = (r' - R)^2 raised to 1e-4 (bits 0x38d1b717); if d2 < g, the record is replaced by the new sphere unless R > r' (the
copy is 32 bytes from the function's frame: centre, radius, then **12 bytes it never wrote**, the caller's stack at
sp - 28 .. sp - 17, which the native unit copies from the same place); otherwise dist = sqrt(d2) (a NaN goes through a libm
call: declined natively), q = d2 * (1 / dist), R' = (q + (r' + R)) * 0.5, C' = C + d * ((R' - R) / q), R' stored first.

**The differential check** (`HL_MATH_CHECK=1`): every call of every unit (or those of `HL_MATH_UNITS`) computes the native
record into scratch (holding the guest's 32 bytes on entry), runs the translated function for real (a gate calls the
lifted function again under a per-thread bypass; the pointer-reached unit calls `hl_translated`), and compares the 32
bytes. The game gets the translated results only. Counts every 10 s; the first ten mismatches of a unit are logged with
their inputs and both outputs. Results, PC 3050, build of `410f88b`, full 110 s runs (the last report of each run,
about 100 s in):

| scene | u2 compared | affine compared (declined) | u1 compared | sphere compared (declined) | mismatches |
|---|---:|---:|---:|---:|---:|
| castle-move | 295,625 | 8,713,228 (263,681) | 708,345 | 622,563 (24,931) | 0 |
| kokiri-idle | 318,018 | 8,740,444 (293,034) | 639,105 | 595,751 (23,317) | 0 |
| hyrule-walk | 338,678 | 9,549,889 (314,698) | 702,411 | 645,939 (25,238) | 0 |

The same check on the main PC (build of `324e7ce`, with every unit native by default otherwise), full runs:

| scene | u2 compared | affine compared (declined) | u1 compared | sphere compared (declined) | mismatches |
|---|---:|---:|---:|---:|---:|
| kokiri-idle | 744,408 | 24,046,249 (788,081) | 1,779,489 | 1,644,222 (62,858) | 0 |
| castle-move | 723,673 | 21,360,215 (668,154) | 1,696,723 | 1,522,972 (60,819) | 0 |
| hyrule-walk | 711,221 | 21,395,643 (666,650) | 1,681,693 | 1,513,747 (58,136) | 0 |

No call was declined for its FPCR. u1 and u2 decline nothing (u1: 348 calls in one native run); about 3% of affine and 4% of sphere calls are declined by
the value rules (which value was not looked into: they run translated, as before). FPSR: in about 19 u1 calls a run the
translated code raised a flag beyond IXC (the outward step of an exact zero gives a subnormal double, whose narrowing
raises IDC under FZ); the native path sets only IXC. FPSR is read back only by the SDK's context save / restore (4.1):
not a difference the game sees.

**Switches**: `HL_MATH` (0: nothing native; 1, the default: the validated units native), `HL_MATH_UNITS=u1,u2,affine,
sphere` (choose units, validated or not), `HL_MATH_CHECK=1`, `HL_MATH_LOG=1`. All four units are validated (the check
above) and native by default.

**Validation with the four units native** (main PC, `build/s5/valid.sh`, tag `s7v`): the three scenes ran to the end
(112 s), no crash, game speed ratio 59.7 (kokiri-idle), 59.9 (castle-move), 59.9 (hyrule-walk); calls answered natively
in the run, e.g. castle-move: u2 384,334, affine 11,894,740, u1 904,874, sphere 820,467. The last shots of each scene
look right (no black, missing geometry or tint); what they show differs from the reference shots as the reference runs
differ among themselves (the scenes' timing moves the characters). Memory: game RAM 2.8 / 3.2 / 3.3 GB, process RAM
7.1 / 8.2 / 7.7 GB (kokiri-idle / castle-move / hyrule-walk): these kernels allocate nothing.

### 8.1 Second session: u3, the fused-sum midpoint, what is declined

**u3, `0x817d9d80`** (the per-body box update, specification 6.3; `hl/math/BodyBox.h`, `src/math/BodyBox.cpp`, the body
and the other layouts in `hl/bridge/MathBridge.h`, the C surface `hl_math_body_box`, the OS layer's `run_body` in
`engine/host/hl_math.c`). Reached by a gate on the lifted function (`HL_GATE(817d9d80, ...)`, its one caller
`0x8182dc20`). The native side reads copies of everything the function reads and returns a plan: the body's 192 bytes
as the function leaves them with the bytes it stores, and the records it appends to the thread's stream blocks (block,
offset, record, the header word afterwards). The native run writes the stored bytes and the records (and marks them
for the write watch); the check runs the translation for real and compares the whole 192-byte body, the moved-bodies
block's header word, and each planned record and header. The shape's box is answered for the four functions that make
96% of the calls: the convex vertex set (`0x8173e2ac` -> u1), the oriented box (u2), and a segment's box `0x8174bb00`
(two vertices plus the radius, single precision: `segment_box` in `Bounds.cpp`); declined, the translated function
runs: the shapes `0x81a011b0` and `0x818556f0` (3.5%), the swept cast `0x817d8c90` (0.1%), no thread context, a full
stream block, and values outside the rules (0.2%: squares of velocities near 1e-20 that underflow, which Arm flushes
under FZ). Its only caller sets every register and the flags it reads after the call (read at `0x8182ecb8` .. and
both branch targets), so what a native answer leaves in caller-saved registers is not seen.

**The fused sum's midpoint rule, widened exactly.** The decline histogram (`hl math: <unit>: declined by rule: ...` in
the log, from the `Guard`'s first failure, `hl::math::Why`) showed that every declined affine call (3%) and all but
13 of 1.5 M sphere calls declined (4%) were on the binary32-midpoint rule of the fused multiply-add, with operands of
short mantissas whose binary64 sum is exact (e.g. a product of -1.5 units of the addend's last place). Rounding twice
differs from rounding once only when the binary64 sum was inexact; the `Guard` now gives up only then (exactness
tested by Knuth's two-sum: a zero error). After it: affine declines 0 to 2 calls a run, sphere 13 to 25 (products
that underflow, which Arm flushes under FZ: not widened, rare).

Check, main PC, build of `2f924ae` (`HL_MATH_CHECK=1`, every unit checked), full runs, the last report of each run:

| scene | u3 compared (declined) | u2 | affine (declined) | u1 | sphere (declined) | mismatches |
|---|---:|---:|---:|---:|---:|---:|
| castle-move | 246,611 (9,994) | 401,414 | 12,484,265 (0) | 976,964 | 912,770 (13) | 0 |
| kokiri-idle | 404,355 (33,863) | 761,782 | 23,947,254 (2) | 1,634,748 | 1,647,810 (25) | 0 |
| hyrule-walk | 360,135 (15,742) | 648,559 | 21,404,529 (0) | 1,525,367 | 1,524,230 (13) | 0 |

The same check on the PC 3050 (build of `8b2bf47`, u3 native by default otherwise), full runs:

| scene | u3 compared (declined) | u2 | affine (declined) | u1 | sphere (declined) | mismatches |
|---|---:|---:|---:|---:|---:|---:|
| castle-move | 147,621 (8,035) | 215,680 | 7,297,090 (0) | 507,015 | 513,282 (13) | 0 |
| kokiri-idle | 138,276 (6,121) | 219,947 | 7,958,830 (0) | 530,411 | 545,122 (13) | 0 |
| hyrule-walk | 167,551 (15,558) | 279,215 | 9,036,233 (0) | 606,182 | 619,576 (13) | 0 |

u3's declines by reason on the main PC (castle-move / kokiri-idle / hyrule-walk): shape 8,673 / 32,957 / 14,728, cast 338 / 250 /
317, values 983 / 656 / 697; none for FPCR (every u3 call ran with FZ set and nothing else). An earlier check build
(before the segment shape) compared 150,959 u3 calls in castle-move with no mismatch. FPSR: in 3 to 5 u3 calls a run
the translation raised a flag beyond IXC (as u1's, 8: not read back by the game).

Calls whose output overlaps their inputs: every C surface reads all it needs before writing (tested in
`math_test.cpp`, `outputs_over_inputs`: the same box with the output written over the inputs); u3 works on copies.
Whether the translated functions are ever called that way in the game is what the check covers: no mismatch.

**Switches**: as before (`HL_MATH`, `HL_MATH_UNITS`, `HL_MATH_CHECK`, `HL_MATH_LOG`); `u3` is a unit name, validated,
native by default with the four others.

**Validation with the five units native** (main PC, `build/s5/valid.sh`, tag `s7bv`, build of `fa783dc`): the three
scenes ran to the end (112 s), no crash, game speed ratio 60.17 (kokiri-idle), 59.76 (castle-move), 59.99
(hyrule-walk); u3 answered 365,279 / 395,701 / 360,936 calls natively (95%). The last shots of each scene look right
(complete geometry, no black, tint or flicker), differing from the reference shots only as the reference runs differ
among themselves. Memory: game RAM 2.4 / 3.2 / 3.1 GB, process RAM 6.9 / 7.7 / 7.7 GB (kokiri-idle / castle-move /
hyrule-walk): u3 allocates nothing (its plan is a per-thread buffer).

**Not done in this session**: `subIntegrateLastStep` `0x8182dc20` (1,591 insns, 7 indirect calls, Havok's block
allocator and profiler calls) and `solveJacobians` `0x8177b2a0` (3,969 insns, 48 calls, 7 indirect) were not read
beyond their call lists; no specification of them is written yet, and `BuildJacobians` was not started.

## 9. `subIntegrateLastStep` `0x8182dc20` (stream 7, third session)

Read whole from the image's instructions (1,591, every path). Names of fields say what the function does with them;
the guesses about their meaning are marked. Section 6.3's names for U3's inputs are kept where they meet: what 6.3
calls the motion M (x1) is the solver step data D below, and its motion state S (x5) is the motion.

### 9.1 Calls and arguments

Three callers: `0x8182f860` (from the solver task `0x819b8e20`, three sites: the one sampled), `0x8182fc40` (a 22-insn
forwarder) and `0x8182b0a0` (from `0x819bf460`). `0x8182f860` calls it with:

| arg | value |
|---|---|
| x0 | the simulation thread context T |
| x1 | the solver step info I (`0x819b8e20`'s +1880 object) |
| w2 | the substep (a byte) |
| w3 | `first` = (I +64)[substep].lo + 1: the first motion slot of the range |
| w4 | `count` = (I +64)[substep].hi - 1 |
| w5 | 0 |
| x6 | a work-range object on the caller's frame, made by `0x81708ba0(obj, 1, count, 1)` |
| x7 | 0 (with another caller: an array of 48-byte stream-range records, below) |
| [sp] | a profiler holder P (9th argument) or 0 |

Nothing is returned (the caller reads no register it did not set again: read at `0x8182f958`).

### 9.2 What it walks

- **The motions of the range, in the order a work-range object deals them.** `i` runs from 0 up to the end of the
  current chunk (object +12); at the end of a chunk `0x8170901c(obj, &chunk)` deals the next one (an atomic add on the
  object's counter, `0x81708ea0` when it runs dry); it returns a negative value when the range is done. With the
  caller's own object (`0x8182f860`) one thread takes every chunk in increasing order, so `i` = 0 .. count-1 (likely:
  `0x81708bc0` was not read; the check of 9.6 verifies it in the game).
- For each `i`, two 32-byte solver velocity entries: **A** = (I +104)->[first + i] and **B** = (I +128)->[first + i]
  (pointers at I +0x68 and I +0x80). An entry holds two vectors: **V** in floats +0, +4, +8 and **W** in floats +16,
  +20, +12 (lanes x, y, z; the vector registers carry `(+0, +4, +8, +12)` and `(+16, +20, +12, +12)`). V turns the
  orientation (angular velocity), W moves the centre of mass (linear velocity): read from what each integrates. A +24 is
  a 64-bit value used only as zero / not zero ("A has a modifier", below). B +24 and +26 are two 16-bit packed floats
  (the half shifted left by 15: `(h << 15)` as a float's bits), **maxW** and **maxV**; B +28 is a word: bits 0-4 a count
  n, bit 5 and bit 6 flags, bits 8-31 the **motion id**.
- The motion: 128 bytes at `(*(I +0x50))->+16 [id >> 6] + (id & 63) * 128` (pages of 64). Fields used: +0 orientation
  quaternion (x, y, z, w), +16, +24, +32 the centre of mass in binary64, +40 .. +60 (V x, y, z, then W z) and +56, +60 (W
  x, y): the velocities of the previous substep; +64 .. +84 the same layout for the "integration velocities"; +92 a byte,
  the motion's cell (0xff: none); +93 a byte, a weight; +94 a 16-bit packed float written 0x3f80 (1.0); +96 the first
  body's id (24 bits); +100 a 16-bit word (bits 0-14: "has integration modifiers"); +102 a half (the high half of a
  float); +112 four halves (damping, likely); +120 a half; +123 a byte, the motion-properties index; +124 a word, flags
  tested against the callback lists' masks.
- D = I +0x10 (pointer): the solver step data. Read: +32 a float, +96 / +100 the step pair t = (t0, t1), +104 a pair,
  +136 a float s, +144 a pair, +192 and +208 two float4 Q0, Q1.
- G = `*0x84ca4680` (as in 6.3): +0x50 a pair g50, +0x60 a float4 C (the constant "one", likely 1.0 in every lane: read
  at run time), +0x150 a pair h, +0x3f0, +0x400, +0x410 three pairs c1, c2, c3 (a polynomial's coefficients).
- The bodies of each motion: a ring from the motion's first body id through body +0x84 (next id) back to the first;
  body address `(*(I +0x48))->+32 [id / 42] + (id % 42) * 192`.

### 9.3 Per motion, in the guest's order (single precision, each operation separately rounded, no fused operation)

Notation: `|x|²` of a 3-vector is `z² + (x² + y²)` (the pair sum first; lane order of the guest), `pk(h)` the float whose
bits are `h << 16`, `rs(x)` the integer step `0x5f375a86 - (bits(x) >> 1)` taken as a float (0 when x's exponent field is
0): an exact integer operation, not an estimate instruction.

1. `dV = A.V - B.V`, `dW = A.W - B.W` (lane by lane). **Steps 2 to 4 run only when A +24 is not zero** (`0x8182e2e0`:
   otherwise `cV = dV`, `cW = dW`, `r = C.lo`, straight to step 5). `mV = maxV²`, `mW = maxW²` (the game's maxima are
   often `0x7f7f0000`, whose square is +inf: harmless, the squares are only compared). `LV = |dV|²`.
2. **Clamp V.** If `mV >= LV`: `cV = dV`, `k = C.lo`. Otherwise: if any lane of `(dV, A.s12 - B.s12)` is NaN, `cV = 0`,
   `k = C.lo`; else `k = (LV's exponent field 0 ? 0 : C.lo / sqrt(LV))` (two lanes, the pair C.x, C.y), `cV = dV · (k ·
   maxV)` (lanes x, z by the first lane of the pair, y by the second); then a **clamp record** is appended (9.4, kind 0),
   and `r = FMIN(C.lo, k · (D104 · pk(M102)))` (used only by the modifier call).
3. `LW = |dW|²`. **The n path** (n = B28 & 31 not zero): `n = float(n)`, `e = (n · 2^-10) · n`, `s3 = e · D32`. The
   threshold pair T: if B28 bit 6 is set and `pk(M120)` is neither 1.0 nor 0: `m = |pk(M120)|`, `T = (m · s3) · rs(m)`
   with the test value `(m · s3)²`; if it is 0: `T = g50`, test `g50²`; else `T = (s3, s3)`, test `s3²`. If `LW >= test`
   nothing happens; otherwise `u = D144 · T`, `e2 = (e · (e · 0.75)) + 0.25`, and: if `LW · e2 > u²`: `dW = dW · FMAX(g50,
   C.lo - rs(LW) · u)` (pair broadcast as above); else `dW = 0`, and `cV = 0` too when `u² > |cV|² · e2` (with `|cV|²` in
   the order `z² + (x² + y²)`).
4. **Clamp W** as V, against `mW >= LW` (LW from before step 3), with maxW, on dW as step 3 left it (the NaN test on its
   four lanes, the factor `C.lo / sqrt(LW)` from the unscaled LW); record kind 1, no `r`. `cW` is the result.
5. **New velocities.** `nW = (B.W + cW · Q0) · (Q1 · s)`, `nV = (cV · Q0 + B.V) · (Q1 · s)` (4 lanes; the order of each
   sum as written). If A has a modifier and `M100 & 0x7fff`: the virtual call (9.5, #2) with `r · Q0` and the two new
   velocities on the stack (it may change them).
6. **Damping.** `f = C - FMIN(t · pk4(M112), C)` (lanes: t0 · d0, t1 · d1, t0 · d2, t1 · d3); `iW = cW · f2`, `iV = cV ·
   f3`. If B28 bit 5 (byte) is set: `iW` is kept where `|nW|² > |iW|²`, else `nW` replaces it (the same for V), and the
   per-body flag value gets 0x1000.
7. **Callbacks** (9.5, #3, #4) when `(M124 | 2)` meets the masks of the context's two callback lists; they may change
   `iW, iV, nW, nV` and the moving flag.
8. **Stores** (always): M +64 = (iV.x, iV.y, iV.z, iW.z), M +80 = (iW.x, iW.y), M +40 = (nV.x, nV.y, nV.z, nW.z), M +56 =
   (nW.x, nW.y), M +94 = 0x3f80. **Moving** = any of lanes 0-2 of iW, iV, nW, nV not equal to zero (a NaN counts).
9. **Integration** (moving only): `a = (nV · t) · h` (lanes by t0, t1, t0 and h0, h1, h0), `x = nW · t`. `s = (a.x² + a.y²) +
   a.z²`; `kq = C.lo + s · (c1 + s · (c2 + s · c3))` (a pair). With q the orientation: `w' = kq0 · q.w - ((a.x q.x + a.y q.y)
   + a.z q.z)`; for each lane j of x, y, z: `q'_j = (a_j · q.w + c_j) + q_j · kq_(j&1)` where c = (a.z q.y - a.y q.z, a.x q.z -
   a.z q.x, a.y q.x - a.x q.y) (each the difference of two products). `N = (q'x² + q'y²) + (q'z² + w'²)`, `q'' = q' /
   sqrt(N)` (4 lanes). Centre of mass: `com_k += (double)x_k` (binary64 add of the widened float), stored +16, +24, +32;
   then the orientation +0.
10. **Deactivation** (when the context's +8 object +0xdb8 is set: the bit field at `*(T +0x4b8)`): when A +24 is zero and
    the motion moves, and the previous W (M +56, +60, +52, read before step 8) and dW (after step 3) are all zero, the byte
    +11 of the motion's 16-byte deactivation
    record (`(*(I +0x88))[id >> 6] + (id & 63) · 16`) is cleared; otherwise `0x81830268(record, motion, properties (T +0x30
    -> + M123 · 68), &previous W, a stack temporary)` (a leaf of 850 lifted lines: reads the motion, writes the record) and,
    when it returns non-zero, bit `first + i` of the bit field is set (plain read, or, write).
11. **Cell.** `P = ((float)com.x, (float)com.y, (float)com.z, 0)`; `cell = (I +0x18)->vtable[+0x28](obj, &P, cache)` (9.5,
    #5; the cache is a 0x130-byte block on the frame, set up on the first motion by vtable[+0x20](obj, substep, count,
    cache) when its first word is 0xff). If M92 != 0xff and differs from `cell`: a **cell record** (9.4). If `*(T +0x4d8)`
    (a table of 32-byte cells) is set: the cell's four int64 accumulators get `FCVTZS(lane) · w` (32-bit) sign-extended,
    lane by lane, for the lanes `(P.x · 128, P.y · 128, P.z · 128, C.x)` (the last not scaled); w = M93 · the byte at I +
    (A +24 not zero ? 0x98 : 0x99).
12. **Bodies** of the motion's ring, in ring order: if body +0x8c bit 0 is clear and the listeners' count `(I +0x48) +0x500`
    is not zero, an atomic OR of `1 << (id & 63)` into word `id >> 6` of each listener bit field; then body +0x5c (16 bits)
    = (moving << 10 | the 0x1000 of step 6) | ((body +0x5c >> 9) & 0x20); then U3 (`0x817d9d80`, section 6.3) with (T, D,
    `*(T +0x10)` + body +0x8d · 104, I + 0x20, body, motion); then the body's 32-byte cache entry (`(*(T +0x4e8))[id >> 8] +
    (id & 255) · 32`) gets +12 (4 bytes) and +16 (8 bytes) zeroed.

### 9.4 Its stream records

All go to the thread's block-stream writer at T +0x520 (its current block's first word: bytes used in the low half,
records in the high half; records from block +32; a block holds 4,096 bytes). Before each record, while the used bytes
are 0xfc0 or more, `0x816ef7bc(T +0x520, *(T +0x4e0))` (the block allocator) gives a new block. Each record adds 0x10020
to the header (one record, 32 bytes) and writes:

| record | +0 | +4 | +8 | +12 | +16 | +17 | not written |
|---|---|---|---|---|---|---|---|
| clamp (V: kind 0, W: kind 1) | 0x03000020 | 6 | 0x00ffffff | motion id | kind (32 bits) | | +20 .. +31 |
| cell | 0x03000020 | 0 | first body id (M96, 32 bits) | motion id | old cell (byte) | new cell (byte) | +18 .. +31 |

The bytes a record leaves unwritten keep what the block held. The writer's own fields (T +0x520 .. +0x538) change only
through the allocator.

### 9.5 Its calls

| # | call | when | class |
|---|---|---|---|
| 1 | `0x800430a0(P, name, name + 0x12)` / `0x800431c8(P, ...)` | P not zero, at entry / exit | profiler (phive timer) |
| | `0x816e09e0` + monitor-stream records, `0x816e23f0` / `0x816e2558` (string id, stream grow), `0x83c61ca0` / `0x83c61cb0` (static-init guard) | the global profiler byte `*(*0x84cb83b0)` set and P +0x70 not zero | Havok profiler |
| | `0x8170901c` | end of a chunk | work-range dealer (atomic) |
| 2 | `(*(T +8) +0xd70)->vtable[+0x28]` | A +24 not zero and M100 & 0x7fff | integration modifier (indirect) |
| 3, 4 | list 1 at `*(T +0x40)` +0x1210 (mask), entries (mask, object) from +0x1220, count +0x1218: vtable[+0xb8]; list 2 +0x1320 / +0x1330 / +0x1328: vtable[+0xc0] | `(M124 \| 2)` meets the list's mask | motion callbacks (indirect) |
| 5 | `(I +0x18)->vtable[+0x20]`, `[+0x28]` | first motion; every motion | cell set-up and cell lookup (indirect) |
| | `0x81830268` | deactivation on and not the zero case | deactivation check (direct, leaf) |
| | `0x816ef7bc` | a block full | block allocator |
| | `0x817d9d80` | every body | U3 (native) |
| 6, 7 | `(*0x84cb8168)->vtable[+0x20]` | the cache's two arrays, when allocated | heap free |
| | `0x8182fddc`, `0x8182ff20`, `0x8182fc94` | x7 not zero | stream-range records (another caller's path) |

With x7 not zero (not `0x8182f860`'s calls) the function also closes and reopens the writer's current range (T +0x730
.. +0x73c, the stream array T +0x768 / +0x770 of 80-byte entries) around each chunk and appends a 48-byte record per
chunk to the x7 array. No lock is taken by the function itself (the listener bits are atomic ORs; "locks" in 2.1's
summary were these and the dealer's atomic add).

### 9.6 The check of the motions' step, and the paths' histogram (unit `sil`)

The native step of one motion (9.3, steps 1 to 9 and the cell position of step 11) is `hl::math::motion_step`
(`hl/math/MotionStep.h`, `src/math/MotionStep.cpp`; the layouts in `hl/bridge/MathBridge.h`: the solver step data, the
velocity entries, the globals' new fields; the C surface `hl_math_motion_step`). It declines on values outside the
exactness rules and on the modifier's path. Its bridge in `engine/host/hl_math.c` (`run_sil`, gate `0x8182dc20`) is a
check only: with `HL_MATH_CHECK=1` (unit `sil`) every call from `0x8182f860` (return address `0x8182f958`, x7 = 0) copies
the motions of slots `first .. first + count - 1` at entry, runs the translated function for real, then compares each
motion's 128 bytes with the native step's and walks the records the call appended to the writer at T +0x520 (two clamp
records and a cell record per motion at most, in that order; a cell record's new cell, from the lookup the native side
does not answer, is taken as written). Motions with a callback (their M124 | 2 meets a list's mask) or a declined step
are counted and not compared, and the records after them are not walked. Nothing is native: the unit has no native mode
yet (9.7).

The first run found two mistakes of the first reading, both corrected in 9.3 and the code: steps 2 to 4 run only with A
+24 set, and the maxima's squares overflow. Results after them, PC 3050, build of `a0981ea`, `HL_MATH_CHECK=1
HL_MATH_UNITS=sil`, full 110 s runs (the last report of each run):

| scene | calls | x7 path (another caller, left) | motions | compared | declined: modifier | declined: values | mismatches (motions / records) |
|---|---:|---:|---:|---:|---:|---:|---:|
| castle-move | 3,673 | 608 | 173,771 | 159,823 | 13,661 | 287 | 0 / 0 |
| kokiri-idle | 3,787 | 492 | 180,769 | 168,827 | 11,574 | 368 | 0 / 0 |
| hyrule-walk | 2,308 | 505 | 93,740 | 84,454 | 8,712 | 574 | 0 / 0 |

(hyrule-walk's last report is from earlier in its run than the others'.) The same check on the main PC (build of
`48749bd`), full runs:

| scene | calls | x7 path | motions | compared | declined: modifier | declined: values | mismatches (motions / records) |
|---|---:|---:|---:|---:|---:|---:|---:|
| castle-move | 6,961 | 589 | 341,061 | 313,480 | 27,160 | 421 | 0 / 0 |
| kokiri-idle | 9,443 | 649 | 431,093 | 393,503 | 35,685 | 1,905 | 0 / 0 |
| hyrule-walk | 9,071 | 668 | 412,052 | 376,295 | 32,338 | 3,419 | 0 / 0 |

**The paths' histogram** (the same in the three
scenes): every call has a profiler holder P (so `0x800430a0` / `0x800431c8` run on every call), about one call in six
comes with x7 (the stream-range path, from the other caller); deactivation and the cell accumulators are on in every
call; no motion meets a callback list (the masks are 0); A +24 is set on 7-8% of the motions and each of them has the
integration modifier (`0x818f2b2c`), so **the clamps and the n path were never taken**; 55-72% of the motions move,
the deactivation's zero case is 40-55%; cell records are rare (about 300 a run, at the start), the writer's block never
filled. Values declined (0.1-0.8%): squares of velocities near 1e-23 that underflow (Arm flushes them under FZ). FPCR was
FZ only in every call. The indirect targets seen: cell set-up `0x8190ac00`, lookup `0x8190ad60`, modifier `0x818f2b2c`.

### 9.7 What a native `subIntegrateLastStep` needs (not built)

The check covers each motion's step and the clamp and cell records, byte for byte. A native answer of the whole
function also writes what the check does not compare yet: the bodies' flags (+0x5c), their cache entries, the listener
bits, the deactivation bits and records (through `0x81830268`), the cell accumulators, U3's results, and it must call,
in the guest's order, the profiler hooks (every call has P), the cell set-up and lookup (`0x8190ac00`, `0x8190ad60`,
every motion), the deactivation check, the modifier for the 8% of motions with one (or decline those calls), the block
allocator and the frees of the cell cache. Its check therefore has to replay: run the translation for real while gates
on those callees record their arguments and results, then run the native driver on the entry copies against the
recording and compare every write. That driver and its check are the next step; until they exist the unit stays a
check (`validated` 0), nothing of it is native.

The build with the gate (`5fe0855`, defaults: the five validated units native, `sil` off) passed the validation runs on
the main PC (`build/s5/valid.sh`, tag `s7cv`): the three scenes ran to the end (112 s), no crash, game speed ratio 59.73
(kokiri-idle), 60.06 (castle-move), 60.06 (hyrule-walk); the last shots of each scene look right. Memory: game RAM 2.6 /
3.1 / 3.0 GB, process RAM 6.0 / 7.0 / 6.7 GB (kokiri-idle / castle-move / hyrule-walk).

## 10. The contact solver's `solveJacobians` `0x8177b2a0` (stream 7, third session: partial)

Read: the entry, the walk over the Jacobian stream, the record header, the cache of the two bodies' velocities, the
three forms of the normal rows' loop (0x8177b9e0 .. 0x8177c634); not read: the friction and angular parts after the
normal rows (0x8177c634 .. 0x8177e0ec), the modifier and "fire modifiers" paths, the tail. Reached through the stream
contact solver's vtable only (no direct caller).

- **Arguments**: x1 the solver thread context (kept at sp +0x7b8), x2 the step info I (the same object as 9.1's x1:
  its +0x68 and +0x80 velocity arrays), x3 the solver parameters P (+0 an int, +8 a pair, +16 a flags byte: bit 1, 2, 3
  choose paths), x4 the first range of the Jacobian stream, x5 a pointer to the impulses' output (the accumulated
  impulse of each contact point is stored there, 4 bytes a point, in record order), x6 and x7 pointers to two ints,
  the first slots of the two velocity sets (`(I +0x68) + 32 · *x6` and `+ 32 · *x7`, the same for +0x80): a record's
  flag bit 0 chooses the set of its first body, the set after it holds its second.
- **The walk**: a range is (+0 block, +8 16-bit offset of the first record from block +32, +12 record count, +16 the next
  range, +24 a byte, the stream id, which must stay the one of the first range); records follow one another in the block
  (their size from their header and point count, not read).
- **The record header** (read): +0 16-bit flags (bit 0 the velocity set, 0x30 the friction kind, bit 6 "two bodies
  move" (the rows subtract the second body's velocity), bit 7 a record handled by `0x81785c00` (the "second Jacobian type"
  of 2.1), 0x300 the form of the normal rows (0, 0x100, 0x200; another value traps), bit 10, 11, 13, bit 14: set bit 5 of
  both bodies' entry flags word at +28); +2 a byte (bits 0-2: the record and its impulse pointer are appended to one, two or three of
  the frame's deferred lists, solved later through `0x81785c00`; bit 0 also a path at `0x8177c968`); +4 the number of contact points; +5 a byte (bits 1-3 material scaling of the rows from tables at
  `*0x84cc1a80` / `*0x84cc1a88`, bit 4 a path at `0x8177ce38`, bit 6 kept); +6 a byte (0xe0 = 0x60 with flag bit 10: a
  separate path); +7 a byte (a count scaled by I's pair at +0x200); +8 and +12 the two bodies' ids (compared with the
  previous record's); +16 a float4 (the normal, and in w a factor); +40 and +44 the two velocity slots; +64 on, 32 bytes a
  point (two float4: the point's angular Jacobians, the w lanes the right-hand side and the effective mass); after the
  points, the friction rows (+0x9c + 32 · points: a factor; +0xc0 + 32 · points: a float4).
- **The velocities' cache**: the two bodies' entries (A: V, W as in 9.2, and their packed maxima as floats) are loaded
  into a frame block at the start of each record (the second body's only with flag bit 6) and written back at its end
  (V and W.z at +0, W.x, W.y at +16; `0x8177cbd0`). The comparison of a record's body ids (+8, +12) with the previous
  record's decides only whether the deferred lists are solved (`0x81785c00`, on the previous pair) before the new record:
  so the deferred records of a pair are solved after all its direct records, and their order is part of the result.
- **After the normal rows**, with friction (flags 0x30) and P +16 bit 3 clear: the friction rows use integer "magic"
  steps (`0x7ef504f3`, `0x5f2fed52`, with `0xbf0e425b`, `0x3fc6fa83`: reciprocal and reciprocal square-root
  approximations by an integer subtraction and a polynomial refinement, exact operations a native version can
  reproduce), and FMIN / FMAX clamps against the globals' +0x1c0 and +0x260 pairs; not read further.
- **The normal rows** (each point in record order, Gauss-Seidel: each point's impulse changes the cached velocities
  before the next point is solved): the relative velocity along the point's Jacobians, `(J.ang0 · V0 + J.ang1 · W0) +
  n · (W0 - W1)` summed as `z + (x + y)`, plus the point's right-hand side; the impulse `Δ = max(-acc, eff · (rhs - v))` (a
  pair through FMAX against the accumulated impulse, the guest's operands in that order), the accumulated impulse at
  `*x5` updated, the velocities changed by `Δ` times the Jacobians and the inverse masses (4-lane products, then sums in
  the guest's order). The three forms differ in which body moves and in a clamp of the target velocity (`FMIN` with the
  solver step data's +0 and +0xd0 factors in the 0x100 and 0x200 forms).
- **Calls** (48 sites): `0x81775920` (set-up of the frame's modifier lists), `0x8169ed90` / `0x8169e414` / `0x8169d98c` /
  `0x8169e0cc` / `0x8169e1ec` (Havok's monitor / timer helpers, likely: not read), `0x81785c00` (5 sites: the records with
  bit 7 and the deferred lists, with the frame's cached velocities), `0x8178413c` (6: an array's reserve, when a deferred
  list is full), `0x81784840`, `0x81784a80`,
  `0x81784260`, `0x81784460`, `0x81784620` (row kinds of the paths not read), `0x8178d820` / `0x8178d870`, `0x816ef7bc`
  (the block allocator: an output stream), the profiler set (`0x816e09e0`, `0x800430a0`, `0x800431c8`, `0x816e23f0`,
  `0x816e2558`, the static-init guards), `0x8017188c` (phive), and 7 indirect calls (the modifiers' callbacks, "fire
  modifiers": 2.1).

Not yet: the friction and rolling rows' arithmetic, the paths by P's flags, what the output stream receives, the
records' size rule, the histogram of paths in the game. No native code for it.
