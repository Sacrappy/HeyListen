# TICK-002 — Shader census of the package
**Goal**: list every NVN shader program in the NUP (count, stage, size, hash) to size the offline SPIR-V build.
**Files**: `tools/census/shader_census.py` (new). Reference: `hookshot-fork/nx2/shader_census.py`, `nx2/nup.py` (read, don't copy — ADR 0001).
**Acceptance**: CSV output with one row per program; runs on both NUPs; totals per stage in stdout; no game data committed.
**Tests**: unit test on a synthetic container header.
**Command**: `powershell -File tools/run-worker.ps1 -Ticket TICK-002 -Slug shader-census`
