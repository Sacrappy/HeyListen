# Readbacks, the replay's start and the frame's loop (stream R2, 2026-11-14, main PC)

Question (stream C1): 81% of the 30.7 ms frame on the PC 3050 is a thread waiting inside the renderer, the present on the replay and the
game's CPU reads of GPU results (`invalidate`) on the replay and the GPU's write-back. Which reads wait, why, and can they be
answered without waiting with the picture right; why does the replay start late in the frame. Everything below is measured on the main
PC (Ryzen 7 7800X3D, RTX 5070, castle-move, 1280x720, main 349e5685 plus this stream's logging), the machine otherwise free for the
speed rows, the frame rate a run of 110 s cut at 66 s as always. The PC 3050 was not used.

## Tools built (logging only, default off)

- `RX_VK_RB_PROF=1` (`vk_replay.cpp`, `RbScope`): every `invalidate` timed by phase (the ticket's wait for the replay section in
  progress, the catch-up, the wait for the write-back's recording split into the flush's late-fix wait / the queue's lock / the
  sending / the recordings' semaphore, the GPU timeline, the strict wait, the texture catch-up), by thread, by range and by call site
  (the exe's frames above the renderer, as offsets from the exe's start: on Windows its RVAs, on Linux, a PIE exe, the
  addresses in hookshot.map), a line every 300 presents.
- `HOS_RB_LR_LOG=1` (`host/nvn_native.c`): the CPU's reads of GPU results by guest return address, size and thread.
- `RX_VK_TL=<present>` or `RX_VK_TL_S=<seconds after the first present>`: a timeline of 3 frames of the renderer's threads (submits,
  replays and their stops at event waits, the CPU's event signals, presents and their drains, fence waits, readbacks with the writer's
  value, recordings sent and flushed, the late fixes), in the log as `vk: tl ...` lines (plain text).

## What a frame reads (castle-move, 89-97 fps)

33-34 `invalidate` calls a frame, 19.4 of them on ranges with write-backs, 6-10 of those pending (the writer not done). The time
they wait, summed over the threads that read (several wait at once, so this is not the chain's loss):

| | ms a frame |
|---|---:|
| all readbacks | 6.7-8.0 |
| the ticket (the replay section in progress when the fence was polled) | 1.2-1.4 |
| the write-back's recording | 5.3-6.7 |
| of it: the flush's wait for the pending late fix (`late_fix_wait_unlocked`, the GPU timeline) | 5.1-6.4 |
| of it: the queue's lock, the sending, the recordings' semaphore | 0.1 + 0.0 + 0.1 |
| the GPU timeline wait (`wait_value`), the strict wait, textures | 0.0 |

By thread: ModuleSystemWorker1 2.25, 2 1.82, 3 0.85, 4 0.47, 5 0.33, the main thread 2.28. By range, one call a frame each:
0x113c88b000 +0x1040 (the light meter's buffer, 2.0 ms), a 0x40 range (1.97), the two 0x2000 ranges that alternate frame by frame
(0.97 and 1.0); the rest (0x3980 x2, 0x8b040, 0x9b040, 0x44b040, 0x1b580) wait 0.01-0.05 ms. By caller (guest return address):
the meter's 0x1040 x3 and 0xc0 from the ring helper `f_80357060` (7 callers), the 0x2000 from `f_8007c120` and `f_8034f440`, the
0x40 from `f_80354060` (many addresses in a row, one wait for all), the big ranges from `f_80362ff0`, 0x1b580 from `f_80364080`,
0x3980 from `f_80365f00` and `f_800998a0`. Only 0.2-0.6 of the 33 calls a frame have the newest submission as their writer: the
game reads what the **previous frame's** heavy submission wrote (value of the queue's 4th submit), after polling that submission's
fence (answered "done" early from the recording, see `direct-emission.md` section 12). So Longshot's `RX_VK_READBACK_LATE=newest`
does not apply here (0.2-0.6 calls a frame match it); its by-size rule would name the 0x2000 and 0x40 ranges.

What the wait is for: the GPU's completion of the previous frame's heavy submission. The readbacks call `wait_recorded`, which flushes
the held recordings first (`queue_flush_for`), and the flush's first step waits for the pending late fix, the timeline value of a
capture's late page, which is that same submission's write-back. The recordings' semaphore itself is signalled before the wait begins
or within 0.1 ms. (The first piece of the submission is what `value_rec` keeps, so on this driver, where `RX_VK_READBACK_STRICT` is
off, that wait is the only one that makes a readback see the whole submission's write-back.)

## The frame's loop, from the timeline (`RX_VK_TL_S`)

One frame, in order:
1. The game's CPU phase (main thread, module workers, executors) until the main thread signals the CPU event that the heavy queue's
   first command buffer waits for (0x1006e38000 / 0x1006e38004, alternating by frame).
2. Until then the renderer has only the Presentation Thread's four small submits (each stops at a GPU-event wait, 0.05-0.1 ms of
   replay each) and the main thread's two held submits (capture 0.6 ms, replay 0.1-0.2 ms each). The heavy submission stops at the CPU
   event: **this is why the replay is idle for the first ~10 ms of the frame on the 3050: it has nothing it may run**. The game
   submits the work before its data is final and holds the GPU with an event the CPU sets when it is.
3. The signal; the order thread (`sync_watch`, `Sleep(1)`) or the Presentation Thread's present finds it 0.5-1 ms later (the order
   thread polls; `RX_VK_WATCH_KICK=1` wakes it at the signal), and the heavy submission replays: 4-9 ms here, in pieces that stop at
   GPU-event waits; the present, called at about the same time, waits for the section's lock.
4. The present returns when the drain is done; the next frame's CPU phase begins.
5. In the next CPU phase, about 3-4 ms in, the main thread and the module workers read the previous frame's results (above) and wait
   for the GPU to be done with that heavy submission, 0 to 5 ms (1.4 ms in the frame traced), then go on to signal the event.

The loop is therefore: replay end of frame N-1 -> GPU done with it -> readbacks of frame N -> event signal of frame N -> replay of frame N.
The GPU's own work is 2.6-2.9 ms a frame, with 9 ms of gaps between recordings (the log): the wait looks like the replay handing
the GPU its last recordings late rather than the GPU's speed (an inference: see "Not verified").

## What was tried, speed and picture

Speed: castle-move, the whole machine held (`slot.sh all`), the same binary with only environment switches differing, arms
interleaved (the order reversed each round), 110 s a run cut at 66 s, fps of the run. Picture: `RX_VK_FRAMELOG` for 25 s of each run
(2,400-2,700 frames at 320x180), `scripts/framelog.py` for its flash events, and the mean luma of every frame (an exposure that
pulses or is wrong moves it; `RX_VK_EXPOSURE_LOG=2` was logged too, but the word it names stays 1.0 in this scene, so it says nothing
here). Shots every 600 presents were kept in `build/s5/r2-fl-*-shots/` (local).

| arm | fps, each run | mean | against the baseline | picture (luma mean, range, pulses, framelog flash events) |
|---|---|---:|---:|---|
| baseline (all switches off) | 97.6, 97.7, 97.0, 97.8, 97.2, 98.1 | 97.6 | | 40.6, 26.8-53.5, 0, 4 one-block events |
| `RX_VK_WATCH_KICK=1` | 104.0, 104.1, 105.0, 104.0, 104.1 | 104.2 | **+6.8%** | 41.0, 27.9-54.6, 0, 0 events: right |
| `RX_VK_READBACK_LATE=2000,40` | 98.3, 98.0, 98.2 | 98.2 | +0.6% (inside the spread) | 40.6, 28.7-53.0, 0, 0 events: right |
| `WATCH_KICK=1` + `READBACK_LATE=2000,40` | 105.3, 105.1, 104.4 | 104.9 | +7.5%, +0.7 over the kick alone | not run (each alone is right) |
| `RX_VK_NO_READBACK_WAIT=1` (the bound) | 115.5, 112.9 | 114.2 | +17% | 42.2, 27.8-67.7, **24 pulses**, frame-to-frame step up to 11 (0.5 in the baseline): wrong |
| `RX_VK_POOL_MIRROR=1` (step 3) | 88.8, 89.0 (against 96.6, 97.6 in the same screen) | 88.9 | -8.5% | not run |
| `WATCH_KICK=1` + `POOL_MIRROR=1` | 93.5, 94.6 | 94.0 | -3.2% (the kick still adds +5.7% on the mirrors) | not run |
| (built, taken out) `RX_VK_WAIT_REC_SENT=1`: no flush before the wait for a recording already sent | 109.6 (one run) | | +12% | **177.7 mean, 245.9 max: burnt out to white**: wrong |

Of the 12 runs of the first screen, two did not leave the title screen (0 frames measured, the game's present count stops at 39.4 s
after the first present, 360 fps of a 50-draw frame): one of the `NO_READBACK_WAIT` runs and one of the `WATCH_KICK` runs. None of the
9 runs of the second screen, none of the baseline or `READBACK_LATE` runs (9 and 3). The scripted input is the first suspect (the same
instant in both); the renderer's change is not excluded: not established. Said plainly: too few runs to rule out that `WATCH_KICK`
makes the stall more likely.

### What each result says

- **The readbacks wait for the GPU's completion of the previous frame's heavy submission, through the flush's late-fix wait.** 5.1-6.4
  of the 5.3-6.7 ms a frame that the recording's wait costs is `late_fix_wait_unlocked` in `queue_flush_for`, called by `wait_recorded`
  before it waits on the recordings' semaphore. The late page is always the same one, 0x100d6c8000 (pool 0x100c000000 +0x16c8000, not a
  readback page): the page of the GPU event words, which the heavy submission's last event signal writes back (log line "late page
  deferred: ... write-back 2415, submission 2417": the write-back of the previous frame's heavy submission against the capture of the
  next frame's held submit; 598 fixes in 600 presents). It is that wait that holds the frame's other recordings (11.5 command buffers a
  frame held until the fix thread sends them), and it is that wait the readbacks join.
- **Skipping that wait (`WAIT_REC_SENT`) is fast and wrong.** The recordings' semaphore a readback then waits on is the one of the FIRST
  recording the writing submission went into (`value_rec` keeps the first), signalled long before the submission's write-back of the
  meter has landed; on this driver `RX_VK_READBACK_STRICT` is off, so nothing else waits for the whole submission. The flush's late-fix
  wait was the only thing that did, by being the same GPU completion. The meter then read stale sums: the exposure went to white (mean
  luma 177). That wait must not be removed without replacing it by one on the timeline value of the range's write-back
  (`RX_VK_READBACK_STRICT=1` does that, and its own wait flushes through the same late fix, so it costs the same).
- **The answer-late rules do not gain here.** `newest` names 0.2-0.6 of the 33 calls a frame. The by-size rule for the two rings that
  are not the meter (0x2000, 0x40: 3.9 ms of the 8 summed) gives +0.6%: the meter's ranges (0x1040 x3, 0xc0), read by the same threads
  at the same moment, still wait for the same GPU completion, and the threads wait together, so removing some of the waits shortens
  nothing. Only removing all (`NO_READBACK_WAIT`, +17%) moves the frame, and it breaks the meter.
