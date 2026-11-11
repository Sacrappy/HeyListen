# TICK-051 — Frame-time spikes (stutter), measured on PC 3050  · model: Opus · after TICK-048
**Why**: the average fps hides stutter. Main PC, Market walk: ~72 fps but a 1% low of ~35 (frames of 28 ms and more). PC 3050
(i5-10400F, RTX 3050 on PCIe 3.0 x4): ~27 fps walking, 1% low ~10.7, p99 77-80 ms. No ticket had measured the spikes one cause
at a time.
**Goal**: a per-frame trace that says what each slow frame did; the spikes (frames over 2x the median) by cause, standing and
walking; the biggest causes fixed behind `RX_VK_*` / `HOS_*` switches, with no quality loss; A/B on 1% low, p99 and spike count.
**Branch**: `feature/TICK-051-stutter` (built and measured on PC 3050 only)

## Result (2026-11-11): the walking spikes were ASTC re-transcodes that the next frame waited for; batched, and cached in system memory

**Trace** (`RX_VK_FRAME_TRACE=1`, `hookshot_frames.csv` beside the game, off by default): the existing columns plus `t_s` (since
launch, for the 66 s cut), `gpu_ms`, `late_wait_ms` / `late_waits` (the capture's GPU wait for its late pages), `submit_ms`,
`present_ms`, `qsubmit_ms` (driver submit), `dispatch_ms`, `astc`, `tex_made`, `scatter_mb`, `vram_mb`, `astc_again` (ASTC
transcodes of storage transcoded before), `astc_mtexels`. `tools/bench/spikes.py` classifies a run's spikes: each spike goes to the
cause whose excess over its own median covers >= 40% of the frame's excess, else "unattributed".

**Spikes by cause** (before; frames over 2x the median after the 66 s cut, a run of ~44 s):

| | standing (kokiri-idle) | walking (castle-move), main 1bb7136 | walking, main b17b933 |
|---|---|---|---|
| spikes a run | 0 (8 runs) | 21.7 (1058 ms over the median) | 8.6 (507 ms; median 39.5 ms, not 34) |
| late-page wait for the GPU | - | 12.3 (573 ms) | 4.0 (211 ms) |
| replay CPU (draws, descriptors) | - | 6.7 (326 ms) | 2.6 (139 ms) |
| unattributed (game CPU, other waits) | - | 1.3 (88 ms) | 1.2 (120 ms) |
| pipelines / programs / translation | - | 1.0 (58 ms) | 0.6 (29 ms) |
| compute dispatch | - | 0.3 (14 ms) | 0.2 (8 ms) |
| texture uploads (CPU), driver submit, readback refresh, VRAM | none | none (qsubmit flat 0.4 ms; VRAM 6.2-6.7 of 7.1 GB) | none |

Why: the game's thread waits once a frame (`take_capture`, late pages) for the GPU to finish the previous frame: ~10 ms a frame on
PC 3050. That wait grows to 40-60 ms in the walking spikes, and correlates 0.94 with the previous frame's ASTC transcodes (0.32 with
the same frame's). A transcode (ASTC decode + BC1/BC7 encode + copy, per level and layer, three full barriers each) costs 1.2-1.8 ms
of GPU on the 3050 (`RX_VK_PASS_TIMING`, new span "astc transcode"); walking makes 30-60 in a frame. **82% of them are of storage
transcoded before**: the textures over the budget go once unused for 60 frames, and the walk brings them back. The "replay CPU"
spikes are the same frames (they carry 25-44 ms of late wait too).

**Changed** (on by default, each with an opt-out):
- `RX_VK_ASTC_BATCH` (=0: as before): up to 8 (level, layer) pieces of a transcode at a time, each in its own scratch layer and
  encoder-buffer range, three full barriers a group instead of three a piece. Same dispatches and inputs: the same blocks.
  1.2-1.8 -> 1.06 ms a texture.
- `RX_VK_BC_CACHE_MB` (1024; 0: off): each batched transcode also copies its blocks into a host-cached buffer in system memory,
  keyed by the texture's shape, the pieces' constants and a hash of the ASTC bytes; a texture made again with the same bytes gets
  its blocks copied from there, no decode or encode. Castle walk: 2107 -> 327 transcodes a run, ~900 MB of RAM held.

**A/B** (PC 3050, one dll on main b17b933, base = `RX_VK_ASTC_BATCH=0 RX_VK_BC_CACHE_MB=0`, 5 interleaved runs, 110 s, 66 s cut;
fps / 1% low / p99 / spikes a run / frames > 50 ms):

| scene | base | TICK-051 |
|---|---|---|
| castle-move | 24.19 / 10.8 / 75.8 ms / 8.6 / 85.8 | 25.21 / 14.0 / 58.1 ms / 2.4 / 37.0 |
| hyrule-walk | 26.34 / 18.6 / 49.9 ms / 0.6 / 14.0 | 25.55 / 16.9 / 50.5 ms / 0.6 / 13.6 |
| kokiri-idle | 26.62 / 20.5 / 45.1 ms / 0 / 2.4 | 25.36 / 21.3 / 45.1 ms / 0 / 0.6 |

kokiri-idle was run again (3 runs each, nothing transcodes there): base 26.08, batch alone 25.78, both 27.42 fps: inside the
spread (sd 0.8-1.5). hyrule-walk is inside the spread too. The street is where it shows: 1% low +30%, p99 -23%, spikes -72%.
Screens on the older base (1bb7136, 3 runs): castle-move base 27.18 / 10.9 / 77.2 / 21.7 spikes, batch 27.76 / 14.0 / 63.2 / 4.3;
again base 26.86 / 11.0 / 75.3 / 18.3, batch 27.21 / 12.9 / 65.4 / 6.3, batch + cache 27.36 / 13.8 / 60.1 / 6.3 (late wait 9.9 ->
8.1 ms a frame).

**Tried, not adopted**: `RX_VK_CAPTURE_DEFER=1` (slack 2 / 1, existing): no late wait, but the wait moves (unattributed) and fps
-1.3 / -1.1 on castle-move, 1 run each. An eviction that stops at 3/4 of the budget: no change (the textures stay over it), dropped.

**Correctness**: shots every 300 presents, standing and walking, base and TICK-051 (base = the switches off): ground, walls, people, foliage, HUD, exposure alike; texture detail the same where the walk used cache hits (1927 textures from the cache in that run); not pixel-equal (other moments). `HOS_SPEED_LOG` ratio over the last 40 s: 60.02 / 60.05 (base), 60.00 / 60.02 (TICK-051), standing / walking. Saves identical to the backup after every run; no game left running.
Tests: `test_frame_trace.py` and `test_astc_batch.py` (new), `test_msync_diff.py` (portable and AVX2), `test_mirror_inherit.py`, `test_capture_stream_copy.py`, and the tests that read the touched functions (`test_capture_wait_log`, `test_mem_trim`, `test_status_line`, `test_slow_frame_limit`, `test_submitted_page`) pass on PC 3050 (clang++ of llvm-mingw). run_all.py not run.

**Main b17b933 on PC 3050**: NVML reads the card's maximum link, **Gen3 x16** (15.8 GB/s), not the slot's x4: the chunk diff is
**off by default** here ("chunk diff off by default (RX_VK_MSYNC_DIFF unset)"), and the scatter is back to ~36 MB a frame. One kokiri-idle run with `RX_VK_MSYNC_DIFF=2` on the same dll: 29.6 fps / 1% low 23.2 / p99 41.4 ms, against 25.4-27.4 with the default: the PC 3050 gain of TICK-048 is lost on main b17b933.
Suggested fix (not in this branch): the current link width (it does not drop at idle), with the maximum generation.

**To check on the main PC (not run there)**: castle-move and hyrule-walk with `RX_VK_ASTC_BATCH=0 RX_VK_BC_CACHE_MB=0` against the
default (1% low, p99, the trace's spikes: `tools/bench/spikes.py`); Kokiri and the Market shots (texture detail as before); the
process's RAM (the cache's up to 1 GB); a scene change (the cache's hits across areas).

Docs: engine/docs/perf-audit.md "TICK-051", docs/benchmarks/2026-11-11-tick051-stutter.md.
