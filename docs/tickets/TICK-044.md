# TICK-044 — Cut the serial actor jobs: FP slow paths first, then job start order  · model: Opus · after TICK-043, Sync 11
**Why**: TICK-042/043: the Market runs at ~75 fps standing, 68-69 walking (~13.5 ms a frame); the game's job chain is ~6.5 ms of it, and the engine's submit only ~2 ms (taking it off the main thread gave nothing, TICK-043). On the chain, single actor jobs run alone at the end of each stage: Link's calc (800b3ce0, 1.5-1.7 ms), 80166100 (1.4-1.7 ms, 20-26% of its samples in the runtime's FP slow paths) and 802ca820 (1.1-1.3 ms). All three run the same model subsystem (f_80168320 > f_8016b620 > f_81b47080).
**Goal**: find which FP helpers these jobs hit and why they leave the fast path (denormals, NaN, FPCR modes, the FPX exactness checks, the binary32 FMA's binary64 route); make those helpers faster, bit-identical to the current ones (a per-lane fallback in the cold `_lanes` helpers, FMA3 for an exact scalar binary32 FMA, cheaper slow-path detection), proven by a unit test and a large random / edge-case comparison as TICK-041's `test_vfpx_elem.py`. Optional, if time is left: a scheduler patch starting the long jobs first (f_8004ab40's lock-free lists; TICK-042 estimated 0.2-0.45 ms a frame), behind an env switch, off by default unless correct and winning. TICK-041's PGO caveat: prefer runtime / host-side changes that leave the lifted callers (and their profile) unchanged.
**Acceptance**: the helpers hit and why; HOS_SPEED_LOG ratio ~60 and shots alike the base build; no new unit-test failure, a test for the new logic; fps / 1% low / p99 in kokiri-idle and castle-move (`--fps-limit 360`, frame generation off, 66 s cut) beyond the 2-4 fps spread (interleaved A/B, at least 3 runs a variant, 6 if close); or proof and a ranked plan.
**Branch**: `feature/TICK-044-fp-slowpath`

## Result (2026-11-10): the slow paths found and removed bit-identically; no fps gain beyond the spread; no default changed

**Which helpers, and why** (probe build counting every exact FP body and estimate by cause): walking ~5.4 M out-of-line FP calls a second (~81,000 a frame), standing ~3.3 M.
- 59% walking: vector FMUL / FSUB / FADD .4S with a **quiet NaN** lane (Vec3 math in 4-lane registers): the cold `a64_vfpx_*_lanes` re-ran all four lanes through the scalar wrappers, the NaN lane through the soft-float body.
- 33%: **FRSQRTE / FRECPE**, always out of line (`recomp_fp_estimate`).
- 6%: **FDIV S by zero** with DZC already raised.
- Not the cause: denormals (~1%), FPCR modes (none), the gate (FPSR 0x9f in 89-95% of the calls), binary32 midpoints (none reached the body).
- Where: f_81c86720, f_81c7f880, f_81b8c000, f_81c7d2e0, ...: the model subsystem under 80166100's job.

**Changed** (lift-time switches, off; the lifted code's object key unchanged):
- `A64_FPX_SLOWPATH=1`: per-lane fallback with quiet-NaN lanes in the cold `_lanes` helpers; quiet-NaN and FDIV-infinity rescues in the scalar wrappers; a table fast path in `recomp_fp_estimate`. Exact bodies 3.6 M -> 0.09 M a second walking.
- `A64_FPX_FMA3=1`: the scalar binary32 FMA fast path by FMA3 (stales recomp_fpx_fma32's PGO record).

**A/B** (6 interleaved runs a variant; fps / 1% low / p99):

| | base | SLOWPATH | SLOWPATH + FMA3 |
|---|---|---|---|
| kokiri-idle | 76.1 / 48.6 / 17.6 | 74.7 / 49.8 / 18.0 | 75.0 / 46.9 / 18.4 |
| castle-move | 69.1 / 35.5 / 23.0 | 70.0 / 36.3 / 22.7 | 71.1 / 37.1 / 22.2 |

Inside the spread (walking +0.9 / +2.0, standing -1.4 / -1.1). Not adopted.

**Correctness**: `tests/test_fpx_slowpath.py` (new; whole Ctx, 22 guest words, plain and hot, x86-64-v3 and v2; every binary32 estimate class): 0 mismatches in 3 M cases a march, 40 M by hand; it fails on each of six hand mutations. Shots alike, HOS_SPEED_LOG 60.01-60.02. run_all 216 pass, 10 fail (the known list), 35 skip.

**Not done**: the job start order (optional part 2): not prototyped.

**Game folder**: Sync 11 exe and dll restored from `backup-tick044` (md5 as before); smoke kokiri-idle 75.0 fps, saves identical. Work folder re-lifted on the default (gen files byte-identical, nothing recompiled).

**Next** (ranked): 1. the job start order (TICK-042, 0.2-0.45 ms); 2. ship both switches (and `A64_VFPX_ELEM`) with a profile recorded on their lift at the next refresh; 3. inline FRSQRTE / FRECPE (1.8 M calls a second left; lifted code); 4. the quiet-NaN test in the inline vector path (profile records); 5. Link's calc is not FP-bound.

Docs: engine/docs/perf-audit.md "TICK-044", docs/benchmarks/2026-11-10-tick044-fp-slowpath.md.
