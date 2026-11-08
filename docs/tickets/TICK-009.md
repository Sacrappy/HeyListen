# TICK-009 — Bench: repeat runs, warm-up from launch  · model: Haiku
**Goal**: `tools/bench/run_bench.py`:
- `--repeat N`: N runs back to back (each ≤120 s, game killed between runs), per-run stats plus spread of fps between runs (ticket 001's "within 5%").
- `--warmup-s` counted from **game launch** (wall clock), not from the first present; document the change. Default warm-up for Hookshot scenes: 45 s after launch… measure from the existing traces in build/bench/ where gameplay starts and pick it.
- `tools/bench/summarize.py <dir>...` prints a Markdown table (fps, p50/p95/p99/max, % > 16.7 ms) like docs/benchmarks/2026-11-baseline.md, for Hookshot traces and PresentMon CSVs.
**Acceptance**: unit tests for the new parsing/stats paths pass; no game launched by the tests.
**Branch**: `feature/TICK-009-bench-repeat`
