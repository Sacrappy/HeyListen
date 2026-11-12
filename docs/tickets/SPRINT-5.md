# Sprint 5 — where the frame stands after v0.2.0, and what a 120 fps frame needs (from 2026-11-12)

Goal unchanged: 120+ fps in game without frame generation (8.33 ms a frame). Main PC (Ryzen 7 7800X3D, RTX 5070), Market save,
`castle-move`, `--fps-limit 360`, windowed 1280x720, `run_bench.py` 110 s cut at 66 s. Raw runs: `build/s5/` (local).

## Session 1 (2026-11-12): measurements on main 855d44f4

| what | fps (castle-move) | reading |
|---|---|---|
| main (v0.2.0), 5 runs | 77.7, 77.6, 76.9, 78.4, 77.7 | the reference: 12.9 ms a frame |
| every unadopted switch at once (TICK-056 / 061 / 062: 13 switches) | 77.0 | nothing |
| the same + `RX_VK_FENCE_POLL_FAST` + `RX_VK_READBACK_TICKET` | 80.1 (1 run) | inside the spread |
| the same + the block lift (`A64_FPX_BLOCK`, its profile), 3 interleaved rounds | 80.4, 78.7, 79.3 against 76.9, 78.4, 77.7 | +1.8 fps |
| `RX_VK_ASYNC=1` | 56.4 | a readback a frame waits ~6 ms for the whole queue |
| `RX_VK_ASYNC=1` + `RX_VK_NO_READBACK_WAIT=1` (picture not right: a bound) | 75.0, 75.4 | the replay off the game's threads is no faster |
| `RX_VK_REPLAY_PIN=0` | 74.1 | the pin helps |
| `RX_VK_SUBMIT_DELAY_US=300` (6 submits: +1.8 ms of replay) | 70.0 | +1.4 ms a frame: the render chain has ~0.4 ms of slack |
| GPU time (log) | 2.8 ms a frame | the GPU is not the limit at 720p |

**The frame (job trace, `frameline.py` / `chainb.py` / `build/s5/seq.py`), 13.05 ms with the trace:**
- Chain A, the game's: the last BaseProc stage (4 executors, ~2.1 ms, the main thread at its end-of-frame wait) -> the main
  thread **alone ~1.5-1.8 ms, every executor and module worker idle** -> the job group (6.0-6.5 ms) -> the module phase (~3 ms).
- Chain B, the render chain: the held flush (capture made 1.29 ms + memory work, 1.84 ms) -> the Presentation Thread's submits and
  present (~9.4 ms) for ~2,200 draws.
- **New: the main thread's "alone" time is a wait, not draw code.** Order inside a frame (ms after the present's end): the
  main thread holds its two submits at 2.690; the held submits' thread starts its flush at 2.694; the main thread polls two
  older fences at 2.826 (both signalled) and reads GPU results back at 2.852, inside the flush's section of the replay's order,
  and waits for all of it until 4.006 (the capture's ~30 MB copy and the replay). 1.3 ms a frame (mark 32 on the default path,
  mark 36 with the ticket: the ticket moves the wait, it does not remove it), with the job group not started.

## `HOS_NVN_HOLD_LINGER_US` (built, off by default; session 2 found its picture wrong: see below)

The held submits' thread starts its flush at most that long after the last hold, and at once when the holding thread has made
its next readback; the Presentation Thread's event poll does not flush meanwhile (host/nvn_native.c).
- With 300: the main thread's wait goes (1.31 -> 0.01 ms a frame), its work a frame 4.44 -> 2.85 ms, the job group starts
  ~0.4 ms after the hold. Chain A then has ~0.6-1.3 ms of slack (it waits for the present, lr 81207a6c).
- But the Presentation Thread's first submit comes 0.17 ms after the hold and makes the flush itself (capture made 1.6 ms on
  it), and its take then **waits ~1.3 ms a frame for the GPU's late pages of the frame before** ("a late page in a live
  texture's storage", the case `RX_VK_LATE_DEFER` does not defer: 59-89 of ~106 takes). The wait changed place again.
- Screen, linger 300 + the 13 switches + fast poll + ticket against main, 3 interleaved rounds: 81.9, 78.9, 79.6 against
  (77.x), 76.5, 78.9: **+2.6 fps, inside the spread**. Traced frame 13.05 -> 11.70 ms. Shots and speed not checked.
- Decision: off by default; nothing adopted; game folder = main's build. `tests/test_nvn_hold.py` follows the new condition.

## Session 2 (2026-11-12, night): what the frame's loop really is, and the checkpoint

**The linger was wrong.** With `HOS_NVN_HOLD_LINGER_US=300`, `RX_VK_LATE_DEFER_TEX` (below) and the switch stack, castle-move read
84 fps, 88.7 with the block lift, ~91 with the background throttle: the picture was burnt out to white in all of them
(shots: mean luma 249 against 105, 95% of the pixels white; `build/s5/chk1-*`). Why: the main thread's readbacks after its
submits are of the light meter's buffers (0x113c88b000 / 88e000 / 891000 +0x1040, 0x11371e7f00 +0xc0). Read while the GPU is
still inside the frame before, they hold what an earlier submission of that frame left (raw sums: 2.1e7 where the measure is
78; `RX_VK_LOG_READBACK2`). The wait the linger removed was not for the flush but for **the GPU done with everything
submitted**. Waiting for exactly that (`RX_VK_READBACK_STRICT=1`) gives the right picture at 67 fps; answering the polls "not
yet" while the GPU is busy (`HOS_NVN_POLL_HONEST=1`) changes nothing (the module workers read too). The linger stays in the
source as a documented experiment, off.

