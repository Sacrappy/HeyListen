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

## Sync 10 (2026-11-10): Hookshot upstream main 9a2e80e (security hardening)
Branch `feature/sync-hookshot-10` (from main 167182c), 10 upstream commits since 75a676e: bounds on copies, buffer binds and indirect draws (8e6bd49), ReadTexels/WriteTexels bounds, kept-texture answer checks (e0e8acb), heap/page call range checks, OS message queue / calendar / vsync event slots, builder update-tag check (f8b11db), Linux update list, mods romfs link, batched write-back flush on texture eviction, notes/state-list sanity checks.
- Conflicts: `Updater.cs` (upstream's tag-name check kept, cache folder stays `%LOCALAPPDATA%\HeyListenBuilder\update`), `linux_update.py` (download cap kept, User-Agent HeyListen), `release_parts.py` (HeyListen UA, `token = None` kept), `hos_update.c` (upstream's `safe_tag` added, our "off unless HOOKSHOT_UPDATE_CHECK=1" note kept; the repo/tag naming Sacrappy/HeyListen is untouched, the tag check only accepts letters, digits and `._+-`), `vk_menu.cpp` (our FPS counter row with Off/FPS/Full and F10 kept; upstream's `menu_note_frame(game_frame, shown)` signature taken and our `perf_note_frame()` called when a frame is shown), `vk_draw.cpp` (our TICK-008 notes-file header kept, upstream's `note_sane` / `targets_ok` added and applied to each record read; at-most-8-targets checks in `pipeline_key` / `build_graphics` added beside our TICK-008 delay), `vk_texture.cpp` (upstream's `Flush` / `flush_record` / `flush_finish` / batched `flush_textures` taken with our TICK-038 `MemKind::Readback` scope; the eviction loop is upstream's choose-first-then-destroy with our TICK-027 `usual_on` / `overwritten_unused` rules and TICK-023 `keep_rendered` 600 for flushable textures kept; upstream's `pending_bytes` budget drift guard not used, since our `usual_on` is decided once as before).
- Fork rules checked after the merge: frame limit option id 10, F10 key 17, vk_menu rows, HOOKSHOT_STATE_STAMP only for vk_draw.cpp, RX_VK_PIPE_WAIT_FRESH, par_threads, TICK-021/031/036/037/038 vk_memory changes, builder branding (HeyListenBuilder, %LOCALAPPDATA%\HeyListenBuilder) all untouched.
- Tests: run_all 187 checks, 23 skipped; the 12 known failures fail as on main plus test_nup_codec (flaky, failed both runs). One new failure from upstream's new `test_windows_display_startup.py`: its fixture has a global that is set and never read, and llvm-mingw's clang with `-Werror` refuses it; one `(void)thread_arg;` in `tests/windows_display_startup_test.c` fixes it (passes). Builder compiles (Debug, 0 warnings, 0 errors).
- Build: dev-build -Lift (633 s), `--prepare-shaders` (31985 programs, 0 failed). Backup of the previous binaries: E:\OOT\HeyListen\build\backup-sync10.
- **Upstream's lifter change costs fps here and is not taken.** Upstream moved the interrupt polls from the loop headers to the back-edges (`nx2/poll.py`, `rtgen.py`: `a64_poll_cur()`, `-DA64_POLL_CTX` for the DLL). Run to run, same session, run_bench, 110 s, 66 s cut (fps / 1% low / p99 ms): main (old build) kokiri-idle 69.3, 73.0 (mean 71.2) / 34.2, 41.4 / 22.5, 19.4, castle-move 64.4, 67.8 (66.1) / 25.7, 28.2 / 30.7, 27.1. The merge with upstream's poll: kokiri-idle 63.5, 68.3, 66.3, 66.6 (mean 66.2) / 20.1-40.1 / 20.4-29.5, castle-move 60.5, 65.8 (63.2) / 26.9, 29.7 / 33.1, 26.7. The merge with the loop-header polls kept (our `nx2/poll.py`, `rtgen.py`, `build.py` as on main): kokiri-idle 69.4, 69.4 / 36.2, 36.5 / 22.6, 22.0, castle-move 65.8, 64.9 (65.4) / 22.7, 27.4 / 29.3, 28.0. So the security checks cost 0-2 fps (inside the 2-4 fps spread) and the new poll placement about 3 fps standing and 2 walking, most likely the PGO profile (`hookshot_pieces.prof`) no longer matching the functions whose loops changed (not measured separately). Decision: the loop-header polls stay (a note on top of `nx2/poll.py`); take the upstream poll when the profile is recorded again on its code.
- Also: `native_replace: strcmp_805afc70 NOT applied` appears with upstream's poll text (hashes of the regions changed: the polls sit before each backward goto). The replacement is off by default (HOS_NR_STRCMP_805AFC70) and the loop-header lift, kept, applies it as before.
- In game (Market save, windowed, run_bench, limit 360, frame generation off): ground, fountain, trees and the HUD drawn (shots in E:\OOT\HeyListen\build\shots\sync10-move and sync10-final-move). Saves identical to the backup after the runs.

## TICK-040 (2026-11-10): hot set and PGO profile at the Market
- Measured with the job trace and the samples, split by object (`jobtrace.py`):
  - Upstream's hot set already holds 72-78% of the Market job path.
  - Extending it with our scenes' functions and their small callees (2949 functions, our profile merged 6:1 with upstream's) raises that to 93-96%, but the path work barely moves (7.09 -> 6.63 ms standing, 8.08 -> 8.09 walking).
