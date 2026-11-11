# Area E: guest memory to GPU memory (static analysis, read only)

Sources read: engine/gpu/vk/vk_memory.cpp (capture_make, take_capture, mirror_sync, rb refresh, write-back), vk_vma.cpp, vk_texture.cpp
(staging, texture_dirty, BC cache), engine/docs/perf-audit.md (TICK-037/043/048/051/053/054/056/057), docs/benchmarks/2026-11-12-tick053.md,
a64.h (A64_MARK). No game code or binary touched. "measured" = a number from a cited file; "estimated" = my reasoning.

## How the data moves today
1. Lifted code sets a 1-byte-per-4KB-page dirty flag on every non-stack store (A64_MARK: two release stores, first and last byte's page).
2. At each submit `capture_make` scans the marks (`collect_in`, 0.23 ms/frame, measured TICK-043), then copies every dirty page of the
   captured pools into a host-cached system-memory buffer (`create_buffer_cached`, vk_vma.cpp:252) with streaming stores on the
   submitter plus 4 helper threads: 1,200-2,000 pages a submit, 25-40 MB a frame, ~1.0-1.4 ms (measured TICK-043/054). 0% of a capture's
   pages were in the capture before it (TICK-037), but 85% of the bytes (256 B chunks) equal what the mirror already held (TICK-048).
3. `take_capture` (pools_mu held) stamps each page's generation, sets `cap_entry` per page, copies texture-storage pages into a 4 KB heap
   "shadow" per page (unordered_map), and waits for the GPU for "late pages" (0.11-0.22 ms/frame on the main PC, 8.8 ms on PC 3050, measured).
4. `mirror_sync` (per pool, under pools_mu, on the PT or segment workers) walks the pool's page range for pages newer than the pool's
   `synced_gen` and queues copies capture buffer -> VRAM mirror through one scatter dispatch (the GPU reads system RAM over PCIe).
   Pre-sync: 105,000 pages looked at for 8,900 dirty; 0.86 -> 0.45 ms with MSYNC_FAST (measured TICK-056; MSYNC_FAST is already tried).
5. The 256 B chunk diff is on only when pcie_link_gbps() <= 8 (PC 3050); off on the 5070 (hashing costs more than the bytes saved).
6. Readback pages (rbrefresh): ~540 whole pages a frame copied guest -> mirror. Write-backs: a shader writes only words that differ from a
   VRAM shadow (already tight: 14.6 MB compared, little copied, TICK-057).

Answers to the brief's questions:
- Unchanged bytes copied every frame: yes. ~85% of every sync's bytes are unchanged (TICK-048). The diff removes only the PCIe/GPU leg, and
  only on slow links. The CPU capture copy (1.0-1.4 ms) still moves every marked page whole.
- Granularity: marks and captures are page-granular (4 KB). Whether the 85% is "page marked, few words stored" or "same bytes rewritten" is
  NOT known (47% of whole pages vs 85% of 256 B chunks unchanged suggests both). That decides whether finer marks would help (E7).
- Double copies: guest -> capture buffer (CPU, system RAM) -> mirror (GPU, over PCIe) is two copies by design: the capture is the
  "as submitted" snapshot (TICK-043). Texture-storage pages are copied a third time into shadows (E2). ReBAR direct writes are ruled out (E12).
- Per-page loops, maps, locks on the hot path: yes, all under pools_mu: take_capture stamps and `cap_entry_set` per page, the mirror_sync
  walk with `cap_bufs.find` (std::map) and `late_shadows.find` per dirty page, rb refresh `imported.upper_bound` per readback page.
- Late-page wait: avoidable only by merging the GPU's words without waiting. RX_VK_LATE_MERGE does that for pages with a valid shadow, but
  only about half of the takes qualified on PC 3050 (measured, tick053 m8).

## Ranked suspects
Capture work sits in the held flush (2.1 ms in the loop, measured TICK-054), which both chains wait for, so I call it "held flush".

| id | location | what costs time and why | chain | est. gain ms/frame | evidence | risk | effort | verify |
|---|---|---|---|---:|---|---|---|---|
| E1 | vk_memory.cpp capture_make / copy_part (~1406-1490); pool_cap_ranges (~930-990) | Capture copies every marked page of every captured pool, but only ~75% of copied bytes are ever synced to a mirror (TICK-037). Pools up to RX_VK_CAPTURE_POOL_MB=64 are captured whether or not any binding uses them. The unused 25% is copy bandwidth and cache lines taken from job threads | held flush | 0.25-0.35 | measured: 25-40 MB/frame, 75% synced (perf-audit ~1260); copy 1.0-1.4 ms | med (a page not captured and later needed reads live memory: the one-frame-flash failure of TICK-036) | M | First only count, per pool, captured pages read by no sync before the capture buffer retires. Then try skipping pools with zero use over N captures, first-use falling back to today's path |
| E2 | vk_memory.cpp take_capture (shadow block, ~2200-2250) | Every dirty page in a texture's storage is memcpy'd again (4 KB) from the capture buffer into a heap `new uint8_t[4096]` kept in an unordered_map, under pools_mu | held flush | 0.15-0.25 | measured 0.1-0.3 ms for "texture shadows, stamps, allocations" (TICK-043) | low-med | M | Time the shadow block alone in the take log. Fix: reference the capture buffer slot (refcount) or slab-allocate the shadows (no per-page new, no map node) |
| E3 | vk_memory.cpp take_capture stamps + cap_entry_set (~2178-2182, ~2287-2293) | Per captured page, serially under pools_mu: stamp (2 stores) and table store, ~7,500 pages/frame. The 4 copy helpers could do their slice (generation reserved before the copy), leaving the lock for the cap_bufs insert | held flush | 0.1-0.2 | estimated from TICK-043 take 0.2-0.5 ms minus late wait 0.1-0.2 | low | M | The take log already splits stamp and lock time (st_tk_stamp, st_tk_lock) |
| E4 | vk_memory.cpp mirror_sync (~3376-3470), lazy_cpu_pages | Even with MSYNC_FAST the pre-sync walks ~52 pools' chunk tables (0.26 ms) and files ~8,900 pages into `cpu_pages` (0.10-0.14 ms). The capture already knows those pages as a sorted list (`c->pages`, `c->copied`); sync could consume a (page, slot) list instead of rediscovering dirtiness by generations | B (PT, before any segment) | 0.15-0.3 in the loop (MSYNC_FAST's 0.27 did not show in fps alone) | measured pre-sync 0.45 ms (TICK-056) | med (must equal the page_gen rule: pages rewritten live after the capture, gw/hazard notes) | M-L | test_msync_fast-style random-table equivalence, then RX_VK_PRESYNC_STATS=1 |
| E5 | vk_memory.cpp take_capture (~2157-2200), late_shadow_* (~2054-2150) | The first capture each frame waits for the GPU write-back of the exposure meter's pages. LATE_MERGE skips the wait only if every late page has a shadow valid for the CURRENT mirror id. A mirror remake, a segment sync (`s.mid=0`), `late_shadow_void*` from a live sync or from the readback refresh voids it, so the take waits again | A on main PC (small); GPU+A on 3050 | main 0.1-0.2; 3050 up to 3-4 of 8.8 if merge reached ~100% (estimated) | measured: late wait 0.11-0.22 ms main (TICK-043), 8.8 ms 3050 (tick053), merged vs no-shadow counters in the 600-take log | med | M | Run RX_VK_LATE_MERGE=1 and read `st_late_why[]` (no entry, bytes not taken, void, other mirror); fix the dominant cause (see below) |
| E6 | vk_memory.cpp memory_read_as_submitted (~1140-1205) | A replay read of a guest word first does a full `host->gpu_read`, then on a one-page-cache miss takes pools_mu, `cap_bufs.find` (std::map) and memcpys a whole 4 KB page into `submitted_page.bytes`. Dry-pass reads touch many different pages, so the cache rarely hits | B (dry pass 1.2 ms, workers share pools_mu) | 0.05-0.2 | estimated; the "replay reads ... (N pages from the captures)" log line gives the count | low | S | Divide N by frames. If >2,000 misses a frame, copy only the n bytes needed and consult the captures before gpu_read |
| E7 | a64.h A64_MARK; capture granularity | Marks are 4 KB. If much of the 85% unchanged is "page marked, few words stored", 256 B marks (16 flags a page, scanned only in dirty pages) would cut capture and sync bytes. If it is "identical bytes rewritten", marks cannot help | A+B | unknown, 0 to 0.5 | none yet | med (mark array 16x, scan cost; a64.h is shared with the lifter) | M to measure, L to build | One-off measurement build (not now): per submit count dirty 256 B chunks by mark vs by comparing with the page's previous capture |
| E8 | vk_memory.cpp mirror_sync diff hashing (~3480-3500), PC 3050 only | The chunk diff hashes each synced page on the replay thread: ~0.13 ms a part, ~4 ms a frame summed (TICK-048). The capture helpers just read the same page; hashing there (once per captured page) takes it off the PT | B on 3050; 0 on main | 3050: 1-2 on the PT (estimated) | measured 4 ms/frame summed | med (hash table written from other threads; rec/gw/mid epoch rules must hold) | L | PC 3050: `d_ns` in the RX_VK_MSYNC_STATS line |
| E9 | vk_memory.cpp memory_rb_refresh (~3708-3790) | Once per submission epoch: walks `rb_pages` (~540), `imported.upper_bound` per page (std::map), builds a std::map by pool, sorts, merges; GPU copies whole pages (1.9 ms profiled on 3050). RB_RANGES cuts GPU to 0.7 ms but gave no fps (TICK-048/053) | B + GPU | 0.03-0.08 CPU | GPU numbers measured; CPU estimated | low | S | Cache the pool per run of consecutive pages; skip pages whose pool is not mirrored without a lookup each time |
| E10 | vk_texture.cpp staging (~429-493), BC cache (~1085) | Texture staging goes through ReBAR/WC arenas (CPU memcpy/unswizzle, then GPU copy). On 3050 mmake 0.8 + ASTC 0.65 GPU. The BC cache holds 1 GB; remaining cost is eviction churn (`astc_again` was 82% before the cache) | GPU (3050), PT | 0.2-0.4 GPU on 3050 | measured upload 5.4 ms profiled (tick053) | low | M | BC cache hit rate in its 600-frame log line; raise the cap or protect entries of textures dropped by the 60-frame rule |
| E11 | queue design | Every GPU-side copy (msync, rbrefresh, mmake, ASTC) rides the graphics queue in the upload command buffer between full barriers. A transfer queue could run frame N+1's pre-sync during frame N's tail. TICK-048 left it undone | GPU, B | 3050 1-2 (serial GPU frame); main PC ~0 (GPU idles) | measured uploads 2.6 ms of 22 (TICK-057) | high (ordering vs write-backs and mirror epochs) | L | Only after TICK-060's GPU-frame data |
| E12 | CPU writes straight into ReBAR mirrors | Removes capture-then-copy, but breaks "the mirror holds submission-consistent bytes while the previous GPU frame still reads it". Listed to rule out | A+B | 0.3-0.6 in theory | none | high | L | Not recommended |

