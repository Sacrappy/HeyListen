# Area C: the NVN2 HLE layer and the submit-time capture (read-only audit)

Scope: `engine/host/nvn_native.c`, `nvn_layout.c`, `nvdrv.c`, `host/rx_backend.c` (collect_in), `gpu/vk/vk_memory.cpp`
(capture_make, take_capture, copy helpers, pre-copy, stamps, shadows), `capture_merge.h`, `msync_diff.h`, and the submit entry
parts of `vk_replay.cpp`. Nothing was built or run. "measured" cites a file; everything else is "estimated" from the code.

## 0. Answers to the three questions

1. **NVN calls per frame and cost.** No call count was ever measured (`HOS_HOSTCALL_STATS` exists, unused in the docs).
   What is measured: ~150-200 recordings (`EndRecording`) a frame (1.2 M recordings over ~8,700 presents,
   `build/bench/default-hyrule/game_log.txt`, "recordings made" lines) and 1,700-2,050 draws a frame. At ~8-12 calls a draw
   that is estimated at 15-25 k NVN calls a frame. The entry path is cheap: the lifted code calls the proc through a cached
   function pointer (`nvn2_dispatch`, `hos_hostcall.c` `a64_lookup_host`, no counters unless `HOS_HOSTCALL_STATS`), `P(i)` is an
   inline range check, a state setter or draw is `cmd_host` (lock-free slot lookup) + `cmd_alloc` + a `memset` of the payload.
   **There is no per-call lock, map lookup or allocation on the command-building path.** Locks exist only per recording
   (`pool_get`/`flatten`, `malloc(rec_node)`, `copy_lock`, `live_handle_add`: ~2 exclusive SRW acquires + one CRT malloc), and
   at 150-200 recordings a frame that is well under 0.2 ms spread over all threads. Not a suspect.
2. **Is the ~30 MB copy necessary in full? Dirty tracking?** It already is dirty-tracked: `collect_in` (rx_backend.c:72)
   reads and clears the lifted code's per-page write marks (`A64_MARK`) over the mirrored/texture/small-pool ranges
   (1,479 ranges, 3.4 GB), and only marked pages are copied (1,860-2,050 pages = 7.3-8.0 MB per submit, 6 submits a frame,
   "pages written = copied", `build/bench/sync10-final-shots/game_log.txt`). The granularity is the page. What is not
   exploited: **content**. TICK-048 measured on the same Market data that 47% of the whole pages a sync copies and 85% of their
   256-byte chunks equal what the mirror already holds (perf-audit.md:2508-2525; PC 3050). So roughly half the copied pages
   rewrite their own bytes. See C4.
3. **Per-call locks, map lookups, allocations?** On the NVN builders: no (above). On the capture path: yes, per page.
   `take_capture` does two `upper_bound` searches and an `unordered_map` erase/insert plus a `new[4096]` per texture page
   (C3), and `collect_in` has a per-range slow path (C2). Those are the real finds.

Context that matters for ranking: with `HOS_NVN_HOLD=2` (default since TICK-054) the frame is one serial loop and the held
flush (capture 1.36 ms + memory work 0.6 ms = 2.1 ms) is directly in it (perf-audit.md:3040-3052). The same copy costs 0.6 ms
on an idle main thread, so ~0.75 ms of the 1.36 is contention with the job group that runs beside it.

## 1. Ranked table

Gain is ms of frame time, not summed CPU. B = Presentation Thread chain / held flush, A = the game's chain.

