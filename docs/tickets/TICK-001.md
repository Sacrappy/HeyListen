# TICK-001 — Frametime benchmark harness
**Goal**: one script that runs a build (Hookshot or Lichii) for 90 s on a fixed save, records per-frame times, and writes p50/p95/p99/max + fps to JSON.
**Files**: `tools/bench/run_bench.py` (new), `tools/bench/README.md`. Inputs: exe path, scene name, output dir. Reuse `E:\OOT\hookshot-fork\scripts\bench-replay.sh` logic for Hookshot (read-only reference).
**Acceptance**: kills the game after ≤120 s wall clock, always (watchdog); runs twice give fps within 5%; JSON schema documented.
**Tests**: `pytest tools/bench/test_parse.py` on a sample log.
**Command**: `powershell -File tools/run-worker.ps1 -Ticket TICK-001 -Slug bench-harness`
