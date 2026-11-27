# Where the translated shaders waste GPU time (stream P4, 2026-11-17)

Question: the PC 3050 (RTX 3050, 1440p) is GPU-bound at ~20.3 ms of GPU work a frame in Kokiri and ~7 ms must go. The
shaders are Maxwell/Ampere-class NVN programs translated to SPIR-V by our recompiler. Which of the translator's output
costs GPU time that the original program did not, and what is each fix worth?

This is a study: code reading, SPIR-V statistics of a program cache, and a join with the GPU times G2 and G3 measured. No game
was run for it. Programs are named by their keys; no game code, SPIR-V or dump is in the repository (the tool below reads a
local cache and writes counts only).

Answer in one line: **in the frames that matter the translator's waste is the global-memory helper that every mesh, task,
compute and many fragment programs call (a search over all candidate storage buffers, ~29 SPIR-V instructions a candidate, inlined
at every access), and 5.4% of the whole cache's code is helper bodies nobody calls. Measured on the three biggest mesh programs it is
worth 1.1 ms (Kokiri, one program: not inlining it) and 0.4-0.5 ms each (hoisting the ranges); the rest I can only estimate.
The predicate/LOP3 chains (~9,000 BitFieldUExtract in the biggest mesh program) are a different thing: one translator bug, not a
GPU cost the driver demonstrably pays, but cheap to fix.** The 7 ms do not come from here: at most ~2-3.5 ms of them can.

## The tool

`engine/scripts/spirv_stats.py` (test: `engine/tests/test_spirv_stats.py`).

    python -I scripts/spirv_stats.py scan   <hookshot_programs.bin> --out <scratch dir>     one JSON line a stage, 2.5 min for 45k records
    python -I scripts/spirv_stats.py report <scratch dir> --logs <g2 run dir> ... --top 25   totals, and the join with the GPU cost
    python -I scripts/spirv_stats.py ops    <hookshot_programs.bin> --key <hex prefix>      opcode histogram of one program

It reads the cache's records as `scripts/validate_spirv.py` does (packed ones through `compression.zstd`), walks each stage's SPIR-V once
and counts: instructions by class, opcodes of interest (BitFieldUExtract, IEqual, Select, compare-exchange, loops), scalar width of
arithmetic (f16 / f32 / f64 / i64), texture operations, local arrays, storage-buffer variables, and the **global-memory helper
functions** (a function whose first parameter is an u64 address and that reaches a storage buffer through a branch: its body size, its
call sites, its candidate count by ArrayLength, whether it is marked Inline, whether it is ever called). "Effective" instructions
are body x call sites: what the code is if the driver inlines it, as it must where the helper is marked Inline. A constant-propagation
pass over the pure integer ops counts what is foldable (every operand a constant). `report --logs` takes G2-style game logs
(`RX_VK_PASS_TIMING=2`: the last "GPU pass timing" block gives the 20 programs whose runs of draws took the most and the top 25 spans,
which include dispatches by program key) and averages them.

Provenance of the numbers below: the cache is a copy of slot b's `hookshot_programs.bin` (2026-11-17 01:18, packed, 45,293 records, taken
before any other run rewrote it; slot a's was rewritten under the first scan by another stream), kept in `E:\OOT\scratch-p4`. Twelve hot
programs the current cache does not hold (1a7482c0..., da7a7ab2..., d8180bd8..., 9dc98871..., 26fe0c70..., 04829841... and others: the
cache of the day of G2 held them) were read from slot hl's older cache. Duplicated stages (a record kept per pipeline state) are counted once.

## The cache in numbers

45,293 records, **39,853 programs, 80,130 distinct stages, 330.5 M SPIR-V instructions** (the median program is 34 KB packed).

| stage | stages | instructions | global-helper call sites | stages with them | storage-buffer accesses bound at translation | helper bodies never called |
|---|---:|---:|---:|---:|---:|---:|
| fragment | 36,616 | 192.9 M | 130,465 | 9,965 | 303,263 | 4.2 M instr |
| vertex | 34,765 | 84.5 M | 390,724 | 5,446 | 230,452 | 8.7 M |
| compute | 6,306 | 29.6 M | 115,463 | 1,796 | 175,448 | 3.7 M |
| mesh | 1,287 | 16.1 M | 44,942 | 819 | 67,515 | 0.6 M |
| task | 1,156 | 7.5 M | 6,682 | 1,104 | 7,350 | 0.7 M |
| all | 80,130 | 330.5 M | 688,276 | 19,130 | 784,028 | 17.9 M (5.4%) |