## Top 5 in short
**E1 capture volume.** Per frame the main thread and helpers move 25-40 MB into capture buffers and about 25% is never used by a
mirror sync (TICK-037). The copy is bandwidth-bound (~27 GB/s; eight helpers did not help), so the only way to shorten it is to copy
fewer bytes. The danger is the known one: a page not captured at submit and later needed is read live, after the game rewrote it
(one-frame flashes). So first add counters per pool; adopt skipping only for pools with zero use over many captures, with a first-use
fallback that is exactly today's path.

**E2 shadow copy in take_capture.** A second 4 KB memcpy plus a node-based map for texture-storage pages the capture just wrote. The
capture buffer already holds the bytes; keeping it referenced by slot, or a slab for the shadows, removes `new[]`, the map node and a
cold read of memory written with streaming stores. Low risk, one function, inside the held flush both chains wait for.

**E3 stamps and cap_entry on the helpers.** The take does serial per-page work under pools_mu for ~7,500 pages. The pages are known when
the copy starts and the generation can be reserved first, so each helper can write the stamps and entries of its own slice, leaving the
lock for the `cap_bufs` insert. With E2 the take could drop from 0.2-0.5 ms to about 0.1 ms.

**E4 pre-sync from the capture's page list.** After MSYNC_FAST the walk still re-derives from generations what the capture knows. A
sorted (page, slot) list per capture, used for pools whose `synced_gen` is older than the capture, removes the pool chunk-table walk and
the separate `cpu_pages` bitmap pass. MSYNC_FAST's own 0.27 ms did not show in fps, so expect a gain only together with E1-E3 or when the
PT is critical (HOLD=2).

