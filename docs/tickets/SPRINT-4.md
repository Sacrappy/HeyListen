# Sprint 4 — Native replacements of hot game code (from 2026-11-09)

Goal unchanged: 120+ fps in game. Sprint 3 showed the frame is held by the game's own CPU job chain (main thread + dependent jobs ~8.4 ms in Hyrule Field, more in Castle Town Market at ~58-61 fps), spread over guest code; engine-side levers are exhausted. Decision (user, 2026-11-09): replace the hottest guest functions with native host code.

Rules for every replacement:
- Only functions on the measured critical chain, ranked by inclusive time at Castle Town Market and Hyrule Field.
- Same observable behaviour: a differential check runs the guest and the native version on recorded inputs (and in a debug mode side by side in game) before it is enabled; floating point must match bit for bit unless proven harmless.
- Each replacement behind its own switch (env var), off -> guest code.
- Injected through the lifter's existing patch mechanism (engine/nx2/oot_patches.py), never by editing generated code by hand; no game code copied into the repo (write the native version from the behaviour, document the guest address).

| Ticket | Model | Content |
|---|---|---|
| TICK-026 | Opus | Hot-function map by system + replacement framework (switches, differential check) + first replacement (Castle Town binary search with strcmp) |
| TICK-027 | Opus | Real occlusion queries (from NX2navi MIT base design) |
| TICK-028 | Sonnet | Subgroup reconvergence + dark-frame diagnostics |
| TICK-029 | Sonnet | Memory pressure follow-ups |
| TICK-030+ | Opus | Next native replacements by rank |

## Decision 2026-11-09 (user)
Real occlusion queries break this game's rendering (permanent white/missing meshes, washed-out picture), a known unsolved issue in Hookshot and Lichii. Occlusion stays OFF. TICK-027 refocused on the Castle Town move/turn drops (60 -> ~30 fps) without occlusion.
