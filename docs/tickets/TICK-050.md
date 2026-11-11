# TICK-050 — A command buffer's replay segments in one round, the Presentation Thread recording one  · model: Opus · after TICK-049
**Why**: TICK-049 found that "GPU frame prep" (`PROF_SUBMIT`, the wall time of `run_submit`) is ~7.1 ms a frame standing and
~8.9 ms walking in the Market, and that ~4.3 ms of it is the Presentation Thread waiting for the 4 replay workers. Each large
command buffer (~3 a frame) gets ~4.7 segments on 4 workers, so a fifth segment is recorded alone in a second round. At 120 fps
(8.3 ms) this wall is the next ceiling.
**Goal**: design 1 of `docs/research/replay-cost.md`, its 8-step plan:
1. measure as it is, with `RX_VK_PROF_FINE=1 RX_VK_FRAME_TRACE=1` (the fine split, for design 2);
2. log the slowest and fastest segment, the rounds and the Presentation Thread's wait (`vk: parallel replay, a command buffer`);
3. a pure planning function `gpu/vk/segment_plan.h` (`segment_count`), with a unit test;
4. `RX_VK_PARALLEL_ROUNDS=1`: no more segments than recorders (one round);
5. `RX_VK_PARALLEL_SELF=1` (with 4): the Presentation Thread records segment 0 itself, the workers 1..n-1;
6. A/B with one binary and env switches (base, ROUNDS, ROUNDS+SELF);
7. the picture (shots standing and walking, `HOS_SPEED_LOG`, `run_all`);
8. the default only if frame prep falls by >= 1.5 ms standing, no new tail, no fps loss beyond the spread, the same picture.
**Acceptance**: GPU frame prep standing <= 5.5 ms (from ~7.1) with the same picture; fps not lower.
**A/B policy** (user, 2026-11-11): screen castle-move only, 3 interleaved runs a variant; confirm only what is to be adopted
(kokiri-idle, castle-move, hyrule-walk, 6 interleaved runs, order reversed in rounds 4-6). Warmup checked on TICK-047's
per-frame data before choosing the cut.
**Branch**: `feature/TICK-050-replay-rounds` (from main 4e6430a). Local only.

## Result (2026-11-11): instrument kept, switches off, not adopted

**Measured (base, kokiri-idle, fine profile).** GPU frame prep 7.83 ms. Descriptors 4.23 ms a frame, of it **textures 2.08**,
user uniform buffers 0.70, storage buffers 0.44. Commands 4.32 ms: bind descriptors 1.02 (push call 0.66), vertex+index binds
0.91, bind pipeline 0.70. Program picks with other textures: 30.8%. A command buffer: dry pass 0.33 ms, segments 1.51 ms
(4.65), slowest 1.25, the Presentation Thread waits 1.42 ms.

**The finding.** The wait is not the second round. The new lines show:
- the slowest segment stays ~1.25 ms whatever the segment count (4, 5 or 6 recorders, split by count or by time);
- the first draws of a command buffer cost ~3 times the last ones;
- a draw costs more the more threads record at once: 5 recorders sum to ~5 ms a command buffer, against ~3 ms serial.

**Built** (all off by default, `gpu/vk/segment_plan.h`, `tests/test_segment_plan.py`):

| switch | what it does |
|---|---|
| `RX_VK_PARALLEL_ROUNDS=1` | caps the segments at the recorders |
| `RX_VK_PARALLEL_SELF=1` | the Presentation Thread records segment 0 |
| `RX_VK_PARALLEL_SELF=2` | it also takes queued segments before it waits |
| `RX_VK_PARALLEL_BALANCE=1` | boundaries by measured time per command-buffer order in the frame, at the nearest point |
| `RX_VK_PARALLEL_POINT=<n>` | a dry-pass point every n draws (default 64) |

**Screen** (castle-move, 3 interleaved runs each). Frame prep:

| variant | frame prep |
|---|---|
| base | 7.76 / 8.43 ms (8.05 ms without one stutter run) |
| ROUNDS | 8.43 ms |
| ROUNDS+SELF | 7.62 / 7.52 ms |
| SELF=2 | 7.66 ms |
| ROUNDS+SELF+BALANCE | 7.35 ms |
| + 5 workers | 7.42 ms |

fps were inside the spread for every variant.

**Decision.** Nothing was adopted: frame prep falls by ~0.4 ms, against the 1.5 ms required, and this is within the
0.5-0.9 ms run-to-run spread. The acceptance (≤ 5.5 ms) is not reached with design 1. No confirm A/B was run. The game
folder has the TICK-045 build back.

**Correctness** (switches on):
- the shots look alike;
- the speed ratio is 60.02;
- the saves are identical;
- `run_all`: 226 pass; only the 4 nup tests fail; 33 skip.

**Next:**
1. Run the all-thread sampler on the workers, to see why a parallel draw costs ~5 us (driver locks, `pools_mu`, ParLock).
2. Design 2, starting with the texture share of the descriptor build.
3. Overlap the dry pass with segment 0.

Details are in `engine/docs/perf-audit.md` ("TICK-050") and `docs/benchmarks/2026-11-11-tick050-replay-rounds.md`.
