# TICK-063 — The lifted game code on the critical path made much faster, bit-exact  · model: Fable · after TICK-058 (parallel with TICK-061 / TICK-062)
**Why**: 120+ fps on the main PC (Ryzen 7 7800X3D, RTX 5070) needs chain A (the game's own: the main thread's draw code, the
job group, the module phase, the last stage; 80-85% lifted guest code) to lose ~5 ms of its ~13-14 ms. The Switch 2 runs the
same code at 60 fps on A78C cores at ~1 GHz; a 7800X3D core is several times faster, so the lifted code runs several times
slower than the native code would. Small fixes (host primitives, priorities, single lifter switches) did not show.
**Goal**: (1) quantify the gap on the critical path (guest instructions, x86 per guest instruction by class, where the
several-fold gap comes from); (2) build the biggest bit-exact lifter improvements the gap points to, each behind a lift switch,
off until proven; (3) prove exactness (lifter tests, differential tests on synthetic AArch64 words, every FPCR mode the game
uses); (4) measure in the game, both sides with a PGO profile of their own lift: castle-move screen, shots, speed, chain A's
length; (5) hand back.
**Machines**: main PC under the lock (`E:/OOT/locks/mainpc`) for the lifts, the PGO builds and the PGO training runs; the
in-game A/B moved to PC 3050 (i5-10400F, RTX 3050, 2560x1440, lock `C:\OOT\locks\pc3050`) on the coordinator's word, the
main PC's lock queue being long; synthetic tests on the main PC at low priority, the final 2M-run test on the laptop.
**Branch**: `feature/TICK-063-lifter-speed` (from main 9bfc390). Local only.

## Result (2026-11-12): the block lift cuts the executors' CPU time 8-13%, no fps on PC 3050; not adopted, switches stay off