* Direct (bound) storage-buffer accesses are 784k, helper call sites 688k: **47% of all global accesses stay global**, and in the mesh
  programs that matter nearly all do (43ec86f35fd937b9's mesh: 77 helper calls, 10 direct; d8e59341cf4f629c: 72 and 21). What the pass
  could bind it has bound; the rest are addresses built from a pointer loaded from memory or a choice among several buffers (see below).
* The helper is 29 instructions a candidate (206 instructions for 7 candidates in 43ec's, 179 for 6 in d8e5's): two loads of the driver
  bank, an ArrayLength, 64-bit adds, a compare-and-select to cap the range, two compares and a branch. 7 instructions of those
  could be a constant-time table read (see the plan). Candidates in the widest helper: 2-4 in 13,000 stages, 8 in 1,180, **18 in 3,635** (the culling computes).
* **Helper bodies the program never calls: 17.9 M instructions, 5.4% of the cache.** Every stage that uses global memory gets
  load/write helpers for three widths plus nine atomics, a compare-exchange and the narrow-store helper whether or not it calls them
  (43ec's mesh: three load variants, two are called; every write is dead). The driver drops them; they cost translation, SPIR-V size
  and the driver's front end, not the GPU.
* **8/16-bit stores: no call site anywhere** (global narrow store helper: 357 compute stages carry the body, 0 call; the storage buffer
  path's own compare-exchange loop: 0 calls; the 17,787 compare-exchange operations in 655 stages are the atomics' helper bodies, dead).
  The CAS-loop worry is not a cost in this cache.
* **Predicate emulation: 6.2 M BitFieldUExtract (1.9% of instructions), 2.1 M IEqual; 1.89 M operations are foldable (every operand a
  constant), 763k BitFieldUExtract of a constant base. Mesh stages hold 565k of those: 1.18 M of the mesh code's 16.1 M instructions
  (7.4%), and 594 stages have over 1,000 foldable operations.** In 43ec86f35fd937b9's mesh 8,698 of 44,883 instructions are foldable and a chain of
  Select/LogicalNot hangs on them (the largest programs of the cache, 93e3e999f2fb9fd8: 219,867 instructions, 5,896 foldable).
* Float: 76.7 M f32 operations against 0.29 M f16 (fragment stages only: 12 in each of the hot ones); 8.7 M 64-bit integer operations
  (the helper's address math; 4.3 M in the vertex stages alone). 709k texture operations. Local arrays: 254 stages, 92 KB in all
  (the largest: 16 bytes in the hot mesh programs). Loops: 44k, 25.8k of them in fragment stages (typically one a program).

## Which programs cost the GPU time (G2, PASS_TIMING=2, two runs a zone)

G2's top-20-programs list and top spans cover 12.5 ms of Kokiri's 22.3 ms busy frame and 10.2 of Hyrule's 19.9: the rest is spread over
hundreds of small spans. Of what is listed, **mesh pipelines are 7.2 ms (Kokiri) and 4.5 ms (Hyrule)**, compute 2.4 and 2.2,
vertex+fragment 2.9 and 3.5.

| program (stages) | Kokiri ms | Hyrule ms | the stage that matters | gm sites x candidates (effective instr) | direct | notes |
|---|---:|---:|---|---|---:|---|
| 43ec86f35fd937b9 (task, mesh, frag) | 2.41 | - | mesh 44,883 instr | 77 x 7 (15.9 k) | 10 | 8.7 k foldable, 14 loops; not inlined: 1.34 ms |
| 918adef1afaa39d4 (task, mesh, frag) | 1.72 | - | mesh 12,849 | 4 x 5 (0.6 k) | 62 | helper-free: neither noinline nor hoist moves it |
| d8e59341cf4f629c (mesh, frag) | 1.41 | 0.33 | mesh 9,242 | 72 x 6 (12.9 k) | 21 | hoist: 1.06 / 0.25 |
| 739acd678ae08655, 4f0c0b40ab3a32cd, d905f52b7517c681 (Hyrule mesh) | - | 1.44, 1.42, 0.79 | mesh 8-9 k | 72-77 x 7 (14.8-15.9 k) | 10-27 | hoist: 0.96, 1.00, 0.55; noinline: 1.42, 1.83, 0.75 |
| 1a7482c0ec9991ea (vertex, frag) | 1.07 | 0.92 | fragment 1,786 | none | - | fullscreen, 16 texture ops: no translator waste |
| 89ca750f9852b7ca, 3b8c07f2eb4bb7d9, e3f341ac12c40e2c | 0.43, 0.23, 0.18 | | mesh | 72-144 x 6-7 (12.9-29.7 k) | 23-39 | |
| da7a7ab28b8e73d9 (mesh, **fragment**) | 0.56 | | fragment 9,164 (1280x720 pass) | **109 x 5 (19.3 k)** | 26 | a fragment shader with 109 global loads |
| 9dc98871eb7fc101 (compute, matrix net) | 0.42 | 0.42 | 23,909, 192 cooperative-matrix ops | none | - | the GPU's tensor path, nothing to remove |
| ee7764a6b1ef459c (compute) | 0.41 | 0.44 | 11,826 | 112 x 6 (24.2 k) | 13 | |
| 27ee9883e2a20323 (compute) | 0.36 | 0.29 | 11,128, 32 texture ops | 1 | 8 | |
| 435a0138b9013040, cc62764061c44f6a (compute) | 0.21, 0.18 | 0.21 | 4.1 k, 14.7 k | 56 x 6, 72 x 8 (12.1 k, 19.4 k) | 13, 75 | |
| 755a777ada52677d (compute, 35 dispatches of 1x1x1) | 0.20 | 0.26 | 1,736 | 3 | - | 5.6 us a dispatch: latency, not code |
| e2db7bfe1ee2a837, e186e5d64641235d (vertex pull) | 0.19 | 0.14 | vertex 4.8-5.5 k | 72 x 4 (6.9 k) | | |

Of the listed milliseconds, programs with global helpers: 9.4 of 12.5 (Kokiri) and 7.5 of 10.2 (Hyrule); with a mesh or compute
stage using 20 or more calls: 4.9 and 4.5 ms (mesh alone).

### What G3 and G5 already measured, program by program (ms a frame, PASS_TIMING=2 runs in `E:\OOT\scratch-g3\runs`)

| program | default | helpers not inlined (mesh) | ranges hoisted (RX_VK_MESH_G3=1) | both |
|---|---:|---:|---:|---:|
| 43ec86f35fd937b9 (Kokiri) | 2.43 | **1.34** | 2.46 | 1.36 |
| 918adef1afaa39d4 (Kokiri) | 1.69 | 1.68 | 1.72 | 1.73 |
| d8e59341cf4f629c (Kokiri) | 1.45 | 1.47 | **1.06** | 1.48 |
| 89ca750f9852b7ca (Kokiri) | 0.43 | 0.56 | **0.33** | 0.58 |
| 4f0c0b40ab3a32cd (Hyrule) | 1.42 | 1.83 | **1.00** | 1.77 |
| 739acd678ae08655 (Hyrule) | 1.44 | 1.42 | **0.96** | 1.38 |
| d905f52b7517c681 (Hyrule) | 0.79 | 0.75 | **0.55** | 0.75 |
| frame total, Kokiri / Hyrule | 22.4 / 19.9 | 21.3 / 20.2 | 21.7 / 18.4 | 21.8 / 20.3 |

Hoisting is the better answer for every mesh program but one; the one it does nothing for, 43ec, gains 1.1 ms (45%) from leaving the
helpers out of line. 4f0c has the same helper (77 sites, 7 candidates, 15.9 k instructions) as 43ec and loses 0.4 ms out of line:
what separates them is the rest of the program (43ec's mesh is 44.9 k instructions, 4f0c's 9.4 k), so the code-size cliff is the whole
program's, not the helper's. G5 (hoist in every stage, `RX_VK_GMEM_HOIST`): fragment +6.5 ms, compute +2.4 ms, vertex 0: the prologue costs
every invocation what a search costs only the accesses reached.

## The translator paths behind it

* **Binding at translation time** (`ir_opt/global_memory_to_storage_buffer_pass.cpp`, `CollectStorageBuffers`): an access whose address
  tracks to one constant-buffer slot is turned into a storage-buffer access; this is Eden's pass with Fritchii's native tracking, and it
  is not the missing piece. What stays global is left on purpose: an address that mixes a pointer loaded from memory (`MixesLoadedPointer`,
  the culling programs' instance and placement records), several candidate pointers (`native_candidates.size() > 1`: all bound, the access
  left global), the c1 rule, a graphics stage's pointer from another buffer than c0 (`cbuf_load_rule`) and a split select/phi. These are
  precisely the hot mesh programs' accesses: nothing more can be resolved statically without knowing where the pointer came from.
* **The helper** (`backend/spirv/spirv_emit_context.cpp`, `DefineGlobalMemoryFunctions`, `define_body`): for each access width a function
  that tests every candidate in turn (each: two cbuf loads, `ArrayLength`, 64-bit math, a conditional branch) and then, in compute (and
  graphics stages where it is wanted), the page table (`guest_lookup`: the pool's device address from a 4 KB-page table, a physical-storage-buffer
  load). Marked `Inline` for task and mesh (issue 994: with `DontInline` NVIDIA's compiler returned the second call's result for the first
  in the tree task 3cb76827), `DontInline` elsewhere. `RX_VK_MESH_G3=1` makes each candidate's range once, in the first block, in two Private
  variables; `RX_VK_MESH_NOINLINE=task|mesh|both` leaves the helper out of line.
* **Predicate emulation**: the Ampere to Maxwell lowering (`frontend/ampere/lower_to_maxwell.h`) saves and restores predicates with P2R and R2P
  (`save_predicate`/`restore_predicate`), and PLOP3 is three selects to 0/~0, a LOP3 and a test. R2P (`move_register_to_predicate.cpp`) emits per
  predicate `BitFieldExtract(mask, index, 1)` with `GetZeroFromOp` attached to it: **the constant propagation pass does not fold an
  instruction that has an associated pseudo-operation** (`FoldWhenAllImmediates`: `HasAssociatedPseudoOperation()`), so a
  `BitFieldUExtract` of an immediate mask stays, with its IEqual and the Select that uses it, and the whole chain stays with it. That is the
  763k constant-base extracts, and why G5's `RX_VK_GMEM_FOLD` (which targets the PLOP3 selects) found nothing: it did not touch this.
* **8/16-bit stores**: `WriteGlobalU8/S8/U16/S16` (global) and the storage buffers' narrow stores go through a compare-exchange loop function.
  No program of the cache calls either.

## The ranked list

Estimates are of the GPU ms of a Kokiri / Hyrule frame; "measured" is G3/G5's, the rest is my reading and marked so. Stamp impact: any
change to the translator's source moves `HOOKSHOT_SHADER_STAMP`, so every player re-prepares shaders once (about the same wait as after
any stamp change), except a switch read only at run time.

| # | what | est. ms (K / H) | effort | risk | stamp | basis |
|---|---|---|---|---|---|---|
| 1 | **Size-aware inlining of the helper in big mesh programs**: helpers out of line (and no hoist) for a mesh stage whose code is past a threshold (43ec: 44.9 k instr), hoisted and inlined below it | **-1.1 / 0** (measured: 43ec 2.43 to 1.34) | S | M: NVIDIA's DontInline miscompile (issue 994) is known for the task stage; no picture was checked with `NOINLINE=mesh`; one data point for the threshold | yes | G3 runs g3-k-nimesh / g3-k-def |
| 2 | **A range table the CPU prepares** (lo and end of each candidate, capped by its binding, as one 16-byte uniform entry) in place of the in-shader range math: 29 to ~8 instructions a candidate, **no prologue**, so it works where hoisting loses (fragment, compute, vertex) | **-0.8 to -1.5 / -0.8 to -1.5** (estimate) | M-L | M: a new descriptor binding written at each draw/dispatch of a program with global accesses; CPU cost | yes | helper = 29 instr/cand; G3 hoist removes the same math in mesh for -0.4/-0.5 a program (-26 to -31%); G5: its prologue costs +6.5 (fragment) / +2.4 (compute): the math is what costs; da7a7ab2's 109-site fragment 0.56 ms; compute helper programs ~1.4 ms |
| 3 | **Fold constant predicate chains** (BitFieldUExtract/BitFieldSExtract of immediates with a Zero/Sign pseudo-op: replace the pseudo-ops by their value, then fold): 1.9 M ops cache-wide, 20-30% of the biggest mesh programs, the Selects that hang on them fold with them | 0 to -1 / 0 (estimate: the driver probably folds constants; the 43ec cliff suggests it may not fully) | S | L: pure folding | yes | the pass's `HasAssociatedPseudoOperation` early return; 565k constant extracts in mesh stages; also shrinks SPIR-V and the driver's compile |
| 4 | **Stop emitting uncalled helpers** (write, atomic, CAS and narrow-store helpers only when an access of that kind exists; load widths likewise): 17.9 M instructions, 5.4% of the cache | 0 GPU (compile time) | S | L | yes | 3.7-8.7 M instr in each stage kind; the driver drops them, but not for free |
| 5 | **Candidate order / fast path**: put the candidate the access hit last time first (a Private variable per helper), or test the single bound candidate before the loop | -0.1 to -0.4 / -0.1 to -0.4 (guess) | S | L | yes | search is a linear chain; only helps misses at the front |
| - | 8/16-bit stores, CAS loops, f16 | 0 | | | | no call site; 0.29 M f16 operations of 330 M; GA10x runs f16 at the f32 rate |
| - | indirect mesh conversion, pass breaks (G4) | 0 | | | | measured: no gain |
| - | PLOP3 as booleans (G5's FOLD) | 0 | | | | measured: no change |

**Sum.** #1 and #2 together are -1.9 to -2.6 ms Kokiri on top of G3's hoist (-0.6) if they do not overlap; #3 may add up to 1. That is
**about 3 ms of the 7 ms** at best, and #2 is the only one of the three that is not already a measured number. The rest (4 ms) is the
fullscreen and pixel work (G2: 3.5 ms of the pass time scales with pixels, 5 ms is a few 1-2.4 ms passes: 1a7482c0 1.07, d8180bd8c6ecc77b 0.49,
da7a7ab2 0.56 ms) and the dispatch fixed costs (120 dispatches, 36 us each on average), none of which is translator waste in the SPIR-V I
can see: 1a7482c0's fragment is 1,786 instructions of ordinary shading with 16 texture ops.

## Plan for the top two

### 1. Size-aware inlining (prototyped, default off)

* In `collect_shader_info_pass.cpp` count the program's IR instructions (`Info::ir_instruction_count`). In `DefineGlobalMemoryFunctions`
  (`inline_here`): for a mesh stage with `RX_VK_MESH_NOINLINE_ABOVE=<n>` set, the load helpers are `DontInline` when
  `ir_instruction_count > n`, whatever `RX_VK_MESH_NOINLINE` says; below, nothing changes. The switch is a name under `RX_VK_MESH`, so it is
  already in the cache stamp's prefix list, and `RX_VK_MESH_G3` continues to apply below the threshold (and is skipped above it, where the
  prologue would only add code the helper no longer has).
* Where to put n: 43ec's mesh is 44.9 k SPIR-V instructions (the IR count is lower; read it from the new log line), the others in the frame
  are 8-14 k (3b8c07f2's 12.7 k with 144 sites is neutral out of line: 0.232 against 0.236). Start at the IR count that corresponds to 30 k SPIR-V instructions and
  confirm on Kokiri's 43ec and on Hyrule (which must not move).
* Test: (a) the PC 3050 A/B that G3 did: default, `RX_VK_MESH_G3=1`, `RX_VK_MESH_G3=1 RX_VK_MESH_NOINLINE_ABOVE=n`, 2 runs, Kokiri
  and Hyrule, PASS_TIMING=2 for the per-program lines (43ec, d8e5, 4f0c, 739a must read 1.3 / 1.06 / 1.0 / 0.96); (b) **pictures**: the scene
  shots of G5's four arms (trees in the sky is what issue 994 looked like: Kokiri's forest and Death Mountain's trees), because `NOINLINE=mesh` was never
  checked against a picture. If it miscompiles, the single-exit form of the helper (one `OpReturnValue` at the end through a Function
  variable) is the first thing to try: the driver bug reads like early-return handling.
* Rollout: default on once (a) and (b) hold; the stamp moves once.

### 2. The range table

* Host: for each draw and dispatch whose program has global helpers (the pipeline knows: `info.uses_global_memory` and the candidate list),
  write one `uvec4` a candidate (aligned base lo/hi, end lo/hi) into a per-frame uniform ring, at the same place the code already knows the
  candidates' cbuf offsets and the bound range of each binding (`vk_draw.cpp` binds them: "the binding's own length"). The candidate set of a
  program is fixed, so the table's layout is a compile-time constant of the program, and the table is two to 18 entries.
* Shader: `define_body` reads the entry (one `OpLoad` of a `uvec4` from a UBO at a constant index: 1 load instead of 2 loads and an
  ArrayLength) and the test becomes `addr - lo < len` on 64 bits, two instructions and a branch. Tables of storage buffers (`ssbo_tables`)
  and the page table stay as they are. No prologue: fragment and compute programs pay only for accesses reached.
* Expected: -29 to -8 instructions a candidate; 43ec's 206-instruction helper becomes ~70 and its inlined code 15.9 k to ~5 k, which may
  make #1 unnecessary for it (test both: the table with inlined helpers against #1).
* Risks: a new binding in every pipeline layout that uses global memory (the push block is used by rescaling and the page table: the
  table cannot go there), the ring's CPU cost at draw rate (mesh draws are tens a frame, compute 120: small), the buffer-size cap at
  bind time must match the shader's (`ArrayLength`) exactly or the null-descriptor case ("an entry the back end could not bind reads 0 instead of the page
  table") breaks. G5's finding that the prologue costs per invocation is the reason the table must be host-made and read lazily.

## Prototype status

* `engine/scripts/spirv_stats.py` and its test: done, committed.
* **#1 is prototyped behind `RX_VK_MESH_NOINLINE_ABOVE=<n>` (default off)**: `backend/spirv/global_helper_policy.h` (the decision, pure),
  its use in `DefineGlobalMemoryFunctions` (the load helpers' inline flag and the hoisted ranges stand down for a mesh stage over n IR
  instructions), `ir_instruction_count` in `EmitContext`, and a warning-level log line per mesh stage while the switch is set that
  names its IR instruction count, which is how n is to be chosen (43ec's mesh is 44.9 k SPIR-V instructions; the log gives the IR figure to
  put the threshold under). Test: `tests/test_global_helper_policy.py` (the decision's table, run, and the wiring checked in the source;
  passes under WSL's g++, the Windows clang here cannot link). **Not built into the game and not run**: the emit context needs boost and fmt,
  which this tree does not carry here. A build and the A/B of the plan above are the next step.
* #3 (folding the constant predicate chains) is a ten-line change in `constant_propagation_pass.cpp` (replace the Zero/Sign pseudo-operations of
  an immediate `BitFieldUExtract`/`BitFieldSExtract` by their values, then let `FoldWhenAllImmediates` run); I did not make it because it
  needs a build and a before/after of the SPIR-V I cannot run here. The statistics tool measures it: rerun `scan` and compare `const_foldable`.

## Not verified

* GPU times are G2's and G3's runs, not new ones; the per-program lists hold the top 20 draws and top spans only (12.5 of 22.3 ms in
  Kokiri, 10.2 of 19.9 in Hyrule); castle-move has no per-program run at all (G2 did not take one).
* My "estimates" for #2, #3 and #5 are not measured. #1's 1.1 ms is G3's; its generalisation is one program.
* The static counts say what the code is, not how often a thread runs it: 77 call sites of a mesh program are not 77 executions
  a thread. That is why the join uses the measured ms and why "effective instructions" is a size, not a time.
* The cache of the day (slot b, 2026-11-17) is the current translator's; the G3 programs' instruction counts match the dumps in
  `E:\OOT\scratch-g3` to the instruction (43ec's mesh 44,883).
* Whether the driver folds the constant predicate chains was not looked at (no `nvdisasm` of a compiled program here): #3's value is
  open. A `nv-shader` dump of 43ec's mesh would answer it in minutes.

## Stream P5 results (2026-11-17/18, PC 3050, RTX 3050, 1440p; build of perf/shader-size)

Switches: `RX_VK_FOLD_PSEUDO=1` (item 3, folds a constant BitFieldExtract/BitwiseAnd/Or/Xor that carries zero/sign flags: the flags get
the value the back end would have made, then the instruction folds; default off; unit test `tests/test_pseudo_fold.py` builds real IR
and runs the pass) and `RX_VK_MESH_NOINLINE_ABOVE=<IR instructions>` (item 1). Both are read by the translator: **the stamp moves**
(`RX_VK_FOLD` joins the stamp's prefixes; the NOINLINE one was already under `RX_VK_MESH`), and every switch value is its own stamp, so each
arm re-prepares the whole 31,985-program set (about 5 runs of 110 s before "prepared N shader programs" shows): measure only after that.

* Mesh IR sizes (the log line `vk: mesh stage: <n> IR instructions, <w> SPIR-V words` while NOINLINE_ABOVE is set; SPIR-V words ~ 4.0 x IR):
  43ec86f35fd937b9's mesh is 218,494 words (about 54 k IR); every other mesh of Kokiri and Hyrule listed above is 36-60 k words
  (9-15 k IR). Threshold used: **30000 IR**. (Some meshes of the cache are bigger, to 268 k IR; none is in the two scenes.)
* Fold, cache-wide (spirv_stats scan of the cache this build wrote, 39,947 programs; before = the same build without the switch):
  foldable operations 1,912,456 -> 404,108 (-79%), total instructions 327.8 M -> 323.2 M (-1.4%), mesh stages 15.84 M -> 12.42 M (-21.6%),
  mesh foldable 1,184,751 -> 58,836. 43ec's mesh: 43,471 -> 19,280 instructions (218 k -> 89 k words), foldable 8,698 -> 184.
  The rest of the foldables (404 k) are other chains the pass leaves (not the R2P extracts).
* GPU ms a frame (PASS_TIMING=2 run per arm) and fps (2 plain runs, then the PASS_TIMING run's fps in brackets: it is slower), 3 prepares + warm runs first:

| arm | Kokiri GPU all | 43ec | Kokiri fps | Hyrule GPU all | Hyrule fps |
|---|---:|---:|---|---:|---|
| a default (G3 on) | 21.63 | 2.451 | 42.4, 38.7 | 18.23 | 48.7, 48.9 |
| b NOINLINE_ABOVE=30000 | 20.51 | **1.335** | 43.8, 43.9 | 18.47 | 48.6, 45.5 |
| c FOLD_PSEUDO=1 | 21.57 | 2.439 | 42.6, 42.1 | 18.33 | 48.6, 48.3 |
| d b + c | 20.45 | 1.326 | 44.5, 44.5 | 18.40 | 48.4, 48.2 |

  (An earlier pass with a threshold of 225,136, set above every mesh by a first attempt that read the log's IR counts wrong, gave arms b and
  d identical to a and c: 21.58/21.61 ms, which shows the run-to-run noise at about 0.05 ms for the frame total.) The fold shrinks 43ec's
  mesh to under half and the driver does not care: the constant chains were already folded by it (item 3's "0" estimate holds). The other
  programs (d8e5, 4f0c, 89ca) move by under 0.03 ms; 739a and d905 jump 0.36-0.58 ms between arms in single timed runs and in arm c, which
  has no code difference for them, so that is the noise of a span in one run.
* Pictures (frame 9000-9600 of each arm, Kokiri forest and Hyrule field, 1440p scaled): trees, grass, house/wall, water, Link, fairy, HUD and
  the leaf particles are right in all four arms; no tree-in-the-sky or garbage as in #994. Differences between arms are the scene's own
  motion (fairy position, leaves, grass LOD), including between two runs of one arm.
* Recommendation: **RX_VK_MESH_NOINLINE_ABOVE=30000 on by default** (-1.1 ms GPU, Kokiri +1.5 to +5 fps in the frame-limited runs, Hyrule unchanged);
  the fold is correct and cuts SPIR-V and translation size (-1.4% of the cache, -22% of mesh code, driver compile time) at no GPU cost:
  take it on for the size, with the same stamp move, once someone wants the smaller cache; it is not a speed item.

**Decision (2026-11-17, owner):** both on by default from v0.4.1: `RX_VK_MESH_NOINLINE_ABOVE` = 30000 (`=0`: off), `RX_VK_FOLD_PSEUDO` on (`=0`: off). The shader stamp moves once with the release's other translator changes.

## Memory of a cold start (PREP1, 2026-11-17, PC 3050)

A start with no program cache prepares every shader and then predicts pipelines for the programs no note names (predict_run, built for the
driver's own cache and let go). On the PC 3050 (RTX 3050, 32 GB) that start held 29.5 GB of C heaps and 36-39 GB of private commit and
refused C++ allocations at the commit limit; play began at 616 s, against 62 s with warm caches. The shader preparation itself is bounded
(private commit 1.5 GB, 31985 programs, 125-175 s, the same with or without the driver's part): the memory is the driver's compile in the
prediction. Two programs of the game, 93e3e999f2fb9fd8 (1014k words of SPIR-V) and cf987f56b971bdb3 (502k), each kept a worker in
the driver for over three minutes and growing past 11 GB (the process's heaps hold the driver's compile); the time budget (90 s) cannot
stop a build in hand, the join waited 347 s, the compile ended -13, and the heap stayed taken. Every other program, up to 133k words, took 25 s at most.
Since then (gpu/vk/predict_limits.h): a program over RX_VK_PREDICT_MAX_KWORDS (300) thousand words is left to the draw that needs it, and the
prediction stops when the system has less than RX_VK_PREDICT_RESERVE_MB (3072) of commit left. The progress lines say the process's private
commit and C heaps (the preparation's every 10 s, the prediction's with the programs the workers have in hand).

"0 stage libraries" on the PC 3050 against 7781 on the main PC is the slot's state, not the GPU: the count is the compute pipelines the
driver's part warms (7781 compute variants), and the driver's part is left out when `hookshot_prepared.bin.warming` is in the folder (a
start that was killed or crashed in the driver's part leaves it: "the last start did not finish the driver's part of the shaders: left
out"). The graphics stage libraries are only built with RX_VK_GPL. It does not change the memory: the compute pipelines are built by the
precompile instead (131 s, at most 3 GB).

## Sections from branches that were not merged (brought over 27 Nov 2026)

Written on those branches and left out when their notes came to main (256de8e6f); kept as they were, each under the branch it comes from. The verdicts on that work are in docs/research/perf-plan-2026-11.md and the branch audit.

### From feat/install-full-pipelines (89fd5fd5c)

### What the install builds, and what the first session still compiles (PIPE1, 2026-11-18, PC 3050)

The owner's first session after an update stuttered and the second was smooth: the shaders were prepared (459 s), the pipeline prediction ran
with its 90 s budget and made 80 of ~35,851 pipelines, the precompile built 6,570 noted pipelines in 37 s, and ~90 programs were translated in
play. Measured here on the PC 3050 (i5-10400F, RTX 3050, 32 GB, a cold driver cache made by `__GL_SHADER_DISK_CACHE_PATH`, another stream's
game on the other slot), a cold `--prepare-shaders` in a slot of its own (current seed: 38,809 programs, 116,427 translation steps), then
the first plays of castle-move, hyrule-walk and kokiri-idle:

| | main 8fba7092 (before) | the shipped list of 14,958 states, the noted pipelines before the predicted ones (after; the plays are of the first form of the change, tier 0 predicted first, and the install's last line of this column is the final one) |
|---|---|---|
| install, whole | 19.7 min | 21.2 min, then 2 to 2.5 min more for the default prediction below (about 24 min) |
| programs (translation, the driver's part) | 859.6 s | 933.4 s (2,000 programs more come with the list) |
| predicted pipelines | 107.8 s: 96 of 41,487 built (the 90 s budget) | 28 built in 24 s with the prediction first and tier 0 only; the default (tier 0 and 1, up to 40 k words, after the noted ones): 24,040 in 123 to 150 s in a separate start |
| noted + compute pipelines (precompile) | 195.7 s: 6,262 + 10,330 | 301 s: 9,995 + 10,330 |
| peak private commit (at the pipeline cache save) | 6.8 GB | 8.4 GB (8.7 GB with the cache's data in hand) |
| first play, castle-move: pipelines built after play began (graphics and compute, cache hits too), the driver's time for them | 312, 20.0 s | 51, 0.001 s |
| first play, hyrule-walk | 62, 0.9 s | 96, 0.046 s |
| graphics pipelines a draw had to build in play, all three scenes (new log line `in play pipeline:`) | not logged; 231 new states in the slot's list | 6, each already built by the driver (listed), 0 ms |
| programs translated in play | 0 | 0 |

**The ~90 programs translated in play are the development slots' old seed, not the product.** The dev slots' `hookshot_shaders.bin` is the
Nov 8 one (511 MB); a builder's current census makes 539 MB. Of the 980 stages kokiri-idle reads, 145 (62 fragment, 55 vertex, 27 compute, 1
mesh) are not in the old seed, so 98 programs (71 graphics, 27 compute) were translated in play: 193 pipelines and 25.8 s of the driver
in that run, a 6.3 s frame, 48 frames over 250 ms. All 980 are in the current seed (every build and update makes it again in `make_game.py`).
The compute ones stall the frame: a dispatch is never left out, so it waits for its translation.

**The predicted pipelines cost what the driver already compiled.** Predicted first (the old order), a pipeline took 3 to 36 s on six
workers, whatever its program's size: the driver compiles each shader module once and a new pairing of known modules is a link. After the
noted pipelines (`started_up`, `prepare_with_screen`) the 24,000 predicted pipelines of known modules (tier 0 and 1, programs up to 40 k
words) took 150 s, 15 to 110 ms each (18,233 made); the rest of the 64,750 predicted there, 8,109 in 308 s, 88 ms (up to 40 k words) to
330 ms (up to 80 k), a few of them seconds where neither module is known. The prediction is built tier by tier, small programs first, so an
install that is closed part way keeps the best of it.

**What play still asked for was states of programs the list knew.** The 231 pipelines three scenes built that the shipped list lacked (9,854
states) were all of programs it names (0 of 231 were of a program no note names, which is what the prediction guesses), each with another
texture variant (`variant` and `answers` differ from the nearest record of the program in all of them): a list from more plays has them. The
lists of the other slots (the benchmark scenes played many times) held 104 of the 231; the merged list of every slot (14,958 states, 6,443
programs, `tools/release/check_states.py` clean) is the new `profile/pipeline_states.bin`. Its scenes are the ones the lists came from, so
the table's after column is what such a list gives for them; an area nobody played with a recording build is still compiled in play.
Frame times of the two columns cannot be compared: another stream's game ran on the PC, and the after arm's three runs met the commit
limit (30 to 38 "refused at the commit limit" retries and a "commit limit near" cut of the textures' budget; the before arm's none), so the
after arm's castle-move shows a 4 s frame where the before arm's showed 1.1 s; whether the 3,700 more resident graphics pipelines (9,995
noted, against 6,262) have a part in it is not known. The first kokiri-idle of the before arm ended at that limit (exit 0xC0000409) and is
left out.

**The order has a trap.** The precompile takes the noted programs' records out of the program cache's index, and the prediction reads the
modules of those programs to know which pairings the notes have: predicted after the precompile in the same process, it found none (0 of
0 programs, the install's second run). The install now works out the prediction first (`predict_plan`), builds the noted pipelines,
then builds what was predicted (`predict_run` with the plan): 24,040 pipelines, 123 s, in the same process.
