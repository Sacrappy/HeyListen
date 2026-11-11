# Where frame time can still be saved: the merged suspect list (2026-11-12)

Six read-only analyses (no build, no game run) of the code, by area:

- A, the guest code on the game's chain: [A-guest-code.md](A-guest-code.md)
- B, the runtime and the host layer under it: [B-runtime-host.md](B-runtime-host.md)
- C, the NVN2 layer and the submit-time capture: [C-nvn-capture.md](C-nvn-capture.md)
- D, the Vulkan replay's CPU cost: [D-vk-replay.md](D-vk-replay.md)
- E, guest memory to GPU memory: [E-memory-xfer.md](E-memory-xfer.md)
- F, the GPU's own work: [F-gpu.md](F-gpu.md)

The gains are estimates unless the area report cites a measurement. Each area report gives the evidence, the risk and how to
verify each suspect.

## Why the gains so far were small

The frame on the main PC has two chains of ~13-14 ms that end within ~1 ms of each other (perf-audit, TICK-054):

- **Chain A, the game's own.** The main thread's draw code, then the job group (~6-7 ms), the serial module-worker phase
  (~2 ms) and the last stage (~1.4 ms). About 80-85% of it is guest code.
- **Chain B, the render chain.** The held flush (capture, ~2.1 ms) plus the Presentation Thread's replay (~7.8 ms).

A cut on one chain shows only once the other chain is cut too. That is why TICK-056's -0.6..-0.9 ms on chain B did not move
the fps. **The plan is to work on both chains at once, then stack the switches.** Everything is behind env switches, which
stay off until a screen and a confirm pass with correct shots.

## Chain B: the render chain (TICK-061)

| id | suspect | est. ms/frame | risk | effort |
|---|---|---:|---|---|
| D1 | Small command buffers recorded alone on the PT (~500 draws, ~2.0 ms measured): split from ~96 draws (`RX_VK_PARALLEL_SEG=96`: env only) | 0.3-0.6 | low | S |
| D8 | Serial chunking every 120 draws (`RX_VK_CHUNK=0` / `480`: env only) | 0.15-0.4 | low | S |
| C1 | Capture copy: 5 fixed parts, the submitter waits for the slowest helper; use atomic chunk claiming | 0.2-0.5 | low | S |
| C2 | `collect_in`: time spent in the per-range alignment ladder at range edges (~1,479 ranges, twice per submit) | 0.2-0.4 | low | S |
| C3 = E2 | `take_capture`'s texture-shadow block: per-page searches, hash erase/insert, 4 KB malloc + copy under `pools_mu` | 0.2-0.4 | low-med | S-M |
| D5 | Slow program pick (31% of picks re-resolve handles and texture types; 1.4-1.85 ms summed, measured): a per-thread cache | 0.2-0.35 | low-med | M |
| D2 + D3 | One-segment buffers parsed twice; a 27 KB `NvnState` snapshot every 32 draws of which ~5 are used | 0.25-0.5 | low | S-M |
| D6 | Shared atomics per draw (`work_epoch`, `all_work_by_cmd`, checkpoint) contended by the 5 recorders | 0.1-0.25 | low | S |
| D7 | Segment setup (begin, pool reset) done on the PT before releasing jobs | 0.1-0.2 | low | S |
| D9 | Cold segment start (~7 us per draw for the first 32 draws vs 3-3.5): per-submission texture touches done once | 0.15-0.3 | med | M |
| D4 | An index-buffer bind per draw | 0.1-0.25 | med (robust range) | S |
| E3 | ~7,500 page stamps a frame serially under the lock; split them across the copy helpers | 0.1-0.2 | low | S |
| F2 | ~900 occlusion proxy draws a frame whose result is fixed in the default mode: skip if they provably write nothing | 0.5-1.0 PT + 0.2-0.4 GPU | low-med | M |
| D10 | TICK-056's `MSYNC_FAST` + `SEG_EARLY` (built, measured -0.6..-0.9 ms frame prep): restack once chain A is shorter | 0.6-0.9 | low | done |
| C4 | ~47% of the copied pages are byte-identical to the mirror (measured on PC 3050): skip them; the exposure-meter ordering applies (TICK-048, TICK-058) | 0.3-0.7 | high | L |
| D11-D13 | Cross-frame reuse of the replay. D11 first (repeat-rate diagnostic), then reuse of the dry pass's output, then of the recorded segments | 0.3-0.9, then 1.5-3 | high | L |

## Chain A: the game's chain (TICK-062)

