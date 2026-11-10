# TICK-041 — Faster lifted guest code on the Market job path (lifter-level)  · model: Opus · after TICK-040
**Why**: TICK-039/040: the main thread waits 7-8 ms a frame for the game's job chain; 96% of that is job code, 80-85% of it guest code, no function above 4%, and one or no executor is busy 36-39% of the wait. TICK-040 showed the compile level is not the lever (-O1 -> -O2 + PGO on ~20% of the path, and -O3: no gain). What is left is what the lifted C itself does: all guest register traffic through `Ctx *__restrict A64_NOESC c` (c->x[], c->vreg[], the flag bytes), a poll at every label, the FPX checks around every float op, the runtime calls (obj_lookup, indirect calls, soft-FP helpers).
**Goal**: measure where the hot guest functions on the Market job path spend their time at the x86 level (context loads/stores, spills around calls, polls, flags, soft-FP / runtime calls) and why that is slower than native; prototype lifter-level fixes ranked by expected gain (callee-saved guest registers in host locals across verified ABI-preserving direct calls, inlining small leaves, dead flags across blocks, fewer/cheaper polls, cheaper indirect calls / obj_lookup, inline soft-FP); each risky change behind an env or build switch, results bit-identical (HOS_SPEED_LOG, shots).
**Acceptance**: a measured breakdown of the guest code's time; fps / 1% low / p99 gain in kokiri-idle and castle-move beyond the 2-4 fps spread (interleaved A/B, at least 3 runs a variant), game speed and picture unchanged, no new unit-test failure; or proof and a ranked plan.
**Branch**: `feature/TICK-041-guest-speed`

## Result (2026-11-10): one lifter change, opt-in; no default changed

**Where the guest code's time goes** (skid-corrected timer samples of the job and main threads, charged to x86 instructions, in the functions holding 80% of the guest samples):

| class | share |
|---|---:|
| guest memory loads/stores | 28% |
| integer work | 25-26% |
| FP ops with the FPX checks | 16.5-17% |
| host stack (spills, the lane loops' reload) | 9-10% |
| context registers (c->x, c->vreg, SP) | 8% |
| flags | 1.1-1.4% |
| FP env checks | 1.3% |
| calls / returns | 1.3% |
| polls | 0.5-0.7% |

- Spills around direct guest calls: 2.4 context stores before, 1.2 loads after; 1% of the samples near calls.
- Runtime under the threads: obj_lookup 0.5%, soft-FP wrappers ~0.8%, mutex_lock / obj ~0.3% of all their samples.
- The code is latency-bound: pointer chasing in guest memory, and store-forwarding stalls where a value is written in parts and read whole. Dropping instructions off those chains moves samples, not time.

**The lever found**: NEON FMUL / FMLA / FMLS by element and vector FMLA / FMLS (.4S) were per-lane loops of scalar helpers, their results through the host stack (a store-forwarding stall each op): ~16% of the guest samples. `A64_VFPX_ELEM=1` (lift time, `third_party/a64-kit/lift/a64_lift.py`) makes them SSE / AVX2 ops with the same exactness tests; any lane outside the fast path runs the old scalar helpers in lane order. Bit-identical: 40 M cases in scratch and `tests/test_vfpx_elem.py` (new), whole Ctx, 0 differences.

**A/B** (fps / 1% low / p99 ms):

| | kokiri-idle | castle-move |
|---|---|---|
| base-np (no profile, 3 runs) | 66.3 / 38.2 / 21.9 | 60.5 / 27.7 / 30.1 |
| v1b-np (no profile, 3 runs) | 70.7 / 36.7 / 22.1 | 64.5 / 29.5 / 27.9 |
| base (shipped profile, 6 runs) | 71.8 / 39.1 / 20.7 | 64.7 / 30.2 / 28.6 |
| v1b-p (own profile, 6 runs) | 73.9 / 38.2 / 20.8 | 66.4 / 28.6 / 29.3 |
| v1 (first version, upstream's profile, 3 runs; base 71.0 / 64.4 that session) | 70.9 / 38.3 / 21.0 | 63.6 / 27.7 / 30.2 |

- +4 fps without a profile, but only +2.1 / +1.7 against the shipped build once each has its own profile: inside the spread. Not adopted.
- The switch stays off: the default lift and a64_fpx.h are byte for byte as on main.
- It would need the profile recorded on its lift shipped with it (a release step), as the back-edge polls do.

**Behaviour**: HOS_SPEED_LOG ratio 60.04 (base) / 60.00 (v1b-p); shots alike (`build/tick041/shots`); saves identical after every run.

**Tests** (run_all, llvm-mingw on PATH): 176 pass, 10 fail, 25 skip. The 10 failures are all on the known list (test_nup_verify passed this time); no new failure. `test_vfpx_elem` (new) passes in ~5 s: it uses the llvm-mingw on PATH as HOOKSHOT_MINGW, which test_fpx_sticky does not (that one still fails building a64emit, as on main). It fails when the FMA fast path's FZ or midpoint test is removed (2144 / 908 mismatches).

**Game folder**: the backed-up Sync 10 exe and dll restored (backup in `E:\OOT\HeyListen\build\backup-tick041`); it starts and plays (kokiri-idle 72.6 fps).

**Committed**: the lifter change (opt-in), `tests/test_vfpx_elem.py` + `tests/vfpx_elem_test.c`, `tools/bench/guestmix.py` + `guestmix_post.py`, the docs (engine/docs/perf-audit.md "TICK-041", docs/benchmarks/2026-11-10-tick041-guest-speed.md).

**Next** (ranked):
1. Ship `A64_VFPX_ELEM=1` with a profile recorded on its lift at the next profile refresh.
2. The remaining c->vreg store-forwarding stalls (small, ~0.3%).
3. Callee-saved guest registers across calls (a few % at most).
4. Scalar binary32 FMA via FMA3 (4-5% of the guest samples in the binary64 route).
5. The job graph (TICK-039) stays the larger lever.
