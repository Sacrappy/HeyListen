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