| id | location | what costs time and why | chain | est. gain ms/frame | evidence | risk | effort | how to verify |
|---|---|---|---|---:|---|---|---|---|
| C1 | vk_memory.cpp:1450-1485 `copy_pages`, `copy_helper_main` | The capture is cut statically into k=5 equal parts; the submitter copies part 0 and then idles in `done.wait` for helpers that may not be scheduled yet while the job group has every core busy. Idle submitter + a late helper = the copy ends at the slowest part. Held copy is 1.36 ms vs 0.6 ms on an idle machine. | B (held flush, in the serial loop) | 0.2-0.5 | measured: 1.36 vs 0.6 (perf-audit.md:3040, 3052); p90 1.2 ms per capture (:3064); `RX_VK_CAPTURE_PRIO` gave +1.2 in one screen, 0 in another (:3068) = scheduling matters | low (same bytes, same `ok[]`) | S | held-flush "capture made" in `chainb.py`; `vk: capture: a submit ... copy` log |
| C2 | rx_backend.c:72-111 `collect_in` | Per-range head/tail walk: ranges are rarely 256-page aligned, so each of ~1,479 (capture) / ~764 (collect) ranges falls through the 8/32/256 ladder into up to ~17 single-page atomic loads at both ends (~50 ns a range). Scan is 0.05-0.07 ms for only ~830 KB of marks. Done twice a submit (capture + `memory_collect_written`), 6 submits a frame. Fix: unaligned 32/64-byte loads for heads/tails (marks are bytes, no alignment need). Same pages, same order. | B (held 2 + PT 4 submits), A small | 0.2-0.4 | measured: scan 0.07 ms over 1,479 ranges, collect 0.03-0.12 ms per submit (sync10-final-shots log); the 256-page skip alone was worth ~+1.5 fps (perf-audit.md:672-680); the head/tail overhead is estimated from the code | low | S | `vk: capture: a submit ... scan` and `a submit's memory work ... writes collected` log lines; unit test equal page lists |
| C3 | vk_memory.cpp:2193-2228 `take_capture` texture-shadow block | For every page the capture took: `in_tex_merged` binary search (twice: erase pass and insert pass), `shadow.erase` (hash), `shadow[p]` (hash) + `new uint8_t[4096]` + 4 KB memcpy of a cold capture page, `shadow_regions` map update; all under `pools_mu` + `shadow_mu`. "retires and stamps" is 0.06-0.11 ms a take, 6 takes a frame; `stamp_page` itself is ~10 us of that. | B (4 PT takes + 2 held) | 0.2-0.4 | measured: "retires and stamps 0.060-0.111" ms a take, 5.6-15.6 k shadow pages (sync10-final-shots log); TICK-043 had "~0.1-0.3 ms walking" for this; the split inside is estimated | low | S-M | log line `a capture taken (ms): ... retires and stamps`; shadow bytes identical (test: compare shadow after N takes) |
| C4 | `capture_make` copy stage (new) | Hash each source page at capture time (reads it anyway), compare with a per-page hash table; a page whose bytes equal its last captured version keeps its old `cap_entry`/stamp and is neither copied nor re-stamped. Cuts copy bytes, DRAM traffic the job threads feel (TICK-037/043), `take_capture` work and the mirror pre-sync's dirty pages (105 k pages looked at for 8.9 k dirty). Must keep the late-page and GPU-write rules (TICK-048's epochs, `diff_gpu_wrote`): a CPU rewrite of identical bytes still has to overwrite a GPU-modified page (the exposure meter). | B and A (less memory traffic) | 0.3-0.7 | measured: 47% of copied pages / 85% of 256 B chunks unchanged vs last sync (perf-audit.md:2518, PC 3050, Market); 0.0% of pages repeat between consecutive captures as page lists (:1261); hashing cost and the traffic share are estimated | **high** (stale/overwritten GPU data, exposure flashes; needs per-mirror reasoning, not per-capture) | L | `RX_VK_MSYNC_STATS` plus a new "unchanged at capture" counter first (diagnosis only); A/B on exposure scenes (Kokiri), flash detector |
| C5 | `capture_make` ranges | Do not capture pages that no sync can ever read: TICK-037 found only ~75% of the capture is ever synced (~7 MB of 30 MB a frame). Needs a per-pool "ever bound by a replayed command" flag; pools are imported two presents behind the game, which is why the ranges are over-inclusive on purpose (the comment at vk_memory.cpp:1013-1028 on the flashes). | B | 0.15-0.3 | measured: 75% synced (perf-audit.md:1262; TICK-043 "what is next" #1 ~0.25 ms) | med-high (the flashes of newly imported pools) | M-L | counter: captured pages per pool vs pages ever synced from them |
| C6 | `copy_helper_count()` vk_memory.cpp:1473, `hos_thread_pin` | In hold mode the copy runs beside 6 executors + 5 module workers. 4 unpinned helpers + the held thread (pinned to the reserved core by `ReplayPin`) compete with them; TICK-037 found 8 helpers = 4 (bandwidth-bound). Try 2-3 helpers, or helpers pinned to the SMT siblings of the reserved core, so the copy stops taking executor time slices. Env only first. | A (job group) and B | 0.0-0.3 | estimated; TICK-037 (helpers 4 vs 8 equal), TICK-054 priorities mixed | low | S | `RX_VK_CAPTURE_THREADS=2/3` interleaved A/B on castle-move; frameline.py job-group length |
| C7 | rx_backend.c:114-119 + vk_replay.cpp:2536 | The capture scan and `memory_collect_written`'s scan read the same marks back to back on the same thread (held thread and PT). One pass over the union, split by the mirrored-range test, would save one traversal and one set of cross-core mark-line fetches per submit. Careful: a page written between the two passes is deliberately left for the next capture. | B | 0.1-0.25 | measured: 0.07 + 0.03-0.12 ms per submit; saving is estimated | med (the "written after the capture" rule) | M | same log lines; page lists identical in a shadow comparison build |
| C8 | nvn_native.c:2425 `resolve_deferred`, 2457 `check_handle` | Each submit walks every record of each handle once to patch deferred binds, then the replay walks it again. A list of the deferred-bind record offsets kept in the recording (built in `bind_handles`/`bind_bufs_deferred`) makes this O(binds). | A (main thread's 2 held submits, before the hold) | 0.02-0.08 | estimated: ~18 k records a frame x ~2.5 ns | low | S | `JT_MARK` 1->3 gap on the main thread (hold mark 3 minus the call start) |
| C9 | nvdrv.c:176 `hos_gpu_guest_ranges` (and vk_memory.cpp:971 `refresh_pool_ranges_locked`) | Ranges merged with a 16 MB slack (legacy of `GetWriteWatch`): the scans read the gaps and `memory_collect_written` stamps and range-tests every CPU-only dirty page in them (`upper_bound` + `stamp_page` per page). Use exact ranges for the Vulkan side (and keep the 16 MB merge off), or skip pages outside a tracked range early. | B small | 0.0-0.15 | estimated; count not measured | low-med (a page leaving the mark list changes nothing for the GPU) | S | counter: pages from `host->collect_written` that are in no capture/GPU range |
| C10 | vk_memory.cpp:924 `cap_buf_get` | A capture takes the best-fit spare; with 24 kept + 12 spares and a `made` counter that was non-zero after the load, a miss costs a `vkCreateBuffer` + VMA + first-touch faults (4 KB each, ~1-4 ms for 16 MB) on the submitting thread. | B | 0.0-0.2 | estimated; TICK-038 saw more buffers made when spares were aged | low | S | `vk: process memory ... N made ... of M asked` delta in steady state |
| C11 | nvn_native.c:1297-1345, 1650-1715 `pool_get`, `flatten`, `EndRecording` | Per recording: `pool_lock` x2, `malloc(rec_node)`, `copy_lock`, a live-handle insert, a guest write of the COPY_MARK (which also marks the control-memory page dirty, so it is captured). 150-200 a frame. | A (recording threads) | < 0.1 | measured count (log above), cost estimated | low | S | n/a, only if sampling shows `EndRecording` |
| C12 | nvn_native.c builders (`rec`, `cmd_alloc`, `watch_tex`, `watch_draw`) | `rec()` zeroes the payload after `cmd_alloc` already wrote a header and `rec` writes it again; `watch_tex`/`watch_draw` are non-inlined calls with a lazy-init static on every bind/draw. Per call ~3-6 ns of avoidable work x ~20 k calls. | A (whoever records; the serial module job if NVN-heavy) | 0.05-0.1 | estimated | low | S | `HOS_SAMPLE_ALL` share of `n_nvnCommandBuffer*` leaf samples on the job-path threads (never measured) |

## 2. The top five

**C1: the copy's join is a convoy.** `copy_pages` splits n pages into k = helpers+1 equal ranges, posts k-1 to a queue, copies
range 0 itself, then blocks until `pending == 0`. Any helper that is slow to get a core delays the end, while the submitter
sits idle holding the held flush (and so the Presentation Thread's first submit, which waits on `flush_cs`). The same copy took
0.6 ms with an idle machine and 1.36 ms beside the job group, p90 1.2 ms per capture; moving 4 KB runs around cannot explain that
for a bandwidth-bound loop, a late-scheduled part can. Change: split into ~16-32 chunks claimed from an atomic counter by every
participant, the submitter included, so it finishes whatever is unclaimed; the output (slot i = page i, `ok[]`) is identical.
Same API, no format change. Expected: the mean moves toward the bandwidth bound (~0.6-0.9 ms), i.e. 0.2-0.5 ms off the loop.
Verify with `chainb.py` "capture made" and the 600-submit copy time.

**C2: `collect_in` is limited by its range edges, not by data volume.** 830 KB of marks should scan in ~20-30 us at cache
speed, yet the scan takes 0.05-0.07 ms. With 1,479 ranges the unaligned head and tail of each (a 7/3/7-iteration ladder at both
ends, one atomic load per page below 8 pages) add up to ~50 us. Replace the heads/tails with unaligned 32-byte vector loads and
a masked tail; keep the exchange only on non-zero bytes. Same output, same order, same acquire semantics. It runs twice a submit
(the capture scan and `memory_collect_written`'s scan over 764 ranges) on 6 submits, of which 4 sit on the Presentation Thread
(chain B) and 2 on the held flush. Estimated 12 scans x 0.03-0.04 ms saved = 0.4 ms of CPU, 0.2-0.3 ms on chain B.

**C3: the texture shadow loop in `take_capture`.** `retires and stamps` is 0.06-0.11 ms a take of which the stamps are ~10 us.
The rest is the shadow block: for each of ~2,000 pages, `in_tex_merged` (two binary searches), then a hash erase, a hash insert,
a 4 KB allocation and a cold 4 KB copy for texture pages, all under `pools_mu` + `shadow_mu` (which also hold off the replay).
The page list is ascending, so a merge-walk against the sorted spans needs no search; a per-page "texture storage" bitmap kept
by `publish_tex_spans` makes it one load; the shadow itself can be a chunked flat table with a 4 KB slab (the capture table
already is, `cap_entry_set`). Contents of the shadow stay the same. Expected 0.03-0.07 ms a take, 0.2-0.4 ms of chain-B time.

**C4: do not copy a page that rewrote itself.** 47% of copied whole pages and 85% of chunks equal what their mirror held
(measured at the syncs). The hash is the same family as `msync_diff.h` (16 x 64-bit per page) and can be computed on the helper
threads that already read the page. Skipped pages keep their previous `CapEntry` and stamp, so downstream they simply are not
dirty: fewer copies, fewer stamps/shadows, a shorter mirror pre-sync, less memory traffic competing with the job group. The
hazard is the whole reason the capture exists plus TICK-053/048: a page identical to the CPU's previous bytes can still need
to overwrite what the GPU wrote (exposure meter, `diff_gpu_wrote`, `wb_pages`), and each mirror last-synced at its own time.
Do it only after a diagnosis build counts, per submit, pages unchanged vs the previous capture of that page and how many of those
have a GPU write noted since. If the count is ~half of 30 MB it is the largest item here; gain stays an estimate until then.

**C5/C6: smaller or friendlier copying.** C5 drops pages nothing can read (25%); it fights the "imported two presents late"
over-inclusion, so it needs per-pool read evidence first. C6 is an env experiment: the held flush runs beside the executors, and
4 free-floating helpers plus the pinned held thread are 5 more runnable threads on 16 logical CPUs; try `RX_VK_CAPTURE_THREADS`
2 and 3, and pinning helpers to the reserved core's siblings.

## 3. Checked and not suspects

- NVN builder path: no locks, maps or allocations per call (see 0.1). `nvn2_dispatch` caches the target; `hos_hostcall` counters
  are off by default (they were removed after costing two interlocked ops per call).
- `tex_copy` (FNV over the whole `nvn_tex` + exclusive `pin_lock`) runs only for texture copy/blit records, dozens a frame.
- `check_handle` takes a shared SRW lock per handle; uncontended.
- `stream_memcpy` per run: indirect call + SEH per 1-2 page run, ~50 us over 1,650 runs spread on 5 threads.
- Pre-copy (TICK-043), its kicked form (TICK-054), HOLD=3, priorities, SCAN_SPLIT: already tried, not re-proposed.
- `RX_VK_LATE_MERGE` / late-page wait: 0.004-0.06 ms a take typical (0.2-0.27 ms in spikes of ~8% of takes, sync10 log); on the
  user's not-paying list.
- `nvnBufferInvalidateMappedRange` -> `invalidate` (vk_replay.cpp:3143): can wait for the replay/GPU; TICK-058 already moved that wait.
- `hold_check` / `hos_hold_snapshot` is diagnosis only.

## 4. Measurements that would settle open points (none run here)

1. NVN call count and mix per frame: `HOS_HOSTCALL_STATS=1` for 10 s (it adds interlocked ops, so use it for counts, not timing),
   plus `HOS_SAMPLE_ALL` for the share of `n_nvn*` leaves on the job-path threads (decides whether C8/C11/C12 are worth doing).
2. Per-submit breakdown on the main PC with hold on: `vk: a submit's memory work`, `a capture taken`, `capture: a submit`
   (`copy`, `scan`), split by thread (the logs print it only per 600 submits from whichever thread crosses).
3. A diagnosis counter for C4: pages unchanged vs the page's previous capture, and how many had a GPU write noted since.
4. `made` vs `asked` in `vk: process memory` in steady state (C10).
