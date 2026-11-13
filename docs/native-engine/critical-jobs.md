# The jobs on the frame's critical chain, and the numeric kernels in them (castle-move, 2026-11-13)

What the jobs that make the job-group chain are, how each could be replaced natively (whole, by its kernels, or by
scheduling), what the 25 self-contained numeric kernels of `hot-functions.md` compute, and what replacing them can and
cannot do to the chain. Static reading of the user's local translation and re-aggregation of the three sampled runs of
`hot-functions.md` (`build/hotfn/r1..r3`); nothing was run. The graphics-command recording job 0x8006cd60 is covered
elsewhere and appears here only where it interacts. Guest addresses, offsets and sizes are of the supported build; no
translated code is quoted (`README.md`, rule 4).

Confidence words: **read** (established from the lifted code or the data), **likely** (shape plus strong context),
**guess** (shape alone). "J" is the sampled job-group chain (the main thread inside its job-group wait, lr 0x80080188), in
ms a frame, mean of the three runs; 5.80 ms in all. "Alone" is the part of a unit's J time during which no other executor
or module worker was running (from the trace's blocked intervals at each sample's instant).

## The answers

1. **The "numeric families" are Havok.** `hot-functions.md` found no strings in the hot functions and named family N
   "unidentified". The jobs' subtrees do form strings (Havok's own profiler and assert strings, kept in the binary): family
   N is **Havok Physics (hknp, the "2025_2 PhysicsClothNavigation" build)** driven by phive, and the "model/animation
   chain" 0x80166100 / 0x8016b320 is **Havok Cloth (hcl)**. On the chain: Havok Physics and phive 2.24 ms (39%), Havok
   Cloth 0.33 ms plus 0.12 ms of cloth run inline in the player's job; the actor stage jobs (gameplay) 1.25 ms; the
   graphics module's jobs 0.81 ms; the job system itself 0.38 ms.
2. **The player's job is one instance of an actor stage job**, 0x800b3ce0, run for many actors at once (up to six
   executors in one sampling round). The chain passes through its longest instance, whose subtree forms the player's
   strings. Of its 0.83 ms on the chain, **0.03 ms is the 25 kernels**; 0.27 ms is one animation-pose routine
   (0x801d7ac0 / 0x801d99a0), 0.12 ms Havok Cloth run inline, 0.07 ms Havok ray casts, 0.29 ms scattered actor logic over
   48 distinct leaf functions. It also waits 0.29 ms a frame for its own cloth child job (0x8016b320), which then runs
   alone on the chain.
3. **41% of the chain (2.40 ms) runs with every other job thread idle.** The longest such stretches are the graphics job
   0x8006cd60 (0.51), the Havok solver task 0x819b8e20 (0.375), the player's instance (0.23), the cloth child (0.15), the
   dispatcher (0.14), the Havok Jacobian-building task 0x819aeae8 (0.11), point queries 0x82bcfae0 (0.10).
4. **Under 3.2 ms is not reachable with the decided set.** All 25 kernels plus the two class-A jobs at 4x take the
   sampled chain from 5.80 to 4.87 ms in the most favourable reading, 5.33 ms in the strict one. Reaching ~3.0 ms needs the
   whole Havok Physics step, Havok Cloth and the actor stage jobs native at 4x, and even that rests on the favourable
   reading (table in "What the numbers allow").

## Method

- **Samples.** `extract.py` (scratch, below) re-read `build/hotfn/r{1,2,3}.samples.txt` and `.jt.txt` with the same
  attribution as `build/hotfn/analyze.py` (waits, `critpath.py` chain segments, the main thread's job-group waits) and
  kept, for every sample of the executors, module workers and main thread in each run's 6 s trace window: its `f_`
  stack, whether it waits, on the chain or not, the number of other job threads running at that instant, and its
  position inside the main thread's job-group wait. ms a frame = samples x (window / main thread's samples) / frames,
  divided by 3 for the mean. One sample is ~0.005 ms a frame in the mean: values under ~0.03 rest on a few samples.
- **Job** = the `f_` frame just inside the first dispatcher frame (the same rule as `hot-functions.md`). "(deeper than 16
  frames)" samples were given to the job most often seen above the same leaf function in complete stacks (0.219 of their
  0.288 ms go to 0x8191ee00).
- **Sizes.** Static closure = functions reachable by direct calls from the job (indirect calls not followed, so a lower
  bound of what the job can run; SDK functions included) with their guest instruction counts (`functions.json`).
  Sampled subtree = the distinct functions the samples show under the job (all samples, and the chain samples only).
- **Identification.** Strings the functions of each job's sampled subtree form (`index.pkl` / `strings.pkl` of the
  earlier agent); where a job's address is stored in data (`image.bin`) and what is stored around it.
- **Scenarios.** Every chain sample whose leaf is a replaced kernel, or whose job is a replaced job, is shortened by
  (1 - 1/s). Upper bound: all such samples shorten the chain. Strict bound: only the "alone" ones do (a unit running
  beside busy threads may shorten without shortening the chain).

