# TICK-040 — Hot set and profile for the Market job chain (-O2 + PGO where the frame is spent)  · model: Opus · after Sync 10
**Why**: TICK-039: the job-group wait is 96% guest job code on the chain (80-85% guest code, no function > 4%), wake/scheduling < 4%. TICK-022 found the lifter's per-instruction overheads already small. What is left that moves all guest code at once is how it is compiled: the lift compiles most chunks at -O1 and only `code_hot*.c` (A64_HOT_FUNCS from upstream's hot_funcs.txt) at -O2 with upstream's PGO profile (hookshot_pieces.prof, RX_PGO). Upstream's hot set and profile were made on their scenes; ours (Castle Town Market, standing and walking) may sit largely in -O1 code.
**Goal**: measure how much of the Market job path (HOS_JOBTRACE + HOS_SAMPLE_NS samples, TICK-039 tools) runs in cold (-O1) vs hot (-O2/PGO) code; extend the hot set with the Market's (and Hyrule street's) hot functions and their callees; regenerate the PGO profile from our scenes (RX_PGO=gen build, merged with upstream's), and try RX_HOT_OPT=O3 / per-piece flags. Keep build time and builder RAM reasonable (document the cost); results bit-identical (HOS_SPEED_LOG, shots).
**Acceptance**: share of the path in hot code before/after; fps / 1% low gain in kokiri-idle and castle-move beyond the 2-4 fps spread (interleaved A/B), or proof and next plan. If it wins, the builder ships the new hot list/profile (no game code in them: function addresses and counters only — check the release guard).
**Branch**: `feature/TICK-040-hot-pgo`

## Result (2026-11-10): no win beyond the spread, nothing shipped

**Share of the path in hot code.** Upstream's hot set already covered most of the Market job path (critical-path samples, innermost guest function by object):

| | before | with the extended set (h2) |
|---|---:|---:|
| kokiri-idle | 77.9% | 95.8% |
| castle-move | 71.5% | 93.0% |

**Path work** (ms a frame) moved little:

| | before | h2 |
|---|---:|---:|
| kokiri-idle | 7.09 | 6.63 |
| castle-move | 8.08 | 8.09 |

So compiling this code at -O2 with a profile instead of -O1 is worth a few percent at most.

**Variants, interleaved A/B** (fps / 1% low / p99 ms; base 6 runs, h2 6, others 3):
- base: 70.8 / 38.3 / 21.3 standing, 65.7 / 30.4 / 27.3 walking.
- h2: upstream's 849 functions + 917 sampled + 1183 small callees, our scenes' profile merged with upstream's. 71.8 / 37.2 / 21.7 standing; 64.9 / 28.7 / 30.1 walking, with a walking p99 above base's in every run.
- h2 at -O3: 70.9 / 63.8 fps.
- p2: h2 with upstream's back-edge polls and a profile recorded on them. 72.4 / 36.1 / 21.7 standing, 67.2 / 30.5 / 27.5 walking; +1.2 / +1.3 fps against base in the same session.

None is beyond the 2-4 fps spread. No default changed. The builder keeps upstream's hot list and profile (the release guard would allow ours: only addresses and counters, under `tools/work/`).

**Upstream's polls.** With a profile recorded on their code, the back-edge polls are even, or slightly ahead (+1.2 / +1.3 fps). The -3 fps of Sync 10 was the stale profile. Not adopted: inside the spread, and they need a shipped profile made on the same lift. With them:
- `native_replace`'s strcmp gate is not applied (hash change; it is off by default);
- the text-speed block still applies.

**Behaviour.**
- HOS_SPEED_LOG ratio: 59.98 (base), 60.02 (h2), 60.01 (p2).
- Shots, walking, in the worktree: `build/tick040/shots/{base,h2,p2}-move`, alike.
- Saves identical after every run.

**Build cost** (8 jobs, this PC):
- h2's hot pieces at -O2 + profile: 98-212 s a piece, 7.3 GB summed over all compiles.
- Upstream's heaviest piece alone: 133 s, 1.4 GB.
- Without a size cap, the giant new functions (h1) made one piece take 337 s and 1.9 GB.
- A gen build: ~735 s and 21.9 GB; it plays at ~10 fps.

**Committed:**
- `tools/bench/jobtrace.py`: hot/cold split of the path's samples.
- `scripts/dev-build.ps1 -Lift`: deletes the earlier lift's `code_*.c` first. Stale chunks gave duplicate symbols after a lift with fewer chunks.
- The docs: engine/docs/perf-audit.md "TICK-040", docs/benchmarks/2026-11-10-tick040-hot-pgo.md.

**Next**: the lever is the lifted code's context traffic (guest registers kept in host registers across calls), or the job graph (TICK-039). The compile level is not the lever.