| id | suspect | est. ms/frame | risk | effort |
|---|---|---:|---|---|
| A1 | NavMesh crowd-step FP kernels (4 functions, 55-62% of the NavMesh worker, 2nd largest path entry at 1.18 ms): a native SSE kernel behind the FPX gate, falling back to the lifted body | 0.3-0.45 | med | M |
| B1 | `SignalConditionVariable` with no waiter: ~570k signals/s from the NavMesh worker, each taking the lock and calling the wake; return early when no thread waits | 0.1-0.25 | low-med | S |
| B2 | `hos_thread_of`: a linear scan of ~70 threads under an SRW lock per priority call (7.1% of the NavMesh worker) | 0.05-0.15 | low | S |
| A10 = B3 | Virtual-core pile-up on core 1: the NavMesh worker waits 0.33 ms a frame behind EX1 and Resource_Memory. First count who asks for its core number | 0.15-0.4 | med-high | S (count) |
| B4 | Sleeps under 1 ms loop SwitchToThread (~290 polls of 20 us a frame), stealing from the SMT siblings | 0.1-0.3 | med | S |
| A4 | ~8,000 nn::os mutex locks a frame in the serial module job; `obj_lookup` misses cost ~1.45 ms of CPU; B7 packs its entry | 0.2-0.3 | med | M |
| A5 = B6 | Global `copy_lock` / recording in guest threads (~0.5 ms on the path) | 0.2-0.3 | med | M |
| A7 | Out-of-line FP rescues (~1.1 ms of CPU), a 67-limb software FMA | 0.1-0.25 | low | S |
| A2 / A3 | Leaf FP kernels in the executor jobs (2.8 ms of CPU); lift-time speculative FP blocks generalise them | 0.2-0.6 | med | L |
| A6 = B5 | Resource decompression / `ReadFile` on ARes_Loading (0.77 ms on the path): first prove the chain waits for it | 0.2-0.5 | low (byte-exact) | M |
| A8, A9 | Module-system leaf kernels; Link's calc f_801d99a0 (1.27 MB of x86, fetch-bound) | 0.1-0.25 each | med | M |

Not a lever (checked): the libc-like guest functions (already host hooks), -O1 / PGO coverage (TICK-040), the mutex fast path,
svc dispatch, hid / audio, wake latency (TICK-058), and the NVN command-building path (no per-call locks or allocations).

## GPU and PCIe: matters mostly on PC 3050 (after TICK-060)

| id | suspect | est. | risk | effort |
|---|---|---:|---|---|
| F1 | A full barrier before every pass, dispatch and game barrier (~150-200 drains a frame). `RX_VK_TEST_BARRIER_DROP` measured -3.1 ms gpu_ms and +2.5 fps on PC 3050 (upper bound). Emit only on a real hazard | 0.8-1.5 (main), ~3 (3050) | high | L |
| E5 | The late-page wait (8.8 ms a frame on PC 3050). LATE_MERGE merged only half the takes; read `st_late_why` | up to 3-4 (3050) | med | M |
| E8 | Chunk-diff hashing on the replay thread (~4 ms summed on PC 3050): move it to the capture helpers | 1-2 (3050) | med | M |
| F5 | Each indirect mesh draw (foliage, grass) ends the render pass and adds 3 barriers + fill + copy | 0.3-1.0 | low | M |
| F3 | `NonWritable` / `NonReadable` never emitted on storage resources | 0.1-0.4 | low-med | S |
| F6 | Global-memory emulation in culling / task / mesh shaders (helper search + 64-bit page lookup) | 0.2-0.6 | med | M |
| F4 | Device-wide `robustBufferAccess2`; `VK_EXT_pipeline_robustness` would scope it to two programs | 0.2-0.5 | med-high | M |
| F12 = E11 | Engine copies on a transfer queue (concurrent sharing) | 0.3-0.8 (main), 0.5-1.5 (3050) | high | L |
| F7-F9 | SPIR-V of the heaviest programs never reviewed; kill as demote; `SignedZeroInfNanPreserve` on all fp32 | 0-0.5 each | low-med | S-M |

## Order

1. **Free env-only screens** on the main PC: `RX_VK_PARALLEL_SEG=96` and `RX_VK_CHUNK=0` (part of TICK-061).
2. **Chain B and chain A at once:**
   - TICK-061 (chain B): C1, C2, C3, D5, D2/D3, D6, D7, E3, F2.
   - TICK-062 (chain A): B1, B2, the A10 count, B4, A7, then A1.
   - Each is built behind switches and screened alone, then stacked with the other ticket's switches and D10.
3. **Measurements that decide the large items:** D11 (repeat rate) for the replay cache, C4's identical-page count, and whether
   the chain waits on ARes_Loading (A6).
4. **PC 3050:** F1 with a hazard check, E5 and E8, ordered by TICK-060's measured breakdown.