Scratch scripts (untracked, in the session's scratchpad `cj/`): `extract.py`, `ana.py` (per job and kernel), `tl.py`
(timeline of the wait, instances), `kids.py` (children of a job), `sstr.py` (strings of sampled subtrees), `static.py`
(sizes, callers, data references), `scen.py` / `scen2.py` (subsystems, scenarios), `player.py`, `rd2.py` (reading view).

## The job-group wait as a timeline

Chain time by tenth of the job-group wait (pooled over the three runs; the long wait is ~6-7 ms, so a tenth is ~0.65 ms):

| tenth | chain ms | of which alone | largest jobs on the chain then |
|---:|---:|---:|---|
| 0 | 0.53 | 0.12 | 0x803749e0 physics step driver, 0x80150180 phive bodies, 0x8006cd60, 0x83aff1f0 stage registration, 0x8191ee00 narrow phase |
| 1 | 0.43 | 0.32 | 0x819b8e20 solver, 0x819aeae8 build Jacobians, 0x8006cd60 |
| 2 | 0.45 | 0.08 | 0x800b3ce0 actor stage (player instance), 0x8006cd60, 0x80150180 |
| 3 | 0.47 | 0.08 | 0x800b3ce0, 0x8006cd60, 0x8042b0c0 |
| 4 | 0.51 | 0.32 | 0x8016b320 cloth (alone), 0x800b3ce0 |
| 5 | 0.60 | 0.24 | 0x803749e0, 0x800b3ce0, 0x80166100 cloth |
| 6 | 0.65 | 0.17 | 0x803749e0, 0x8191ee00, 0x8006cd60, 0x81a42cd8 broad phase |
| 7 | 0.66 | 0.26 | 0x8191ee00 (and its deep stacks), 0x819b8e20 |
| 8 | 0.81 | 0.55 | 0x819b8e20, 0x83af59c0 actor stage, 0x8006cd60, 0x82bcfae0 point queries, 0x800493e0 |
| 9 | 0.67 | 0.25 | 0x83af59c0, 0x82c43780, 0x8006cd60, the dispatcher, 0x80117320 |

The Havok physics jobs appear twice in a wait: at tenths 0-1 (0x803749e0, 0x8191ee00, 0x819aeae8, 0x819b8e20) and at
tenths 5-8 (0x803749e0, broad phase 0x81a42cd8 / 0x81a191a0, 0x8191ee00, 0x819b8e20). Whether that is two worlds, two
substeps or one step spread over the wait is **not established**.

## Per job

### Summary

Ranked by J. Sizes: functions / guest instructions. "Inst." = how many threads ran the job in the same sampling round
(max, and the share of rounds with two or more).

| job | what (evidence) | reached | static closure | sampled subtree (all / on chain) | J (min-max) | CPU | alone | others running (mean) | tenths | inst. | class |
|---|---|---|---|---|---|---:|---:|---:|---|---|---|
| 0x800b3ce0 | actor stage job; calls the actor virtual 0x82c3fab0 -> 0x82c415e0; the chain instance is the player's (read / likely) | pointer: a 5-word record at 0x84c7bf10 | 397 / 44,136 | 209 / 235,294; 119 / 179,414 | 0.831 (0.59-1.18) | 4.22 | 0.23 | 3.35 | 2-5 | 6, 62% | B (+C) |
| 0x803749e0 | phive / hknp world step driver: hknpWorldEx::preCollide, hknpWorld::generateCollideTasks, "[Phive] post collide / post solve", per-body next_position / next_rot (read) | pointer: address formed by 0x806a48f0 and pushed as a job | 668 / 76,717 | 44 / 14,944; 30 / 13,236 | 0.478 (0.45-0.51) | 0.77 | 0.02 | 3.42 | 0, 5-6 | 1 | B |
| 0x819b8e20 | hknp solver task: gatherSolverVelocities, solve Jacobians / constraints (0x819ba420), subIntegrate / subIntegrateLastStep (0x8182d2c0 / 0x8182dc20) (read) | pointer: vtable slot 0x848d9e80 | 164 / 16,532 | 33 / 18,743; 31 / 18,202 | 0.409 (0.32-0.55) | 0.52 | 0.375 | 0.06 | 1, 7-8 | 1 | A (+C) |
| 0x8191ee00 | hknpProcessNarrowPhasePairsTask::process; body pairs through mergeAndCollide2Streams (0x81928de0) (read) | pointer: vtable slot 0x848d7bb8; one direct site in 0x8191fdac | 208 / 23,490 | 79 / 32,613; 46 / 26,177 | 0.590 incl. 0.219 of deep stacks (0.365 without) | 1.77 | 0.05 | 3.85 | 0, 6-7 | 6, 72% | B |
| 0x83af59c0 | actor stage job; calls the actor virtual 0x82c43780 -> 0x800cdbe0 (actor update): model bounds, transforms (read) | pointer: records at 0x84c7bf38 and 0x84cd9670 | 1 / 484 (its work is behind 2 indirect calls) | 81 / 64,562; 39 / 47,601 | 0.321 (0.24-0.42) | 1.62 | 0.03 | 4.29 | 8-9 | 6, 76% | B |
| 0x8016b320 | Havok Cloth: hclWorld::stepSingleThreaded (0x8016b620) of one cloth world (read) | pointer: 0x84c70b28 | 239 / 21,759 | 45 / 79,304; 24 / 51,922 | 0.245 (0.13-0.31) | 1.05 | 0.15 | 1.38 | 4 | 5, 38% | B (+C) |
| 0x82bcfae0 | hknp point queries: hknpWorld::queryPoint, getClosestPoints, through 0x803aa320 (read) | pointer: record at 0x84b37ed0 | 1 / 187 | 32 / 22,716; 24 / 20,867 | 0.112 (0.09-0.15) | 0.24 | 0.10 | 0.48 | 8 | 3, 39% | C / B |
| 0x819aeae8 | hknpNarrowPhasePairsBuildJacobians::process (second-round results, deferred manifolds) (read) | pointer: vtable slot 0x848d9d20; one direct site in 0x819aff3c | 183 / 19,411 | 27 / 22,706; 23 / 21,765 | 0.112 (0.07-0.16) | 0.20 | 0.11 | 0.04 | 1 | 1 | A (+C) |
| 0x800493e0 | executor job of the sub-runner 0x80049280: own body (716 insns, 15 indirect calls), object lookups; no strings (guess) | direct from 0x80049280; also stored at 0x84c57310, 0x84cd7ef8 | 202 / 19,040 | 4 / 859; 3 / 787 | 0.108 (0.06-0.16) | 0.23 | 0.05 | 2.68 | 8 | 5, 68% | C (unread) |
| 0x80150180 | phive rigid bodies / character motion: next_linear_velocity, next_rot, ray casts (read) | pointer: 0x84c56840 | 329 / 29,024 | 25 / 18,235; 17 / 9,925 | 0.103 (0.09-0.12) | 0.27 | 0.005 | 4.29 | 0-2 | 4 | B |
| 0x82c43780 | the actor virtual of 0x83af59c0 run as a job of its own (read) | pointer: vtable 0x84b45fc0 | 2,103 / 290,318 | 39 / 45,599; 15 / 24,468 | 0.102 (0.04-0.17) | 0.47 | 0.005 | 3.0 | 9 | 5, 67% | B |
| 0x83aff1f0 | job system: stage worker registration (0x806b0e80, 0x806b0260), run inside the main thread's module job runner (read) | pointer: 0x84c7c9c8 | 609 / 83,840 | 29 / 12,604; 16 / 5,759 | 0.088 (0.04-0.15) | 0.19 | 0.07 | 0.17 | 0 | 1 | C (native job system) |
| 0x8007c120 | graphics module: GPU memory invalidate; its chain time is the native back end's wait under 0x80354060 (read) | direct from 0x8007fe60, 0x80083960 | 403 / 44,508 | 4 / 2,338; 3 / 2,053 | 0.083 (0.04-0.13) | 1.56 | 0.03 | 1.53 | spread | 3, 78% | not CPU code |
| 0x80166100 | Havok Cloth, the other cloth worlds (parallel bulk) (read) | pointer: 0x84b46418 | 486 / 50,232 | 61 / 109,994; 23 / 56,999 | 0.083 (0.03-0.16) | 2.38 | 0.00 | 3.59 | 5-6 | 5, 52% | B |
| 0x8006cd60 | graphics recording (other agent) | direct | 927 / 169,007 | 179 / 125,645; 97 / 79,970 | 0.629 (+0.020 deep) | 2.25 | 0.49 | 0.82 | all | 2 | - |
| (deeper than 16 frames) | 0.219 to 0x8191ee00, 0.020 to 0x8006cd60, the rest unknown | - | - | - | 0.288 | 3.99 | 0.06 | 3.42 | 7 | - | - |
| (the dispatcher) | 0x8004ab40 itself 0.204, 0x8007fe60 0.034, sub-runner 0x80049280 0.020 | - | - | - | 0.273 (0.18-0.35) | 1.74 | 0.14 | 1.50 | spread | - | C (native job system) |

How each is reached matters for installing a replacement: every job except 0x800493e0, 0x8007c120 and 0x8006cd60 is
called by the dispatcher through a pointer held in data, so `a64_lookup_replaced` can answer it at its address with no
new lift. The five-word records holding 0x800b3ce0, 0x83af59c0, 0x82bcfae0 and 0x80166100 all begin with the word
0x80b25110 followed by a code address, then the job function, then two zero words: a delegate object (a vtable, then a
member-function pointer) is the likely reading (likely). The family-N jobs are slots of C++ vtables (entries
0xffffffffffffffe0 beside them: an offset-to-top of -0x20), i.e. `hkTask::process` overrides; 0x8191ee00 says so in its
own strings. 0x8191ee00 and 0x819aeae8 also have one direct caller each, which a pointer-answered replacement would not
catch (those direct calls were not seen in the samples).

Arguments: the dispatcher passes the job object in x0 (`hos.h`, JT_JOB_B). For the Havok tasks that is the task object
(`this`) and x1 the `hkTask::Input` (from the signature string; likely). For the delegate-record jobs, x0 is the job
object holding the delegate; which actor or world each instance acts on is not read.

### 0x800b3ce0: the actor stage job (the player's instance), class B, partly C

- **Shape.** 1,028 instructions of its own; it calls the actor virtual 0x82c3fab0 (a slot of the table at 0x84b45f80,
  which also holds 0x82c43780), which calls 0x82c415e0 (0.669 of the job's 0.831 ms on the chain) and 0x803d4840
  ("CrumblingGanonCastle", 0.073). Statically reachable from 0x82c3fab0 by direct calls alone: 4,005 functions, 657,555
  instructions. The samples hit 209 functions under the job (119 on the chain).
- **Why it is on the chain.** It is the body of a stage that runs on all six executors at once (of the 271 sampling
  rounds in which it ran, 50 had six instances and 169 two or more). While the chain is in it, five other job threads are busy in 109 of 170 samples and none in 47 (0.23 ms
  alone): the chain is the instance that ends last. That instance's subtree forms the player's strings (player game
  parameters, climb / crouch / ladder state names, foot bone names Ankle_L, Toe_L, lock-on camera parameters): the long
  instance is the player's (likely). It does not make everything else wait in the sense of a barrier; the stage ends when
  its last instance ends, and the stage's end is on the chain.
- **It waits for its own children.** 0.289 ms a frame of its samples are waits (condition variable 0.171, event 0.118),
  all inside the job-group wait and all under 0x80168320, the function that starts Havok Cloth steps and waits for them
  (`jobs-boundary.md`, rule 5). The chain follows the hand-off to the cloth job 0x8016b320, which runs at tenth 4, alone
  in 30 of its 50 chain samples. So the player's chain is: its own work -> its cloth child (alone) -> its own work.
- **The 0.831 ms on the chain, by part of the subtree** (each sample counted once, first match in this order):

  | part | J ms | share |
  |---|---:|---:|
  | animation-pose routine 0x801d7ac0 -> 0x801d99a0 ("UseRotatedTranslate"; 14,411 insns, 421 direct call sites to 36 functions, 41 indirect, ~70 calls to sqrt) | 0.274 | 33% |
  | Havok Cloth stepped inline (0x80168320, 0x8016b620, hclSimulateOperator) | 0.117 | 14% |
  | Havok ray casts (hknpWorld::castRay 0x818f4f28, 0x81a20b40, 0x818623a0) | 0.073 | 9% |
  | 0x80341840 / 0x80162aa0 (FP-heavy, unidentified) | 0.044 | 5% |
  | foot / bone targets (0x8058cb20, 0x801881a0) | 0.019 | 2% |
  | 0x803d4840 | 0.015 | 2% |
  | the rest: actor logic, 48 distinct leaf functions, none above 0.02 | 0.288 | 35% |
  | of all the above, in the 25 kernels (0x81501180, 0x81b8c000) | 0.029 | 3.5% |

  By kind: lifted code 0.78, FP exactness helpers 0.04, native 0.01.
- **Reads and writes**: the actor object behind the delegate; not read further.
- **Class B.** Thousands of functions of gameplay; nothing in it is replaceable whole. Kernel-like parts: the
  animation-pose routine (0.27 ms, but not self-contained: 421 call sites, 41 indirect calls, the libm sqrt), cloth (its
  own subsystem, below), ray casts (physics). The 25 kernels are 3.5% of it. **C aspect**: its cloth wait is a true
  dependency as far as can be seen (the cloth step needs this frame's pose; inference), so running the cloth earlier is
  not available; making the cloth step itself faster is.

### 0x803749e0: the physics step driver, class B

- Its chain time: 0x8037adc0 -> 0x804f5030 (0.341; 3,467 instructions, 102 direct calls, 48 indirect, strings
  next_position / next_rot: phive's per-body write-back, likely), 0x8037adc0 itself (0.054), 0x803799c0 -> 0x803798c0
  (0.044, "next_center_of_mass_local"), 0x80372000 -> 0x80372ad4 (0.034: "[Phive] post collide/solve" signals and
  hknpWorld::generateCollideTasks), 0x8014aad0 (0.073 as a leaf: FP with exact 64-bit FMA helpers), 0x817da900 (0.053).
- 22 distinct leaves on the chain, 3 make half of it. Of its 0.478 ms, 0.010 is in the 25 kernels.
- Not alone (3.4 other threads busy on average); bimodal in the wait (tenth 0, and 5-6).
- Reads / writes: phive's body records and Havok's world (next position, rotation, center of mass); offsets not read.
- **Class B.** It is the boundary between phive (Nintendo's physics layer, `sead/Phive`) and hknp: per-body logic with
  virtual calls. It belongs to the physics module of the plan (`README.md`, step 6), not to a kernel replacement.

### 0x819b8e20: the Havok solver task, class A (bounded, numeric), and C

- Children on the chain: 0x8182f860 -> 0x8182dc20 subIntegrateLastStep (0.175), 0x819ba420 SolveJacobians /
  SolveConstraints -> 0x8177b2a0 hknpStreamContactSolver::solveJacobians (0.175), 0x8182f520 -> 0x8182d2c0 subIntegrate
  (0.029), 0x817d8300 (0.010). Leaves: 0x8177b2a0 0.097, 0x817d9d80 0.068, 0x8182d2c0 0.024, kernels 0x81746440 0.024
  and 0x81a040bc 0.020.
- 31 functions, 18,202 instructions on the chain; statically 69 game functions plus Havok's common SDK helpers (memory,
  asserts). No locks of its own were sampled; no waits.
- **Alone in 77 of 82 chain samples** (0.375 of 0.409 ms), one instance only. It runs twice in the wait (tenth 1 and
  tenths 7-8), each time with nothing else to run beside it: it is the serial point of the physics step (likely: one
  solver task for the scene's islands).
- Reads / writes: `hknpSimulationThreadContext`, `hknpSolverStepInfo`, the solver's velocity and Jacobian streams, body
  motions (names from its strings; layouts not read).
- **Class A**: bounded subtree, numeric, clear role, reached through a vtable (no new lift). Not "days": the inputs and
  outputs are Havok's internal stream formats, and a physics step is chaotic, so it must be bit-exact. **C**: a native
  solver could run independent islands on more than six threads (native work is not held to the six lanes); splitting one
  island's Gauss-Seidel order would change results.

### 0x8191ee00: the narrow-phase pairs task, class B

- All its chain time goes through 0x81928de0 (mergeAndCollide2StreamsIntern / collide_bodyPairs) into the shape-pair
  collision routines (0x8197a620 0.132, 0x8197a22c 0.112, 0x81902240 0.064, 0x8176c980 0.024; strings: convex-convex,
  GSK, GSK penetration, face clipping, capsule-capsule, convex-plane, hierarchical convex).
- Up to six instances at once (72% of rounds with two or more); on the chain with 3.9 others busy on average. Its deep
  stacks (another 0.219 ms) are in 0x8173a830, 0x8173b340, 0x81746440, 0x817472e0, 0x8173ab70.
- Kernels of the 25 in it (own + deep): 0x8173a830 0.063, 0x81746440 0.078, 0x817472e0 0.058, 0x81779980 0.039,
  0x8173ab70 0.019, 0x817036a0 0.010: about 0.27 of its 0.59 ms (45%).
- **Class B** with a high kernel share. Whole replacement means Havok's narrow phase (every shape-pair routine, the
  manifold and cache streams): weeks to months.

### 0x83af59c0: the late actor stage job, class B with a large kernel share

- 0x82c43780 -> 0x800cdbe0 (actor update, 0.214 of 0.321) -> 0x800d43d0 -> 0x831e65d0 -> **0x800a8400** (bounding
  sphere, 0.107 here), and the transform virtual **0x80ba37a0** (pointer-reached, stored at 0x84b41c50, 0x84c28fb8,
  0x84c775d0; 3,444 instructions, 10 indirect calls) -> **0x80ba6d80** (AABB transform, 0.058 here).
- Kernels 0.165 + 0x80ba37a0 itself 0.039: 0.20 of its 0.32 ms (63%) is numeric work that can be specified.
- Runs at the very end of the wait (tenths 8-9) on up to six executors (76% of rounds with two or more), 4.3 others busy
  on average: parallel bulk at the end; its end is the wait's end.
- **Class B.** The kernels are replaceable; the actor update around them is gameplay.

### 0x8016b320: one Havok Cloth world, class B, partly C

- 0x8016b620 hclWorld::stepSingleThreaded -> 0x81b47080 hclSimulateOperator::_execute (0.230 of 0.245): solve
  (0x81b56de0), collide-and-solve (0x81b58720, 0x81b57a80 -> kernel 0x81b8c000), link / local-range / bend constraints
  (0x81c86720, 0x81c67fa0 -> 0x81c7bd00, 0x81c7ecc0 -> 0x81c7f880), skinning (0x81db9a40 -> 0x81cdff40).
- Kernels of the 25: 0.083 of 0.245 (34%); with the constraint-set `apply` functions (0x81c86720 0.054) the numeric
  share is about half.
- Alone in 30 of 50 chain samples, at tenth 4, while the player's job waits for it (above). Several cloth jobs run at
  once (up to five); the one alone is the last, the player's (likely).
- **Class B**, and C only within limits: one cloth world's operators are sequential (inference), so the chain gains only
  by making that world faster. Havok Cloth is a self-contained middleware module (operators over particle buffers), a
  better unit than its kernels for a later step.

### 0x82bcfae0: point queries, class C / B

- 0x803aa320 -> 0x803aaef0 (0.073), 0x803a9020 (0.034): hknpWorld::queryPoint, hknpShapeQueryInterface::queryPoint /
  getClosestPoints, a debug format "pos / normal / hit_body".
- Alone 0.098 of 0.112, at tenth 8 (after the second solver run). Whether it needs the step's results (and so cannot move
  earlier) is not established; queries after the step would.

### 0x819aeae8: building Jacobians, class A (bounded) and C

- hknpNarrowPhasePairsBuildJacobians::process, finishing the narrow phase's second round and deferred manifolds; 23
  functions, 21,765 instructions on the chain; leaves spread (0x8176a780 0.034).
- Alone 0.107 of 0.112, early (tenth 1), right before the solver's first run. Same remarks as 0x819b8e20.

### The other jobs down to 80% of the chain

- **0x800493e0** (0.108): its own body is 0.098 of it, with runtime lookups 0.039 (indirect calls). No strings; not read.
  Runs at tenth 8 on up to five executors. Class not decided (reading needed); small enough (716 instructions, 4 sampled
  functions) to be A if its indirect calls turn out to be few kinds.
- **0x80150180** (0.103): phive bodies (next_linear_velocity) and ray casts; logic, class B; early in the wait with 4.3
  others busy.
- **0x82c43780** (0.102): the same actor virtual as in 0x83af59c0, at the end of the wait. Class B.
- **0x83aff1f0** (0.088): the job system's stage registration (`jobs-boundary.md`: 0x806b0260 and its loads), run by the
  main thread's module job runner at the start of the wait, alone. Class C: it goes away with the native job system's
  stage protocol, not with a kernel.
- **0x8007c120** (0.083): a GPU memory invalidate whose time is the native back end's wait. Not CPU code to replace; the
  GPU back end's synchronisation.
- **0x80166100** (0.083 of 2.38 ms CPU): the other cloth worlds, parallel bulk. Class B.
- **The dispatcher** (0.273): 0x8004ab40's own code 0.155, native (event signal, wake) 0.083, runtime 0.035. Class C: the
  native job system.

### What the jobs wait for and what comes before them

Only waits were sampled, not the job objects, so predecessors are read from the timeline (inference):

- physics, per pass: step driver 0x803749e0 (pre-collide, collide tasks) -> broad phase (0x81a42cd8, 0x81a191a0: simd
  tree collide) -> narrow phase 0x8191ee00 (parallel) -> build Jacobians 0x819aeae8 (early pass only) -> solver 0x819b8e20
  (alone) -> phive post-solve in 0x803749e0 -> point queries 0x82bcfae0;
- actor stage 0x800b3ce0 (tenths 2-5) -> its cloth child 0x8016b320 (tenth 4, alone) -> back to 0x800b3ce0;
- actor stage 0x83af59c0 / 0x82c43780 last (tenths 8-9).

Sampled waits inside jobs: only 0x800b3ce0's cloth wait (0.289 ms) and 0.015 ms in deep stacks under 0x80046180. No job
on the list was seen waiting on a lock.

## The 25 kernels

From the top-100 table of `hot-functions.md` (kind "kernel"). Callers: direct call sites / functions over the whole lift;
"data" = the address is stored in data (a vtable or table), so it is reached through a pointer. A directly called
kernel needs the lift's hook list and a new lift; a data-only one can be answered by `a64_lookup_replaced`. CPU = guest
self ms a frame (`hot-functions.md`); J = its self time on the job-group chain; "alone" = the part of J with no other job
thread running. Context = strings formed in the six frames above it in the samples.

| # | kernel | insns | callers | context (job, strings above) | what it computes | args / results as read | FP and exactness | engine structures | J (alone) | CPU | spec confidence |
|---:|---|---:|---|---|---|---|---|---|---|---:|---|
| 1 | 0x800a8400 | 217 | 8 / 4 direct (0x831e65d0 96% of samples, 0x8009bb80, 0x800ac970, 0x835b97a0) | actor stage 0x83af59c0 via 0x831e65d0; culling 0x8009bb80 | world bounding sphere of a model: for each local sphere (centre + radius, 16-byte records) take its bone matrix (64-byte matrices, by a 16-bit index), transform the centre, scale the radius by the largest axis length of the matrix, and grow one enclosing sphere step by step (move the centre and enlarge when a sphere lies outside) | x0 = bounds object: +0 descriptor (its +32 bone index list, +64 sphere records, +84 / +88 16-bit counts, +90 flag), +56 the output sphere (centre at +0, radius at +16); x1+32 = the matrix palette; in place, no return value | FMA (fused multiply-add, as the guest), sqrt; a libm sqrtf call on the NaN path only; order-dependent (sequential growth: keep the order) | yes: the model's bounds object and its descriptor (offsets above) | 0.131 (0.010) | 0.539 | read |
| 2 | 0x81746440 | 124 | 2 / 2 direct (0x8173e24c, 0x8173e2ac) | hknp: solver (subIntegrateLast, gatherSolverVelocities), narrow phase, getClosestPoints | AABB of a convex vertex set under a transform: rotate N float3 vertices by a 3x3 matrix, component min / max, then add the translation in double precision and round outward to float | x0 = a relative array (int32 offset from x0, int32 count) of 12-byte vertices; x1 = transform (rows at +0, +16, +32; translation and extra terms at +48..+72); two stores | FMIN / FMAX NaN and signed-zero rules; float -> double -> float conversions with directed rounding (bit adjustments around the conversion): must be reproduced exactly | the shape's vertex array and the body transform only | 0.112 (0.029) | 0.383 | read |
| 3 | 0x80ba6d80 | 23 | 9 / 6 direct (0x80ba37a0, 0x835b6f90, 0x835b80e0, 0x808a17d0, 0x83bf29b0, 0x808109c0) | actor stage 0x83af59c0 via the transform virtual 0x80ba37a0 | transform an AABB given as centre and half extents by a 3x4 affine matrix: centre' = M c + t, extent' = abs(M) e; write min and max | x0 = out (min, max: two float4, 32 bytes); x1 = box (centre float3 at +0, half extents at +12); x2 = matrix (four float4 columns) | FMA as the guest; abs by sign-bit clear | none (plain values) | 0.068 (0.005) | 0.223 | read (certain) |
| 4 | 0x8173a830 | 207 | 4 / 3 direct (0x81738fa0, 0x8173a110, 0x8173a5c0) | hknp narrow phase (deep stacks), collide_bodyPairs | one face of an expanding polytope: from three vertex indices of face i, the edges, cross-product normal, its length (sqrt), the plane distance; writes the face's plane and flags into the polytope state (penetration depth, EPA-style; what exactly: likely) | x0 = a large solver state (vertices at +2256, 16 bytes each; face vertex indices as bytes at +3426 + 4i; face records written at +5472 + 16i, +0x1d60, +9568 + 4i, +0x2b60); x1 = face index | float and double mixed; sqrt, divide | yes: Havok's internal polytope state | 0.063 (0.010) | 0.358 | likely |
| 5 | 0x817472e0 | 186 | 1 / 1 direct (0x81743538) + 1 indirect call inside | hknp narrow phase | support query of a small convex (1 to 4 vertices, chosen by a jump table on the count): dot products of a direction with the vertices, the largest (likely) | x0 = relative array (offset, count) of vertices; x1 = direction (float4) | float only | Havok shape data | 0.058 (0.005) | 0.251 | likely |
| 6 | 0x81b8c000 | 17,427 | 2 / 1 direct (0x81b5a3c0) | Havok Cloth collide-and-solve: _iterateCollidables | cloth particles against collidables (8,416 vector operations, 30 reciprocal-square-root steps, 12 indirect calls: per collidable type, likely) | not read | FRSQRTE / FRSQRTS estimates (x86's estimate differs: the ARM table must be emulated) | Havok Cloth buffers | 0.078 (0.069) | 0.335 | guess |
| 7 | 0x81a33440 | 1,141 | 4 / 3 direct (0x81a15ec0, 0x81a12220, 0x81a19ca0) | hknp broad phase: SimdTree CollideTrees | integer tree-versus-tree pair collection, writing pair outputs; 5 indirect calls (not a pure kernel) | not read | no FP | Havok broad-phase tree | 0.068 (0.024) | 0.088 | guess |
| 8 | 0x81779980 | 1,413 | 2 / 2 direct (0x8177f720, 0x8177b020) | hknp contact solver (under solveJacobians) and narrow phase | contact Jacobian work, 4-wide (263 vector helper sites, double add / sub; likely) | not read | double and float | Jacobian streams | 0.049 (0.020) | 0.243 | guess |
| 9 | 0x81a040bc | 128 | data only (0x84cbfcc8) | hknp solver integrate (0x8173b340, 0x817d9d80) | a body's AABB expanded by its motion (calls 0x8170341c; double adds, outward rounding as in #2; likely) | x0 = body (+104, +128..+160 read) | directed rounding through double | Havok body | 0.029 (0.020) | 0.070 | guess |
| 10 | 0x81c7bd00 | 253 | 1 / 1 (0x81c67fa0, hclLocalRangeConstraintSet::apply) | Havok Cloth | local range constraints over particles (likely, from the caller's string) | not read | reciprocal square-root estimate steps | cloth particle buffers | 0.015 (0) | 0.174 | guess |
| 11 | 0x81501180 | 156 | 1 / 1 (0x81500960) | actor update 0x802ca820, player job | loop, 3 divides, 33 vector helpers, 30 stores | not read | divide | not read | 0.019 (0) | 0.206 | guess |
| 12 | 0x8173ab70 | 158 | 1 / 1 (0x8173ae00) | hknp narrow phase | double-precision geometry (mul / div / add in double) | not read | double | not read | 0.019 (0) | 0.088 | guess |
| 13 | 0x814f9460 | 267 | 1 / 1 (0x802c8380) | actor update 0x802ca820 ("CommandUseIK") | loop with sqrt (libm call twice), 31 stores: bone-chain update (guess) | not read | sqrt | not read | 0.015 (0) | 0.420 | guess |
| 14 | 0x81a2bba0 | 553 | 3 / 1 (0x81a2ae80) | hkcd simd tree optimise | integer tree rebalance step; 3 indirect calls | not read | none | Havok tree | 0.015 (0) | 0.086 | guess |
| 15 | 0x81c7f880 | 167 | 1 / 1 (0x81c7ecc0, hclBendStiffnessConstraintSet::apply) | Havok Cloth | bend-stiffness constraints (likely, from the caller) | not read | rsqrt estimate steps | cloth buffers | 0.010 (0) | 0.337 | guess |
| 16 | 0x817036a0 | 111 | 5 / 4 direct | hknp | double-precision vector arithmetic | not read | double | not read | 0.010 (0) | 0.080 | guess |
| 17 | 0x80029240 | 42 | 14 / 6 direct (culling and render recording) | culling 0x8009bb80, render recording | box against a plane list: quick reject on two masks, then for each plane (32-byte records, count at +224) pick the box corner along the plane's normal by a per-plane sign mask and test its signed distance; returns 0 at the first plane the box is outside, 1 otherwise | x0 = plane set (+0, +16 masks, planes from +32, +224 count, +228 flag); x1 = box (two float4); returns w0; no stores | float adds and multiplies | the culling plane set | 0.005 (0.005) | 0.212 | read |
| 18 | 0x81cdff40 | 1,721 | 60 / 4 direct (0x81d57f20, 0x81d90bc0, 0x81d3d4e0, 0x81db9a40) | hclObjectSpaceSkinPOperator | object-space skinning of cloth vertices (714 vector operations, no calls; likely) | not read | vector float | cloth buffers | 0.005 (0) | 0.091 | likely (what), not read |
| 19 | 0x81b3a558 | 202 | 1 / 1 (0x81b47080) | hclSimulateOperator::_execute | per-substep numeric step (rsqrt steps) | not read | rsqrt estimate | cloth | 0.005 (0) | 0.080 | guess |
| 20 | 0x808226e0 | 1,466 | 1 / 1 (0x80822260) | culling job (module workers) | bounds / LOD math (63 loops, 116 FMAs) | not read | FMA, divide | yes | 0 | 0.365 | guess |
| 21 | 0x814f59a0 | 43 | data only (0x8489a7f8) | last stage, "CommandUseIK" | small loop calling 0x814f6c60 | not read | none | not read | 0 | 0.231 | guess |
| 22 | 0x814efa00 | 56 | data only (0x8489a680) | last stage | 19 FP ops (10 FMA, 1 divide), no loop: an interpolation (guess) | not read | FMA | not read | 0 | 0.186 | guess |
| 23 | 0x814ef920 | 56 | data only (0x8489a670) | last stage | the same shape as #22 | not read | FMA | not read | 0 | 0.148 | guess |
| 24 | 0x800a8120 | 173 | 17 / 6 direct (render recording, culling) | render recording | small loop (10 back edges) | not read | float adds | not read | 0 | 0.107 | guess |
| 25 | 0x81500200 | 215 | 1 / 1 (0x8002a5a0) | culling job | vector loop calling 0x800a8df0, 0x800a9510 | not read | vector | not read | 0 | 0.080 | guess |

Together: 0.775 ms of the chain (0.195 alone), 5.4 ms of CPU.

**Ranking by chain ms x confidence** (read 1.0, likely 0.7, guess 0.3): 0x800a8400 0.131, 0x81746440 0.112, 0x80ba6d80
0.068, 0x8173a830 0.044, 0x817472e0 0.041, 0x81b8c000 0.023, 0x81a33440 0.020, 0x81779980 0.015, 0x81a040bc 0.009,
0x81501180 / 0x8173ab70 0.006. The rest are under 0.005 or zero on the job-group chain (0x808226e0, 0x814f59a0, 0x814efa00,
0x814ef920, 0x800a8120, 0x81500200 are never on it: they run in the culling and render jobs or the last stage).

**Exactness, for all of them.** The translation reproduces AArch64 floating point exactly (`a64_fpx_*`, `a64_vfpx_*`:
fused multiply-add as one rounding, FPCR's flush-to-zero and default-NaN modes, FMIN / FMAX NaN and signed-zero rules,
FRSQRTE / FRECPE estimate tables). A native AVX2 version is bit-exact only if it uses FMA where the guest fuses and
separate operations where it does not, keeps the guest's operation order (no reassociation, no contraction by the
compiler: `-ffp-contract=off`), emulates FMIN / FMAX (x86 `minps` returns the second operand on NaN), emulates the ARM
reciprocal-square-root estimate (x86 `rsqrtps` differs) in #6, #10, #15, #19, and matches flush-to-zero (ARM flushes
inputs and outputs: MXCSR needs both DAZ and FTZ, and the two differ on what counts as tiny; a difference to look for in
the differential runs). Whether callers compare results bit-exactly was not read; Havok's physics and cloth are chaotic
(a last-bit difference grows over frames), so physics and cloth kernels must be bit-exact; the bounding sphere and AABB
of the actor stage feed culling and are tolerant in effect, but a bit-exact replacement is the only one that can be
checked automatically.

## What the numbers allow

Sampled job-group chain 5.80 ms (under the sampler; the main thread's job-group wait was 7.6-8.1 ms in these runs,
6.9-7.3 ms unsampled). Every replaced unit's chain samples shortened by (1 - 1/s). "Upper" counts all of them; "strict"
counts only those when no other job thread ran. "Floor" = the chain with the units taking no time.

| replaced | units' chain ms | 2x upper (strict) | 4x upper (strict) | floor |
|---|---:|---:|---:|---:|
| nothing | - | 5.80 | 5.80 | 5.80 |
| the 5 fully read kernels (0x800a8400, 0x81746440, 0x80ba6d80, 0x817472e0, 0x80029240) | 0.375 | 5.61 (5.77) | 5.52 (5.76) | 5.42 |
| all 25 kernels | 0.775 | 5.41 (5.70) | 5.22 (5.65) | 5.02 |
| class-A jobs (0x819b8e20, 0x819aeae8) | 0.521 | 5.54 (5.56) | 5.41 (5.44) | 5.28 |
| **all 25 kernels + class-A jobs (the decided set)** | 1.237 | **5.18 (5.49)** | **4.87 (5.33)** | 4.56 |
| + the rest of Havok Physics / phive whole (0x803749e0, 0x8191ee00, 0x82bcfae0, 0x80150180, broad phase, ...) | 2.451 | 4.57 (5.38) | 3.96 (5.16) | 3.35 |
| + Havok Cloth whole (0x8016b320, 0x80166100) | 2.681 | 4.46 (5.33) | 3.79 (5.10) | 3.12 |
| + the actor stage jobs whole (0x800b3ce0, 0x83af59c0, 0x82c43780) | 3.726 | 3.94 (5.21) | 3.00 (4.91) | 2.07 |

**What remains after the decided set** (4.56 ms even at infinite speed), by subsystem with the kernels and the two
tasks taken out: Havok Physics and phive 1.36 ms, the actor stage jobs 1.05, the graphics module's jobs 0.81 (0x8006cd60
0.65), other jobs 0.72, the job system 0.38 (dispatcher 0.27, stage registration 0.09), Havok Cloth 0.23, native time
with no guest frame 0.03. Outside the sampled chain, the job-group
wait also holds 1.3-2 ms of hand-offs, polls and core waits (whole-frame chain in r2: polls with no traced setter 0.34,
wake hand-offs 0.23, virtual-core waits 0.17, dispatcher wake-ups lr 0x8004af4c ~50 us).

**Plainly: under 3.2 ms is not reachable with the 25 kernels and the class-A jobs.** Even at infinite speed they leave
4.56 ms. What the data says would also be needed:

1. **Havok Physics as a native module**, not as kernels: 2.24 ms of the chain, including 0.85 ms that runs alone. This is
   the plan's step 6 (`README.md`), and it is on the critical chain now.
2. **Havok Cloth** as a native module: 0.33 ms as its own jobs, plus 0.12 ms inline in the player's job.
3. **The serial stretches**: 2.40 ms of the chain runs with every other job thread idle. A native module can use more
   threads than six for its own work (`README.md`): the solver over independent islands, several cloth worlds, broad
   phase. Overlapping stretches of different subsystems (graphics recording 0x8006cd60 alone 0.51 ms, point queries,
   solver) depends on their data dependencies, which are not read.
4. **The job system's own share**: the dispatcher 0.27 ms and stage registration 0.09 ms go only with the native job
   system answering the dispatcher and the stage entry points (`jobs-boundary.md`), and the hand-offs between stages with
   it.
5. **The actor stage jobs** are gameplay (rule 3 keeps them translated): their 1.25 ms falls only by their kernels and by
   what they call (cloth, ray casts, the animation-pose routine 0x801d99a0 if it can be specified).

## The first three replacements

All three keep the translated function as the reference and switch per unit with a variable, like `HL_JOBS`.
Differential check, the same for each: the native handler, when the unit's switch says "diff", copies the unit's inputs
(the argument registers and the input bytes listed below), runs the **translated** function on the real state, then runs
the native one on the copies with its stores going to a log (as `engine/host/hos_native.c`'s differential mode does), and
compares bit for bit: the output bytes, x0 and v0, and every logged store against what the translated code left. Every
call for the first 600 frames, then 1 in 64; in castle-move, kokiri-idle and hyrule-walk; zero differences before the
switch's default changes. The translated body must stay callable under its own address for this: for a pointer-reached
unit it is (`a64_fn_ptr` in the lift's table; the replacement only wins in `a64_lookup`); for a hooked direct call, the
hook list must keep the lifted body (not established that it does today: `image.py`'s hooks are for SDK exports).

1. **0x80ba6d80 (AABB transform) and 0x800a8400 (bounding sphere): the actor stage's model bounds.**
   - Boundary: two leaf functions, fully read, plain inputs: 0x80ba6d80 reads 16 + 64 bytes and writes 32; 0x800a8400
     reads the bounds object, the sphere records, the used matrices, and writes the output sphere (16 + 4 bytes). Both
     are called directly (9 and 8 sites): this is the unit that builds the **hook list for main-module addresses** and
     the new lift it needs; everything later uses it.
   - Gain: 0.199 ms on the chain (0.015 alone). 2x: -0.10 upper / -0.008 strict; 4x: -0.15 / -0.011. They run in the
     last, six-wide stage (tenths 8-9), so the upper value is plausible if all six executors spend the same share there
     (the 25 kernels are 0.73 of 0x83af59c0's 1.62 ms CPU, 45%; these two nearly all of it).
2. **0x81746440 (convex vertex AABB) in the Havok solver and narrow phase.**
   - Boundary: a leaf, fully read: a relative vertex array and a transform in, a min / max pair out. Directly called (2
     sites): through the hook list of step 1. The test of the exactness work: FMIN / FMAX and the outward double rounding.
   - Gain: 0.112 ms on the chain (0.029 alone): 2x -0.056 / -0.015, 4x -0.084 / -0.022.
3. **0x819b8e20, the Havok solver task, whole.**
   - Boundary: the task's `process` slot (vtable 0x848d9e80), answered by `a64_lookup_replaced` with no new lift; inside:
     gather solver velocities, solve Jacobians and constraints (0x819ba420, 0x8177b2a0, 0x817d9d80), sub-integrate
     (0x8182d2c0, 0x8182dc20); 31 functions, ~18,000 instructions on the chain. Its inputs and outputs (the solver step
     info, velocity and Jacobian streams, motions) must be read first; the differential check needs the full store log,
     as its write set is not known in advance.
   - Gain: 0.409 ms on the chain, 0.375 of it alone, so the gain is close to its share: 2x -0.20 upper / -0.19 strict, 4x
     -0.31 / -0.28. The largest gain of any single unit outside the graphics job that the strict reading supports.

Expected after the three, at 4x: 5.80 -> about 5.27 (upper) / 5.49 (strict) (0x81746440's 0.024 ms inside the solver
task counted once).

## Not established

- **Which actor each instance of 0x800b3ce0 / 0x83af59c0 runs**: the job objects were not sampled. "The long instance is
  the player's" rests on the strings its chain-sampled subtree forms (likely, not read from an object).
- **The two physics passes in one wait**: two worlds, two substeps or one step spread out; and the order of the physics
  tasks within a pass is read from sample positions, not from the task graph (`hkTaskGraph` dependencies not read).
- **Data dependencies between the alone stretches** (graphics recording, point queries, solver, cloth): whether any can
  run earlier or beside another. Without them, the scheduling gains of class C are not numbers.
- **What 0x800493e0, 0x80341840, 0x80162aa0 and 0x801d99a0 compute.** 0x801d99a0 forms "UseRotatedTranslate" (animation)
  and is the largest single function on the chain; it is not a self-contained kernel.
- **Kernels marked guess** (16 of 25): their inputs and outputs were not read; only their shape and the strings above
  them. Their "what it computes" is a reading of context.
- **Whether the hook list can keep the lifted body** for a differential check of a directly called function, and whether
  a call site's FPCR mode (flush-to-zero, default NaN) is the same at every call of a kernel.
- **The scenarios' accuracy**: shortening a unit's samples is a first-order model; a shorter unit can move the chain to
  other work (the strict column bounds that from below), and the sampled chain is under the sampler (~2 ms slower frames).
  ~380-430 chain samples a run: per-kernel chain values under ~0.03 ms are a few samples.
- **One scene, one machine** (castle-move, main PC), as in `hot-functions.md`.
- **A correction to `hot-functions.md`**: "no string references were found" and "Physics (Havok) is not shown for any
  function" do not hold at the job level: the jobs' subtrees form Havok Physics and Havok Cloth strings (above).
