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
**Machines**: main PC under the lock (`E:/OOT/locks/mainpc`) for the lift, the builds and the runs; the laptop was unreachable
(SSH timed out), so the synthetic tests ran on the main PC at low priority. PC 3050 not used.
**Branch**: `feature/TICK-063-lifter-speed` (from main 9bfc390). Local only.

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
- Teeth (each fault put in by hand, 100,000 runs, then restored): the product keep test without its FZ operand test 100
  mismatches; FMLS's negation dropped 122; EXT's byte map off by one 577; the sum's FZ big-or-zero test dropped 4; the scalar
  FMA's addend FZ test dropped 3; FCMP's unordered test dropped 100; FCMP's FZ subnormal test dropped 18; a general register
  written in a block but not committed 981.
- `test_fpx_xmm`, `test_fpx_slowpath`, `test_fpx_sticky`, `test_vfpx_elem`: see the run_all line in the result.

## 4. In the game

(filled in below)
