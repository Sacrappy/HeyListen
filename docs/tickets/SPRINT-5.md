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

## `HOS_NVN_HOLD_LINGER_US` (built, off by default)

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
