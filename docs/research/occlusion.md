# Occlusion in OoT (TICK-032): why real counts break the picture

Research only (no game runs). Paths are relative to the HeyListen tree at `4711895` unless a repo is named.

## 1. What the engine does today

| Piece | Where | Behaviour |
|---|---|---|
| Occlusion Off | `engine/nx2/oot_occlusion.py:1-40`, `engine/host/hos_main.c:1161-1166` | `HOOKSHOT_OCCLUSION_OFF=1` forces the byte the game loads at six sites (`main+0x11ACE68, 0x11B828C, 0x11B82A4, 0x11ACEC8, 0x11B82DC, 0x11B82F4`, `ldrb w8,[x0]`) to 0: no z-prepass occlusion queries, no samples-passed reports, no conditional renders; every candidate is drawn. Off by default (Hookshot `46f9fb5`, from cLohan's patch, Lichii #641 / Fritchii `1a46ba972`). |
| Report recording | `engine/host/nvn_native.c:1586` (`nvnCommandBufferReportCounter`), `:1575` (`ResetCounter`) | Records `{type, addr}`; with `HOS_COND_RECORD` it may zero "clear flag" words at record time. |
| Report write | `engine/gpu/vk/vk_replay.cpp:763-801` | Written **on the CPU, in stream order, at replay**: `{counter, timestamp}`, 16 bytes. SAMPLES_PASSED counter = `RX_VK_OCC_FIXED` (default **4096**, "everything visible", Hookshot PR #487 / `5257559`); `-1` = 16 per draw since the reset (`:1167, 1179, 1191, 1210`). Address marked `memory_cpu_owned` (`:800`) so mirror write-backs never overwrite it. |
| Conditional render | `engine/gpu/vk/vk_replay.cpp:1093-1116` | CPU-evaluated at replay: reads the 64-bit counters at `addr` and `addr+16`; mode 0 = render if equal, 1 = if different. Parallel recording (TICK-006) evaluates it in the dry pass (`par_conds`, `:1112`) and workers replay the result. No `VK_EXT_conditional_rendering`. |
| Real GPU queries | `engine/gpu/render/rx_core.cpp:620-775`, `rx_nvn.cpp:819-849` | Exist **only in the older WebGPU `rx` renderer**: per-draw queries between reset and report, resolved at batch end, the counter word written from the map callback (a batch later), timestamp written at once. Not used by the Vulkan replay. |

So in the shipping Vulkan path no count ever comes from the GPU. TICK-027's "real counts" runs therefore used new code; its
branch (`feature/TICK-027-occlusion`, last commit `513884b`) has no occlusion section committed yet, so what exactly it did is
unknown here.

## 2. What the game does with the results

Evidence points to three consumers of the samples-passed reports made between a reset and a report:

1. **GPU-side draw gating (the big one).** Each object of the colour pass (depth EQUAL after a depth prepass) is drawn
   under `SetRenderEnableConditional` on a samples-passed report; the test draws are proxies, mostly GPU-culled
   *indirect mesh* draws (oot-pc `docs/phase3.md:956-962`). OoT makes ~1200 samples-passed reports a frame
   (oot-pc `phase3.md:1020-1021`), and Lichii saw reset-to-report windows of 257-346 query slots (Lichii `88783d558d`).
   The condition compares two counters 16 bytes apart (`nvn_native.c:1721-1723`; NX2navi `docs/maxwell.md` rows 1.4, 4.2):
   count 0 => equal to the zero word => object not drawn. Many conditions also gate **single lights** (light volumes)
   and **clears** (`nvn_native.c:1576-1578`: reading early "leaves the scene white or throws white blobs over it";
   clear flags `vk_replay.cpp:792-797`).
2. **Sun / lens-flare visibility -> sunlight and haze.** Hookshot `5257559`: with 16-per-draw counts that varied frame
   to frame, the sun visibility followed the count; the sunlight left the ground and the haze brightened over the whole
   picture. So a count is turned into a sun-visible fraction that drives sunlight on the ground and fog/haze brightness.
   A count that is too low (or 0) = sun hidden = **washed-out, hazy picture**.
3. **CPU-side culling.** NX2navi (same engine family): the game's CPU reads the counts (guest `f_70010c3298`) to cull
   objects (`docs/maxwell.md:126-130`). Lichii `7ae3e74014`: stale macro-DMA reads of report regions gave a zero
   visibility count and culled visible objects, matching OoT's missing vegetation (Lichii #101).
   Whether OoT skips *actor updates* for occluded objects is not shown anywhere; the evidence is for draw skipping only.
   No evidence of LOD selection or exposure metering from the counts (exposure changes come through the sun/haze path).

Units: the counter is a 64-bit sample count (not a boolean); layout `{u64 counter, u64 timestamp}` per 16-byte report;
TIMESTAMP reports write counter 0 (NX2navi `maxwell.md` 4.2; GPU timestamps are 614.4 MHz ticks there, the replay writes
19.2 MHz ticks, `vk_replay.cpp:789`). For conditional renders only zero vs non-zero matters; for the sun the magnitude
matters (a fraction of the proxy's area).

## 3. Why real counts give white/missing meshes and a washed-out picture (ranked)

### H1 (most likely). Counts are 0 because the proxy test draws draw nothing or the wrong thing
The proxies are GPU-culled indirect mesh draws whose group/instance counts come from culling compute kernels.
- For: oot-pc measured 892 of 2196 queried reports reading 0 with real counts, 46.6% white (`phase3.md:1106-1111`);
  after fixing mesh vertex / stray-polygon bugs "the GPU's own samples-passed counts give a complete picture", "the 46%
  white was these polygons, not wrong culling" (`phase3.md:1121-1124`), and real counts became oot-pc's default.
  Hookshot has open culling/mesh-count bugs (issue #994 floating trees / missing mountains / vanishing vegetation;
  comments on #1002/#1004: categories with zero counts from culling programs `b6122162 -> 4247f1b4`). Any proxy that
  draws nothing (wrong indirect record, skipped pipeline, failed lowering, "mesh stage missing", raster discard) yields
  0 with real queries, while the fixed/fake model hides it.
- Against: none strong; it is the one hypothesis with a measured before/after in a sibling port.
- Cheap test: with real counts, log per report `{addr, draws, samples}` and per draw `{program, indirect args, recorded?, samples}`;
  list 0-count reports whose draws were indirect mesh draws with 0 groups or were skipped.

### H2. Reports read before the GPU wrote them (in-flight reports / one-frame latency)
Conditions are evaluated on the CPU in stream order, often a few hundred draws after their report *in the same command
buffer*. With GPU counts the value is not known then; the word holds an older count or the game's cleared value.
- For: oot-pc `phase3.md:1017-1026`: a 256-entry in-flight ring vs ~1200 reports a frame => conditions read stale or
  cleared words; whole objects (fence, ferns) vanished depending on how far the GPU was. `nvn_native.c:1576-1578`
  (early reads: white scene/blobs). Lichii `7ae3e74014` (stale macro DMA reads), `5b89a3daf4` (counter reset at each
  submission => report too low => white flashes on Link). The rx path writes counts a batch late (`rx_core.cpp:714-731`).
- Against: oot-pc's `OOT_SUBMIT_WAIT` / `OOT_DRAW_WAIT` changed nothing, so staleness alone did not explain its white share.
- Cheap test: real counts, but a condition whose report is still in flight decides "render" (NX2navi / oot-pc rule:
  unknown = visible); count how often that happens. If the white goes, latency was it.

### H3. Counter scope wrong: reset->report windows broken by submits, segments or pool limits
- For: Lichii `5b89a3daf4` (a submission between reset and report reset the count), `88783d558d` / `a08bf8f02d`
  (windows over a 256-slot bank dropped counts). In our replay TICK-006 segments (160 draws) split windows: the dry pass
  sees "the reports before it written and none after" (`vk_replay.cpp:1094`), and a Vulkan query cannot span command
  buffers or render passes, so a naive implementation silently loses samples. The rx path caps a set at 1024 queries and
  ends the render pass at each reset (`rx_core.cpp:748-754`).
- Against: Occlusion Off does not fix TICK-006's fog/shadow wash-out (`engine/docs/perf-audit.md:58-59`), so that one is a separate bug.
- Cheap test: real counts with `RX_VK_PARALLEL` off; log the largest window and every window crossing a submit, segment or pass boundary.

### H4. The sun needs a magnitude, not just non-zero
- For: `5257559` shows the sun follows the count's value. A real count of a small or partly hidden proxy, or one in other
  units (MSAA samples, imprecise queries returning 1, render targets at another resolution than the game assumes) gives
  a fraction below 1 and a hazy picture even when meshes are right.
- Against: washed-out also follows from H1/H2 (sun proxy reads 0).
- Cheap test: find the report that moves with the camera facing the sun; log its real count against its proxy area;
  try `VK_QUERY_CONTROL_PRECISE_BIT`.

### H5. Results not visible to the CPU (TICK-015 write-back skip) or written to the wrong address
- For: TICK-015 skips write-backs of CPU_CACHED pools never invalidated (`perf-audit.md:337-350`); a GPU-side write
  (`vkCmdCopyQueryPoolResults` into a mirror) would never reach guest memory, so CPU readers (sun, CPU culling) see 0.
- Against: today's path writes reports on the CPU into guest memory and exempts them (`memory_cpu_owned`,
  `vk_replay.cpp:800`); oot-pc saw no change with `OOT_MEMORY=import` (`phase3.md:762-763`). Applies only if a new
  implementation copies results on the GPU.
- Cheap test: compare the value written for a report with a CPU read of the same guest address a frame later.

### H6. Conditional rendering inverted / timestamp counter not 0
- Against: mode semantics (0 = equal) match NX2navi and rx; the same code is right with the fixed 4096; `RX_VK_COND_ALWAYS`
  exists as a diagnostic. Only relevant to a `VK_EXT_conditional_rendering` port (it tests one 32-bit word against 0, so
  NVN's equality needs a predicate word computed first).
- Cheap test: synthetic mode 0/1 pairs; check TIMESTAMP reports write counter 0.

## 4. Recommended design

1. Keep the CPU, stream-order write of every report (as now) so conditions and clear flags stay deterministic; a GPU
   count *updates* the word later.
2. Real counts as an opt-in refinement: Vulkan occlusion queries from a host-reset ring (idea from NX2navi base import
   `0478c13` `runtime/vk_exec_xfer.c` "occlusion counts", MIT, and oot-pc `runtime/vk_exec_xfer.c:113-140`): a query per
   pass span between reset and report, ended at every command-buffer / segment / pass boundary and summed, read on the
   completion thread in submit order, written into guest memory only once the submission is done. Never reset the count
   at a submit.
3. Condition whose report is not done: **visible**. Use the previous frame's real count only to *skip* when the same
   report address read 0 last frame (one frame of latency; rare pop-in, no white holes).
4. For the sun keep a magnitude: write `max(real, fixed)` for it until H4 is understood, or leave its report on the fixed value.
5. A draw skipped while counting (pipeline not ready, failed lowering, skipped mesh draw) counts as visible.
6. Do not enable by default before H1 is ruled out (indirect mesh proxies with correct group counts); measure with the
   flash detector and a white-share log, as oot-pc did.

## 5. Next experiment

An instrumented real-count run (one submit per frame, `RX_VK_PARALLEL` off) that keeps today's CPU-written fixed value
and only *logs* the real count next to it, per report: `{addr, draws, mesh draws with 0 groups, skipped draws, real samples}`.
If most 0-count reports trace to zero-group or skipped indirect mesh proxies, H1 is confirmed and the fix is in culling /
mesh translation, not in the query plumbing. It changes no picture.
