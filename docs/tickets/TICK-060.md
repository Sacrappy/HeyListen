# TICK-060 — PC 3050: cut the frame time without lowering quality · model: Opus 5.5 · after TICK-053/057/058
**Why**: PC 3050 (i5-10400F 6C/12T, RTX 3050 8 GB on PCIe 3.0 x4) runs the Market at ~28-29 fps with the GPU idle a third of
the frame (60-69%). TICK-053 found the frame serial (the late-page wait); TICK-057 found no engine GPU cut that moves fps.
**Goal**: where the GPU's idle third goes (without PASS_TIMING's serialisation where possible), then quality-preserving cuts, each
behind an env switch, off until proven; picture identical; the original behaviour kept.
**Branch**: `feature/TICK-060-pc3050-gpu` (from main af1cc93). Local only; built and measured on PC 3050 (session 0 over SSH);
bundle `E:\OOT\tick060.bundle`. Details and every table: `docs/benchmarks/2026-11-12-tick060-pc3050.md`.

## Result
@@RESULT@@

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

@@REST@@
