# The lifted code on x86-64: an audit of what the lifter makes clang emit (TICK-055, 2026-11-11)

What the AArch64 -> C lifter (a64-kit + `engine/nx2`) costs on x86-64, beyond the game's own work, ranked by the gain
expected against the risk and the effort. Phase 1 of TICK-055: no game run, no heavy build. The fixes and their
measurements are in the sections at the end.

## Sources and method

- Build: the dev work folder `%LOCALAPPDATA%\HookshotBuilder` (`dev-build.ps1 -Work`): lifted C in `sa\gen`
  (89 files, 1.5 GB: 8 `code_hot_*.c` pieces at -O2 with the PGO profile, the rest -O1), objects in `sa\build\obj`, the
  exe and its map in `sa\build`. The guest code addresses are the same in TICK-052's `B` build and in the current one
  (same lift, same hot list), so samples taken on either map onto both. Lift switches: the TICK-045 defaults
  (`A64_VFPX_ELEM`, `A64_FPX_SLOWPATH`, `A64_FPX_FMA3` on).
- Samples: TICK-052's castle-move run `B` (`HOS_PROFILE` + `HOS_SAMPLE_ALL`, every 2 ms, 72-104 s, 89,664 samples of
  the job and main threads: BaseProcExecutor0-5, ModuleSystemWorker1-5, sead::MainThread), with that build's exe and map.
  16,826 samples (18.8%) have a guest function as their leaf; the rest wait in the system (77%) or run the host runtime.
- Instructions: `tools/bench/guestmix.py --cover 0.8` disassembles the sections that hold 80% of the guest samples
  (370 sections, 6.93 M instructions). A timer sample's IP is the instruction after the one holding retirement, so every
  share below is skid-corrected (charged to the instruction before the IP), as in TICK-041.
- The profile is flat: the top function holds 4.1% of the guest samples, the top 50 hold 43.5%, the top 80 52.7%.
  So the levers are the systematic patterns, not single functions. The large functions are giant after inlining
  (f_801d99a0: 267 k x86 instructions, f_8009bb80: 114 k).
