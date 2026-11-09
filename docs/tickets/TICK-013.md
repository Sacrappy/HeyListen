# TICK-013 — Bench metrics for the 120 fps goal  · model: Haiku
**Goal**: tools/bench/summarize.py and run_bench.py: add 1% low fps, 0.1% low fps, % frames > 8.33 ms and > 6.94 ms (120 / 144 Hz budgets), and a `--fps-limit` passthrough already exists: document how to bench at 120/144/240. Summary table columns updated; README updated.
**Acceptance**: unit tests on synthetic traces; the existing four bench dirs still summarize. No game launched.
**Branch**: `feature/TICK-013-bench-120`
