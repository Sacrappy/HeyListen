# TICK-004 — Hyrule Field baseline (Hookshot + Lichii)
**Goal**: run TICK-001 in Kokiri and Hyrule Field for both builds on the same machine; store results.
**Depends**: TICK-001. Needs a save in Hyrule Field (user provides).
**Acceptance**: `docs/benchmarks/2026-11-baseline.md` with the four numbers sets and the machine spec.
**Command**: `powershell -File tools/run-worker.ps1 -Ticket TICK-004 -Slug baseline`
