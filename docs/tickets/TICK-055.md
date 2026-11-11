# TICK-055 — Audit of the lifter's generated code on x86-64, then the top fixes  · model: Opus · after TICK-054 (parallel)
**Why**: the ~14 ms CPU frame at the Market is the game's own lifted code: a job chain of ~6.5 ms over 6 executors, serial long
actor jobs (Link 800b3ce0, 80166100, 802ca820, module job 80083580), ~1.3 ms of the main thread's draw code and module phases.
Faster lifted code shortens all of it at once, and more on ordinary CPUs (PC 3050, i5-10400F).
**Goal**:
1. phase 1, no game run: the generated C and the x86-64 code clang makes of it for the hottest functions, the systematic
   inefficiencies classified with counts and an estimated cost, micro-benchmarks on synthetic AArch64 (no game code),
   `docs/research/lifter-x86.md` ranked by gain against risk and effort;
2. phase 2: the top 1-3 fixes, each a lift-time switch (`lift_switch` / off until measured), bit-identical (differential tests
   against the old lowering on synthetic sequences: flags, NaN, edge cases, unaligned accesses), measured like with like on PGO
   (TICK-045: a changed lift without its profile loses 4-5 fps): the hot serial jobs' CPU time first, then fps (castle-move, 3
   interleaved runs a variant).
**Machines**: main PC: no game, no heavy build (TICK-054 benchmarks there). PC 3050: builds and game runs once its busy flag
is gone. Laptop: game-free tests and micro-benchmarks only.
**Branch**: `feature/TICK-055-lifter-x86` (from main c632e9b, main 8ece4b3 merged in). Local only.

## Result (2026-11-11): audit done; two lift switches built and tested bit-exact; no in-game gain, not adopted

**Audit** (`docs/research/lifter-x86.md`; castle-move samples of TICK-052's build, skid-corrected over the sections holding 80%
of the guest time): guest loads 33.5%, integer work 22.9%, FP/vector with FPX checks 18.2%, context registers 8.9%, host stack
4%, flags 1.5%, prologues 1.2%, FP env 1.1%, dirty marks 1.0%, polls 0.7%. The profile is flat (top 50 functions 43.5%).
Already as good as x86 allows: guest addressing (one `[base+reg+disp]` instruction, no checks), atomics, DMB (0.01%),
exclusives, the icall site cache. Ranked: (1) scalar FP values bounce GPR <-> XMM (5.6% of samples on the moves, more than
the FP ops' 3.7%); (2) the FPX gate re-read per op (~1.6%); (3) the dirty-page marks (~1.7%, `.refptr` loads, a pinned
register); (4) flag stores (~1.5-2.5%); (5) a store-forwarding stall on LR at function entry (SLP-merged prologue loads,
~0.9%); (6) context write-through; (7) the Win64 calling convention (~1.1%).

**Built** (lift time, off by default; with both off the lift is byte for byte as before, key e8dc207cb2):
- `A64_FPX_XMM=1` (`a64_lift.xmm_text` / `xmm_header`, hot functions): scalar binary32 FADD/FSUB/FMUL/FNMUL/FDIV/FMA in XMM
  registers with the default keep tests and slow paths; S/D/Q loads straight into XMM; scalar results as one 16-byte store.
- `A64_LR_SCALAR=1` (`op_text`): the frame record's STP reads LR as a scalar (no 16/32-byte merged load of a just-stored LR).

**Bit-exactness**: `engine/tests/test_fpx_xmm.py` + `fpx_xmm_test.c`: 34 guest words and dependent sequences, old against new
lowering on random registers, guest memory (unaligned too) and FPCR/FPSR (FZ, DN, rounding modes, NaNs, subnormals,
midpoints), whole Ctx + memory, x86-64-v3 and v2: 1 M runs each, 0 mismatches; four seeded bugs caught (447 to 31,735
mismatches). test_vfpx_elem, test_fpx_slowpath, test_fpx_sticky, test_mark_groups pass; run_all (laptop) 235 pass, 2 fail (both
from the TICK-053 merge), 33 skip.

**Micro-benchmarks** (`tools/bench/lifter_mb/lifter_mb.py`, synthetic): dependent FP chain -61% (Zen 2) / -43% (Comet Lake),
FDIV chain -52/-40%, FNEG then vector read -52/-52%, LDR Q then vector op -51/-50%, LDR/FADD/STR -43/-32%, vec3 dot -16/-13%.

**In the game** (PC 3050, both builds without a PGO profile): guest CPU ~21.9 ms a frame base against ~22.3 with both switches
(sampler, 2+2 runs); the serial jobs unchanged; castle-move 29.0 / 1% low 15.3 against 28.3 / 14.2 (3 interleaved rounds):
inside the spread. The crossings' samples fell 5.0% -> 3.9% but moved to the FP ops and the keep tests' bit copies: the game's
FP chains are short and sit between guest loads, which are the critical path. **Not adopted**: both switches stay off.

**Not verified**: a PGO-profiled pair (a gen build and three profile runs per lift); the main PC (busy with TICK-054); Kokiri and
Hyrule; shots and the speed log (the switches are not adopted, and the in-game state is bit-exact by the tests).

**Next**: the guest loads are the critical path. Lifter levers on it: guest registers kept in host registers across calls
(the call-boundary store/reload hops). Host: large pages for the guest heap (needs the user's "Lock pages in memory" right).
Findings 2-4 and 7 are instruction-count cuts off the critical path: only as one combined, profiled lift, judged first on the
job chain's CPU.