- fps against base, interleaved:
  - extended set: +1.0 standing, -0.8 walking (walking p99 +2.8 ms);
  - -O3: no gain;
  - upstream's back-edge polls with a profile recorded on them: +1.2 / +1.3, inside the spread.
- So the Sync 10 loss of those polls was the stale profile.
- Decision: no default changed; the builder keeps upstream's hot list and profile; the game folder keeps the Sync 10 build.
  - Take the back-edge polls only together with a profile made on that lift.
  - Next lever: the lifted code's context traffic (a lifter project) or the job graph, not compile levels.
- Also: `dev-build.ps1 -Lift` now deletes the earlier lift's chunks first (duplicate symbols after a lift with fewer chunks).

## TICK-041 (2026-11-10): faster lifted guest code
- Measured where the guest code on the Market job chain spends its time, per x86 instruction (skid-corrected timer samples; new `tools/bench/guestmix.py`):
  - guest memory 28%, integer work 25%, FP with the FPX checks 17%, host stack 9-10%, context registers 8%;
  - flags 1.1-1.4%, polls 0.5-0.7%, calls 1.3%; the runtime under it (obj_lookup, soft-FP, locks) ~1.6% of the threads' samples.
  - The code is latency-bound (pointer chasing, store-forwarding stalls). Keeping guest registers in host registers across calls could win a few % at most: not prototyped.
- Found and fixed (opt-in): NEON FMUL / FMLA / FMLS by element and vector FMLA (.4S) were per-lane loops through the host stack, ~16% of the guest samples. `A64_VFPX_ELEM=1` at lift time vectorizes them, bit-identical (new `tests/test_vfpx_elem.py`).
- fps:
  - no PGO profile on either side: +4.4 standing, +4.0 walking;
  - each with a profile recorded on its own lift (6 runs each): +2.1 / +1.7, inside the 2-4 fps spread.
- Decision: not adopted; the switch stays off, so the default lift is byte for byte as on main. Take it at the next profile refresh, together with a profile recorded on that lift (as for the back-edge polls). Game folder: the Sync 10 build restored.