**So the frame's loop is:** the Presentation Thread's last submit -> the GPU's tail (2.1 ms busy with no gap, `GPU tail` in the
log) -> the main thread's readback -> its hold -> the capture (1.3-1.9 ms: 46 MB, at the memory bus's ~60 GB/s) -> the replay
(7-9 ms) -> the last submit. GPU time is on it, 1:1, though the GPU is only 2.8 ms a frame.

**Measured on the way** (castle-move unless said):
- The capture's pages (`RX_VK_CAPTURE_WHERE`, `RX_VK_CAPTURE_DIFF`): ~11,900 pages a frame (46 MB), ~9,000 of them in one
  256 MB region (0x104e000000-0x105f000000). **51% are byte-identical to their last copy, and only ~1 MB of 8-byte words
  differs a frame**; changed pages have 2.85 changed 256-byte chunks of 16 on average.
- The pre-copy rewritten as one parallel round a frame (`RX_VK_PRECOPY_KICK=2` at the main thread's end-of-frame wait, `=3` at
  the present; off): 7,900 pages copied ahead in 1.0 ms, 31 new and 44 copied again a submit; the main thread's wait
  1.3 -> 0.25 ms, but the frame does not move (the copy takes the bus from the main thread's own code, and the GPU's tail is
  waited for anyway).
- The game's chain by how many executors run (`build/s5/par.py`): all six 2.7 ms, one alone 1.6, one module worker alone 1.4,
  four (the last stage) 0.95, the main thread 3.0. By function it is flat (`build/s5/lone.py`: nothing over 11%).
- Thread placement: `HOS_PIN_BY_CORE=1` 68-72 fps; `RX_VK_PARALLEL` 2 / 3 / 7 as 5; `HOS_OBJ_PACK` as without.
- OptimizeChangeBanc ran 35-44% of a logical processor at the host's lowest priority; throttled (`HOS_BG_THROTTLE`, 300 us of
  running then 3 ms asleep, on) it is 5%.

**Checkpoint, on by default** (each `=0` off; `host/hos_main.c` sets the switches before main):
- the switches of TICK-056 / 061 / 062 and TICK-058 / 059's fast poll with the ticket (`RX_VK_MSYNC_FAST`, `SEG_EARLY`,
  `CAPTURE_CLAIM`, `COLLECT_FAST`, `SHADOW_FAST`, `STAMP_SPLIT`, `DRY_LEAN`, `WORK_LOCAL`, `SEG_SETUP_PAR`, `OCC_PROXY_SKIP`,
  `FENCE_POLL_FAST`, `READBACK_TICKET`, `HOS_CV_SIGNAL_FAST`, `HOS_THREAD_OF_CACHE`, `HOS_SHORT_SLEEP_PAUSE`);