**E5 late-page wait.** About 0.1-0.2 ms on the main PC but the largest single item on PC 3050 (8.8 ms of the game thread's 27 ms in
submits). Word-exact merging already exists; the work is to learn why the other half of the takes had no valid shadow (`st_late_why`).
Likely causes are `late_shadow_void_range` in memory_rb_refresh (the refresh copies guest bytes over the meter's page, voiding the
shadow; merge the refresh's bytes into the shadow instead) or mirror remakes under VRAM pressure (111 mirrors in 40 s, TICK-057).
Recheck the Kokiri exposure meter (TICK-017) after any change.

## Checked and not proposed
- Write-back diff pass and VRAM shadows: already copy only changed words.
- Mirror churn, MSYNC_FAST, SEG_EARLY, LATE_MERGE, RB_RANGES: already tried (TICK-048/053/056/057).
- Precopy thread (RX_VK_CAPTURE_PRECOPY): fps -2 to -4 from re-copies and cache-line theft (TICK-043). E1 goes the other way (copy fewer
  pages) and avoids re-copies.
- A64_MARK: ~1 ms/frame of game-thread CPU (a64.h comment). "Read the mark first" (+1.1 ms) and "mark last byte only on crossing"
  (-0.3 ms, +20 MB code) were rejected. Lifter area; only E7's granularity question is mine.
- ReBAR alone on PC 3050: no change (TICK-048). Capture buffers are cached system RAM on purpose (the replay and CPU read them back).
