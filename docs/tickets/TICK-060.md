# TICK-060 — PC 3050: cut the frame time without lowering quality · model: Opus 5.5 · after TICK-053/057/058
**Why**: PC 3050 (i5-10400F 6C/12T, RTX 3050 8 GB on PCIe 3.0 x4) runs the Market at ~28-29 fps with the GPU idle a third of
the frame (60-69%). TICK-053 found the frame serial (the late-page wait); TICK-057 found no engine GPU cut that moves fps.
**Goal**: where the GPU's idle third goes (without PASS_TIMING's serialisation where possible), then quality-preserving cuts, each
behind an env switch, off until proven; picture identical; the original behaviour kept.
**Branch**: `feature/TICK-060-pc3050-gpu` (from main af1cc93). Local only; built and measured on PC 3050 (session 0 over SSH);
bundle `E:\OOT\tick060.bundle`. Details and every table: `docs/benchmarks/2026-11-12-tick060-pc3050.md`.

## Result (2026-11-12): +18-21% on PC 3050, picture unchanged; adopted (on by default)

The GPU idled a third of the frame because the CPU stopped recording while it waited ~8 ms for the previous frame's late pages,
then needed ~6 ms of mirror pre-sync (mostly re-hashing capture buffers for TICK-048's chunk diff) before the GPU had any of the new
frame. `RX_VK_LATE_DEFER` moves that wait from the capture's take to the queue (the GPU reads the same bytes; the CPU records
meanwhile) and `RX_VK_CAPTURE_HASH` makes the diff's hashes in the capture's own copy; with TICK-056's `RX_VK_MSYNC_FAST`:

| scene (confirm: 6 rounds, 4-6 reversed, one binary) | base fps / 1% low / p99 | new | gain | rounds above base |
|---|---|---|---:|---:|
| kokiri-idle | 30.85 / 24.5 / 38.9 | **37.24** / 26.4 / 33.6 | +6.4 (+20.7%) | 6 of 6 |
| castle-move | 29.13 / 14.7 / 54.2 | **34.40** / 16.9 / 47.0 | +5.3 (+18.1%) | 6 of 6 |
| hyrule-walk | 31.35 / 19.5 / 45.1 | **37.20** / 25.9 / 36.2 | +5.9 (+18.7%) | 6 of 6 |

GPU utilisation 62-72% -> 73-83%, the queue's gaps 8-13 -> 4-7 ms a frame, the late wait 6-8.5 -> 0 ms. Shots alike standing and
walking (Market luma 103.7 / 103.5 base, 103.3 / 102.6 new; no white or black frame in play), `HOS_SPEED_LOG` median 60.00-60.07.
**Adopted**: `RX_VK_LATE_DEFER` and `RX_VK_CAPTURE_HASH` on by default (=0 off); `RX_VK_MSYNC_FAST` on by default where the
chunk diff is on (PCIe links of 8 GB/s or less). **Main-PC check needed** (not run from this ticket): there the diff, the hashes and
MSYNC_FAST stay off; only the deferral is new, and it acts only when a take's late value is not reached yet.
Game folder on PC 3050: build `t60e` (exe 17c3e4fb, dll 2dc76b85); main32d kept in `C:\OOT\builds\main32d`.

## Where the frame went (base, castle-move, unprofiled)
- Frame 35.3 ms (trace on); GPU: recordings 19.7 + uploads 2.8, **idle 12.8** between recordings (one long gap a frame, 10-17 ms);
  nvidia-smi 60%, PCIe rx 0.54 GB/s of 3.9 (the link is not the limit).
- The loop (job trace): held flush 11.5 ms, of it **8.1 the late-page wait** (~100 Market pages a frame the previous frame's last
  submission writes back), holding the replay's lock; then the Presentation Thread's 23 ms of recording, of which **5.8 the mirror
  pre-sync** before the frame's first big command buffer reaches the GPU (~4 ms of it TICK-048's chunk diff re-hashing 37 MB of
  capture buffers from DRAM); present 2.2.
