# Benchmark analysis — Hookshot vs Lichii (2026-11-08)

## 0. Correction of the brief
The target is the **Switch 2** release of OoT (the two `.nup` files, title 0400486024DEC000), not the N64 ROM.
There is no F3DEX2, RSP or RDP and no 20 Hz N64 loop here: the game is AArch64 code (with LSE atomics, PAC, LDAPR)
driving the **NVN2** graphics API, whose shaders are Ampere **SM86 SASS**. The real problems are:
- CPU: translating AArch64 → native (Hookshot: static lift to C; Lichii: dynarmic JIT inside Eden).
- GPU: replaying the NVN command stream on Vulkan and translating SASS → SPIR-V.

## 1. Hookshot (static recomp) — 45-55 fps in Kokiri
Source: `E:\OOT\hookshot-fork\docs\perf-audit.md` (sampled profile, 7800X3D + RTX 5070).
- The CPU recomp is **not** the bottleneck: the game's logic thread is 77% idle; no guest function exceeds 1%.
- The limit is **one thread**: the game's Presentation Thread replays the NVN stream inline, 15-16 ms/frame (80-86% busy). GPU only ~8.7 ms.
- Per frame: ~3,050 draws at ~3.2 µs each. `build_descriptors` 19% (≈30 descriptors resolved per draw), NVIDIA driver 20-25% (push descriptors, binds), `program_for` lookup 8%, mirror syncs 5-6% (22 MB copied / 67 MB written back per frame), waiting on the game 13-14%.
- No real occlusion results: the game draws every candidate. Real queries removed ~29% of draws in oot-pc.
- `RX_VK_PARALLEL=4` (multi-thread recording) gives 55→65 fps / 12.6→8.5 ms but **renders wrong** (washed fog, missing shadows) with segments ≤320 draws: a race on shared state (texture write tracking, mirrors, per-thread caches). Unsolved.
- First-visit stutter remains: pipelines built at draw time when the prepared cache misses.

## 2. Lichii (Eden emulator fork) — 60 fps Kokiri, 30-45 Hyrule Field, freezes
Source: `E:\OOT\Lichii-1\NX2_NOTES.md`, `NX2_GPU_THREAD_SPLIT.md`.
- Full emulation: dynarmic JIT + Maxwell3D engine emulation on a GPU thread (GPFIFO parsing, macros, register writes, draw setup). Measured on another title: ~100% GPU thread, host GPU 20-40%. GPU thread split (front/back) is 1.4-1.6x at best.
- **Freezes**: SASS→SPIR-V translation runs **on the GPU thread** (row 121): it reads live GPU state (texture types, constant-buffer values) while translating, so it was never moved to workers. A big shader took 3.4 s (0.85 s after fixes); typical 2-4 ms each, plus Vulkan pipeline compile. Every new shader/state combo = hitch.
- **Hyrule Field drop** (hypothesis, to measure): wide view ⇒ many more draws and macro calls, and the per-draw cost is emulated register/state processing on one thread; plus inline-write cache invalidations (row 134) and read traps/fence downloads (row 150).

## 3. What HeyListen must do differently
1. Keep Hookshot's approach for the CPU (static AOT; it is already free).
2. **Get the replay off the critical path**: replay on its own thread(s), with the game thread only producing a compact, pre-decoded command list.
3. **Per-draw cost ÷4**: bindless descriptor heap / descriptor buffers instead of push descriptors; precomputed program keys; cached descriptor sets keyed by binding generation.
4. **Correct parallel recording**: make all shared state per-segment (write tracking, mirrors), merge at submit. That is the 65+ fps path already measured.
5. **Zero stutter**: offline shader census of the whole game (all SASS programs are in the package) → SPIR-V at build time; Vulkan pipeline library (`VK_EXT_graphics_pipeline_library`) + persistent `VkPipelineCache`; async compile with a fallback pipeline so draws never block.
6. Real occlusion (GPU queries, results one frame late, with conservative "visible" default to avoid flashes).

## 4. Open measurements (first tickets)
- Hyrule Field profile for Hookshot (never tested) and Lichii, same machine, same save.
- Frametime capture (p50/p99/max) in Kokiri and Hyrule for both, as the baseline.

## 5. TICK-003 - per-draw descriptor strategy microbenchmark (2026-11-08)
Standalone headless Vulkan C++20 bench, no game data: `bench/descriptors`.
Build: `cmake -S bench/descriptors -B build/descriptors -G "Visual Studio 17 2022" -A x64` then
`cmake --build build/descriptors --config Release`. Run: `build/descriptors/Release/descriptor_bench.exe`.
- Machine: NVIDIA GeForce RTX 5070, Vulkan 1.4.351, MSVC 19.44 (Release x64), Windows.
- Workload: 3,000 draws/frame, 30 combined-image-sampler descriptors resolved per draw, 10 measured frames after 2 warmup frames, 8x8 render target.
- Metric: CPU command-buffer recording time only; queue submit, fence wait and GPU execution are excluded.
- Validation: `VK_LAYER_KHRONOS_validation` enabled, 0 errors / 0 warnings.
- Test: `ctest --test-dir build/descriptors -C Release` passes (`--frames 2 --draws 300 --warmup 1`).

| Strategy | CPU µs/draw | vs bindless |
|---|---:|---:|
| Push descriptors (`VK_KHR_push_descriptor`) | 22.28 | 19.7x |
| Bindless descriptor indexing (runtime array, push-constant indices) | 1.13 | 1.0x |
| `VK_EXT_descriptor_buffer` | 13.94 | 12.3x |

Median of four full runs; run-to-run spread was <1% for push and descriptor buffer and ~5-8% for bindless.

Notes:
- The descriptor-buffer case still writes all 30 descriptors per draw (`vkGetDescriptorEXT` into a host-visible ring) because it models Hookshot's per-draw `build_descriptors`; pre-building the ring once would remove almost all of that cost.
- Bindless wins because the per-draw work is 30 index stores plus one `vkCmdPushConstants`; the descriptor set is built once.
- This supports section 3.3: replace per-draw push descriptors with bindless indexing (or a pre-built descriptor buffer) to cut the replay thread's per-draw CPU cost by roughly 12-20x.