## TICK-042 (2026-11-10): the game's job graph at the Market
- Mapped with a job-marked diagnostic lift (`tools/bench/jobmarks.py`, `hos_jt_job` records in HOS_JOBTRACE) and `tools/bench/jobgraph.py`. A frame: the main thread alone 2.1 ms standing / 3.0 walking (mostly the engine's submit), six BaseProc calc stages each ending in one long per-actor job (Link's calc 1.5-1.7 ms, alone ~0.85 ms; 80166100 and 802ca820 1.1-1.7 ms, all in the same model subsystem f_80168320 > f_81b47080), module-worker work (80083580, which the next frame waits for), and a last stage that only four of six executors join.
- Scheduling losses found: Link's and 802ca820's jobs start late in their stage (#13-17 and #119-124: 0.17-0.45 ms a frame); the last stage misses two executors because their partner threads post them late (f_83acfb90) and the six virtual cores are full.
- Tried (env only): a 16x larger per-thread nn::os object cache for 80083580's ~8,000 mutexes a frame: 68.6 -> 67.7 standing, 66.4 -> 65.2 walking, inside the spread; removed.
- Decision: no default changed; the game folder keeps the Sync 10 build. Next: the submit off the main thread's chain, a job-order patch of the game's scheduler, the model subsystem's FP slow paths, more executors (game patch).

## Sync 11 (2026-11-10): Hookshot upstream main a2e6505 (v2.0.12 / v2.0.13)
Branch `sync/hookshot-11` (from main ea5f68a), 158 upstream commits since 9a2e80e: the Insert menu, FPS counter and start-up screen drawn with Dear ImGui 1.92.9b (GDI path removed), render-target image pool (#1416/#1436, dead targets swept every 10 presents), FSR 3.1, HDR10, a Frame rate cap row, Discord status, Debug tab, keyboard shortcut chooser (#1609), capture/screenshot fixes, builder bounds checks, #1127 (upstream's own retired-mirror inherit rule) and #1434 (CPU write watches kept during pool imports), compute list (hookshot_computes.bin), GPL cold libraries, upload budget, shadow PCF.
- Conflicts (20 files):
  - `vk_memory.cpp`, `mirror_inherit.h`, `test_mirror_inherit.*`: #1127 is TICK-036's page rule; upstream's header, test and its extra rule (a retired pool synced in this submission is inherited whole) taken; our `made_epoch`/`made_seg` later-segment rule, the kept/newer stats, `RX_VK_INHERIT_ALL` and `RX_VK_LOG_INHERIT` watch kept (test checks both). TICK-037 streaming capture copy, TICK-038 resource code untouched.
  - `vk_menu.cpp`: upstream's ImGui menu taken. Kept: Frame rate limit row (option id 10), FPS counter Off/FPS/Full with F10 at key index 17, title/version "HeyListen", wordmark HEYLISTEN. Upstream's Frame rate cap row (frame_rate_cap=, read at start) not taken: same feature as ours; `nn_vi.c` keeps our live limit, `HOS_FPS_MAX` is read as `HOS_FPS_LIMIT`, the vsync thread stays at 60 Hz as in Sync 10.
  - Our performance overlay (`vk_overlay.cpp`) had its own ImGui 1.91.9 copy and context: ported to draw inside upstream's ImGui frame (its Roboto font added to upstream's atlas, its style swapped in and out); `gpu/vk/imgui` removed (two ImGui copies would clash), Roboto kept in `gpu/vk/overlay_font`.
  - `vk_texture.cpp`: upstream's target image pool taken; TICK-031 spare images kept for the images it does not pool (compressed, not made for a target); TICK-027 overwritten-target rule kept with upstream's 10-present dead sweep; TICK-030 dedicated big BC images kept (`vk_vma.cpp`: our `create_image_` wrapper takes upstream's `target` pool flag).
  - `vk_draw.cpp`: TICK-008 kept (the pipeline is noted and linked on the worker, not the draw's thread) with upstream's cold libraries handed to workers after it; our notes file format (TICK-008 NoteHead) kept, upstream's 16-byte-head `notes_open_locked(known)` removed; upstream's state list opened before play taken; upstream's compute list used only when the TICK-038 lazy compute start is off (default unchanged).
  - `vk_bank.cpp`/`vk_state.h`: upstream's sampler slot versions (TICK-007's, taken upstream) replace ours. `vk_replay.cpp`: frame stats merged, upstream's present-geometry trace, option id 10 kept. Smaller: `vk_main.cpp`, `vk_internal.h`, `vk_draw.h`, `run_all.py`, `release_parts.py` and `linux_update.py` (HeyListen UA; upstream's `Request(_https(url), url, ...)` passed the URL as POST data: fixed), builder `Strings.cs` (HeyListen texts, upstream's 45 GB).
- Also: F10 comes to the window as WM_SYSKEYDOWN, so with the queued key presses it never reached the menu; `display.c` now notes it (and returns, no window-menu mode). Memory warning box captioned HeyListen.
- Polls: upstream did not touch `nx2/poll.py` / `rtgen.py` since Sync 10; our loop-header placement stays. `build.py`: upstream's changes taken (ImGui/zstd/fonts objects, `--dry-run`, compiler memory, no gdi32), with our ImGui lines removed and `vk_overlay.cpp` added to the ImGui users. Lifted code changed only by upstream's `oot_jitter` sites now always lifted (one function, no effect unless DLSS/FSR runs): the hot pieces were recompiled; PGO profile is still upstream's.
- Build: dev-build -Lift; shader stamp changed (`vk_shader.cpp`, `vk_api.h`, `vk_state.h`, `vk_draw.h`, `vk_bank.cpp`): `--prepare-shaders` run (2751 graphics pipelines made, 0 failed). Backup of the Sync 10 exe, dll and prepared files: E:\OOT\HeyListen\build\backup-sync11.
- In game (Market save, windowed, run_bench, limit 360, frame generation off): Market drawn correctly with the Full overlay (E:\OOT\HeyListen\build\shots\sync11-check\win\t78.png), Insert menu opened by RX_VK_MENU_SCRIPT (win2\t87.png: HeyListen title, Frame rate limit row, overlay under it). Saves identical to the backup after every run.
- A/B, interleaved, each build with its own prepared files, 3 rounds, 110 s, 66 s cut (fps / 1% low / p99 ms):
  - kokiri-idle: Sync 10 69.8, 72.0, 70.0 (mean 70.6) / 34.7, 36.2, 42.9 / 23.2, 22.2, 19.8; Sync 11 74.3, 72.3, 73.9 (73.5) / 53.0, 49.9, 54.5 / 17.7, 18.2, 17.2.
  - castle-move: Sync 10 66.0, 65.0, 60.7 (63.9) / 29.7, 29.8, 26.4 / 29.5, 29.0, 33.3; Sync 11 66.5, 67.7, 68.3 (67.5) / 28.1, 35.6, 35.2 / 22.6, 23.2, 22.8.
  - No loss: +2.9 / +3.6 fps, about the spread, and p99 -4 to -7 ms, 1% low standing +15 (not attributed per change; likely the target pool and upload budget).
- Tests: run_all 214 pass, 12 fail, 33 skip: the 10 known failures plus the flaky test_nup_verify / test_nup_codec; none fixed upstream. Four new failures fixed by adapting checks to the kept code (test_gpl_cold_library, test_target_image_pool, test_pipeline_notes_open rewritten for our notes format) and F10 in two fixtures (test_menu_keys, test_screensaver). test_mirror_inherit, test_capture_stream_copy, test_vfpx_elem pass. Builder not compiled (no .NET SDK here).
- Game folder: this build.

## TICK-043 (2026-11-10): the engine's submit off the main thread
- Measured (default build, samples): the main thread's submit is 1.8 ms a frame standing and 2.4 walking.
  - The capture copy is ~1.0 ms of it, the mark scan 0.23, take_capture 0.2-0.5 (a GPU wait for late pages 0.1-0.2), memory_collect_written 0.06-0.15.
  - The overlay's "GPU frame prep" is the replay summed over all threads, mostly the Presentation Thread's, off main's path.
- Tried, both behind env switches (off):
  - `RX_VK_CAPTURE_PRECOPY=1`: a thread copies the written pages ahead of the submit; the submit copies only the pages written or written back since (`capture_merge.h`, new `tests/test_capture_merge.py`). It took ~0.6 ms a frame off main, but fps fell: 75.4 -> 71.4 standing, 68.8 -> 66.3 walking (2 ms period: 73.3 / 67.2). The copy thread costs the job threads more than main gains.
  - `RX_VK_CAPTURE_SCAN_SPLIT=1`: the scan split over the copy helpers. 73.0 / 69.8: inside the spread.
- Correctness: Market shots of the default build alike, standing and walking; game speed ratio ~60. Tests: 215 pass, 12 fail (known and flaky), 33 skip.
- Decision: no default changed; the game folder keeps the Sync 11 build. Next: fewer capture bytes (bindings' read sets), the late pages' recopy on the GPU, a flat table for the texture shadows; the game's job chain stays the larger lever.

## TICK-044 (2026-11-10): the FP slow paths of the serial actor jobs
- Measured (probe build): ~5.4 M out-of-line FP calls a second walking, ~3.3 M standing. Quiet-NaN lanes in vector FADD / FSUB / FMUL (59%), FRSQRTE / FRECPE (33%), FDIV by zero with DZC raised (6%); not denormals or FPCR modes. Mostly the model subsystem under 80166100's job.
- Changed, behind lift-time switches (off; lifted code unchanged): `A64_FPX_SLOWPATH=1` (per-lane fallback with quiet-NaN lanes in the cold `_lanes` helpers, NaN / FDIV rescues in the scalar wrappers, an estimate table fast path; exact bodies 3.6 M -> 0.09 M a second), `A64_FPX_FMA3=1` (scalar binary32 FMA by FMA3). Bit-identical: new `tests/test_fpx_slowpath.py`.
- A/B, 6 runs each: walking 69.1 -> 70.0 / 71.1, standing 76.1 -> 74.7 / 75.0: inside the spread.
- Decision: not adopted, no default changed; the game folder keeps the Sync 11 build. The job start order (part 2) was not prototyped. Next: the start-order patch; ship the FP switches with a matching profile at the next refresh.


## TICK-045 (2026-11-10): the three lift switches with a profile recorded on their lift
- Built C: `A64_VFPX_ELEM`, `A64_FPX_SLOWPATH` and `A64_FPX_FMA3` together, with a profile recorded on that lift (TICK-040 method: gen build, our three scenes, 6:1 with upstream's). Also built B: the base lift with a profile recorded the same way. Upstream's hot list and link order kept.
- A/B, 6 interleaved runs each, against Sync 11 (A):
  - C: standing 75.3 -> 78.0, walking 68.1 -> 72.9, the street 70.9 -> 74.4. Walking p99 28.7 -> 22.3 ms.
  - B: inside the spread (+1.9 / -0.6).
- Decision: C adopted.
  - The switches are on by default in the lifter, with `=0` to opt out.
  - The profile lives in the repository under `profile/`, with no game data. dev-build.ps1 installs it, and the release package takes it.
  - The game folder runs the new default build; Sync 11 is backed up in `build/backup-tick045`.
- Next: the job start order; inline FRSQRTE / FRECPE and the quiet-NaN test, each with a profile refresh.

## TICK-047 (2026-11-11): the game's long jobs first
- Found: the BaseProc scheduler has no priority; a stage's jobs start in actor-list order (lock-free rings per level, FIFO). Link's calc starts ~15th of its stage and ends it alone for ~0.7 ms.
- Built: hooks in f_8004ab40 / f_8004fa40 that put a long job (>300 us) in its level's A ring, taken before the waiting jobs. Behind `OOT_JOB_ORDER=1` (lift time) and `HOS_JOB_ORDER=1` (run time); the PGO records still match.
- Chain: Link's calc #13-17 -> #5, alone 0.71-0.74 -> 0.25-0.29 ms.
- A/B, 6 runs each: 76.3 -> 77.4 standing, 71.6 -> 71.8 walking, 74.4 -> 73.4 on the street: inside the spread.
- Decision: not adopted; the default lift is as before; the game folder keeps the TICK-045 build. Next: shorter jobs (80166100, 800b73d0) or the main thread's serial part.

## TICK-050 (2026-11-11): replay segments in one round, the Presentation Thread recording one
- Instrument added (on): the parallel replay line now gives the slowest and fastest segment, the rounds, and the Presentation Thread's wait and own recording; a new per-place line. Fine profile (kokiri-idle): descriptors 4.23 ms a frame, textures 2.08 of it.
- Found: the slowest segment is ~1.25 ms whatever the count; the second round was not the wait. A parallel draw costs ~5 us against ~3.6 alone, and a command buffer's first draws cost ~3x its last ones.
- Built, behind switches (off): `RX_VK_PARALLEL_ROUNDS`, `RX_VK_PARALLEL_SELF` (=1, =2), `RX_VK_PARALLEL_BALANCE`, `RX_VK_PARALLEL_POINT`; `segment_plan.h` + `tests/test_segment_plan.py`.
- Screen (castle-move, 3 runs each):
  - frame prep falls 7.76 -> 7.62 (ROUNDS+SELF) and -> 7.35 (+BALANCE); ROUNDS alone 8.43 (no gain);
  - fps inside the spread.
- Decision: not adopted (~0.4 ms, not the 1.5 ms needed; acceptance <= 5.5 ms not reached). The game folder keeps the TICK-045 build.
- Next: the sampler on the workers (why concurrency costs), then design 2 (textures first), then the dry pass overlapped with segment 0.

### TICK-048 on the main PC (2026-11-11)
- Diff on loses here: castle-move 70.5 -> 62.9 fps (3 runs each), frame prep +3.2 ms; the bytes it saves cost only 0.4 ms of GPU on Gen5 x16.
- The default now follows the card's maximum PCIe link (NVML): on at <= 8 GB/s (PC 3050 Gen3 x4), off otherwise. Tests fixed (2 broken by TICK-048). Game folder holds this build. Branch `verify/TICK-048-mainpc`, not merged. See docs/benchmarks/2026-11-11-tick048-mainpc.md.

## TICK-051 (2026-11-11): frame-time spikes, PC 3050
- Instrument (off by default): `RX_VK_FRAME_TRACE=1` per-frame trace with late-page wait, GPU, driver, ASTC, textures made, scatter, VRAM; `tools/bench/spikes.py` classifies spikes (> 2x median).
- Found: walking spikes = the game's thread waiting (late pages) for GPU work of ASTC transcodes (1.2-1.8 ms each on the 3050, 30-60 a frame); 82% re-transcode textures the eviction dropped. Standing: no spikes.
- Built, on by default: `RX_VK_ASTC_BATCH` (8 pieces a group, 3 barriers a group) and `RX_VK_BC_CACHE_MB` (1024: BC blocks kept in system memory by ASTC bytes).
- A/B (5 runs, main b17b933): castle-move 1% low 10.8 -> 14.0, p99 75.8 -> 58.1 ms, spikes 8.6 -> 2.4; standing and Hyrule inside the spread.
- Found on the way: main b17b933's NVML link check reads Gen3 x16 on PC 3050 (slot is x4): the chunk diff is off there (-3 to -4 fps).
- Branch `feature/TICK-051-stutter`, not merged; main PC not checked.

## TICK-052 (2026-11-11): why parallel replay draws cost more
- Found (sampler with waits counted, new `RX_VK_PARLOCK_STATS`): ParLock is 10-14% of the workers' time, ~1,050 takings a command buffer; caches stale for the wrong reasons (pool table aliasing, every eviction voiding all views and slots) feed it; a segment's first 32 draws cost ~7 us. Driver 20%, program pick 14%.
- Built, off by default: `RX_VK_GONE_RING`, `RX_VK_TL_POOLS_WIDE`, `RX_VK_PARLOCK_SPIN`, `RX_VK_PARALLEL_AFFINITY`, `RX_VK_PARALLEL_IDEAL`; split per-thread counters always on.
- Confirm (29/36 runs, stopped for the next ticket): draw CPU -5..8%, frame prep -0.21 / +0.11 / -0.11 ms, fps inside the spread.
- Decision: not adopted. Game folder: main build restored. Next: the program pick's slow path (needs --prepare-shaders), the touches ahead of segments.

## TICK-053 (2026-11-11): the CPU's per-frame wait for the GPU, PC 3050
- Found: the frame is serial (CPU ~13 ms of GPU-idle + GPU ~22 ms = 35 ms): the game's thread drains the GPU at its first submit (the late-page wait) and records the frame while the GPU idles. GPU 63% busy.
- Found: without the wait, 24-26 ms frames (40 fps) for a while, then a mode of 66 ms frames: the queue idles 25-50 ms a frame between our own recordings while both game threads wait for the GPU. Not the acquire, present mode, present queue, blit, VRAM, clocks or event ordering. Not placed; session 0 (SSH) may be part of it.
- Built, behind switches (off): `RX_VK_LATE_MERGE` (late pages merged word by word against a shadow, no wait), `RX_VK_ACQUIRE_FENCE`; on: GPU timestamps for uploads, the queue's gaps and the blit (`gpu_up_ms`, `gpu_gap_ms`).
- Screen (castle-move, 3 runs each): main 28.21 fps, branch defaults 28.64, `RX_VK_RB_RANGES=1` 27.97 (1% low 14.4 / 14.9 / 13.8): no gain, RB_RANGES stays off. Merge shots show white world-less frames; speed ratio ~60.0 on the default. docs/benchmarks/2026-11-12-tick053.md has the desktop-check instructions (`RX_VK_LATE_MERGE=1` against the default).
- Decision: not adopted. Branch `feature/TICK-053-gpu-sync`, bundle `E:\OOT\tick053.bundle`, not merged. Next: the queue's gap on the desktop, then the overlap.

## TICK-055 (2026-11-11): the lifted code on x86-64
- Audit (`docs/research/lifter-x86.md`): guest loads 33.5% of the guest samples, integer 22.9%, FP 18.2%, context 8.9%; lifter overheads ranked: GPR <-> XMM crossings of scalar FP (5.6%), FPX gate re-reads (~1.6%), dirty marks (~1.7%), flags, LR store-forwarding at function entry (~0.9%), calling convention (~1.1%). Guest addressing, atomics, barriers already minimal.
- Built, lift time, off by default (default lift byte-identical): `A64_FPX_XMM` (scalar FP, S/D/Q loads and scalar results in XMM registers) and `A64_LR_SCALAR`; `tests/test_fpx_xmm.py` (bit-exact, 1 M runs a -march, teeth), `tools/bench/lifter_mb` (synthetic: -13% to -61%).
- PC 3050, both builds unprofiled: guest CPU ~21.9 -> ~22.3 ms a frame, castle-move 29.0 -> 28.3 fps (3 rounds): inside the spread. Not adopted.
- Branch `feature/TICK-055-lifter-x86` (main 8ece4b3 merged), not merged. Next: guest registers across calls (the loads are the critical path); large pages need the user's privilege.

## TICK-054 (2026-11-12): the frame's critical path, and the switches that shorten it
- Instrument: `HOS_JOBTRACE` marks for submits, presents, held submits, the GPU's progress and the renderer's steps (capture, take, dry pass, parallel setup, segment waits: `hos_jt_mark_ext`); `tools/bench/frameline.py` (per-frame gantt) and `tools/bench/chainb.py` (the loop); tests.
- Found: base = two chains ending ~1 ms apart (main thread / Presentation Thread). With `HOS_NVN_HOLD=2` (the main thread's submits replayed by a host thread) one serial loop: present -> main draw code 3.0 -> held flush 2.1 -> PT replay 7.8 -> present 0.6. The GPU is never waited for.
- Tried and not kept: the capture pre-copy kicked once a frame (also with a 12 MB buffer), HOLD=3 (capture at the submit, replay held), thread priorities.
- Confirm (6 rounds, 4-6 reversed): kokiri-idle 72.3 -> 76.1, castle-move 68.3 -> 70.2, hyrule-walk 69.9 -> 72.1 fps; 1% low 45.3 -> 47.3, 31.8 -> 31.2, 26.7 -> 36.4.
- Adopted (on by default, `=0` off): `HOS_NVN_HOLD=2`, `RX_VK_PARALLEL_ROUNDS`, `RX_VK_PARALLEL_SELF=2`, `RX_VK_PARALLEL_BALANCE`, `RX_VK_PARALLEL_POINT=32`, `RX_VK_PARLOCK_SPIN=4000`, `RX_VK_TL_POOLS_WIDE`, `RX_VK_GONE_RING`; `RX_VK_PARALLEL` 5 at 16+ threads.
- Shots alike, speed ratio ~60, saves identical, run_all 235/2 (nup)/35. Not checked on PC 3050. Game folder holds this build. Branch `feature/TICK-054-cpu-frame`, not merged.
- Next: the mirror pre-sync off the PT's path, segments started during the dry pass, a smaller held-flush capture.

## TICK-057 (2026-11-12): GPU time a frame on PC 3050
- Found: unprofiled, 19.4 ms of recordings + 2.6 ms of uploads, ~15 ms of it busy with no gap before each late-page wait (new
  `GPU tail` line; the parallel segments are now timed, the trace's gpu_ms read 5 ms without them). The engine's own GPU work is
  small; pass timing inflated it. Mirror churn at 84-90% VRAM: 101 of 111 mirrors given back came back within 6000 submissions.
- Built, off by default: `RX_VK_MIRROR_STICKY`, `RX_VK_MIRROR_VRAM_FREE`, `RX_VK_BARRIER_DEDUPE_PAR`, `RX_VK_BARRIER_TRUST`,
  `RX_VK_GAME_BARRIER_NARROW`, `RX_VK_TEST_BARRIER_DROP` (measurement only); `tests/test_gpu_time_057.py`.
- Screens (castle-move, 3 runs): every switch inside the spread (28.9-29.5 fps against 28.95-29.52); the barrier-drop bound
  31.47 against 28.95 (gpu_ms -2.8, late wait -2.3): the game's own barriers' drains, which narrowing does not shorten here.
- Decision: nothing adopted. Game folder: main32d. Branch `feature/TICK-057-gpu-time`, bundle `E:\OOT\tick057.bundle`, not merged.
  Next: fewer game barriers (disjoint targets, split barriers) under sync validation; the serial frame's 12 ms of queue gaps.

## TICK-056 (2026-11-12): the Presentation Thread's part of the frame's loop
- Found: the mirror pre-sync (0.78 ms of the loop) is one job a frame of ~8,900 dirty pages among ~105,000 looked at (0.86 ms): its per-page work, not its place. Run on a worker beside the dry pass it does not fit (dry pass ~0.25 ms).
- Built, off by default: `RX_VK_MSYNC_FAST` (the sync's walk and write-back notes cut, same copies: pre-sync 0.86 -> 0.45 ms), `RX_VK_PRESYNC_EARLY` (not kept), `RX_VK_SEG_EARLY` (segments handed out as the dry pass passes their planned ends; the PT's segment waits 0.71 -> 0.28 ms; plans used / no boundary reached / rejected logged), `RX_VK_PRESYNC_STATS`; chainb marks; tests.
- Confirm (FAST + SEG_EARLY against base, 6 rounds): kokiri-idle 74.7 -> 76.6, castle-move 69.9 -> 70.2, hyrule-walk 72.7 -> 72.4 fps; frame prep -0.86 / -0.94 / -0.58 ms. Inside the spread, not above base in every round.
- Decision: not adopted; the game's own chain (main thread) is the limit again. Shots alike, speed ~60, validation clean of new VUIDs, saves identical. Game folder: TICK-054's build. Branch `feature/TICK-056-chain-b`, not merged.
- Next: the main thread's chain (draw code, job group, module phase), then the held flush's capture (pre-copy aimed at the held submits).

## TICK-058 (2026-11-12): the game's own chain: hand-offs, and the main thread's fence poll
- Measured: the host's hand-offs are not the limit. Wake latency p50 6.4 us, p90 10 (`tools/bench/wakelat.py`); on the game chain's critical path (`tools/bench/critpath.py`) wakes, virtual-core waits and poll lag ~0.3-0.5 ms a frame (2-3%); no timer-granularity path; 80083580's lookups ~0.4 ms on the path; priorities kept (the virtual cores' Horizon order).
- Found: the main thread's two nvnSyncWait polls a frame wait 1.42 ms for the renderer's `sync_order_mu`, held by `submit_q` / `present` for whole replays. `RX_VK_FENCE_POLL_FAST` (off) answers them without it: +1.4..+6 fps in screens, +3.0 / +1.5 / +1.9 in a confirm (16 of 18 rounds above), but the Market's exposure blew out to white. The lock also ordered the exposure readback after the replay's write-back registrations. With that wait moved to the readbacks (the picture right), castle-move 69.6 -> 63.2.
- Decision: nothing adopted; the switch stays off. Game folder: TICK-054's build. Saves identical after every run (103 runs), no hang.
- Next: the readbacks' ordering without the replay's whole lock (per-range write-back state published at a replay's start): ~1.4 ms off the main thread's chain, then MSYNC_FAST + SEG_EARLY.

## TICK-059 (2026-11-12): the readbacks' order without the replay's whole lock
- Found: every write-back registration is made inside a `sync_order_mu` section, and a submission's write set is complete only at its recording's end (`finish_recording` at `burst_end`): a readback that keeps the locked poll's order must wait for the end of the section the poll would have waited for, not earlier.
- Built, off by default: `RX_VK_READBACK_TICKET` (the counted `sync_order_mu`; the fast poll keeps the section in progress as a ticket; `invalidate` waits for that section only), `HOS_NVN_POLL_AWAY` (the first look without the virtual core); `tests/test_readback_ticket.py`.
- Measured: shots right (standing luma 102-105 against 102-106), speed 60.0; the main thread's wait moves from the poll (0.80 ms a frame) to its first readback (0.97 ms), with the virtual core given up there; module workers wait now too. Screen castle-move 68.7 -> 70.1; confirm rounds 1-3 73.9 / 66.5 / 63.3 -> 73.6 / 67.7 / 69.3, above in 5 of 9: the bar out of reach, stopped.
- Decision: nothing adopted. Game folder: TICK-054's build. Saves identical after every run (34), no game left. `HOS_NVN_POLL_AWAY` once stayed on the title (not pursued).
- Next: the capture out of `sync_order_mu` (shorter sections), a ticket that ends at the submission's `finish_recording`, a per-thread ticket for the workers.

## TICK-062 (2026-11-11): host-layer overhead on chain A
- Five switches, all off: HOS_CV_SIGNAL_FAST, HOS_THREAD_OF_CACHE, HOS_SHORT_SLEEP_PAUSE, HOS_OBJ_PACK, HOS_CORE_COUNT (counters). B9 and A7 analysed, not changed.
- Counts: only OptimizeChangeBanc (225k) and SceneLowPriority (61k) call GetCurrentCoreNumber after their start; svc 0x10 never.
- castle-move screens (3 rounds): B1 +2.4, B4 +1.7, B2 +0.8, pack -1.0 fps, stack +1.5: inside the spread; chain A traces not separable. Nothing adopted; shots alike, speed ~60.
- Next: the module workers and NavMeshCAStepDepWorker1 virtual-core waits.

## TICK-060 (2026-11-12): PC 3050's idle GPU third
- Found (unprofiled: frame trace, nvidia-smi/dmon, job trace): the GPU idled 12.8 ms of a 35 ms castle-move frame, not for PCIe
  (0.54 of 3.9 GB/s) or uploads: the held flush waited ~8 ms for the previous frame's late pages holding the replay's lock, then the
  Presentation Thread needed a 5.8 ms mirror pre-sync (~4 ms of it TICK-048's diff re-hashing capture buffers from DRAM) before the
  GPU had the frame's first big command buffer.
- Built: `RX_VK_LATE_DEFER` (the late-page wait moved from the take to the queue: recordings held until a fix thread applies the same
  copies at the value's end; CPU readers of a pending slot wait or copy whole), `RX_VK_CAPTURE_HASH` (the capture's copy makes the
  diff's hashes in the same pass, bit-exact); tests; trace column `late_fix_ms`.
- Screen (castle-move): base 28.09, defer 29.80, + hash 33.19, + MSYNC_FAST 33.83. Confirm (6 rounds, 18 of 18 above base):
  kokiri-idle 30.85 -> 37.24, castle-move 29.13 -> 34.40, hyrule-walk 31.35 -> 37.20 fps; 1% low and p99 better; shots alike,
  speed ~60, saves identical after every run.
- Adopted (on by default, =0 off): LATE_DEFER, CAPTURE_HASH, and MSYNC_FAST where the chunk diff is on. Main-PC check pending.
  PC 3050's game folder: build t60e (exe 17c3e4fb, dll 2dc76b85); main32d kept in `C:\OOT\builds\main32d`. Branch
  `feature/TICK-060-pc3050-gpu`, bundle `E:\OOT\tick060.bundle`, not merged.
- Next: the CPU recording chain (SEG_EARLY, RX_VK_PARALLEL=5: +1 fps in a screen), then the GPU's ~22 ms at 1440p.
- Main-PC check (TICK-060m, branch `feature/TICK-060-main` on main 43cc30f): review found the late fixes' queue assumed rising late
  values (a flush could send recordings reading unfixed slots), a CPU texture upload over a pending late page could read guest
  memory before the write-back, and a replay read could differ from the slot the GPU reads: all fixed, tests
  (`test_late_fix_order.py`). A/B on the 7800X3D + RTX 5070 (only the deferral differs there): screen 75.65 -> 77.54, confirm
  kokiri 78.3 -> 79.4, castle 77.0 -> 76.9, hyrule 75.1 -> 77.4 fps, hitches alike, shots and speed right: does not lose.
  Recommended: merge with the defaults on, with the fixes.