- **Built** (lift switches, all off by default; the default lift is byte for byte main's, key e8dc207cb2): `A64_FPX_BLOCK`
  (straight-line FP runs as speculative XMM blocks, one keep test, the exact per-op code as the fallback), and on top of it
  `A64_FPX_BLOCK_ST` (vector stores inside blocks, made at the commit) and `A64_FPX_BLOCK_CVT` (SCVTF / UCVTF / FCVTZS / FCVTZU
  inside blocks). Exactness: `tests/test_fpx_block.py`, 44 synthetic sequences x 3 switch sets, 2M runs, 0 mismatches; every
  keep test and commit has a tooth that fails it (sections 3 and 2b).
- **In the game** (PC 3050, no PGO on any side, castle-move; section 4): the guest work of the six executors (the job group's
  FP-heavy jobs) is **-8% with the block lift, -13% with stores and conversions** (56.2 -> 51.8 -> 49.1 ms of executor work a
  frame; every blkx trace is below every base trace); the module workers -4 to -6%; the main thread unchanged. Chain A's walked
  work goes 30.0 -> 28.9 ms a frame, inside the trace-to-trace spread. **fps unchanged**: Market fps 29.16 (8 runs) / 29.22 (8) /
  28.60 (4). At 2560x1440 the 3050's frame (36 ms) is held by the main thread and the Presentation Thread, not the executors.
  Shots right, speed 60.0 on every variant, saves identical after every run.
- **Decision**: not adopted (no fps gain); the switches stay off. The executor cut is real and stacks with the render-chain
  work: where the executors are on the critical path (the main PC's job-group wait, TICK-039: ~97% execution), it should show.
- **Main PC with PGO** (both sides with their own profile, castle-move screen): 78.8 -> 79.3 fps, blk above base in 2 of 3
  rounds: inside the spread, not clearly positive (section 4).
- **Not verified**: the block + stores + conversions lift with a PGO profile (session3.sh prepared, not run: the screen was not
  clearly positive); kokiri-idle and hyrule-walk; the full confirm.

## 1. The gap, quantified (castle-move, TICK-052's run B samples with its map; TICK-058's critical path)

Method: `tools/bench/guestmix.py --cover 0.8` over run B's 89,664 samples (BaseProcExecutor0-5, ModuleSystemWorker1-5,
sead::MainThread; 16,826 guest-leaf samples, the 370 sections holding 80% of them, 6.93 M static x86 instructions), joined
with `functions.txt` (guest instructions per function) and the map (x86 bytes per function): `build/tick063`'s `gap.py`
(scratch, not in git). No hardware counters: AMD uProf is not installed and installing it is a system change, so IPC and
front-end stalls are estimated, not measured.

**Expansion (x86 instructions per guest instruction).** Static, over the 370 hot sections: 12.8 x86 / guest instruction
(61.9 bytes). Weighted by the samples (each function's share of the guest time times its own ratio): **16.3 x86 per guest
instruction (78 bytes)**. The whole image: 24.6 bytes per guest instruction over 17.6 M guest instructions (414 MB of x86).
By bucket of the function's ratio:

| x86 per guest instruction | share of the guest samples | functions |
|---|---:|---:|
| under 4 (integer code: f_8008fc00 3.3, f_800e98c0 3.4, f_8004ab40 4.2) | 15.7% | 112 |
| 4-8 | 16.3% | 82 |
| 8-12 (f_8009bb80 9.2, f_80021ca0 10.6, f_80084de0 8.6) | 16.8% | 43 |
| 12-20 (f_801d99a0 18.5, f_808226e0 19.0, f_80029240 19.0) | 22.2% | 56 |
| 20-40 (f_800a8400 27.6, f_8173b340 32.8, f_81730760 33.5, f_81c7f880 35.4) | 22.9% | 62 |
| over 40 (f_8173a830 47.9, f_81746440 59.1, f_81b8c000 46.4) | 6.0% | 15 |

So **half of the guest time runs in functions where one guest instruction became 12-60 x86 instructions**, and those are the
FP / vector kernels (model, matrices, NavMesh, Link's calc). Integer code is at 3-4, which is what a native translation
costs on x86 (the extra context traffic and marks).

**Where the expansion goes** (the static class shares of the 6.93 M instructions): FP / vector ALU 21.6% (the ops and their
lane tests), register moves and LEAs 14.4%, branches 14.8% (the keep tests' branches: one or two per FP op), compares and
tests 9.5%, integer ALU 11.2%, host-global loads 3.0% (the FPX gate's mask, `.refptr`), setcc / cmov 3.0%, context stores
2.6% + loads 1.5% + vector register stores / loads 2.4%, host stack 3.2%; guest memory accesses are only 6.5% of the
instructions (but 33.5% of the time: TICK-055). An FP op of the default lowering is: the gate re-read (2 context loads, a
10-byte mask, 2 tests, a branch), the operand loads from `c->vreg`, the op, ~12 instructions of lane tests on the result's
bits, a branch, the FZ test and branch, a 16-byte store of the register — **~25-40 x86 instructions per guest FP
instruction**, and each op's operand comes from the previous op's context store (a store-to-load hop on the dependent chain).

**In ms of chain A.** TICK-058's walked path: 15.85 ms a frame, of which the game's threads' work ~13.8 ms (main 1.30, the
NavMesh worker 1.18, EX0-5 5.5, MW1-5 2.5, ARes 0.8, the rest) and 68% of the path's work samples are guest leaves: **~9.4 ms of
lifted code on chain A's path a frame** (plus ~1 ms of the runtime under it: FP rescues, obj_lookup, the icall lookup). If the
hot FP code ran at the integer code's expansion (~4 x86 per guest instruction instead of 16-60), the guest part of chain A
would be ~3-4 ms shorter: that is the several-fold gap, and it is concentrated in the FP / vector lowering. The NavMesh
worker alone (1.18 ms on the path, 62% of it in four FP kernels at 45-60 x86 per guest instruction) is ~0.7 ms of it.

**IPC and front end** (estimated): the FP kernels are short dependent chains through the context (store -> load forwarding
~5-7 cycles per hop on Zen 4) between guest loads; TICK-055's finding that moving samples off the chain did not move the
time says they are latency-bound (IPC ~1-1.5), not issue-bound. Code size is a second cost: f_801d99a0 is 1.27 MB of x86
for 14.4 k guest instructions, run about once a frame near straight-line, i.e. ~20 k cache lines of instruction fetch a
frame from L2 / L3 (0.2-0.3 ms of its 1.25 ms if from L3). Both say the same thing: fewer, denser instructions per FP op,
and no context round trip between dependent ops.

## 2. What was built: `A64_FPX_BLOCK` (lift time, off by default)

`third_party/a64-kit/lift/a64_block.py` (called from `a64_lift.hot_text` and `fpx_header` when `A64_FPX_BLOCK=1`; with the
switch off the lift and `a64_fpx.h` are byte for byte as before, checked by the test). In a hot function's text, a maximal
straight-line run of statements the pass knows becomes one speculative block:

```
{ int _bk = 0;
  if (RECOMP_FPX_OPEN(c)) {                       /* the gate once per run */
      const __m128i _fzm = a64_bk_fzm(c); __m128i _bad = 0;
      __m128i _bv20 = a64_bk_ld(c->vreg[20]), ...;  /* the registers the run reads, loaded once */
      _bv1 = a64_bk_vsub(&_bad, _fzm, _bv21, _bv20, L4);  /* each op: the SSE op + its lane tests OR-ed into _bad */
      ...
      if (!_mm_movemask_epi8(_bad)) { a64_bk_st(c->vreg[1], _bv1); ... _bk = 1; }   /* the commit */
  }
  if (__builtin_expect(!_bk, 0)) { <the run's original per-op statements> }   /* the exact fallback, cold */
}
```

- Ops: vector FADD / FSUB (.2S, .4S), FMUL, FMUL / FMLA / FMLS by element, vector FMLA / FMLS, scalar FADD / FSUB / FMUL /
  FNMUL / FDIV / FMADD / FMSUB / FNMADD / FNMSUB / FSQRT, FMUL / FMLA / FMLS scalar by element, FADDP (pairwise and scalar),
  FCMP S (as a block's last op: its flags written at the commit); data movement, exact by construction: DUP (lane, scalar,
  from a general register), INS (lane, zero), EXT (16- and 8-byte), ZIP / UZP / TRN (every width, 16- and 8-byte), ORR / AND /
  EOR / MOV, FNEG / FABS / FMOV (S and D, immediates, from a general register), LDR S / D / Q, LDP S / D / Q (no writeback);
  pure general-register statements between FP ops (pointer loads, address arithmetic, immediates, CSEL: the registers
  shadowed in locals, written at the commit; `A64_FPX_BLOCK_GPR=0` leaves them out). A block needs 2 arithmetic ops
  (`A64_FPX_BLOCK_MIN`). Anything else (a store, a call, a label, a branch, a flag write, FCVT / SCVTF, FMIN / FMAX, the
  binary64 ops) ends the run.
- The lane tests are the default helpers' (`A64_VFPX_SUM`, `a64_vfpx_mul32`, `a64_vfpx_fma32_fast`, `recomp_fpx_div32`,
  `recomp_fpx_sqrt32`, `a64_fcmp32`), lane for lane, computed in SSE and accumulated: a kept lane is one where the Arm op
  returns the IEEE result the host op returns and raises no FPSR bit (the gate: IXC already set, no FPCR control bit but FZ;
  a big result, or the zero rules; under FZ no subnormal operand or addend). The FMA is the hardware fused multiply-add
  (`vfmadd`, one rounding, as the Arm op: the default route's binary64 trick computes the same value). The rescues the
  default helpers have past the fast path (a tiny product once UFC is raised, an overflow once OFC is, quiet-NaN lanes) are
  not in the block: such a lane is "bad" and the whole run takes the fallback, which has them.
- Why it is exact: inside the gate and with every lane kept, each op's result is the same whether computed in the block or
  alone, and no op changes the FPSR, so the context after the run is the same; the block stores nothing before the commit,
  so the fallback starts from the state the run started from, and runs the original statements. Guest loads inside a block
  are the same loads the run makes (no store precedes them in a block), so a racing writer gives one of the interleavings
  the original run could see.
- Builds without FMA3 / SSSE3 (x86-64-v2) compile the blocks out (`A64_BK 0`): the fallback alone, as before.

Coverage on the current lift's hot pieces (the pass run over `code_hot_*.c`, output discarded): 11,236 blocks holding 79,518
of the ~118 k FP arithmetic sites (67%), 123,851 statements in all (4,485 general-register statements carried), the longest
143 statements, mean 7 arithmetic ops a block; f_820e2520 (the NavMesh kernel): 31 blocks, 145 of its ~160 FP ops;
f_801d99a0: 591 blocks, 4,203 ops. What still ends runs: stores from vector registers (~4,500 sites), FCVT / SCVTF (~900),
FMIN / FMAX, 64-bit FP, LDR with writeback.

Static cost on the micro-benchmark kernels (x86 instructions of the whole function, block path + fallback): the NavMesh
kernel 6,524 -> 8,660 (the block path alone ~2,100 for 136 guest instructions, ~16 per guest instruction against 48), vec3
leaf 5,874 -> 7,921, the scalar chain 3,222 -> 5,184.

## 3. Exactness

- `tests/test_fpx_block.py` + `tests/fpx_block_test.c`: 32 synthetic sequences assembled by llvm-mingw's clang (every shape
  above, the NavMesh and vec3 kernels' own sequences, aliased registers, runs broken by stores / compares / general-register
  writes, FCMP of registers the block did not compute, general-register statements inside blocks), lifted with the switch off
  and on, both compiled in one file against the switch's header and the build's guest model, run on random registers (all 32
  vector registers: zeros, subnormals, infinities, quiet and signalling NaNs, extreme normals, midpoints), random guest memory
  (aligned and unaligned), random FPCR (FZ, DN, the rounding modes, other control bits, the host inhibit bit) and FPSR; the
  whole Ctx and the guest memory compared. **2,000,000 runs at x86-64-v3: 0 mismatches, the block path taken in 496,756 of
  them; 2,000,000 at x86-64-v2 (blocks compiled out): 0.** The switch off leaves `fpx_header` and `hot_text` as before.
- With the stores and conversions (section 2b), 132 cases (44 sequences x 3 switch sets), on the laptop (Ryzen 7 5700U):
  **2,000,000 runs at x86-64-v3, 0 mismatches, the block path taken in 441,297; 2,000,000 at x86-64-v2, 0**.
- Teeth (each fault put in by hand, 100,000 runs, then restored): the product keep test without its FZ operand test 100
  mismatches; FMLS's negation dropped 122; EXT's byte map off by one 577; the sum's FZ big-or-zero test dropped 4; the scalar
  FMA's addend FZ test dropped 3; FCMP's unordered test dropped 100; FCMP's FZ subnormal test dropped 18; a general register
  written in a block but not committed 981.
- `test_fpx_xmm`, `test_fpx_slowpath`, `test_fpx_sticky`, `test_vfpx_elem`: their inputs are unchanged with the switches off
  (the default lift is byte for byte main's: `test_fpx_block` checks it, and the lifted-code key is e8dc207cb2 on both PCs).

## 2b. The second lever: stores and conversions inside blocks (off by default)

What still ended runs on the hot pieces (the pass over `code_hot_*.c`): the vector stores (~6,600 sites: STR S / D / Q, STP S / D /
Q, through SP or not), then FCVT / SCVTF (~900 vector / lane, ~900 to and from W registers).
- `A64_FPX_BLOCK_ST=1`: a store joins the run. Its address and value are taken where it stands (`_saK`, `_svK` locals), and the
  store itself is made at the commit, after the keep test, in program order, through the same `recomp_store*` / `recomp_stp*`
  helper, so the soft write watch's marks are the same. A vector load, or a general-register statement that reads guest
  memory, after a store starts a new run, so no load in a block reads past a delayed store; stores move later only past register
  arithmetic. Leading stores stay outside the block (nothing to carry them for). The fallback still starts from a run that stored nothing.
- `A64_FPX_BLOCK_CVT=1`: SCVTF / UCVTF (binary32 from 32-bit: vector, lane, from a W register) and FCVTZS / FCVTZU (binary32 to
  32-bit, toward zero: vector, lane, to a W register; the register is shadowed and committed like the other general registers).
  In the gate the rounding is to nearest and IXC is set, so the host conversion is the Arm one with no new flag. Bad lanes (to the
  fallback): UCVTF of a value >= 2^31; FCVTZ* of |x| >= 2^31 (NaN and infinity included); FCVTZU of x <= -1; under FZ a
  subnormal operand (IDC). A to-W conversion is matched only as a statement on a line of its own.
- Coverage (the pass over the hot pieces): blocks 11,236 -> 10,452 (runs merged), arithmetic ops in blocks 79,518 -> 82,545,
  5,643 stores and 571 conversions inside blocks, the longest block 159 statements.
- Exactness: 11 more sequences (stores inside a run, stores that overlap through register offsets, a register stored then
  overwritten, a base register moved between stores, a load and a general-register load after a store, STP Q / STUR Q; the
  conversions: vector, lane, to and from W registers, in range, subnormal). Every sequence runs three times against the default
  lowering: the switch alone, + _ST, + _ST + _CVT. Teeth (100,000 runs each):
  - stores committed in reverse order: 72 mismatches;
  - a stored value replaced: 1,559;
  - the value read at the commit, not where the store stands: 162;
  - the address replaced: 1,605;
  - no new run at a load after a store: 202;
  - UCVTF without its >= 2^31 test: 6;
  - FCVTZ* without its range test: 133;
  - FCVTZU without its negative test: 124;
  - FCVTZ* without its FZ subnormal test: 7;
  - FCVTZ* to a W register, not committed: 206.

## 4. In the game

**Main PC (PGO): built, not measured.** Session 1b (22:40-23:04, under the lock):
- three PGO training runs of `bin/gen` (the block lift, instrumented): kokiri-idle, castle-move and hyrule-walk, 115 s each; saves
  identical, no game left;
- the merged profile (6:1 with the shipped one, as TICK-045);
- `bin/blk`, built from the same block lift (lifted code 877598a7ef, as gen's) with that profile;
- the default lift put back in the shared work folder (verify_shared: base lift, the repo's profile; game folder as found).

The screen (session2) was run later, after the PC 3050 measurement: see below.

**Main PC, PGO pair (session 2, 2026-11-12 03:12-03:32, under the lock after TICK-060m and SYNC-12).** `bin/base` (default lift,
the shipped profile) against `bin/blk` (`A64_FPX_BLOCK=1`, its own profile from three `gen` runs), the same host code; run_bench
110 s, 66 s cut, castle-move:
- Shots (every 600 presents) and the speed log first: the title, the save's load and the Market on both; shot-by-shot mean luma
  alike (Market: base 106 104 101 98 100 72 81, blk 106 106 106 105 62 89 76, the walk's timing a little apart); speed (last 40 s)
  median / min 59.99 / 59.11 base, 59.99 / 59.42 blk. fps in those runs 76.8 base, 79.5 blk.
- Screen, 3 interleaved rounds (fps / 1% low / p99 ms; frame prep = the log's submit):

| round | base | blk |
|---|---|---|
| 1 | 78.9 / 46.9 / 17.4 | 80.6 / 49.6 / 17.0 |
| 2 | 77.6 / 47.1 / 18.1 | 78.4 / 46.3 / 19.0 |
| 3 | 79.8 / 48.0 / 17.5 | 78.9 / 47.8 / 17.8 |
| mean | 78.8 / 47.3 / 17.7, submit 7.67 | 79.3 / 47.9 / 17.9, submit 7.84 |

  blk above base in 2 of 3 rounds (3 of 4 counting the shot runs; +0.5 fps on the screen, +1.0 over the four pairs): inside the
  2-4 fps spread, **not clearly positive**, so session3 (blk + stores + conversions with its own profile) was not run.
- One sampler run each (`HOS_JOBTRACE` 88-91 s inside `HOS_SAMPLE_ALL` 86-93 s; the sampling lengthens the traced frames to
  12.6 / 13.3 ms): executors' work a frame 25.9 -> 25.2 ms (-2.6%), module workers 11.6 -> 11.8 ms, the executors' part of the
  walked path 4.35 -> 4.05 ms, chain A's walked game work 8.52 -> 8.33 ms. One trace a side: inside the noise.
- After the session: game folder as found (TICK-054's build), saves identical to user-save-backup-1111-2131, no game left, the
  shared work folder on the base lift (verify_shared), the lock released at 03:32.

**PC 3050** (i5-10400F 6C/12T, RTX 3050, 2560x1440, session 0 over SSH). Setup: the branch as a bundle in `C:\OOT\t63`, three
work folders copied from `w56`, TICK-055's no-PGO recipe (`b63.bat`, `-march=x86-64-v3`). Builds:
- `t63base`: the default lift (e8dc207cb2); every object came from the cache;
- `t63blk`: A64_FPX_BLOCK (877598a7ef); hot pieces compiled in 1198 s;
- `t63blkx`: + _ST + _CVT (6764240245).

The exe is 462 MB with blocks, against 448 MB without.

- Shots (every 300 presents) and the speed log, castle-move. All three show the title, the save's load and the Market. Market
  shots' mean luma: base 99; blk 104, 104, 79, 65, 66, then a scene load (the triforce screen: with the Market loaded earlier,
  Link walks into a door); blkx 96. Speed median 60.05 / 60.06 / 60.01. The first base run was spent in the start-up precompile
  (a new hookshot_vk.dll: 3 min of pipelines, killed by run_bench at 112 s) and is left out.
- fps: run_bench's 66 s cut takes in the end of the save's load (300+ fps) in some runs (one base run gave 85.5), so the table
  counts the Market only (the log's fps lines after the load). castle-move, interleaved rounds (screen, trace rounds, shot runs):

| variant | Market fps per run | mean |
|---|---|---:|
| base | 29.33 29.00 29.50 28.73 28.75 29.58 29.17 29.25 | 29.16 |
| A64_FPX_BLOCK | 29.00 29.58 30.36 28.64 29.83 28.55 28.67 29.17 | 29.22 |
| + _ST + _CVT | 28.17 28.42 28.00 29.83 | 28.60 |

- Chain A (`HOS_JOBTRACE` 88-91 s, castle-move; `critpath.py --anchor main` and `jobtrace.py`; 4 / 4 / 3 traces):

| variant | frame (ms) | chain A's walked work a frame (ms) | executors' work a frame (ms, six threads) | module workers' (ms) | main thread a second (ms) |
|---|---|---|---|---|---|
| base | 36.8 36.5 35.3 36.7 | 30.3 31.1 30.2 28.3 = 30.0 | 56.2 | 23.4 | 469-489 |
| A64_FPX_BLOCK | 37.3 38.4 33.9 36.3 | 27.5 30.3 27.7 30.0 = 28.9 | 51.8 (-7.8%) | 22.0 | 471-488 |
| + _ST + _CVT | 37.4 37.2 37.2 | 29.7 29.1 28.0 = 28.9 | 49.1 (-12.6%) | 22.5 | 474-488 |

  Executors' work a second: base 1524-1567, blk 1384-1499, blkx 1302-1329 (no overlap between base and blkx).
  - The main thread's work is the largest piece of the walked path (12.6-13.9 ms) and does not move. Its hot code (draw lists,
    the module phase) is integer and pointer code, which the blocks do not touch.
  - The frame is held by the main thread and the Presentation Thread (GPU 18.8 ms a frame at 1440p on the 3050), so the
    executors' cut does not reach fps here.
- After every run: no game left, saves identical to `C:\OOT\user-save-backup`. After each lock hold (00:17-00:54,
  00:59-01:44, 01:48-02:08) the game folder went back to main32d (exe 2979897a, dll bbf74311) and the lock was released.

## 5. Next

1. **The main-PC PGO pair is measured** (+0.5 fps, 2 of 3 rounds: inside the spread). If the blocks are taken up later, take
   them with the stores and conversions (session3.sh: its own instrumented build, training runs and profile) and a full
   confirm; alone they do not clear the spread.
2. **The NavMesh kernels.** In f_820e2520, 31 blocks already hold 145 of ~160 FP ops. What remains is the guest loads between
   the blocks and the code size: 118 KB with the fallback, against 94 KB. A block whose fallback is a call to an outlined copy
   of the run, instead of inlined text, would halve the hot text growth (29 -> 39.6 MB of hot code now).
3. **The main thread** is the part of chain A the FP lowering does not reach. Its guest loads (TICK-055: the critical path) and
   the context traffic are the next lifter levers (guest registers in host registers across calls, TICK-041 / TICK-055).
