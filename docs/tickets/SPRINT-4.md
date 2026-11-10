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

## Sync 9 (2026-11-10): Hookshot upstream v2.0.11 + main 75a676e
Branch `feature/sync-hookshot-9` (from main c7311f6), 120 upstream commits since 09e4eb5: v2.0.11 (write-back diff on for integrated GPUs, integrated-GPU memory caps, builder drops objects after a failed build), DLSS (opt-in: only with `HOOKSHOT_NGX_SDK` / the builder's tick; not built here), HD rumble, Sharpness (RCAS-style), restart-needed rows and page, texture phi indices, pipeline cache save fixes, lift mark groups, #994 task/mesh inline global load, #1184 no mirror made inside a parallel segment.
- Conflicts: `vk_menu.cpp` (upstream's new Row layout with hints/restart: our Frame rate limit row kept, preset-exempt, after NVIDIA Reflex; our Off/FPS/Full counter with F10 at key 17; upstream's Render resolution replaces the old Resolution row; title HeyListen + upstream's Restart needed page; upstream's GDI counter stays out, the ImGui overlay draws it), `vk_replay.cpp` (upstream's DLSS options 24-26, our frame rate limit stays id 10: upstream uses 0-9, 16-26, 32-35), `nx2/build.py` (imgui include and HOOKSHOT_STATE_STAMP for vk_draw.cpp kept, NGX flags for vk_dlss.cpp added), builder (`--work` always passed, upstream's DLSS fetch added; texts say HeyListen).
- #1184 vs TICK-036: both kept. Upstream no longer makes a mirror inside a parallel segment (`!par`); our `made_seg` later-segment rule and the `mirror_inherit.h` page rule stay. In all runs below the log shows "0 retired mirrors left out": ours is now a safety net, the page rule still applies (31.7 MB CPU-newer pages taken from guest memory at the Market load).
- Tests: run_all 151 pass, 10 fail, 23 skip; the 10 failures fail identically on main c7311f6 (pre-existing). test_mirror_inherit, test_parallel_mirror_make, test_shader_stamp_cache, test_menu_restart, test_menu_hit, test_rumble_menu pass.
- Build: dev-build -Lift (653 s). Shader stamp changed (vk_shader.cpp, vk_api.h, Eden texture_pass/spirv_emit_context): `--prepare-shaders` run (173 s, 31985 programs, 0 failed). Backup of the previous binaries: E:\OOT\HeyListen\build\backup-sync9.
- In game (Market save, windowed, run_bench): ground, fountain and trees drawn (shots build/shots/idle, move, hyrule in the worktree). fps / 1% low / p99, 60 s cut (TICK-036 in brackets): kokiri-idle 110 s 66.1 / 14.0 / 26.1 ms (67.3 / 9.9 / 26.9); castle-move 110 s 62.1 / 12.9 / 30.0 ms (63.6 / 26.7 / 28.6). The 60 s cut includes the scene-load hitch when the load ends late (it lands at 56-63 s); after the load (66 s cut, both builds): kokiri-idle 68.8 / 35.7 / 22.0 (67.7 / 32.9 / 22.4), castle-move 64.2 / 27.2 / 27.0 (63.4 / 25.9 / 29.1), hyrule-walk 65.2 / 28.4 / 26.3. No regression. Saves identical to the backup after every run.

## TICK-037 (2026-11-10): what holds the frame after Sync 9
- Attribution at the Market (limit off, frame generation off): the game's CPU chain holds the frame; the engine's largest piece on it was the main thread's submit-time capture copy (~1.1-1.3 ms a frame, bandwidth-bound). GPU ~8-10 ms (next limit); replay and fence waits are not limiting. Decision: the capture copies with streaming stores, default on (`RX_VK_CAPTURE_STREAM=0`: memcpy): +2.9 fps standing, +3.3 walking, +2.4 on the street walk, 1% lows unchanged. Two-stage capture tried and dropped (the capture is taken right after it is made). Next lever: the guest job chain, then the GPU.

## TICK-033 (2026-11-10): real occlusion counts after #994
- Measured with real Vulkan occlusion queries (`RX_VK_OCC_REAL`, new; logs with `RX_VK_OCC_LOG`). #994 did not matter: the game's ~900 proxy test draws a frame are plain draws and their counts are right (H1 ruled out). What breaks the picture is timing (H2): each render condition reads a report from the same frame, and report addresses change every frame, so any one-frame-late value is another object's count (mode 2 reproduces the washed-out mint/white picture with missing buildings: `build/occ/late2-idle/shots`).
- Same-frame counts (mode 4: the replay waits for the GPU once a frame, at the first condition) give a correct picture at the Market standing (`build/occ/par4-idle/shots`, `build/occ/sync4-idle/shots`) and walking/turning (`build/occ/move-m4/shots`), the same as the fixed value (`build/occ/def-idle/shots`, `build/occ/move-m0/shots`); ~350 hidden objects a frame are not drawn.
- But slower: kokiri-idle 61.7 / 33.6 / 24.2 ms (fps / 1% low / p99, 66 s cut) against 70.2 / 34.8 / 20.8 with the fixed value; castle-move 59.7 / 24.8 / 30.0 against 65.4 / 25.1 / 29.0. GPU time (serial replay, where every submission is timed): 8.5 ms a frame fixed against 8.1 ms with mode 4 (-0.4 ms; serial fps 63.9 against 57.1). The GPU saving is small and the GPU is not the limit; the wait costs the CPU chain ~2 ms.
- Decision: the fixed value stays the default (the 2026-11-09 decision stands, now for performance, not for correctness); `RX_VK_OCC_REAL=4` stays as the proven-correct instrument. Game folder: this build (default path identical to TICK-037: 70.2 / 65.4 fps against TICK-037's means 70.3 / 66.3).

## TICK-038 (2026-11-10): resource use
- Measured at the Market (RAM by address-space scan with residency, VRAM by allocation kind, CPU by thread): the precompile's 7808 compute pipelines and their modules held ~2.2 GB of the NVIDIA driver's memory, ~110 of them used. Decision: compute programs load and build at first dispatch when the pipeline cache is warm (`RX_VK_LAZY_COMPUTE=1`), their modules let go after (`RX_VK_CS_MODULE_DROP=1`): working set -1.9 to -2.3 GB (14.2 -> 11.9 GB standing), start-up shaders 8.5 -> 2.8 s, fps / 1% low not worse (standing +1.7 fps). Total VRAM unchanged within noise; render targets, mirrors, BC scratch and write-back shadows kept (each holds a past picture fix or a perf feature). Game folder: this build.

## TICK-039 (2026-11-10): the job-group wait
- Reconstructed with a new trace (`HOS_JOBTRACE`, `tools/bench/jobtrace.py`): the main thread's job-group wait (7.0 ms standing, ~8 walking) is ~97% job execution on the chain (80-85% of it guest code), ~3% wake latency, < 0.5% virtual cores / polls. Spin 0/5/100 us and fixed affinity were all slower; more virtual cores change nothing (six executors are the game's). Decision: no default changed; next levers are the game's job graph (serial tail, more executors via a game patch) and guest-code speed, not the host's sync. Game folder: this build (same fps as TICK-038).