- **Reading the previous frame's value is not what the game is built for.** The game reads, in frame N, what frame N-1's heavy
  submission wrote, after polling that submission's fence; on the console the poll can say "not done" and the game then skips the read
  (its own protocol). The meter is read and reset (0 written), and the GPU adds the next frame's sums into the reset value: a skipped or
  a stale read doubles the next measure (78, 163, 254 in sprint 5's log) and the exposure moves. That is also what the 24 pulses of
  `NO_READBACK_WAIT` are. A rule for the meter's ranges has to be exact.
- **The replay's late start is the game's own wait, not the renderer's.** The heavy queue's first command buffer waits for a CPU event
  (0x1006e38000 / 0x1006e38004), which the main thread signals at the end of its CPU phase. Until then the replay has the Presentation
  Thread's four small submits (0.05-0.1 ms each, stopped at GPU-event waits) and the main thread's two held submits, and nothing else it
  may run: the idle first ~10 ms of the 3050's frame (a 10-14 ms CPU phase there; 5-8 ms on this PC). After the signal the order thread
  (`sync_watch`, `Sleep(1)` with a 1 ms timer) finds it 0.5-1 ms later (0.56, 0.95, 0.64 ms in three traced frames);
  **`RX_VK_WATCH_KICK=1` wakes it at the signal and is worth +6.8% here**, picture right, because that 0.5-1 ms is on the frame's loop
  (signal, replay, present, next frame's CPU phase, signal). The replay stays serial: the replay of frame N cannot overlap the CPU phase
  of frame N+1, because frame N+1's readbacks wait for frame N's heavy GPU work, and its event signal, which the next replay waits for,
  comes after them.

Step 3 (the replay's own work, 4.6 ms on the 3050: page hashing, mirror sync, the GPU-write diff): `RX_VK_POOL_MIRROR=1` makes the
replay do more, not less, on this PC (a staging copy per pool fill and write-back: -8.5%, 2 runs against 2); it saves memory (its
purpose), not replay time, and none of the three costs went away. The per-phase cost with it on was not measured.

## Switches built (default off, except the kick: see the second session)

| switch | what it does | state |
|---|---|---|
| `RX_VK_WATCH_KICK` (**on by default since the second session**; `=0` off) | the CPU's event signal sets an event the order thread waits on (1 ms timeout), so the stopped queue's replay starts at the signal | +7.1% castle-move (10 against 10), +7.7% kokiri-idle (3 against 3), picture right; 23 runs from the title, no stall |
| `RX_VK_READBACK_LATE=<rules>` | Longshot's #238 (`newest`, a hex size), plus `-<hex size>` to never; a ranged readback is read without its wait | no gain here; picture right for `2000,40`; the meter must not be named |
| `RX_VK_RB_PROF=1`, `RX_VK_TL=<present>` / `RX_VK_TL_S=<s>` (now with the GPU's timestamps), `HOS_RB_LR_LOG=1` | the measurements above | logging only |

## Not verified

- One scene (castle-move), one PC. Kokiri-idle and hyrule-walk were not run; the 3050 was not used (its numbers, 81% in waits, are C1's,
  not reproduced here: on this PC the readbacks wait 1.4-2.0 ms a frame on the chain, not 9 ms).
- The two runs that stayed on the title screen (above): not reproduced in the second session (see there); their cause is not known.
- `WATCH_KICK`'s picture: second session, three scenes by eye and 12 frame-log captures (17,700 kick frames); two exposure pulses in the kick
  arm, one in the baseline: not shown to differ, not shown equal. The PC 3050 was still not used.
- The GPU completion's own time: the log's GPU time is 2.6-2.9 ms a frame and its "GPU tail" 0.4-0.7 ms, but a recording submitted at
  4.5 ms of the traced frame was not done on the timeline before 9.9 ms. Where the other ~3 ms go (the segments of the heavy command
  buffer are recorded together by the parallel replay and reach the queue together; the queue submit's own time was not logged until
  the last build) is the open question and the one worth the next stream's time: the heavy submission's GPU work is not pipelined with
  its replay.

## Second session (2026-11-14 evening, main PC): the switch's safety, and what the GPU does between the pieces

### RX_VK_WATCH_KICK: the title-screen stalls, the three scenes, the speed

- **Stall test.** 10 baseline and 10 `RX_VK_WATCH_KICK=1` runs of castle-move, interleaved, in the whole-machine lock, from the title
  (scratch `stall.sh`): **20 of 20 left the title screen** (4,205-4,310 frames measured baseline, 4,507-4,604 kick). Baseline 96.2-98.1 fps,
  mean 97.5; kick 103.9-105.2, mean 104.5: **+7.1%**, every kick run above every baseline run. With the 10 runs of this test, the three
  validation runs, 7 frame-log runs and 3 kokiri-idle runs, 23 runs had the kick on and none stalled (the first session: 1 of ~6 with the kick stalled,
  1 of ~3 with `NO_READBACK_WAIT`). The stalls were not reproduced with the kick or without it; the first suspect stays the scripted menu
  input, known to be missed now and then. The kick's code has no path that can wait for something that never comes (the order thread waits
  on the event with a 1 ms timeout, as it slept 1 ms before). Said plainly: 23 clean runs rule out a stall rate above ~12%, not a rare one.
- **Validation, the three scenes** (`valid.sh`, kick on): kokiri-idle, castle-move, hyrule-walk all ran (4,358 / 4,541 / 4,374 frames), no
  crash line, game-speed ratio 59.99 / 60.06 / 60.06; the last shots of each scene looked at by eye (the Kokiri lake at night, a field at night
  with Link and Navi, a night road with ruins: not black, not white, not tinted, geometry and HUD whole).
- **Picture by the frame log** (`RX_VK_FRAMELOG` 25 s, mean luma of every frame, scratch `lumastat.py` / `steps.py`): kokiri-idle base 50.67,
  kick 50.42, no frame-to-frame step above 0.12; hyrule-walk base 47.0, 46.8, 48.0, 47.0 and kick 47.2, 46.0, 46.9, 47.3; castle-move base 41.3,
  41.4 and kick 40.8, 41.7 (session 1: base 40.6, kick 41.0). One kind of event shows in both arms: the picture goes 8-15% brighter for
  15-16 frames and comes back (the exposure pulse of the `RX_VK_READBACK_STRICT` note, a doubled meter sum): base 1 in 16,600 frames
  (hyrule-walk), kick 2 in 17,700 frames (one hyrule-walk, one castle-move). The first session's baseline has them too (4 one-block events).
  The kick does not make it obviously more frequent (2 against 1), and it is not new; it is also not proven not to.
- **Speed**: castle-move above (10 against 10, +7.1%). Kokiri-idle, whole machine, 3 against 3 interleaved: base 93.8, 94.8, 94.5 (mean 94.4),
  kick 101.5, 101.9, 101.6 (mean 101.7): **+7.7%**, no stall. **The kick is now on by default** (`RX_VK_WATCH_KICK=0` turns it off): the
  three scenes ran clean and the picture is right by eye and by luma; it is its own commit, easy to take back if the owner's play shows
  anything.

### What the GPU does between the pieces of the heavy submission (`RX_VK_TL_S`, now with the GPU's own timestamps)

`RX_VK_TL` / `RX_VK_TL_S` now also print the GPU's side: every recording's upload and command buffer start and end on the host clock
(`gpu-up+/-`, `gpu-cmd+/-` lines; the timestamps are read when the recording's slot comes round, so they are printed 14 presents after the
window opens; `VK_EXT_calibrated_timestamps`, enabled only with the timeline on). The queue's other submits (present's blit, sparse binds,
texture and memory readback submits) have `blit-submit` / `bind-sparse` / `tex-rb-submit` / `mem-submit` events: in a frame's window only
the present's blit is submitted besides the recordings (no bind, no readback submit). Scratch `gpulag.py` tables each recording's queue
submit, GPU start and end. Castle-move, 3 frames, kick off, the heavy submission (4 pieces, cut where a queue stops at an event wait):

| piece | queue submit (ms) | GPU start after the submit (ms) | GPU idle before it (ms) | GPU time (ms) |
|---|---:|---:|---:|---:|
| 1 | 0.57 | 0.02 | | 0.24 |
| 2 | 1.77 | 0.02 | | 1.14 |
| 3 | 2.98 | **0.94** | 1.0 | 0.55 |
| 4 (the last: the write-backs, the timeline value) | 4.87 | **4.56** | 4.96 | 0.20 |

The same in every frame and every run (piece 3 starts 0.78-1.08 ms late, piece 4 4.0-4.7 ms late). The pieces' queue submits take 15-40 us
(`rec-qsubmit`) and the GPU is idle (the GPU time of the whole heavy submission is 2.1 ms) while the piece sits in the queue. So the **3 ms
between the submit of the heavy recording and its completion is neither GPU work nor the submit's cost: the GPU starts the last piece 4.5 ms
after it was submitted, and the readbacks of the next frame's CPU phase wait for that**: the late fix's wait ends with the end of piece 4 (the
fix thread's `fix-wait-` 0.05 ms after `gpu-cmd-`). Once the GPU starts piece 4, everything queued behind it (the Presentation Thread's six
small recordings, a dozen command buffers) runs back to back in 0.3 ms. The two late starts (piece 3's and 4's) are 4.5-5.8 ms apart, ~5.2
on average, and fall at the same phase of every frame.

What did not change it (each a timeline run of castle-move, the same delays): a `vkGetFenceStatus`, a `vkGetSemaphoreCounterValue` or an
empty `vkQueueSubmit2` after each submit (to make the driver hand the work over: `RX_VK_SUBMIT_KICK`, built and taken out), `RX_VK_CHUNK=0`,
`RX_VK_POOL_MIRROR=1`, `RX_VK_PRESENT_QUEUE=0`, `RX_VK_BATCH=1` (with and without `RX_VK_BATCH_IDLE=0`), and holding the third and later
non-signalling pieces to send them with the signalling one in one submission (`RX_VK_TAIL_HOLD`, built and taken out: piece 3 then started
at once when it went out alone 2.2 ms after piece 2's end; piece 4, submitted 0.2 ms after piece 3's end, still started 4.9 ms later; fps
95.7 against 97). What fits all of it: **a submission that reaches the queue soon (under ~0.5 ms) after the previous one's completion waits
~1 ms (after piece 2) or ~5 ms (after piece 3) before the GPU starts it, with nothing running; one that comes 0.9 ms or more after a
completion starts at once.** The cause is not found (the queue's scheduling after a completion in the driver or the OS; not PCIe, which is
Gen5 x16, and not a pool or a write-back cost: pool mirrors, chunking, batching and the presents' own queue change nothing). It is the one
lever left under the frame's loop: with piece 4 started when it is submitted, the GPU would be done at ~5.4 ms of the frame instead of 9.6,
before the readbacks come (7.8 ms), and the late fix's wait (1.9 ms in that frame) would be gone. Not built: nothing tried moves it.

One more try, `RX_VK_PIECE_GAP_US=1000` (built and taken out): a piece of a submission is sent only 1 ms after the GPU's end of the
previous piece (its bottom-of-pipe timestamp against the calibrated device clock now). Piece 3 then started 0.02-0.09 ms after its submit
(the rule holds for it), but piece 4, submitted 1.2-1.3 ms after piece 3's end, still started 4.28-4.4 ms later, and the frame rate fell
(91.5 against 97). So the piece-4 delay is not about its arrival after the previous completion either; nothing at that moment on the
CPU side of the timeline (all the threads are in waits or quiet) explains it.

### The late-fix dependency (item 3): real

The readbacks of frame N+1 do not wait for the late page in order to read their own range; they wait for the GPU's completion of the
write-back of the previous heavy submission, and their range depends on that. (1) The writer value of the meter's ranges (`rb-val`) is the
heavy submission's own, `value_rec` maps it to the FIRST recording of that submission (an earlier one, long done), so the recordings'
semaphore alone lets the read pass: five readbacks of the same writer made 4.9 ms into the frame, before any late fix existed, passed in 4
us each and read what the GPU had not written yet (ranges of other uses, so the picture is not what they decide). (2) The late fix that
holds the later ones has the same value as that writer: the capture of the next frame's first held submit finds the heavy submission's last
write-back still pending (`fix-pending`, the value the readbacks wait on). So there is no readback that waits for the late page and does not
depend on it here; skipping the flush's wait (`RX_VK_WAIT_REC_SENT`, session 1) burned the picture out for that reason. What would shorten the
wait is the GPU completing sooner (above), or the meter being written back by an earlier piece than the last (registered per submission, at
its end today; not changed).

## Hand-over (second session)

- Branch `stream/r2-readback`, worktree `E:\OOT\HeyListen-wt\r2-readback`. New tooling, all logging: the GPU side of `RX_VK_TL` /
  `RX_VK_TL_S` (`tl_gpu_dump`, `TlGpuRec` in `vk_replay.cpp`; `VK_EXT_calibrated_timestamps` is enabled only when the timeline is asked for),
  `vk_tl_note` (events from `vk_texture.cpp` / `vk_memory.cpp`). Scratch scripts, not in the repository, under
  `E:\OOT\scratch-from-c\r2b`: `tlrun.sh` (one castle-move run with the timeline at 70 s, merged and tabled), `tlmerge.py`, `gpulag.py`
  (each recording's submit, GPU start and end), `stall.sh` (interleaved arms with frames and fps), `flmany.sh` + `steps.py` (frame-log runs,
  luma steps), the older `lumastat.py` / `tlshow.py` under `r2`.
- Session 1's tools (RX_VK_RB_PROF, HOS_RB_LR_LOG, RX_VK_TL) are described under "Tools built".
- Built and taken out (the numbers are in "What the GPU does between the pieces"): `RX_VK_SUBMIT_KICK`, `RX_VK_TAIL_HOLD`,
  `RX_VK_PIECE_GAP_US`.
- Next, in order: (1) **find what delays the GPU's start of the heavy submission's last piece by 4.3-4.7 ms** (and the third by ~1 ms):
  piece 4 is submitted at ~5 ms of the frame and starts at ~9.5, with the GPU idle, whatever the submit path (batched, kicked, alone, with a
  gap, on the presents' own queue or not). Ideas not tried: the same timeline with the game's window in fullscreen (`fullscreen=1`: DWM is out
  of the queue's scheduling), with `HOS_` / driver settings that change the GPU's scheduling (hardware-accelerated GPU scheduling), a timeline
  on the PC 3050 (it is the machine where the readbacks cost 9 ms) to see if the late start is the same there, a Vulkan `VK_NV_low_latency2`
  off/on A/B, and a trace of the process with ETW GPU events (GPUView) for one frame: it shows what the queue's hardware context waits on.
  If the late start goes away, the late fix's wait (1.4-1.9 ms a frame) goes with it, and `NO_READBACK_WAIT`'s +17% is the bound of the gain.
  (2) The exposure pulse (15-16 frames, +8-15%) exists with and without the kick at about 1 in 10,000 frames; `RX_VK_READBACK_STRICT=1`
  is its known cure (18% on this driver): a cheaper exact rule is wanted. (3) The PC 3050 was not used by either session.

## Third session (2026-11-14 night, main PC): what the late pieces wait on

Question: the heavy submission is cut into 4 pieces at event-wait stops; the GPU starts piece 3 ~1 ms and piece 4 ~4.5 ms after the piece was
submitted, with the GPU idle by our own timestamps, and every readback waits for piece 4. What does a piece wait on, and why is the wait satisfied
so late? Everything below is castle-move on the main PC (RTX 5070, driver 617.42, 1280x720 window, `RX_VK_WATCH_KICK` on), the whole machine held,
timelines by `RX_VK_TL_S=70` (about 104 fps with the probes on).

### 1. At the API level a piece waits on nothing

`submit_recording` (every piece, every held or batched submit) has **no wait semaphores and no events**: each submit signals the two timelines
and a fence and that is all; no `VkEvent`, no `vkCmdWaitEvents`, no `vkSignalSemaphore` anywhere in the renderer. The only wait on the queue is
the present's blit (the acquired image's semaphore; `RX_VK_ACQUIRE_FENCE=1` takes it off the queue). The game's GPU events are host-side stops of
the replay, not GPU waits. The event a stopped piece waits for is a guest event the replay polls (the order thread, and `sync_watch`); the
heavy queue's first piece waits for the CPU event the main thread signals (`cpu-signal` in the timeline: value 0 resets, value 1 signals,
alternating between two event words). Those stops explain when a piece is *recorded and submitted*, which the timeline shows (piece 4 is submitted
0.5 ms after the GPU ended piece 3: the replay resumes right after the GPU's write of the event word). They do not explain a GPU that is idle after
the submit.

### 2. The late start is the graphics queue holding new work: probes (`RX_VK_TL_PROBE`, logging only)

Built: `RX_VK_TL_PROBE=1` (needs `RX_VK_TL` / `RX_VK_TL_S`): before each recording's queue submit a one-timestamp command buffer is submitted on the
replay's queue (probe 1); `=2` also on a second queue of the same family (probe 3); with `RX_VK_PRESENT_QUEUE=1` also on the compute queue right
after the piece (probe 2); `=3` adds a monitor thread that submits a probe on the replay's queue every 0.2 ms while the window is open; `RX_VK_PROBE_JOB=<n>`
(+ `_EVERY`) adds synthetic jobs of n fills of a 256 MB buffer; `RX_VK_TL_FRAMES=<n>` lengthens the window; `RX_VK_QUEUE_PRIO` sets the queue priority.
Results (scratch `probes.py`, `monitor.py`, `holes.py` under `E:\OOT\scratch-from-c\r2c`):

- **A trivial probe submitted just before piece 4 starts on the GPU at the same instant as piece 4** (+4.4 to +5.0 ms after the submit, to within
  0.01 ms), and a probe before piece 3 the same at +0.9-1.1 ms. So the delay is not the piece's content, size, signals or fence: it is the queue.
- **The compute-family queue is not held**: its probe, submitted right after the piece, runs 0.013-0.03 ms after the submit in every frame, while
  the graphics queue sits in its hole. The second queue of the graphics family (probe 3) is held too but differently (+0.3 to +2.1 ms).
- **The monitor (a probe every 0.2 ms) shows the graphics queue gated in two windows a frame**: after the end of piece 2 (a ~1.1 ms job) the queue
  runs nothing for **1.1-1.3 ms**, then runs everything queued back to back (2-3 us apart); after the end of piece 3 (0.55 ms) it runs the probes
  already queued and then runs nothing for **4.9-5.05 ms**; probes and pieces submitted during that time all start together when it ends. The lengths
  are the same in every frame (1.13-1.16 and 4.99-5.05 within a run; 4.5-5.4 on other runs) and are not related to anything the CPU does in
  the window (no event of any thread falls at the end of a hole). Outside those two windows the queue answers in 6-20 us.
- The windows and lengths are **the same** with: fullscreen (a 2560x1440 swapchain, `fullscreen=1` in the slot's options), `RX_VK_IMMEDIATE=1`
  (immediate present mode), `RX_VK_ACQUIRE_FENCE=1`, `HOS_NO_PRIORITIES=1`, `RX_VK_QUEUE_PRIO=0`, `RX_VK_PRESENT_QUEUE=1`, no implicit Vulkan layers
  (`VK_LOADER_LAYERS_DISABLE=~implicit~`: the RivaTuner layer, Steam's and Epic's overlays were loaded before), `RX_VK_NO_WRITEBACK=1` (piece 4's GPU
  time falls from 0.20 to 0.03 ms: the write-back copies are nearly all it holds; the holes stay), `RX_VK_NO_HOST_IMPORT=1`, and with a **60 fps cap**
  (`HL_FPS_LIMIT=60`: CPU not saturated; 1.13-1.16 and 4.9-5.04 ms again). Earlier sessions: `RX_VK_CHUNK=0`, `RX_VK_POOL_MIRROR=1`,
  `RX_VK_BATCH`, a gap before the submit, holding pieces, empty submits and fence/semaphore queries after each submit.
- **It is not a property of the GPU after any long job**: synthetic 1.2-1.4 ms fill jobs in the same process, at the title screen (light load), are
  followed by no hole. At the title screen no hole appears at all (probes answer in 10-20 us); the holes belong to the game's real heavy
  submission (about 160 render passes, 1,900 draws and 140 dispatches a frame, per `RX_VK_PASS_TIMING=1`).
- The GPU clock is steady (P0, 2.89 GHz at 50 ms sampling); no other process uses the 3D engine more than 1% (`\GPU Engine` counters), yet the
  counter gives HeyListen's 3D engine **82% busy** against ~25% of real work by our timestamps: Windows counts the holes as busy, so the engine
  (or its context) is occupied by something that is not our timestamped work.
- Not admin, so no ETW GPU trace: the next step is outside what an agent here can do.

### 3. What this means for the frame

The loop is: the main thread's event signal -> the replay (pieces 1-4 submitted over ~5 ms) -> the GPU's holes -> piece 4's end at ~9.7 ms ->
readbacks of the next CPU phase (which arrive at ~7.8 ms and wait ~2 ms) -> the next signal. Without the 5 ms hole after piece 3 the GPU would be
done at ~5.4 ms, the readbacks would not wait, and `RX_VK_NO_READBACK_WAIT`'s +17% is the bound of the gain. Piece 4 is almost only the write-back
copies (0.17 of its 0.20 ms), which are what the readbacks wait for.

Not built, and why: (a) the write-back copies of piece 4 on the compute-family queue (it is not held): the copies depend on the draws of pieces 1-3, the
write-back machinery (`vk_memory.cpp`: captures, mirror syncs, late pages, the late fix) assumes one queue and `dev.present_mu`, and the compute queue
for presents is off by default since the device lost of issue 1161; too large for what is verified; (b) writing back each piece's ranges at the end of
that piece and telling the readbacks which piece wrote their range (so the meter, written by an earlier piece, would not wait for piece 4): needs per-piece
write registration in `vk_memory.cpp` and a rule that no later piece rewrites the range; not started.

### Tools an owner could run (not installed here, not downloaded)

`wpr -start GPU -start CPU` (built in, needs an elevated shell) around a castle-move run, then the Windows Performance Analyzer or GPUView on the
`.etl`: it shows what the 3D queue's hardware context waits on in those two windows. Also Nsight Systems (a GPU trace of the queue), and the same run with
hardware-accelerated GPU scheduling toggled in Settings (the owner's setting; `HwSchMode` is not set in the registry here) and, if one exists, another
NVIDIA driver.

## Hand-over (third session)

- Branch `stream/r2-readback`; new logging only: `RX_VK_TL_PROBE`, `RX_VK_PROBE_JOB`, `RX_VK_TL_FRAMES`, `RX_VK_QUEUE_PRIO` (diagnostics, default off, no behaviour
  change when off). Scratch under `E:\OOT\scratch-from-c\r2c`: `tlrun.sh <slot> <name> ENV=..` (one run with the timeline, merged), `probes.py`, `monitor.py`,
  `holes.py` (idle-while-queued windows), `late.py`, `gpu_sample.ps1` (`\GPU Engine` by process), `smi.ps1`.
- The open question is exactly one: **what holds the graphics queue for ~1.1 ms after the end of piece 2 and ~5.0 ms after the end of piece 3 of the
  heavy submission**, with new submits (any content) held, the compute queue free, and no CPU or API cause found. A reproduction outside the game that submits
  the same kinds of work (passes, dispatches, barriers) and watches probes would show whether it is the driver; removing one kind of work at a time in the replay
  (render passes, dispatches, mesh draws, the 2560x1440 targets) would show which one starts the hold.

## Sections from branches that were not merged (brought over 27 Nov 2026)

Written on those branches and left out when their notes came to main (256de8e6f); kept as they were, each under the branch it comes from. The verdicts on that work are in docs/research/perf-plan-2026-11.md and the branch audit.

### From perf/meter-latch (a3b5a46fb)

### Stream R3 (2026-11-15): the N-1 latch, frames in flight, and Longshot's render thread

Branch `stream/r3-pipeline` (worktree `E:\OOT\HeyListen-wt\r3-pipeline`, from main e80cb82e; stream R2's fourth session, `RX_VK_WB_COMPUTE`, is not in
main and was not used). Everything below is default off.

#### Axis 1: does a game thread wait on uploads, mirror syncs or captures? (`RX_VK_WAIT_LOG=1`, logging only)
Castle-move, main PC, per frame: the slot ring (`recording_wait`) 0.04-0.06 ms (5 waits, 0.05 of them over 50 us) on the Presentation Thread, 0.01 on the
held-submit thread; the replay lock taken by a submit 0.00 ms; by the present 0.04-0.24 ms; a capture's write-back wait: none seen; the capture's own
copy (work, not a wait) 0.13 + 0.16 + 0.07 ms. The only game-thread wait of size is the main thread's readback (`wait_recorded`, 1-2 ms a frame, 0.8-1.4
waits a frame) and the module workers' readback ticket. **The owner's guess (uploads of N+1 wait for N) does not hold here: no ring of upload slots
is worth building.**

#### Axis 2: the N-1 latch (`RX_VK_READBACK_LATCH=<sizes>`, `+` or `;` also separate): result, flashes. Not adopted.
Built: the latched ranges' runs are cut out of the write-back copies and copied into a staging record per submission; a readback of that size is
answered from the newest record the GPU's timeline has reached, written into guest memory at that range's own readback, with no wait and no ticket.
- First build (a record applied at any latched range's readback): castle-move, whole machine, 3 against 3: base 100.4, 98.8, 95.6, latch 102.9,
  103.1, 102.9 (+4.8%, against +17% for no waits at all); the frame log: **106 exposure pulses in 2,473 frames** (luma up to 77 against 53 in the
  baseline, steps up to 15), the baseline 0. A write-back landing after the game's reset of the meter was taken into the next sum by the readback
  refresh (a doubled sum).
- Second build (each range answered at its own readback from a kept copy, never changed elsewhere): the exposure went to **white and stayed**
  (frame-log luma 245 against 136 in the baseline, same scene). The owner saw the white flashes in the windows of these runs (r3-t1..t3, r3g-B-r1).
  The meter is read by an auto-exposure loop that does not tolerate a late or zero sum. **Dropped by the owner's decision; the code stays, off.**
  The targeted wait (`RX_VK_READBACK_STRICT` by sizes) is the way for the meter.
- Side result: with the latch the readbacks' remaining cost was the ticket (1.56 of 2.14 ms a frame summed over threads), not the late fix.

#### Axis 3: frames in flight (`RX_VK_FRAMES_IN_FLIGHT=<k>`, 1 = today)
The present is handed to a thread of its own (drain, blit, present under the order lock) and returns at once while fewer than k earlier presents are
being replayed; the host answers the game's present poll at once (`HOS_PRESENT_NOWAIT` is set by it). Longshot's 82886e89 (#588) does the same with a
render thread and found the same flash and its fix ("whole frames"): the replay of a frame starts after the main thread's signal. Ours already
starts the heavy queue only at that signal; what does not match is **the capture**.
- PC 3050 (own numbers, `RX_VK_READBACK_STRICT=1040+c0`, castle-move, one run each, not interleaved): off 1,374 frames in the 66 s cut (32.0 fps), k=2
  2,602 frames (59.4 fps, +85%), but the picture is wrong: frame-log luma step mean 12, 565 pulses in 661 frames.
- Why (found in the code, then tried): a stopped queue that goes on from a CPU event takes its capture when the replay goes on
  (`resume_queue`, `memory_capture_now`: the CPU's writes up to that signal). With the CPU a frame ahead that is the next frame's data. Built:
  the capture is made at the CPU's signal (`signal_stash`, `memory_capture_stash_*`, only for events a queue has stopped at; `RX_VK_SIGNAL_CAPTURE=0`
  off) and taken when the queue goes on. First runs with it: one run crawled (8 s frames), the next **lost the device** at the scene load. Not found
  why (candidates: the stash taken out of the capture queue's order against the submit captures; resources released while the previous frame's
  replay is under way; Longshot takes both captures in one ordered job queue, which this design has not). Not adopted; picture not verified.
- Longshot's own answer to the readback question: their readbacks wait per part for what is recorded (#2602). Without that wait their thread was not
  tried here; with ours (strict by the meter's sizes) the meter's value is the same as today's, but frames in flight makes the "previous frame's heavy
  submission" a frame older at the CPU's read, so whether the meter-size wait stays enough is **not established** (the picture failed before that
  could be told apart).

#### Second session (2026-11-15, PC 3050): one ordered capture sequence, the picture right, no speed
**How the captures are ordered now** (`RX_VK_FRAMES_IN_FLIGHT=2`; `RX_VK_SIGNAL_CAPTURE=0` off; with k=1 nothing of this runs): the capture sequence
(`captures` in vk_memory.cpp) holds items in the CPU's order: a submit's capture, and every CPU event signal (`memory_capture_signal`), a signal with
a capture where it meets a wait a submission is stopped at (the waits are noted at each submit, `note_waits`, as Longshot's `bh_note_waits`). Both are
made on the CPU's thread under one lock (`submit_order_mu`), so the write marks are consumed in the CPU's order. The replay takes the items from the
front (`consume_signals`, at the top of `schedule()` and of `submit_job`: the open recording is ended, each signal's capture taken, its event set),
so a stopped queue goes on from the signal's own capture and never re-takes one (`resume_queue` skips `memory_capture_now` for it). The staging
copies are plain host memory; the mirrors are written only by the replay.
**Cause of the crawl and the device loss of the stash build**: the stash's capture was made on the main thread while a submit thread made its own: two
`capture_make` at once (the write-mark scan, the copy helpers, the buffer pool are not made for it), and the stash was taken out of order against the
submits' captures. The lock and the order fix both. **A second cause of the wrong picture**: the present thread's `drain(0)` forced stopped queues
past waits the main thread had not signalled yet, the next frame's included: now it forces only submissions up to the value handed out at its own
present (`PresentJob::upto`, `drain(why, limit)`), and waits (without any lock) for the frame's signal (`cpu_wait_open`, 200 ms at most).
**Picture, k=2** (castle-move, kokiri-idle, hyrule-walk, strict wait on 1040+c0): frame-log pulses 0 in all three scenes (baseline k=1: 0-1 in a
25 s capture); frames looked at (frame-log frames) right: lit, no flash; 10 starts from the title all reached play (32-36 fps), no device loss.
**Speed, PC 3050, castle-move, 3 interleaved**: k=1 31.7 / 31.9 / 31.9 fps, k=2 31.8 / 31.9 / 32.5. **No gain.** The 59 fps of the first session is not
reproduced and was the forced waits: k=2 with `RX_VK_SIGNAL_CAPTURE=0` here gives 24 fps and 411 pulses. Without the meter's strict wait (`RX_VK_READBACK_STRICT=0`)
33.9 fps and 17 pulses; with no readback wait at all (`RX_VK_NO_READBACK_WAIT=1`) 36.7 fps, 3 pulses. So the meter's wait is not what holds the frame.
**Where the threads wait** (`RX_VK_WAIT_LOG=1`, ms a frame): k=1: Presentation Thread's replay lock at the present 14.8; main thread's `wait_recorded`
6.5 + readback ticket 2.7; module workers 3.0 + 2.1 (`wait_recorded`) and 1.2 + 0.9 (ticket). k=2: the present's wait is gone from the game's threads
(the game waited 0.00 ms for a free present slot), but the main thread still waits 5.3 + 2.2, the workers about the same, the CPU signal's lock and capture
0.4, a submit's replay lock 0.6 (the held-submits thread), its capture copy 2.9. The GPU is idle (5.5 ms of a 31 ms frame): the frame is CPU-bound on this
6-core PC, and the replay of every submit still runs inline on the game's submitting threads, so overlapping the present's replay moves nothing the chain waits on.
Not done: the main PC batch, the present arm (`RX_VK_PRESENT_QUEUE=1` with `RX_VK_ACQUIRE_FENCE=1`: the swapchain already asks for 4 images).
The owner's three rules: (1) the staging copy is made at the CPU's submit or signal (host memory) and the mirrors are written only by the replay when it
takes the item; (2) no lock is held across a readback wait (the present thread waits on a condition variable, `submit_order_mu` is held only for a
capture; not checked by an assert); (3) validation as above.
#### Hand-over (second session)
Next: the replay itself, not the present: make the submits' replay leave the game's job threads (a render thread consuming the same ordered sequence,
Longshot's `replay_thread_main` with its whole-frames gate), since with the sequence ordered the capture is no longer the obstacle. Then the main PC batch
(castle-move and kokiri-idle x3, k=1 against k=2) and the present arm. Scratch: `E:\OOT\scratch-from-c\r3b` (`pcfl.sh`, `ab.sh`, `starts.sh`, `batch2.sh`).

### Third session (2026-11-15, PC 3050): a render thread of its own (R3c)
`RX_VK_RENDER_THREAD=1` with `RX_VK_FRAMES_IN_FLIGHT=2` (default off; k=1 and the switch off are exactly the previous behaviour).
**What it is.** The game's threads capture (host staging, at the CPU's time, under `submit_order_mu`: the ordered sequence of the second session), hand
out the fence value and queue a job; the render thread (`render_thread_main`, pinned to the core kept for the replay, one thread) replays the jobs in the
order they were queued: a submit as `submit_q` does inline (under `sync_order_mu`, `submit_job`), a present, a texture or program release and a texture or
sampler registration after the submits before them (a registration at once would have the replay sample a later frame's slots: the async mode's lesson).
The mirrors are written only by the replay; no lock is held across a readback wait.
**What the game's threads wait for** (a fence's value, a finish, a read of a range the GPU writes back):
- a fence wait or poll of value f waits for f's job to be replayed (`rt_wait_value`; a poll says "not yet" until then), `fence_q` answers from `q_last` under its own lock;
- `finish` and the catch-ups wait for the queue to empty (`rt_wait_idle`);
- a CPU read (`invalidate`) of a range the GPU may write back waits for the submits queued before it (`rt_readback_catch_up`: the write-backs the replay has not
  reached are not registered yet). Ranges are learned by 64 KB regions (one a write-back was seen in always waits; one is trusted after 4 reads with none), so
  most of the 33 readbacks a frame wait for nothing; the strict wait on the meter's sizes is kept.
**A cycle found and removed.** The first build queued the present as a job. The Presentation Thread presents before the main thread's CPU signal for the frame,
so the render thread stood at the present waiting for the signal while the main thread's readback waited for a submit queued behind the present: 9 presents of
600 let go after the 200 ms limit. Now a present is **held** (`rt_held_presents`) and queued when the CPU's signal its frame's waits need has been made (`seq`: only the
waits noted before the present call; `upto`: the work handed over up to the call, so the next frame's stopped work is not forced); 0-1 let go in 2,400 frames.
Work the CPU queued after the present call replays before it.
**Picture** (frame log, strict wait on 1040+c0): pulses castle-move 0, 0, 0, 0, 0 (baseline 1, 0, 1), kokiri-idle 2, 0, 2 (baseline 0, 2, 0), hyrule-walk 0 (baseline 0);
frames of the three scenes looked at (lit, no flash, no missing geometry). 11 starts from the title with it on all reached play, no crash, no hang, no device loss.
**Speed, PC 3050, interleaved, fps of the whole run** (k=1 / k=2 + render thread): castle-move 32.0, 32.6, 33.3 / 31.3, 33.1, 32.6 (mean 32.6 / 32.3), kokiri-idle
33.0, 31.9, 33.3 / 33.1, 35.9, 33.3 (32.7 / 34.1), hyrule-walk 32.0 / 35.1 (one run each); the earlier ab1 batch (k=1, k=2, k=2 + render thread: 32.3 and 31.8,
32.3 and 31.1, 32.9 and 31.6). **No gain beyond the spread in castle-move; at most +4% in kokiri-idle.** One run (c1a) gave 37.2 and was not repeated.
**Why** (`RX_VK_WAIT_LOG=1`, castle-move, ms a frame; render thread's own log line every 600 presents): the present's wait on the replay lock is gone for good
and the main thread's waits fell from ~8 ms (second session) to ~4.5 (wait_value 2.4, wait_recorded 0.9, ticket 0.3, CPU signal 0.4, catch-up 0.45; the module workers
0.5-2.3 each), but **the render thread is busy 21 ms of a 31 ms frame** (submits 1.8, the present job 19.0: the heavy submission is replayed inside the present's
drain, with the parallel segments on the replay's workers, then the blit; the wait for a job 10.6) and the queue behind it is 52 jobs deep on average (247 at most:
texture releases and registrations, 184 a frame). The heavy replay cannot start before the main thread's signal (its first command buffer waits for it), and the next
frame's first readbacks of GPU results (the meter, strict) wait for it plus the GPU's tail: the chain is signal N -> replay 19 ms -> GPU tail -> readbacks of N+1 -> rest of
N+1's CPU phase. The GPU is not the limit here (6.5 ms a frame of work, gaps between recordings 22.6 ms). A thread of its own for the replay does not shorten that
chain; only a shorter heavy replay (more parallel segments, a faster CPU) or a readback that does not wait for it (the latch, dropped) would.
**Not verified**: the main PC (frames of 10 ms: the render thread may matter more there; needs `slot.sh all`), the present arm (`RX_VK_PRESENT_QUEUE`), `RX_VK_LAG_LOG`
and the replay dump with the render thread on (not supported: `lag_*` and the dump assume the inline replay), Linux. Test suite: 1 failure (the known one).
#### Hand-over (third session)
Scratch: `E:\OOT\scratch-from-c\r3` (`pcfl.sh`, `ab3.sh`, `batch3.sh`). Next, if the owner wants the render thread kept: the main PC batch (castle-move and kokiri-idle x3, k=1
against k=2 + render thread, whole machine); then shorten the present job (what the 19 ms are made of: drain's segments against `do_present`'s waits).

### Stream C1o (2026-11-17, main PC): the light meter latched word by word, and the phase-lag question

Branch `perf/meter-latch` (worktree `E:\OOT\HeyListen-wt\c1b-meter`): main 8045fe73 + stream/r3-pipeline (merged: R3's latch code, `RX_VK_FRAMES_IN_FLIGHT`,
`RX_VK_RENDER_THREAD`) + this stream. Everything below is default off.

#### What the CPU reads, at the data level (`RX_VK_METER_DIFF=<present>`, logging only)
`RX_VK_METER_DIFF` logs, for each read of a 0x1040 or 0xc0 range, the words that changed since the range's last read (what the CPU wrote, or a
write-back that landed before this read) and the words that changed during this read's wait (the write-back it waited for). Default waits,
castle-move (Market save), the Deku tree (inside, dark) and Kokiri Forest. The reads all come from the ring helper `f_80357060` (five sites), once
a frame each except the 0xc0 block, read twice:
- **0xc0 block** (one address in all the scenes, 0x11371e7f00): words 4-11 hold one float eight times that the CPU writes after its first read
  (outdoors it drifts slowly and steadily, 8.1e-4 to 5.6e-4 over 1,200 frames of the Market, not with the sums; in the Deku tree it does not
  change: a light scale of the time of day, by inference); words 12-27 are four vectors of sums the GPU adds (outdoors; negative terms: a
  spherical-harmonic sky probe, E1's "sky probe sums"); words 32-35 four more sums (E1's "RGBL"; seen in the Deku tree and Kokiri). The CPU
  reads them, then writes 0 into words 12-35 and its new scale into 4-11 before its second read of the block.
- **0x113c88e000 +0x1040**: words 0-3 are sums the GPU adds (about 19, 19, 7, 18 in the Deku tree: R, G, B and a luminance), set back to 0 by
  the CPU after it reads them; ~950 other words are a table the GPU rewrites every frame and the CPU never writes. In the Deku tree this range's
  write-back landed during the read's wait in 2,968 of 3,756 reads: it is the read the wait is for.
- **0x113c88b000 +0x1040** (outdoors only): word 0 a count of at most 256 (the CPU turns it into v/256: a sun visibility, by inference),
  set to 0 by the CPU after it reads it; the rest per-entry state the GPU writes. **0x113c891000 +0x1040**: a table the GPU rewrites (~160 words a frame).
- What the CPU does with the counts (read in the lifted code, not copied here): a moving average per value over a ring of samples, then a
  64-byte block of constants for the next frame. The sums of the 0xc0 block and of the first 0x1040 range are read and reset; where the
  exposure is derived from them was not found. **The final pass's cbuffer 3 (`RX_VK_EXPOSURE_LOG=3`) holds no exposure**: over 770 frames of the
  Deku tree only one flag and a three-vector change, and the word `RX_VK_EXPOSURE_LOG` names (c3+0x38) stays 1.0.

#### The latch, word by word (`RX_VK_METER_LATCH=1`; `gpu/vk/meter_latch.h`, `vk_memory.cpp` `latch_answer_words` / `latch_refresh_words`)
Built on R3's latch (the ranges' runs cut out of the write-backs and kept in a staging record per submission). What changed, and why R3's
second build burned out to white: R3 wrote the whole range back at each read through `hos_gpu_write_live`, which refuses a page the CPU
wrote since its last collection, and the CPU writes these pages every frame (its resets): the reads saw the CPU's own zeros, the light
measured 0, the exposure went to its maximum. Here:
- **The answer** (a read of 0x1040 or 0xc0, no ticket, no wait) takes the newest record the GPU's timeline has reached and merges it word by
  word into guest memory against the range as the CPU last saw it (`merge`): a word the CPU did not change takes the record's (the GPU's
  table and state); a word the CPU set to 0 (a sum it took and reset) takes the record's only when the record is new to the CPU or a new
  frame reads with none newer (the sample held); at the 0xc0 block's second read of the frame it stays 0 (the first version put the sums
  back there: the GPU then added the next frame onto them, the sums grew without end and the picture burned out to white, the same symptom
  as R3's); a word the CPU set to anything else is the CPU's own from then on and is never written (the 0xc0 block's scale).
- **The readback refresh** (each submission's copy of the read pages from guest memory into the mirror) gives the GPU only the latched
  words the CPU wrote since the last answer (its resets and its scale), not the whole page: guest memory holds the GPU's words of an older
  frame at best, and copying them would set the GPU's own state back.
- The records keep coming whatever the CPU does; a read of a range with no finished record, or before the CPU's view is known, waits as before.
- Diagnostics: `RX_VK_METER_DEBUG=<n>` (n answers after the 3,000th, with the CPU's word, the record's and the answer; n records' runs), and
  a line every 3,000 asks (`vk: meter latch: ... asks`).

#### Pictures (main PC, one slot, at most two games on the machine; build c1o-e = the latch and its fix)
Frame logs (320x180) for 25 s from 5 s after play began, scratch `E:\OOT\scratch-from-c\c1o\flrun.sh`; pulses by E1's detector (a run of frames more
than 3 luma off the median of the 121 around it, last 30% of the log); shots every 600 presents in `build/s5/c1o-a4-*-shots` (local). Saves from
`build/s5/savelib` (the game loads the save it wrote last). A run that stayed on the load screen (the known flake, any arm) was run again.

| scene | arm | fps (one run, the machine shared) | frames | luma mean | pulse runs | largest frame step |
|---|---|---:|---:|---:|---:|---:|
| kokiri-idle (Kokiri Forest) | default (waits) | 87.6 | 2,327 | 56.6 | **0** | 0.1 |
| | no wait (`RX_VK_NO_READBACK_WAIT=1`, the reference) | 105.0 | 2,684 | 57.4 | **10** | 12.9 |
| | latch | 70.8 | 1,375 | 56.8 | **0** | 0.6 after the fade-in |
| | latch + ratio | 91.4 | 2,402 | 57.7 | **0** | 1.1 |
| | latch + freeze | 87.8 | 2,275 | 57.2 | **0** | 0.1 |
| | latch + frames in flight 2 + render thread | 97.3 | 2,523 | 56.8 | **1** (a 3.0 step) | 3.0 |
| castle-move (Market; a moving view: E1's detector counts the view's own changes) | default | 40.6 (a loaded moment) | 579 | 74.0 | 13 (18 bumps) | 30.6 |
| | no wait | 76.6 | 2,269 | 75.4 | 52 (57 bumps) | 33.3 |
| | latch | 75.8 | 1,995 | 73.2 | 11 (10 bumps) | 31.4 |
| | latch + ratio | 75.8 | 2,020 | 73.7 | 11 (7 bumps) | 31.3 |
| | latch + freeze | 70.8 | 1,866 | 74.2 | 12 (11 bumps) | 50.3 |
| | latch + frames in flight 2 + render thread | 58.7 | 1,554 | 73.6 | 14 (11 bumps) | 30.6 |
| hyrule-walk (Hyrule Field) | default | 46.2 (shared machine) | 1,113 | 117.6 | **0** | 1.6 |

("bumps": a frame more than 5 luma off the mean of the frames 8 before and 8 after while those two agree within 3: a short brightness bump the
view's motion rarely makes; scratch `bumps` in this stream's notes.) Shots of the castle runs looked at: lit right, no flash, no burn.

#### Speed (build c1o-f = d471b9bc)
**Main PC** (Ryzen 7 7800X3D, RTX 5070), whole machine held (`slot.sh all`), arms interleaved and rotated each round, fps of the run (110 s,
cut from play). Before and after every run the script checked that no clang / lld / build.py / other game ran and no build or slot lock
existed; one run caught a compile and was redone (scratch `speed2.sh`). "ratio" = `RX_VK_METER_LATCH=1 RX_VK_METER_COMP=ratio`;
"FIF" = `RX_VK_FRAMES_IN_FLIGHT=2 RX_VK_RENDER_THREAD=1` (without the latch the meter's strict wait is on: R3's default).

| scene | base | ratio | ratio + FIF | FIF alone |
|---|---|---|---|---|
| castle-move (Market) | 77.1, 83.9, 79.5 (80.2) | 67.6, 86.2, 81.3 (78.4) | 80.0, 79.5, 81.8 (80.4) | 81.9, 81.3, 40.6* (81.6 without *) |
| kokiri-idle (Kokiri Forest) | 90.8, 90.6, 94.3 (91.9) | 89.9, 89.7, 95.1 (91.6) | 97.7, 96.3, 96.9 (**97.0, +5.5%**) | 80.5, 83.4, 91.0 (85.0) |

(*the whole run slow, every phase of the render thread ~1.6x: an outside disturbance between the checks, by inference; not redone.)
1% lows: castle base 15.7 / 21.4 / 16.4, ratio + FIF 15.3 / 20.1 / 9.5; Kokiri base 21.5 / 19.6 / 21.0, ratio + FIF 22.4 / 21.3 / 25.0.

**PC 3050** (i5-10400F, RTX 3050), castle-move, whole machine, one series of 3 rounds with the latch without compensation (the ratio is ~1
there, see below) before the coordinator's arm list; the first rounds ran slower for every arm (caches settling): base 27.3 / 25.6 / 31.8,
latch 24.9 / 28.6 / 32.2, latch + FIF 27.4 / 31.9 / 32.7, FIF alone 26.7 / 31.4 / 32.0. The settled third round: 31.8 / 32.2 / 32.7 / 32.0, all
within the spread: no gain there, as R3 found (the chain is the heavy replay on that CPU). A second series with the coordinator's arms waited
3 h for the PC 3050 (held by another stream) and gave up.

#### What the phase-lag question came to
- The pulses of the no-wait reads are an **accounting** fault, not a phase lag: a sum reset by the CPU came back (a write-back landing after the
  reset, or an answer given again after it) and the GPU added the next frame onto it, or the CPU read its own zeros. With the words kept exact
  (the latch above) the pulses are gone with no compensation: Kokiri 0 (no wait: 10), castle-move at the default's level (11 against 52 for no
  wait), Hyrule Field's default 0 (the latch arms there were not finished: the main PC was taken for the owner's tests).
- The only CPU value in the meter's block that looks like an exposure (the 0xc0 block's scale, words 4-11) drifts by about 0.1% a frame
  outdoors and not at all indoors; `RX_VK_METER_COMP=ratio` scales the sums by it (a factor of ~1.000; 7,011 scalings in a castle run) and
  `freeze` holds the sums when it moves past 0.1% (it practically never does). Both are neutral by the counts above. Where the game turns the
  sums into its exposure was not found; the final pass's c3+0x38 (`RX_VK_EXPOSURE_LOG`) is constant 1.0 in every scene logged.

### Hand-over (C1o)
- Branch `perf/meter-latch`, worktree `E:\OOT\HeyListen-wt\c1b-meter`; builds `build/s5/bin/c1o-e` (the latch fixed), `c1o-f` (HEAD logging
  counters). Switches, all default off: `RX_VK_METER_LATCH=1`, `RX_VK_METER_COMP=ratio|freeze` (`RX_VK_METER_COMP_WORDS`, `RX_VK_METER_COMP_SCALE`,
  `RX_VK_METER_FREEZE_EPS`), `RX_VK_METER_DIFF=<present>`, `RX_VK_METER_DEBUG=<n>`, `RX_VK_METER_EXPOSURE_AT=<hex>`; the R3 merge brings
  `RX_VK_FRAMES_IN_FLIGHT` and `RX_VK_RENDER_THREAD`. Test: `tests/test_meter_latch.py`.
- Scratch `E:\OOT\scratch-from-c\c1o`: `flrun.sh` (frame-log run, load-screen retries, two-game gate `gate2.sh`), `speed2.sh` (guarded whole-machine
  series), `speed_pc3.sh` (PC 3050, Kokiri and Hyrule), frame logs in `fl/`.
- Not done (the machines were held by others): the frame-log counts of ratio + FIF on castle-move, Kokiri and Hyrule Field (latch + FIF without the
  ratio: castle 14 / 11 bumps, Kokiri 1 run of a 3.0 step), Hyrule Field's ratio arm, and the PC 3050 series on Kokiri and Hyrule.
- A known flake made planning hard: with three games at once, or a zone not prepared for the build's shader stamp, runs stay on the load
  screen or load past the 110 s of the run (Hyrule Field from the `lake-hylia` save took 99 s to begin play next to another game).


### From feat/native-readback-early (50db91b9e)

### HL_RB_EXACT: which readback waits are needed (stream HLE3, 2026-11-17/18, PC 3050)

Question (brick 3 of `build/s5/hle-plan-2026-11-17.md`): a CPU read of GPU results that waits only where a write-back or in-place GPU write
the game will read is pending, by an exact ledger answer, 1.5-1.9 ms off the PC 3050's ~27 ms frame chain; the named risk is the light meter.
Built on main 8ccfbeb4, branch `feat/native-readback-exact`, default off. Everything below is the PC 3050 (i5-10400F, RTX 3050), the check
mode on the three scenes; **it is a measurement of the waits, not of speed** (see "Not verified").

#### The waits there are

On the CPU's read of a GPU result (`invalidate`, `vk_replay.cpp`), in order:

| wait | where | needed? |
|---|---|---|
| the ticket: wait for the replay section in progress when the fence was polled (`order_ticket_wait`, `RX_VK_READBACK_TICKET`, on by default) | first, before any ledger lookup | yes for the exactness of any ledger answer: a section in progress has not registered its write-backs yet; 1.2-1.4 ms a frame summed over threads; **not touched here** |
| the write-back wait: `v` = the page ledger's value (`memory_writeback_value`: the largest submission value on any page the range touches); when `v` is above the GPU's progress, `wait_recorded(v)` = `queue_flush_for` (first `late_fix_wait_unlocked`: the capture's view of the previous frame's last write-back, i.e. the GPU finishing that frame) and then the recordings' timeline up to the FIRST recording `v`'s submission went into; with no recording known, `wait_queue_empty` and `wait_value(v)` | after the ticket, only for ranges with a write-back (`ever`) | the dependence is real for a range a write lands on; **the wait as built is neither minimal (it waits for the whole frame's last write-back) nor strict (the first recording)**, which is why `RX_VK_WAIT_REC_SENT` burned the picture out and why the late fix cannot be dropped without replacing it (above) |
| the strict wait: `wait_value(v)` on the timeline for the light meter's sizes (`RX_VK_READBACK_STRICT`, on by default on RADV only; `fix/white-flash-load` adds it on any driver while the presents are slow) | after the write-back wait | for the meter on RADV and under load; unchanged here |
| the texture catch-up: `sync_settle` / `wait_queue_empty` and `texture_flush_to_guest` when a texture the GPU wrote may be in the range | after the above | only for ranges a GPU-written texture is in; unchanged here |

Outside `invalidate`: the fence polls (answered "done" from the recordings' timeline, `wait_inner`), the capture's wait for a late page
(`vk_timeline_wait`) and the late-fix waits before every submit (`late_fix_wait_unlocked`); none is a CPU read of GPU results, none is touched.

#### What the logs say (check mode, three scenes)

~45-48 reads a frame go to ranges with a write-back; **2.0-3.3 a frame are pending by the page ledger**, always the same ranges (`rb exact size ...` lines):

- the small rings and the light meter: 0x40 (27 reads a frame, 0.6 pending), 0x1040 (3 reads, 0.45-0.7 pending: the meter's sums), 0x2000 (2, 0.45-0.7), 0x3980 (3-4, 0.16-0.3): **every pending read of these has a write on its own bytes**. The recording that carries that write is, at the time of the read, **1.8-2.7 recordings ahead of the GPU** and never already done (`carrier done`: 0.00 a frame): the CPU reads the previous frame's results at ~4-8 ms into its phase and the GPU finishes the carrying recording at ~9.7 ms (this file, second and third sessions). These waits are the real dependence.
- the big ranges: 0x1b580, 0x8b040, 0x9b040, 0x44b040 (1-2 reads a frame, 0.1-0.4 pending by pages): **no write-back on their bytes at all**, a write-back elsewhere on a page they touch. The byte ledger skips them: 0.25-1.05 a frame, **24-40% of the pending reads**.

| scene (check run) | presents | reads a frame | pending by pages a frame | no write on the bytes | carrier already done | waited for a recording | unnumbered write (old wait) |
|---|---:|---:|---:|---:|---:|---:|---:|
| kokiri-idle | 7,800 | 45.1 | 2.65 | 1.05 (40%) | 0.00 | 1.59 | 0.002 |
| castle-move | 600 | 47.5 | 2.0 | 0.23 | 0.00 | 1.78 | 0.007 |
| hyrule-walk | 3,600 | 48.0 | 2.84 | 0.53 | 0.00 | 2.3 | 0.002 |

What the exact answer saves, from the check mode (the old wait done after the exact one, its time counted): **0.29 (kokiri-idle) and 0.34 ms
(hyrule-walk) a frame summed over the reading threads**; the exact waits themselves are 20-40 ms a frame summed in the same frames. The ranges
the byte ledger frees are read after another read of the same frame has already waited for the same GPU completion, so freeing them shortens
nothing. The check mode and a second game on the machine (below) slowed the frames to 5-25 fps: the per-frame pending counts are lower bounds
(at 27 ms R2 found 6-10 pending of 19) and the milliseconds are not the chain's.

#### The design (default off, `HL_RB_EXACT=1`)

- **A byte ledger beside the page ledger** (`gpu/vk/rb_exact.h`, `Ledger`): every `wb_pages_note` (write-back runs and in-place GPU writes: the same calls, under the same lock) is kept as a byte range with its submission's value until the GPU is done with it and, once the recording that carries it is submitted, with **that recording's number on the recordings' timeline** (`memory_writeback_seal(seq)`, called from `submit_recording` right after `++rec_seq`). A piece's write-back copies are recorded at the end of that piece's recording (`memory_end_submission`) and an in-place write happens while its recording runs, so a read of the range is safe once that recording is done. A note with no number (not sealed yet) or applied by value (RX_VK_POOL_MIRROR's staged write-backs, `fixed`) keeps the old wait, on the ledger's value. A ledger that has to drop unfinished entries (16,384 held) says so (`exact` false) and the page ledger's answer stands.
- **The wait** (`invalidate`, `xmode`): the exact answer is asked only where the page ledger said pending and no `RX_VK_READBACK_LATE` rule named the range. No pending write on the bytes, or only writes whose recordings are done: **no wait**. Else `wait_recorded_exact(seq)`: held recordings sent unless a pending late fix holds them (the fix thread sends those), then the recordings' timeline up to the highest carrying recording; **no wait for the late fix**, no first-recording shortcut. The strict list (`RX_VK_READBACK_STRICT`) is asked after it, on the ledger's value. The ticket, the texture catch-up and the late rule are as before.
- **The check** (`HL_RB_EXACT_CHECK=1`, implies the switch): the exact action, the bytes the CPU would read taken (ranges up to `HL_RB_EXACT_CHECK_KB`, 256), then **the old wait in full and the submission's own value waited for on the timeline**, then the bytes taken again and compared. A difference is a read the exact answer would have changed; the first sixteen are logged (the words differing, a third read at once to tell bytes still moving, the newest write ever noted on the bytes). A line every 600 presents (`rb exact window`, `rb exact all`, `rb exact size ...`).
- Unit test: `tests/rb_exact_test.cpp` + `tests/test_rb_exact.py`: the header against brute force over 560,000 reads of a made-up world (submissions of several recordings, writes sealed with the recording's number, some by value, the GPU finishing recordings in order, a ledger small enough to drop entries): 0 mismatches, answers exact or erring upward when entries were dropped; the wiring: every write goes into both ledgers, the seal is made with the number the recording signals, the late fix is not waited for, the old wait is done in full in the check mode.

#### Check results (builds `hle3c` and `hle3d`, PC 3050, `HL_RB_EXACT_CHECK=1`)

| scene | runs | reads compared | skipped by the exact answer (no write on the bytes) | waited for a recording | differed |
|---|---:|---:|---:|---:|---:|
| kokiri-idle | 2 | 16,979 | 8,243 | 13,958 | **0** |
| castle-move | 2 | 6,750 | 1,091 | 6,241 | **1** |
| hyrule-walk | 2 | 16,305 | 3,114 | 15,110 | **1** |

(reads above 256 KB, 0x44b040 and the like, are not compared: 6,000-7,000 of them, all skipped by the exact answer.) The light meter's ranges (0x1040, 0xc0) and the rings 0x40 and 0x2000: **0 differences**. The two that differed are the same kind of read: the 0x3980 range at one word (0x40), **1-2 ulps** of a float near 0.9-1.0 (`3f67df18` against `3f67df1a`; `3f7fdbe4` against `3f7fdbe5`), after a skip, 1 in 8,000-9,000 compares; the first build (the submission's last recording instead of the carrier's) had one more in the same range at word 0x244 (a float near 8.4e-4, which is the size of the CPU's meter scale of C1o). The cause is **not separated**: the words look like values the CPU smooths itself (the range is read and written by the game); the later runs, which had the third-read diagnostic and a page watch on the two pages (`RX_VK_WATCH_CPU`), had no mismatch. Said plainly: two reads of ~40,000 would have changed by 1-2 ulps in a word the CPU may have written itself; none in the meter's or the rings' ranges.

#### Pictures

Shots every 300 presents (`RX_VK_SHOTS`, 1280x720) of every run, looked at by eye and by mean luma: kokiri-idle (the Market in these slots), castle-move and hyrule-walk, in the check mode and the exact mode: lit right, geometry and HUD whole, no tint, no black frame after the fade-in, no flash. The luma of the picture in play was 98-106 in kokiri-idle (both modes) and 112-116 in hyrule-walk (s2); in the slow runs (5-14 fps) the luma of the same view moved between 81 and 111 shot to shot (hyrule-walk, check mode, the fountain): the baseline run (default waits, hyrule-walk, the same slow conditions, `hle3-s4-base-hyrule-walk`) has **one blown-out, blue-tinted frame (luma 173, shot 7500) among shots of 109-113**, the exposure pulse this file's first sessions describe; none of the ~100 shots of the check runs and the exact runs shows one (the exact hyrule run reached only one shot after its fade-in, at 2 fps). The first shot after a fade-in is bright in every arm (base 130, check 149, exact 148): the fade, not the meter. The view of hyrule-walk differs from run to run (the script walks by time), so the luma of two runs is only comparable in the steady stretches.

#### Expected saving

**~0.3 ms a frame summed over threads on the PC 3050, ~0 on the chain**, against the 1.5-1.9 the plan estimated. The estimate came from the experiment with every wait off (27.0 -> 25.5 ms), which includes the meter's and the rings' real dependence; the ledger cannot remove it, because those reads wait for write-backs that land at the end of the frame's last recordings and the GPU gets there ~5 ms into the next CPU phase (the queue hole above). What the ledger does give: (1) the exact answer a latch (`perf/meter-latch`, C1o) or an earlier-landing write-back needs: it says which recording carries each range, and that no pending read is carried by an early one, so **moving the write-back of the 0x40 / 0x1040 / 0x2000 rings to a small recording submitted right after the last pass that writes them is the lever, not the ledger**; (2) a wait that does not hold the late-fix thread and does not flush the queue under `dev.present_mu`.

#### Not verified

- **Speed.** No fps round was made (as asked), and the runs were slow: the check mode (copies of 256 KB ranges, two waits) and a second game on slot a (another stream's) cut every run to 5-25 fps. The exact mode's own frame rate against the old wait's is open: the one pair run under the same load, hyrule-walk (base `HL_RB_EXACT=0` against `HL_RB_EXACT=1`), gave **7.2 fps (windows 4.7-21.9) for the base and 2.0 fps for the exact mode, whose readback waits were 360-400 ms each against 7-8.6 ms**; the second game was running, the windows of both swung by 5x, and the exact mode's earlier runs on kokiri-idle (24.3 against the check mode's 25.1) and castle-move (12.7) were not slower than the check mode's. So the exact mode is **not shown to be faster and one pair says slower**; it needs the same pair on a machine with nothing else running (the log lines `readbacks of GPU results: ... ms each` and `rb exact all` carry the wait times).
- A hand-played zone change: the exact answer is only as good as the registrations (`wb_pages_note`); a GPU write that is never noted (a binding over 1 MB written in place, the `RX_VK_WB_MAX_KB` cut) is not waited for by either ledger on its own bytes, and the old wait covered it by accident when it waited for the whole frame.
- RADV, AMD and Intel: the batch path (`batch_on`) and the late fix hold recordings there; the exact wait sends them unless a fix holds them. Not run.
- `fix/white-flash-load` (the meter's wait under load) is not on main; with `HL_RB_EXACT` the meter's reads wait for their carrying recording, which is what the strict wait means, so the two do not conflict, but they were not run together.

### HL_RB_EARLY: the small write-backs out of the big recording (stream HLE4, 2026-11-18, PC 3050)

Question (HLE3's lever): the rings (0x40, 0x2000), the light meter (0x1040) and the 0x3980 block are read by the CPU at ~4-8 ms into its
phase, and the exact ledger shows their write-back copies in the frame's LAST recording, 1.8-2.7 recordings ahead of the GPU. Copy them
earlier, in a recording the GPU finishes sooner, and the read waits less. Built on `HL_RB_EXACT` (its ledger; `HL_RB_EARLY=1` implies it),
default off. **Result: the mechanism works on the ledger, the bytes are not right (0x3980), and the frame rate did not move. Not adopted.**

#### Where the writes and the copies are (`HL_RB_EARLY_LOG=1` with `RX_VK_TL_S`; the `rbe-*` lines)

`rbe-end`/`rbe-note`/`rbe-tag`/`rbe-seg`/`rbe-read` give, per recording number, the small notes (kind 3: a binding bound for writing, 0: a
write-back copy, 1: in place), the parallel replay's segments and the reads with the number they waited for. Castle-move, PC 3050:

- The meter's 0x1040 (three buffers), the 0x3980 block and ~24 of the 0x40 rings are bound for writing in the recordings that go to the
  queue ahead of the big command buffer's segments (`r`, submitted with `submit_recording(r, false)` and **no write-back at its end**);
  the segments (a few 0x4, 0xc0 and 0x5b40 notes carry a segment tag) and the last recording bind only some more 0x40 and 0xc0 words.
- Every copy of them (`k0` notes: 0x1040 x3, 0x2000, 0x3980, 0x40 x27-29, 0x1000 x18, ...) is made at the end of the LAST recording of the
  submission, which follows the heavy recording, its segments, by 12-15 ms of GPU time at 21 fps: that recording is the carrier every read
  waits for. The GPU time between a ring's last writing pass and its copy is the whole big command buffer.

#### The design (`HL_RB_EARLY=1`)

Two places copy a small result back at the end of the recording that holds its last write so far, with the same copy as before (cut at the
CPU's own words, the lazy rule, the diff shadows told with `shadow_forget`), noted in the byte ledger and removed from the submission's list:
1. `memory_early_flush(r.cmd)` before `submit_recording(r, false)` in the three parallel-replay paths: what the write-back list holds, small
   (`HL_RB_EARLY_MAX_KB`, 16), in a pool with an import and in pages the CPU has read (`rb_pages`).
2. `memory_seg_early_begin/_end` around a segment's recording (`record_job`, both paths): the small CPU-read writes bound in the segment are
   copied at the segment's end, and the segment is submitted with a recording number of its own (`seg_early_number`: `++rec_seq` in the
   queue's order under `dev.present_mu`, signalled on the recordings' timeline, notes sealed by tag: `BasicLedger::seal_tag`).
Unit tests: `tests/rb_exact_test.cpp` (tagged notes sealed out of order, 560,000 reads against brute force, 0 mismatches) and the wiring
checks in `tests/test_rb_exact.py`. A recording recorded whole with no segments (the serial path) is not touched: the cut after the last
writing pass (`split_part`) was not built.

#### What the PC 3050 said

- Check mode (`HL_RB_EARLY=1 HL_RB_EXACT_CHECK=1`, castle-move, build hle4c): **792 compared, 224 differed**; every difference logged (the first
  16) is the 0x3980 block, 749-818 words, values within ~3% of each other (`31f99d53` against `320452b3`): the early copy holds the block
  as it was when the recording ended, the whole submission leaves more in it. The meter's and the rings' differences were not counted by size
  (the log keeps 16 lines); none of the 16 is theirs. Cause not found (candidates: writes through a binding noted before the copy and used
  after it, the CPU-word cut made at another time). **This alone stops adoption.**
- Reads: `rb exact all` of an A/B arm: 43.7 reads a frame, 26.9 pending by pages, **23.3 already carried by done recordings** (HLE3: 0.00),
  1.38 still waited for a recording (10.1 ms a frame summed over threads).
- Speed (castle-move, whole machine under a speed lease, `--slot b`, build hle4c, one throwaway then base, early, early, base; the PC ran
  slow in this window, 43-47 ms frames, 21 fps, where HLE3 saw a 27 ms chain; the base arms had `RX_VK_RB_PROF` on, a few timestamps a read):
  fps base 21.3, 21.2; early 21.4, 22.4 (p50 46.7, 46.9 against 40.2, 45.4 ms; p99 95, 91 against 139, 93): inside the spread, **no gain
  shown**. Reads in the early arm (`rb exact all`): 43.7 and 44 reads a frame, 26.9 and 24.2 pending by pages, **23.3 and 21.1 already
  carried by done recordings** (HLE3: 0.00), 1.4 waited a recording each (10-11 ms a frame summed over threads). Base arm (default waits): 300-450 of
  3,000 reads waited, 2-7 ms each. Why fewer waits did not move the frame: in these runs the GPU is the long pole (the heavy recording
  is 12-15 ms of GPU time and the frame is 43 ms), and the reads that still wait are the late 0x40 ring (102dd34020, bound 22 times in the last recording).

#### Not verified

Pictures were taken in the check run only (RX_VK_SHOTS in `hle4-c1`); kokiri-idle and hyrule-walk were not run for this switch; the A/B has
too few rounds (the speed lease allowed 5 runs); the serial (whole-recording) path, where the big write-backs of the 3050's frame are
recorded, is not covered; RADV, AMD, Intel not run.


### From stream/r2-readback (7283f4204)

### Fourth session (2026-11-15/16, main PC): the write-backs on the compute-family queue (`RX_VK_WB_COMPUTE`, default 0)

#### What was built
- `RX_VK_WB_COMPUTE=1`: a queue of the compute-only family (`vk_main.cpp`, `dev.wb_queue`), buffers made while it is set shared by both
  families (`buffer_share`: concurrent sharing, so no ownership transfers), a command buffer of that family per recording
  (`Recording::wb_cmd`), and in `memory_end_submission` the copies of what *already sent* recordings wrote (writers tagged per write-back by
  the command buffer of the recording that bound the range, `draw_recording_cmd()`) recorded there; the copies of what the recording's own work
  wrote stay in its own command buffer. Every submit on the replay's queue signals `gfx_tl` with a number; the copies' submit waits for the
  numbers of their writers, signals `wb_tl` (its own counter) and `wb_done_tl` (the recordings' timeline values, "copies landed"); a later
  submit on the replay's queue waits for `wb_tl` only if the copies are not done. `completed()` is the minimum of `timeline` and `wb_done_tl`.
  Readbacks of pages whose last write-back was a compute copy wait for `wb_done_tl` alone (`memory_writeback_need`), 2 ms at most, then as before.
- `=2`: the queue and the shared buffers only, copies where they were (a measurement). `RX_VK_TL_WB=1` (with the timeline): the small write-back
  runs, the ones left in the recording's own buffer (`wb-own`) and the compute submits (`wb-submit`, `wb-writer`). Refused with `RX_VK_POOL_MIRROR`.

#### Findings (castle-move, RTX 5070, whole machine)
1. **The meter, the rings and the event words are written by the heavy submission's last recording's own work**, not by the pieces before it: the
   earlier pieces (render passes) register no buffer write-back; the last piece (0.03 ms of GPU work: dispatches, the final event signal) registers
   all of them (`wb-own`: the meter's 0x1040 ranges, the 0x2000 rings, the event page). That recording is the one the graphics queue holds.
   So the compute copy has nothing earlier to wait for: the hold only moves.
2. **Making the compute queue wait for that held piece makes the frame slower**: first build (all copies on compute, awaiting their writers
   including the last recording): castle-move 100.0 -> 77.3 fps (3 against 3: 99.9, 100.2, 99.9 against 78.7, 76.3, 76.8). The Presentation
   Thread's `vkQueuePresentKHR` (under `dev.present_mu`) then blocks 4-5 ms, until the held piece runs, and every readback's flush waits for that
   lock: `RX_VK_RB_PROF` shows the late-fix wait (1.0-1.2 ms a frame) gone and a lock wait of 1.8-2.0 ms in its place (2.4-2.7 ms a frame of readback
   wait without the switch, 2.0-2.2 with it). The late start of the last piece is unchanged (3.6-4.2 ms after its submit).
3. Isolated: queue + shared buffers only (`=2`): 98.2, 97.1 against 98.3, 100.0, no cost. The copies not waiting for the last recording's own
   work (a diagnostic, wrong picture): 100.2 fps; no waits at all: 106.2 (+7%, the bound for this route).
4. Final build (copies of earlier writers only; here none exist, so only signals go through the compute queue), castle-move x3 interleaved: base 95.7,
   93.0, 92.6 (mean 93.8), switch 77.1, 78.2, 72.0 (mean 75.8): **-19%**. Readback wait 3.7-3.8 ms a frame (the wait moved to the full timeline).
5. Picture with the switch on: frame log 1,859 frames, luma mean 41.21 (range 28.6-52.7, largest step 0.77, 0 pulses) against 40.66 (28.8-54.0, 0.92,
   0 pulses) without; `valid.sh` three scenes ran (3,342 / 3,287 / 3,121 frames, no crash, ratio 60.4 / 60.0 / 59.7), last shots looked at by eye: right.

Result: the hypothesis does not hold for this game: the readbacks wait for the graphics queue's hold because what they read is produced by work inside
the hold, and a cross-queue wait on that work costs more than it saves. Not built / not verified: running the last piece's dispatches themselves on the
compute queue (they share the recording's state with the draws); the PC 3050; the picture over long runs (one frame log per arm).

#### Hand-over (fourth session)
Branch `stream/r2-readback`. Scratch under `E:\OOT\scratch-from-c\r2d` (`ab.sh` interleaved runs waiting for builds, `tl2.sh`, `final.sh`). The switch is
off by default and changes nothing then (except that `memory_end_submission` returns a bool and `WriteBack` carries a command buffer).


### From perf/meter-clear (98961ce6d)

### RX_VK_METER_CLEAR: the sky sums zeroed on the GPU (stream E1, 2026-11-16)

Ported from ProjectReLink (`gpu/vk/vk_draw.cpp`, "sky_sums", commit 433d229 by pixl27): right before each dispatch of the compute
programs `499dbfd0721d2858` (sky probe sums, 0x40 bytes at c1[0] of the block at c0+0x20) and `ab7699ee7f536755` (RGBL sums, 0x10
bytes at +0x80 of the block at c0+0x30) the block is zeroed with `vkCmdFillBuffer` plus a barrier, so a CPU reset or a mirror sync of the
page that lands between the game's clear (`6ccba775ea111ee9`) and the adds cannot double the frame's sums. ReLink runs without the
readback and capture waits by default (opt-in there: 59.0 against 47.5 fps in the village). Switch `RX_VK_METER_CLEAR=1` (default off;
`=2` also logs the compute programs seen). Test: `tests/test_meter_clear.py`.

Measured on the main PC (whole machine held, slot a, build e1 = main cb087555 + the clear, frame logs at 320x180, last 30% of each log,
a pulse = a run of frames more than 3 luma off the median of the 121 around it; kokiri-idle here is the castle-gate view after the load).
Many runs did not leave the load screen (0 frames; the known flake, in every arm, and worse with a frame log): few valid runs per arm.

| arm | valid runs, fps | luma mean | pulses (peak, max frame step) |
|---|---|---:|---|
| a: default (waits on) | 91.7, 89.4, 89.5 | 65.3-65.9 | 0 (0.1) |
| b: `RX_VK_NO_READBACK_WAIT=1` | 94.2, 90.0, 84.8 | 71.7 | 228-267 (peak 28-31, step 25-33) |
| c: b + `RX_VK_METER_CLEAR=1` | 89.9 (clean run), others cut by a load | 69.6 | 182 (peak 13, step 3.9) |

castle-move (a moving view: the pulse count is not clean there): a 90.6 / 86.2, b 89.1, c 94.6 / 65.1 (cut); pulses a 18-45, b 127,
c 33-106. Verdict: the clear works (it ran about 2 zeroings a frame, the blocks bound, window 0xc0) and cuts the size of the
pulses (peak 28 -> 13, frame step 25 -> 4 luma) but does not remove them: 182 against 0 with the waits. So it is NOT on by default
with the waits off, and the waits stay. Waits off gave no fps gain in these runs on this build (b ~ a, noise +-4%), where the older
note measured +17% (main now has early GPU work). What else the meter reads is not found: the other ranges the CPU reads
(0x1040 x3 from the ring helper f_80357060) are not the blocks these two programs write; a probe binding-size log found none. Next:
find the programs that write the 0x1040 blocks (RX_VK_LOG_WRITTEN is capped at 10 lines; widen it), and zero or version those too.
