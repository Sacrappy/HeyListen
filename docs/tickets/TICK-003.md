# TICK-003 — Per-draw cost microbenchmark (descriptor strategies)
**Goal**: standalone Vulkan C++20 bench: 3,000 draws/frame × 30 descriptors, comparing push descriptors vs bindless descriptor indexing vs VK_EXT_descriptor_buffer. Report CPU µs/draw.
**Files**: `bench/descriptors/` (CMake, `main.cpp`).
**Acceptance**: builds with MSVC + CMake, no game data; prints a table; result recorded in `docs/BENCHMARK_ANALYSIS.md`.
**Tests**: runs headless with validation layers clean.
**Command**: `powershell -File tools/run-worker.ps1 -Ticket TICK-003 -Slug descriptor-bench`
