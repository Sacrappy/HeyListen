# TICK-029 — Memory pressure follow-ups  · model: Sonnet
**Goal** (one at a time, each measured): texture eviction when system commit runs low (not only VRAM); bound the write-back snapshot pool (~1/64 of the VRAM budget); render-target image reuse (same parameters, released >= 2 frames ago) to cut VMA fragmentation (TICK-023 saw GBs free inside blocks); push descriptors with update templates. Ideas from docs/references/NX2navi.md (own code).
**Acceptance**: VRAM peak / steady and fragmentation (free in blocks) down; fps unchanged or up; picture correct.
**Branch**: `feature/TICK-029-memory`