- So the GPU idles because the CPU does not record while it waits for the GPU, and then needs ~10 ms before the GPU has the new
  frame's work: not the uploads, not PCIe. The GPU's own time (`RX_VK_PASS_TIMING`, serialised): the game's passes 13.4 and
  dispatches 4.6 of 26 ms profiled (top: pass `1a7482c0ec9991ea` 2560x1440 1.08 ms, upscaler dispatch `9dc98871eb7fc101` 0.52).

## Built (vk_memory.cpp, vk_replay.cpp, vk_texture.cpp, msync_diff.h, vk_internal.h)
- **`RX_VK_LATE_DEFER`**: a capture whose late value is already sent to the queue is taken without waiting; its late slots are
  fixed (the same copies, from guest memory, at the value's end) by a fix thread, or by any submit or flush first; recordings are
  held until then (RX_VK_BATCH's pending list); waits for a fix happen outside the queue's lock, polls never wait; a replay read of
  a pending slot waits and reads guest memory; the mirror sync's diff copies such a page whole. The GPU reads the same bytes.
- **`RX_VK_CAPTURE_HASH`**: the capture's copy makes TICK-048's chunk hashes in the same pass (`msync_diff::copy_hash_page`,
  bit-exact with `page_hash`); the mirror sync takes them instead of re-reading the capture buffer. Same hashes, same copies.
  Only where the diff is on (slow links) and with AVX2.
- `RX_VK_MSYNC_FAST` (TICK-056, unchanged code) measured with them.
- Diagnostics: trace column `late_fix_ms`, marks 38/39, log lines `late pages deferred`, `late fixes`, `capture hashes`.
- Tests: `tests/test_late_defer.py`, `tests/test_capture_hash.py` (new), `tests/test_msync_diff.cpp` (copy_hash_page),
  `tests/test_late_merge.py` (updated needle).

## Measured (PC 3050, castle-move unless said)
- Traces (one run each): base 28.27 fps, gap 12.8 ms, GPU 60%; LATE_DEFER 30.12, gap 10.7, the held flush 11.5 -> 4.6 ms; all three
  33.12, gap 7.8, GPU 71%, the pre-sync 5.8 -> 1.6 ms (105k pages looked at -> 9k; 1386 slot hashes taken a submit, 17 made again).
- The first LATE_DEFER try waited for a fix under the queue's lock in the main thread's fence poll: the Presentation Thread's
  held submits waited behind it (7.5 ms a frame, +1.8 fps only); fixed (waits before the lock, polls never wait). A textured pool's
  late page at first made every Market take wait (the texture-shadow rule was the pool's, not the texture's storage): narrowed
  to a live texture's storage.
- Screen 1 (3 rounds): base 28.09, LATE_DEFER 29.80, + CAPTURE_HASH 33.19, + MSYNC_FAST 33.83 (every run above every base run).
  Screen 2 (against the three): + `RX_VK_SEG_EARLY=1` 34.11, + `RX_VK_PARALLEL=5` 34.19 against 33.18: inside the spread, not taken.
- Shots (`sh60-*`, every 300 presents, standing and walking): same sequence and Market; luma above; speed ~60.
- Confirm (36 runs): the table above; per-round fps / 1% low / p99 in the benchmark doc.
- The default build (`t60e`, no environment): 36.08 / 33.40 fps standing / walking, switches named on in the log, speed 60.02 / 60.00.

## Correctness
- The GPU reads the same bytes: every submission that may read a capture buffer goes to the queue after its pending fixes (the
  same copies, from guest memory, at the late value's end, as the take's wait made them); the CPU's readers of a pending slot wait
  (a replay read as submitted) or copy the page whole without recording a hash (the mirror sync). Same hashes with CAPTURE_HASH
  (bit-exact, tested), so the same chunks are copied.
- Saves hash-equal to `C:\OOT\user-save-backup` before and after every run (~60 runs) and at the end; no HeyListen.exe left;
  no hang; every run loaded the title -> Market.
- `run_all` (llvm-mingw on PATH): 245 pass, 2 fail (the nup tests, known), 35 skip.

## Not verified
- The main PC (to be checked there), the interactive desktop (session 0 only), other GPUs, frame generation on, a scene change
  by walking, `RX_VK_ASYNC=1`, Vulkan validation of the held recordings (the same command buffers, sent later in the same order).
- A texture created over a pending late page and uploaded before the fix would read guest memory (none seen: 0 such pages).

## Next lever
- The frame is now the CPU's recording chain (GPU 73-83%, gaps 4-7 ms): the Presentation Thread's own segments (2 x ~4-5 ms),
  the held flush's capture copy (3.1 ms, memory-bound), the dry passes. `RX_VK_SEG_EARLY` and `RX_VK_PARALLEL=5` gave +0.9-1.0
  fps in a screen (confirm both on PC 3050, and the main PC).
- Then the GPU: ~22 ms of busy time a frame at 1440p (the game's passes and compute); at ~43 fps the GPU becomes the limit.
- The trace's gpu_ms misses SEG_EARLY's early segments (no timestamps there): add them before measuring it.

## Main-PC check (TICK-060m, 2026-11-11/12): review, fixes, A/B on the Ryzen 7 7800X3D + RTX 5070
Branch `feature/TICK-060-main` (local main 43cc30f + this branch, worktree `HeyListen-wt/TICK-060m`); details and every table:
`docs/benchmarks/2026-11-12-tick060-pc3050.md`, section 9.

### Review of RX_VK_LATE_DEFER and RX_VK_CAPTURE_HASH
- **Held recordings vs their fix: one real bug, fixed.** A capture's late value is the newest write-back of *its own* late pages, so
  the next capture's can be lower (its late pages written back by an older submission still on the GPU). The fixes' queue assumed
  rising values: `memory_late_fix_apply` applied from the front only and set `late_fix_top` to the *back's* value. With fixes
  [10, 8] pending, the top was 8: a flush (`late_fix_flush_locked`) waited for 8, applied nothing (the front, 10, not reached) and
  sent the held recordings that read the unfixed late slots of the first capture; the GPU could then pass that capture's
  submission and `take_capture` retire its buffer into `cap_free` while the fix still pointed at it (a later fix would write into
  another capture's buffer). Now every fix whose value is reached is applied, in any position, and the top is the highest pending
  value (`late_fix_push` only raises it). Not seen on the main PC (the new log count "fixes below a pending one's value" was 0 in
  every run read), so reachable but rare here; the PC 3050 numbers do not depend on it.
- **Every path to the queue applies the fixes first.** The batched flush, the direct submit (after flushing the held ones), the
  parallel/early segment submits, the early pre-sync, the readback submit, the sparse binds (textures, and now the pool import,
  which also waits outside the lock), the present: all go through `queue_flush_locked` (wait for the top, apply) under
  `dev.present_mu`; `tests/test_late_defer.py` checks every `vkQueueSubmit2`/`vkQueueBindSparse` on `dev.queue`. No submission waits
  on the timeline, so a sent value is reached without the CPU (no deadlock in the waits under locks). A capture buffer under a
  pending fix cannot be retired: its submission's value is held behind the fix.
- **Fence polls (TICK-058's failure): nothing similar.** LATE_DEFER does not touch `sync_order_mu` or the poll's locked path. A
  poll neither waits nor flushes while a fix is pending; the held recordings' timeline and recordings'-timeline signals are not
  sent, so a poll can only answer "not yet" for them (as base, whose replay had not even recorded them, stopped at the take).
  Write-back registrations are made while recording, before the hold; a readback (`invalidate`) waits with a timeout, so it waits
  for the fix, sends the held work and then waits for the write-back value: it never reads guest memory before the GPU's results.
  A liveness nit fixed: the fix thread could reset its wake value while a fix registered during its pass was pending (only delayed
  that fix to the next submit or flush).
- **Textures: reachable, fixed.** A deferred take gives no texture shadow of its late slots. A texture the host creates over such a
  page after the take (so no live texture's storage at the take), uploaded by the CPU path (`guest_bytes`) in a later submission
  before the fix, read guest memory possibly *before the write-back landed* (base read the take's shadow, after the write-back):
  other bytes. Now `guest_bytes` waits for the value of any pending late page in its range and applies the fixes
  (`memory_late_fix_wait_range`; log: "texture storage reads (waited)", 0 on the main PC). The GPU unswizzle path reads guest memory
  on the GPU after the fix: fine as it was.
- **CPU reads as submitted: tightened.** `memory_read_as_submitted` read guest memory after the value, while the GPU reads the slot
  the fix copies at another moment; the game's CPU rewrites such pages soon after the write-back (OoT's exposure page), so the two
  could differ. Now the reader waits, applies the fixes and reads the slot (guest memory only past the 2 s timeouts). The copies are
  also made as soon as the value is reached, before the queue's lock (fix thread, `late_fix_wait_unlocked`), as the take's wait
  made them, not after another thread's submit. (Reachable: 4 such reads in 25 main-PC runs; 0 on PC 3050.)
- **Hashes: valid on reuse; no issue.** Each kept capture has its own arrays (`CapBuf::hash`); `hok` is cleared at every
  `capture_make` and set only for a slot the one-pass copy read whole; the late slots lose theirs (waited or deferred); a pending
  late slot is synced whole with its diff record voided (`d_void`); retired arrays are recycled by swap and come back with a fresh
  `hok`; the pre-copy path makes none; slots are written only by the copy and the late copies. On the main PC the diff, hence the
  hashes and MSYNC_FAST, are off (log: Gen5 x16, 63 GB/s, "chunk diff off by default").
- Tests: `tests/test_late_fix_order.py` (new: the queue cut out and compiled; fails on the TICK-060 code: top 8 instead of 10),
  `tests/test_late_defer.py` (needles updated, the sparse bind added). `run_all`: 247 pass, 2 fail (the nup tests, known), 35 skip.

### A/B on the main PC (one binary, defaults = new against `RX_VK_LATE_DEFER=0 RX_VK_CAPTURE_HASH=0` = base)
- Log: PCIe Gen5 x16 (63 GB/s), "chunk diff off by default": CAPTURE_HASH and MSYNC_FAST off in both; only the deferral differs.
  Every take that would have waited was deferred (~95 late pages each; 1 waited for a texture's storage in 25 runs), the fix
  thread applied them all.
- Screen (castle-move x3): base 75.65, new **77.54** fps (every new run above every base run); frame prep 7.88 -> 6.69.
- Shots (every 300 presents) standing and walking: the same Market, exposure as base (standing luma 103.2 / 101.8, walking 93.3 /
  97.1, the same walk into the stalls' shade), no white or black frame; `HOS_SPEED_LOG` median 60.00 / 59.98 (new standing / walking).
- Confirm (6 rounds, 4-6 reversed): kokiri-idle 78.3 -> 79.4, castle-move 77.0 -> 76.9, hyrule-walk 75.1 -> 77.4 fps; 1% low and
  p99 medians alike; 10 of 18 rounds above base; all inside the spread. **Does not lose** (the bar for the main PC).
- Saves identical to `user-save-backup-1111-2131` after all 46 runs; no game left; game folder restored after each lock session.

### Recommendation
Merge with the defaults on (LATE_DEFER on everywhere; CAPTURE_HASH and MSYNC_FAST follow the chunk diff), **with this branch's
fixes** (the non-rising late values above all: without it a flush can send recordings that read unfixed slots). No gate by link
speed: on the main PC the deferral costs nothing measurable and steadies the runs; its gain does not come from the link but from
the CPU recording while the GPU finishes. Not verified: PC 3050 with the review's fixes (they only copy sooner and apply more
fixes at once; the fast path is unchanged), Vulkan validation of the held recordings, frame generation on, a scene change by
walking.