- the block lift (`A64_FPX_BLOCK`, a lift switch) with its profile (`profile/hookshot_pieces.prof` is TICK-063's `blk.prof`);
- `RX_VK_LATE_DEFER_TEX`: a late page in a live texture's storage no longer makes the take wait (it was one page,
  0x11371e7000: two 1x1 textures of 128 bytes beside the meter's words; its shadow is made from the copy at the submit);
- `HOS_BG_THROTTLE`.
One run a scene with shots (mean luma alike, speed 59.9-60.2): kokiri-idle 80.9 -> 85.5, castle-move 78.3 -> 84.0, hyrule-walk
79.7 -> 84.5 fps (`build/s5/chk2-*`). Castle-move, 3 interleaved rounds, no environment: 85.5, 82.0, 82.2 against (79.x), 79.0, 79.4:
**+4 fps, every run above every reference run.** Tests: run_all 256 checks, 33 skipped, one failure (nx2/test_nup_paths.py, as on main).

## Session 2, after the checkpoint (2026-11-12 morning): the GPU's tail, and why the readback cannot simply be let go

- **GPU time is on the loop.** `RX_VK_TEST_BARRIER_DROP=1` (not shippable: every full barrier dropped) 87.5 against 83.0 fps, GPU
  time 4.2 -> 3.2 ms a frame, tail 2.0 -> 1.3 ms. The real barrier switches do not reach it: `RX_VK_BARRIER_DEDUPE_PAR` 84.7,
  `RX_VK_BARRIER_TRUST` 83.7, `RX_VK_GAME_BARRIER_NARROW` 81.8, the three 83.3, `RX_VK_PASS_BARRIER=game` 83.0 (ref 83.0-83.4).
  `RX_VK_MSYNC_DIFF=1` 72.8 (the hashing costs the CPU more than the smaller uploads give). Pass timing: the 3D is drawn at
  2560x1440 whatever the window; passes 56%, the 136 compute dispatches 26%, barriers ~10%, the mirror syncs' uploads 0.6 ms.
- **The parallel pre-copy** (`RX_VK_PRECOPY_KICK=2` + `HOS_PRECOPY_KICK=1`): 83.6-83.9 against 82.4 over 3 rounds, inside the
  spread: with the capture short the readback waits for the GPU's tail instead.
- **The meter, watched** (a thread logging each change of the buffers' first words with the GPU's completed value): guest
  memory holds 0 for all of the frame but ~0.3 ms: the main thread's first held submit writes the measure (77.8, 128.4), the
  write-back lands, the CPU reads it and writes 0. The 0 must reach the GPU **after** that submit's conversion and before the
  frame's accumulation: that is the order the wait keeps, and what the linger broke (the 0 went to the GPU first).
- **Reading a frame late** (`docs/research/own-readback-experiment.patch`, not merged): the readback of the thread whose held
  submits are being flushed waits only for a capture scan after its hold, reads what the last frame's conversion left, and the
  late copies of those ranges keep the CPU's bytes. The order is then right frame by frame, but the game's own polls now say
  "not done" in some frames and it skips the read and the reset: the next measure is two frames' worth (78, 163, 254) and the
  exposure moves (shots 96-140 against 64-107). 80.5-85 fps, no clear gain. Not merged; three source-reading tests would need
  to follow (test_late_defer, test_invalidate_vcore, test_readback_ticket).

## Where this leaves the 120 fps goal (2026-11-12)

Target since 2026-11-12 07:00: 120 fps average in Kokiri Forest, Hyrule Field and the castle town, no drop under 110 while
moving. Measured now (the checkpoint, main PC): 84-85.5 fps average in the three scenes, p50 10.9 ms, p90 13.1, p99 15.7, 1% low
~50 fps. The target is every frame under 9.1 ms: the whole distribution has to move by ~40%, not its tail.

Three chains of ~11-12 ms each hold the frame, and each was seen to bind once another was shortened:
1. **The sync loop**: the GPU's tail (2 ms) -> the main thread's meter readback -> its hold -> the capture (1.3-1.9 ms) -> the
   replay (7-9 ms). Bound by GPU time, by the 46 MB the capture copies, and by the replay's ~4 us a draw.
2. **The game's chain** (~11 ms of lifted code, flat by function): all six executors 2.7 ms, one executor alone 1.6, one module
   worker alone 1.4, the last stage on four executors 0.95, the main thread 3.0.
3. **The processors**: ~6.7 threads' worth of work on 8 cores; executors, recorders and helpers share cores at the peaks.

What would move it, largest first (none is a switch; each is a piece of engine work with a picture risk):
- **The capture by written bytes, not by page** (finer write marks in the lifted stores, slots that hold only what was written):
  46 MB -> ~8-12 MB a frame; -1 ms on the sync loop, less bus traffic for the game's own code, smaller GPU uploads. The six
  readers of the capture table (mirror syncs, reads as submitted, late fixes, texture shadows) must take partial slots.
- **A wider job system** (twelve executors: a patch of the game's per-core tables, f_80b5e9e0 and its users): up to -1.6 ms
  of the game's chain. High risk.
- **GPU time**: barriers only where a hazard is (bound: -1 ms), the 136 dispatches' global-memory emulation, a lower render
  resolution with an upscaler (DLSS / FSR are in the engine: a user's choice, not frame generation).
- **The lifted code itself**: guest registers kept across calls; the block lift's stores and conversions with a profile of
  their own.
- **Frames pipelined like a console's** (the CPU a frame ahead of the GPU): needs the meter's read-and-reset to be exact a
  frame late, which the experiment above did not reach.
Honest estimate: the first three together might reach ~100 fps; 120 with 110 minimum is not within reach of this machine
without all of them and a faster replay.

## What a 120 fps frame needs (the three walls, in the order they bind now)

1. **Render chain (binds now, ~12 ms):** capture copy 1.3-1.6 ms (30 MB of dirty pages a frame), the late-page GPU wait ~1.3 ms
   once the flush is early, the Presentation Thread's replay 6.5-7 ms with the switches (9.4 without). Needed: under ~7.5 ms in
   all. Levers: the late pages in live textures' storage deferred like the others; a smaller capture (C4: ~47% of the copied
   pages are byte-identical to the mirror); the dry passes (2 x ~0.9 ms serial); the mark scan (`collect_in`, ~1 ms a frame).
2. **The game's chain (~11.5 ms once (1) moves):** job group 6-6.5 ms (serial tails of Link's and the model jobs), module phase
   ~3 ms, last stage 2.1 ms on 4 of 6 executors. Needed: -3 ms. Levers: the block lift (-8 to -13% of executor work, exact),
   the last stage on six executors, more executors (a game patch).
3. **The replay off the game's frame** (`RX_VK_ASYNC`) only pays after (1): today it is throughput-bound at the same ~12 ms.

Honest reading: no single change found gives more than ~3%; 120 fps needs (1) and (2) each cut by a third.