- Micro-benchmarks: synthetic AArch64 words only (no game code), lifted by the lifter's own pipeline
  (`a64emit.translate` -> `a64_lift.op_text` -> `hot_text`), compiled with the hot pieces' flags against the build's
  generated headers (nx2/rtgen's 64-bit window, soft write watch, polls), run on the laptop (Ryzen 7 5700U, Zen 2).
  Each kernel repeats its words 8 times in one function; time is the best of 41 interleaved runs of 100,000 calls.

### Where the guest code's time goes (castle-move, skid-corrected, 13,462 samples in the covered sections)

| class | share | TICK-041 (base, before the switches) |
|---|---:|---:|
| guest memory loads and stores (`[base + reg + disp]`) | 33.5% | 28.1% |
| integer work (ALU, compares, moves, setcc) | 22.9% | 25.7% |
| FP and vector ops with their FPX checks | 18.2% | 16.5% |
| context registers (`c->x[]`, `c->vreg[]`, SP) | 8.9% | 8.1% |
| host stack (spills, reloads) | 4.0% | 10.0% |
| other memory (pointers derived from the context) | 3.6% | 3.9% |
| branches | 3.4% | 2.7% |
| context flags (`c->n/z/c/v` bytes) | 1.5% | 1.1% |
| calls, returns, prologues | 1.2% | 1.2% |
| FP environment (`fpcr`/`fpsr` loads and tests) | 1.1% | 1.2% |
| host globals (dirty-page marks, site caches) | 1.0% | 0.8% |
| polls | 0.7% | 0.7% |

The guest loads are the game's own pointer chasing: each is one x86 instruction already (below). What the lifter
adds is spread over the other rows; the findings below cut across them.

## What is already as good as x86 allows (checked, no change proposed)

- **Guest memory access**: one instruction. nx2/rtgen.py's 64-bit window makes `A64_PTR(a)` = `A64_MEM + a`, with
  `A64_MEM` a constant folded into a callee-saved base register, so a load is `mov rax, [base + reg + disp]`. No
  translation table, no bounds or escape check, no byte swap (both little-endian); `recomp_load*/store*` are
  `always_inline` memcpy's. `restrict` + `A64_NOESC` on the context keep the guest memory (inttoptr) from aliasing it.
- **32-bit guest ops**: the `_r &= 0xFFFFFFFF` of W-register ops compile to 32-bit x86 ops (free zero-extension).
- **Atomics** (TICK-022): `lock xadd` / `lock cmpxchg` for the LSE ops are what x86 needs (1.2% of the samples).
- **Barriers**: every DMB / DSB / ISB is `recomp_barrier()` (an out-of-line seq_cst fence). 870 of the 3,822 DMBs in the
  game are `DMB ISHLD` (154 in hot functions), which x86-TSO needs no fence for. But the runtime leaf `recomp_barrier`
  has 0.01% of the samples: not worth a change.
- **Exclusives** (LDXR/STXR calls): not in the sampled leaves at all; the game uses LSE.
- **Indirect calls**: one site cache per BLR (`A64_SITE_CALL`), 0.06% of the static instructions.
- **Compile level and layout**: the hot pieces (-O2, PGO) hold 93-96% of the job path (TICK-040); -O3 was no faster.

## Findings, ranked (expected gain against risk and effort)

### 1. Scalar FP values bounce between the integer and the vector registers — built as `A64_FPX_XMM`, not adopted (below)

The lifted C handles every FP value as an integer bit pattern (`uint64_t`, `memcpy` from `c->vreg`), and the FPX fast
paths test the result's bits. clang therefore keeps the values in general registers and moves them to an XMM register
for each op and back after it. On a dependent chain an FMUL S becomes: `vmovd xmm, r` (operand) -> `vmulss` ->
`vmovd r, xmm` (result, for the checks and the vector-register write) -> `vmovq xmm, r` + 16-byte store (the write)
-> `vextractps r, xmm` (the next op's operand, forwarded) -> `vmovd xmm, r` -> the next op: ~4 domain crossings of
~3 cycles each around a 3-4 cycle op.

- Samples on the crossings (`vmovd/vmovq` both ways, `vpinsr*`, `vpextr*`/`vextractps`): **5.6%** of the guest
  samples, more than the scalar FP arithmetic itself (3.7%). Per serial job (the samples whose stack holds the job):
  Link's calc f_800b3ce0 8.8% crossings + 5.2% FP ops; 80166100's job 4.9% + 6.4%; f_802ca820 6.4% + 4.6%; the
  dispatcher f_8004ab40's whole subtree 6.0% + 3.8%.
- Static, in the hot pieces: 16.5 k `a64_fpx_hot_mul32_fma32`, 12.7 k `fma32`, 7.8 k / 4.1 k scalar FADD / FSUB S,
  2.6 k FDIV S, 17.7 k `LDR S` and 4.5 k `LDP S` into vector registers (each a GPR load + `vmovq` + store), 5.6 k float
  results written as `vset64(0)` + a 4-byte store (FNEG, FABS, FCSEL, SCVTF..., read whole later: a store-forwarding
  failure, TICK-041's remaining stall).
- Micro-benchmark (prototype, laptop Zen 2), ns per guest instruction, base -> float-domain lowering:

  | kernel | base | new | |
  |---|---:|---:|---:|
  | FMUL / FADD / FMADD / FSUB S, one dependent chain | 4.14 | 1.62 | -61% |
  | vec3 dot from memory (LDP S, LDR S, FMUL, 2 FMADD, STR S) | 1.07 | 0.84 | -22% |
  | LDR S -> FADD S -> STR S through guest memory | 2.05 | 1.19 | -42% |

  Final context and guest memory identical between the two in every kernel.
- Expected: FP/vector is 18% of the guest time and the crossings 5.6%; removing most crossings is worth ~3-6% of the
  guest time, more in the FP-heavy jobs (model, matrices, Link's calc). Risk low: the same IEEE ops on the same bits,
  the same keep tests, the same slow paths; only where the values live changes. Effort medium: a lift-time text pass
  plus float-typed helpers. It changes hot functions' text, so it needs a profile recorded on its lift (TICK-045).

### 2. The FPX gate is re-read before every FP op

`RECOMP_FPX_OPEN(c)` (`fpsr.IXC` set and no FPCR control bit but FZ) is two context loads, a 10-byte `movabs` of the
mask and two tests before each FP op, because any out-of-line slow path in between could have written `c->fpsr`.
1.9% of the hot instructions are `fpcr`/`fpsr` loads and tests, 0.6% the mask `movabs` (it also pins a register);
samples 1.4% + 0.2%. The gate only closes through MSR FPSR / FPCR or a guest call (the helpers only ever set FPSR
bits), so it can be evaluated once per function and again after calls and MSRs; a closed gate only sends ops to the
exact body, which is always correct. Expected ~1%; risk low-medium (every MSR and call site must refresh it);
effort medium; changes lifted text (profile).

### 3. The dirty-page marks

Every guest store not through SP/FP marks the page of its first and last byte in `a64_dirty` (soft write watch,
`A64_MARK`): `lea`, `shr 12`, `and 0x7ffffff`, a byte store, twice, plus a load of `.refptr.a64_dirty` (mingw's
indirection for an extern array) wherever the base is not kept in a register. In the covered sections: 37.7 k mark
stores, 60.0 k `.refptr` loads (0.87% of the instructions), 22.8 k masks; samples 1.0% + 0.3% + 0.3% + 0.05%.
A table at a fixed host address next to the guest window (e.g. `A64_HOST_BASE - 2^27`) would be addressed from the
guest base register (`mov byte [base + idx - 0x8000000], 1`): no `.refptr` load, no second pinned register, and with a
table covering the whole 39-bit window no mask (an address past it faults on the data store first). Expected ~1-1.5%
and smaller code; risk medium (the host's scanners in `rx_backend.c` / `hos_mem.c` use the array); effort medium.
Merging the two marks was measured before (TICK-022 `RX_MARK_CROSS_ONLY`: +26 MB, noise).

### 4. Flags written to the context

`setcc` is 3.3% of the hot instructions (2.5% of the samples), flag byte stores 0.8% (1.5% of the samples). The dead-flag
pass already drops flags no later instruction reads (the ABI's calls clobber NZCV, RET reads none); what stays is
stored to `c->n/z/c/v` and re-read across joins. `A64_LOCAL_FLAGS` (a lifter experiment, never measured in HeyListen)
keeps them in locals and writes them back only where control leaves other than by RET or BL. Expected ~1-2%; risk low
(the switch exists); effort: a profiled A/B only.

### 5. Store-forwarding stall on LR at function entry

clang's SLP vectorizer merges a prologue's `STP x29, x30` / `STP x27, x28` context reads into one 16- or 32-byte load
(`vmovups xmm, [ctx + 0xe8]`, `vpermpd ymm, [ctx + 0xd8]`), but the caller has just stored `c->x[30]` (the return
address) with an 8-byte store: the wide load cannot take its data from it and waits for the store to commit
(~10-15 cycles), at the entry of nearly every non-leaf function. 292 such loads in the covered sections, 0.9% of the
samples on them and the instruction after. Fix: read LR as a scalar (an opaque copy) in the frame setup, or
`-fno-slp-vectorize` for lifted code (broader). Expected ~0.5-1%; risk low; effort small.

### 6. Context registers written through (`c->x[]` stores)

2.6% of the hot instructions are `c->x` stores (written through: the value stays in a host register), loads 1.5%;
context traffic 8.9% of the samples, mostly as the consumer of a slow guest load. `A64_LOCAL_REGS` (locals with
write-back at calls and returns) measured no gain in Hookshot (2026-10-21); TICK-041 bounded the call-boundary part at
~1%. Keeping callee-saved guest registers in host registers across calls is the large project (TICK-040/041); not
attempted here.

### 7. The Windows x64 calling convention between lifted functions

A lifted function is `void f(Ctx *)` with the Win64 ABI: a non-leaf one saves rbx, rbp, rsi, rdi, r12-r15 and any of
xmm6-xmm15 it uses. Pushes/pops and xmm6-15 saves/restores: 1.1% of the samples (push 0.69%, the first instructions
after a call also absorb the call's front-end cost). `sysv_abi` (rsi, rdi and all XMM volatile) or `preserve_none`
on the lifted functions and `A64Fn` would remove most of it, but every hook, stub, table and host call through `A64Fn`
must agree. Expected ~0.5-1%; risk medium; effort high.

### 8. Smaller items

- Polls (`cmp [ctx.poll], 0` at loop headers): 0.7-0.8%; needed by the virtual cores.
- `vzeroupper` before each call (0.6% of the instructions, 0.04% of the samples): cheap.
- Guest memory TLB: the guest heap is in 4 KB pages; pointer chasing over a large heap misses the dTLB (Zen 4 L2 dTLB
  covers 12 MB, Comet Lake's 6 MB). Large pages need the "Lock pages in memory" privilege (a system setting for the
  user to grant); not a lifter change, not measured here.

## Phase 2 choice

1. `A64_FPX_XMM` (finding 1): the largest share, low risk, and it shortens the serial jobs' FP chains.
2. The LR read at function entry (finding 5): small, cheap, low risk.
3. The FPX gate (finding 2), if time allows; it touches the same helpers as 1.

Fixes, their tests and measurements: see below and `docs/tickets/TICK-055.md`.

## Phase 2: what was built (lift-time switches, off by default)

### `A64_FPX_XMM=1` (finding 1)

`a64_lift.xmm_text` runs on the hot functions' text (in `hot_text`, after the FPX helpers' hot names) and
`xmm_header` adds its helpers to `a64_fpx.h`. With the switch off, `fpx_header` and `hot_text` give the text they gave before
(checked by the test), so the default lift is byte for byte the same (PC 3050's base lift: lifted-code key e8dc207cb2, as
TICK-045's).
- Scalar binary32 FADD / FSUB / FMUL / FNMUL / FDIV / FMADD / FMSUB / FNMADD / FNMSUB: the operands are `float`s read from
  `c->vreg` and pinned in XMM registers (`A64_XF`, an empty asm with an `x` constraint, so their bits are a copy and not
  their source); `a64_fpxf_hot_*` runs the same IEEE op under the same gate and the same keep tests (the
  `RECOMP_FPX_*` macros on copies of the bits); past them a `noinline` wrapper calls the default out-of-line helper
  (`a64_fpx_*`, with its rescues and the exact body) and returns a float. The result is written by `a64_vset_f32`: the
  lane zero-extended (`_mm_set_ss`) and stored as one 16-byte store, as `a64_vset64` did. The FMA's fast path is the
  hardware FMA only where the default one is (`A64_FPX_FMA3` and an FMA3 build); elsewhere it takes the default inline
  helper. The negations of FNMUL / FMSUB / FNMADD / FNMSUB are float negations (a sign flip, as the `^= 0x80000000`).
- LDR S / LDR D / LDP S into vector registers: `a64_vld32` / `a64_vld64` load straight into an XMM register
  (`movd` / `movq` from guest memory, upper lanes zero); LDR Q / LDP Q: `a64_vld128`, one 16-byte load and store instead of
  two 8-byte halves that a later 16-byte read could not forward from.
- A scalar result written as a zeroed register plus a 4- or 8-byte store (`a64_vset64(v,0); memcpy(&v[0],&r,n)`: FNEG,
  FABS, FMOV, FCVT...): `a64_vset_lo4f` / `a64_vset_lo8f`, one store.
- Sites in the hot pieces: FMUL 15,288, FMA 12,461, FADD 4,759, FSUB 4,101, FDIV 2,378, LDR S 17,714, LDR D 1,602, LDP S
  4,452, LDR Q 13,495, scalar writes 8,619. Every game patch (oot_patches) still applies (the same 14 places).

### `A64_LR_SCALAR=1` (finding 5)

In `op_text`, the frame record's `STP x29, x30, [sp, ...]` (`recomp_stp64_sp(c, ..., c->x[29], c->x[30])`) becomes
`recomp_stp64_lr`, which passes LR through an empty asm (`"+r"`): the SLP vectorizer can no longer merge the two context
reads into one 16- or 32-byte load. The guest stores are the same two 8-byte stores. (`MG_PLAIN` knows the new name, so mark
grouping treats it like `recomp_stp64_sp`.)

### Equivalence (`engine/tests/test_fpx_xmm.py` + `fpx_xmm_test.c`)

34 guest words and sequences, lifted with the switches off and on, compiled in one file against the switches' header and the
build's own guest model (nx2/rtgen: 64-bit window, soft write watch, polls), run on random registers, random guest memory
(aligned and unaligned addresses) and random FPCR / FPSR (FZ, DN, rounding modes, other control bits, the host inhibit
bit), comparing the whole Ctx and the guest memory; at x86-64-v3 and x86-64-v2. The sequences chain ops through the register
the previous one wrote (as a lane and as a whole). 1 M runs a -march: 0 mismatches. Teeth (by hand, 200 k runs): the add's
FZ sum test dropped: 447 mismatches; the FNMUL negation dropped: 12,861; `a64_vld32` loading 8 bytes: 31,735; the FMA's
FZ addend test dropped: 224 at v3. `test_vfpx_elem`, `test_fpx_slowpath`, `test_fpx_sticky`, `test_mark_groups` pass;
run_all (laptop): 235 pass, 2 fail (both from main's TICK-053 merge: test_parallel_rb_refresh, test_present_semaphores).

### Micro-benchmarks (`tools/bench/lifter_mb/lifter_mb.py`, both switches, ns per guest instruction)

| kernel | laptop (Zen 2) base | new | | PC 3050 (Comet Lake) base | new | |
|---|---:|---:|---:|---:|---:|---:|
| FMUL / FADD / FMADD / FSUB S chain | 4.14 | 1.60 | -61% | 3.39 | 1.94 | -43% |
| vec3 dot from memory | 1.08 | 0.91 | -16% | 1.26 | 1.10 | -13% |
| FDIV S -> FMUL S chain | 4.78 | 2.29 | -52% | 4.05 | 2.42 | -40% |
| FNEG S -> FADD .4S -> FNEG S | 3.04 | 1.45 | -52% | 2.79 | 1.33 | -52% |
| LDR S -> FADD S -> STR S | 2.07 | 1.19 | -43% | 1.60 | 1.09 | -32% |
| LDR Q -> FADD .4S | 4.26 | 2.11 | -51% | 4.18 | 2.10 | -50% |

Static, one hot piece (code_hot_7, -O2, no profile): GPR -> XMM moves 25,605 -> 10,441; XMM -> GPR 9,186 -> 24,092 (the
keep tests' copies of the result bits, off the data path); `vpinsr*` 1,278 -> 1,026; `vextractps`/`vpextr*` 4,889 -> 3,239;
instructions +0.7%, object -0.07%.

## Phase 2: measured in the game (PC 3050, i5-10400F, castle-move)

Both builds from the same source (this branch, main 8ece4b3 merged in), lifted and built on PC 3050, **neither with a PGO
profile** (like with like; the shipped profile matches only the default lift): `t55base` (switches off; lifted code key
e8dc207cb2, TICK-045's lift) and `t55xmm` (`A64_FPX_XMM=1 A64_LR_SCALAR=1`, key ce778b52cd). Exe 448 / 450 MB.

**CPU of the job threads** (`HOS_PROFILE` + `HOS_SAMPLE_ALL`, 72-104 s, 2 ms samples, runs base, xmm, xmm, base), ms of
CPU per second of wall time:

| run | fps | busy | guest leaf | Link 800b3ce0 | 80166100 | 802ca820 | dispatcher 8004ab40 |
|---|---:|---:|---:|---:|---:|---:|---:|
| base 1 | 27.5 | 746 | 599 | 85 | 41 | 13 | 409 |
| base 4 | 27.9 | 776 | 612 | 84 | 51 | 14 | 429 |
| xmm 2 | 27.8 | 781 | 625 | 82 | 50 | 18 | 442 |
| xmm 3 | 27.7 | 774 | 616 | 87 | 49 | 13 | 426 |

Guest code ~21.9 ms of CPU a frame (base) against ~22.3 (xmm): no reduction, inside the run-to-run spread; the serial jobs
the same.

**Where the samples moved** (guestmix, skid-corrected, base 4 against xmm 3): GPR -> XMM moves 2.55% -> 1.13%, `vpinsr*`
0.74% -> 0.51%, lane extracts 0.78% -> 0.42%, but XMM -> GPR copies 0.90% -> 1.84% and the scalar FP ops 3.17% -> 3.82%:
the crossings fell from 5.0% to 3.9% of the guest samples, and the time went to the ops and the bit copies that feed the
keep tests. FP/vector as a whole 17.4% -> 18.6%. Prologue/epilogue samples 2.4% -> 2.0% (`A64_LR_SCALAR`), small.

**fps** (3 interleaved rounds, `ab53.py`, 110 s runs, 66 s cut; fps / 1% low / p99 ms):

| | r1 | r2 | r3 | mean |
|---|---|---|---|---|
| base | 28.1 / 14.7 / 56.7 | 29.0 / 15.2 / 53.9 | 30.0 / 15.9 / 48.9 | **29.0** / 15.3 / 53.2 |
| xmm | 28.2 / 11.4 / 59.3 | 28.5 / 15.7 / 54.1 | 28.3 / 15.4 / 52.7 | **28.3** / 14.2 / 55.4 |

-0.7 fps, inside PC 3050's 6-13% spread. Saves identical to the backup after all 10 runs; no game process left; the game
folder has the t53f build back.

**Why the micro-benchmark gain does not show.** The kernels are one long dependent FP chain, where the domain crossings sit
on the critical path. In the game the FP code is short chains between guest loads (the 31% class): the out-of-order core
already overlaps the crossings with the load latency, so taking them off the chain moves the samples (to the ops and the
keep tests' bit copies) but not the time, as TICK-041 found for its v1. The crossing count in the hot code fell less than
the static count suggests (the keep tests still need the result's bits in a general register).

**Decision: not adopted.** Both switches stay off; the default lift is byte for byte as before. They stay in the lifter
(tested) for a later PGO-profiled A/B on the main PC (TICK-045 showed a profile can change such a result by 2-4 fps), but
the in-game sampler gives no reason to expect a gain.

## What this leaves (ranked again with the phase 2 result)

1. **The guest loads (31-34% of the guest time) are the critical path**, not the instructions around them. Lifter-side
   levers that act on that path: guest-register values carried in host registers across calls (the call-boundary
   store/reload hops, TICK-040/041's large project) and anything that shortens a load's address dependency. Host-side:
   large pages for the guest heap (dTLB reach; needs the "Lock pages in memory" right, a user decision).
2. Instruction-count cuts off the critical path (findings 2-4, 7: FPX gate, marks, flags, calling convention) each
   0.5-1.5% of samples; by this ticket's evidence they would move samples more than time. Worth trying only as one
   combined lift with its own profile, measured on the job chain's CPU first.
3. The PGO A/B of `A64_FPX_XMM` + `A64_LR_SCALAR` on the main PC once TICK-054 is done: a gen build of each lift, the three
   scenes' profiles, and the screen (castle-move x3). Not expected to clear the spread.
