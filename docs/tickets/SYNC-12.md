# SYNC-12: Hookshot upstream main 15646d1 (643 commits since a2e6505)

Branch `sync/hookshot-12` (from main 43cc30f). `git subtree merge --prefix=engine upstream-hookshot/main`
(upstream-hookshot already fetched). 35 files conflicted; the merge commit has the resolution, a second commit adapts
the fork's code and the tests to what upstream changed around them. Not merged into main, not pushed.

## What upstream brings (the ones asked for, all in)

| upstream | what | here |
|---|---|---|
| #2383 perf(fpx) | single-precision FMA, FRECPS and FRSQRTS fused on x86-64-v3 (`nx2/suyu_rt_fixes.py`) | in. TICK-044's `A64_FPX_FMA3` already fused `recomp_fpx_fma32` the same way: `suyu_rt_fixes.patch` leaves a guard that is already fused alone, and fuses the two others (FRECPS, FRSQRTS: new for us). The lift changes, so the PGO profile was recorded again (below). |
| #2439 | `texture_answer` asks a flat table by slot | in (no conflict) |
| #2420 | texture spans aged once a present | in (no conflict) |
| #2419 | a fence wait covers every part of a split submission | in; see "#2419 and TICK-058/059" |
| #2263 | the game's short sleeps on the high-resolution timer with 6 logical processors or fewer | in; see "Short sleeps" |
| #2387 | `hos_vcore_back` ends an escape | in (no conflict) |
| #2323 | prepare: fewer predicted pipelines, pools sized to the usable CPUs, translated programs kept across cache-code changes | in; our background precompile (TICK-008) uses `usable_cpus()` now |
| #2490 | a recorder packet bigger than its room is made directly | in (no conflict). The recorder (`RX_VK_RECORDER`) is off by default and refused with the parallel replay; every Vulkan command now goes through its `rec::rx_*` packer (one relaxed load and a branch) |
| DLSS / FSR | DLSS frame generation and DLAA mip bias, FSR 3.1 as a temporal-chain engine, FSR 4, reactive masks | in (code). DLSS is only built with `HOOKSHOT_NGX_SDK`; not built or run here |
| `docs/cpu-profile.md` | "where the CPU goes" | read; summary below |

Other upstream work that comes with it: VRAM compaction (`RX_VK_COMPACT`, on by default), the texture pool of its
own, the Audio tab (volume, mute in background), HDR contrast, Camera speed, Text size, AMD Anti-Lag 2 row, menu
redesign of the Debug / Inventory / Teleport pages, the >60 fps step through the frames' own time (`hos_frame_seconds_bits`,
water ripples, pick-up counter, cutscene cues), the portable Builder home, the release tracks (release / rc / nightly),
the pipeline-skip rule that waits for draws whose shaders write memory, `env_on` for every on/off switch, the per-test
`RUN_ALL_*` declarations of `tests/run_all.py`, many fixes.

## Conflicts and how they were resolved

HeyListen keeps its features and defaults (TICK-054's combo on, every off switch kept); upstream's fixes and features
are taken. Where both changed the same logic:

### gpu/vk
- **vk_work.h** (TICK-016/057 counts by command buffer vs upstream's recorder): every macro keeps our count
  (`note_work_cmd(c_)`, `note_other_work`, `note_xfer_work`) and calls upstream's packer `rec::rx_<name>`;
  `vkBeginCommandBuffer` keeps our count and gets upstream's recorder boundary. Upstream's set/bind macros and boundaries
  taken as they are.
- **vk_memory.cpp**: the capture copy helpers. Upstream replaced the shared `pending` count by a count per caller
  (`CopyPart::left`, a caller runs its own queued parts itself) and added `copy_pairs`; TICK-043's scan split
  (`collect_split`) now uses the same per-caller count and lets `copy_part` run a scan part (on a helper or on its caller).
  `RX_VK_WB_MAX_KB` (upstream) only gates the write-back; TICK-048's `diff_gpu_wrote` is always noted. Upstream's write-back
  diff table growth (`wbd_table_make`) taken, with TICK-038's allocation kind. TICK-036's inherit rule kept (now `env_on`).
- **vk_replay.cpp**: `submit_recording` flushes the stream (upstream) before our timestamps; `dry_marks_begin()`
  (upstream's fix for the dry pass's stale last targets) is called before our TICK-056 early-segment path, so both dry
  passes start clean; `FpsRender` in `wait_inner` (our split of `wait`); the readback ticket (TICK-059) kept; upstream's
  device-lost checks added to our acquire / blit timestamp code; `RX_VK_ACQUIRE_FENCE`'s wait moved to
  `acquire_fence_wait()` (each call checked for a lost device, as upstream checks every presentation call).
- **vk_draw.cpp**: the precompile. HeyListen's background precompile (TICK-008: entries made before the game draws, a
  `Pre` kept for good, the last worker finishing) kept, with upstream's additions: the mesh fallback programs' compute
  pipelines, the first graphics pipeline alone (NVIDIA fault), `cached_compute`, `usable_cpus()`, the 10 s progress line,
  `prepare_left_workers_note`, `predicted_check()` (from `precompile_finished`). Recording streams: upstream's
  `stream_insert` taken (TICK-038 kind, TICK-019 log moved into it). Pipelines being built: upstream's rule (a draw whose
  shaders write global memory or storage is never left out; an untranslated program is waited for, `RX_VK_PIPE_SKIP=2`
  restores the old skip) combined with TICK-008's fresh-target rule (`RX_VK_PIPE_WAIT_FRESH`); the stages are only looked
  at when the draw could be left out.
- **vk_texture.cpp**: upstream's VRAM compaction (textures' own pool, on by default) with our TICK-023 dedicated big
  images (they stay out of the pool), TICK-031 spare images (taken first) and BC1/BC7 views (`view_format_of` now gives
  the BC twin, not BC3 always); TICK-023's target rule, refused-image rule and TICK-030's recent floor apply to
  upstream's `rule_budget` too. The eviction loop: upstream's hold pass (`by_hold`) and its stop at 7/8 of the budget are
  taken for the usual pass (HeyListen had lost that stop in Sync 10: it evicted every candidate once over 7/8); TICK-027's
  overwritten targets go on past it. Compaction's view-cache bump goes through TICK-052's ring (`destroy_gen_bump(nullptr)`).
- **vk_vma.cpp**: upstream's textures' pool and defragmentation, our TICK-038 allocation accounting wrapper and
  TICK-023 refused-image count, then upstream's retry without the budget flag.
- **vk_menu.cpp / vk_menu.h**: HeyListen's FPS counter (the performance overlay, Off / FPS / Full, F10 at key 17) and
  Frame rate limit row kept; upstream's FPS counter levels and Game frame rate cap row (Uncapped) not taken
  (`fps_level_now` left at Off: vk_fps.cpp measures nothing for them). Upstream's Audio tab, HDR contrast, Anti-Lag 2,
  Reflex text, the menu size range (1..3), the version line's fit taken; HeyListen title and wordmark kept. Upstream's
  recorder boundary before each Dear ImGui recording taken.
- **Frame rate limit option id**: upstream gave option id 10 to Text size; ours moves to `HOS_OPT_FPS_LIMIT` = 60
  (`vk_api.h`, `nn_vi.c`, `vk_replay.cpp`). The menu's saved value is unchanged (the row is `opts.fps_limit`).

### host
- **nn_vi.c**: rebuilt from upstream's file with HeyListen's changes on it: `fps_limit()` (live, the menu's Frame rate
  limit, `HOS_FPS_LIMIT` / `HOS_FPS_MAX`, 30..360) stands where upstream's `fps_max()` is, so upstream's step above 60
  (the frames' own time through the lifted hook, `realtime_rate`, which is upstream's version of TICK-014's carry fix)
  follows our limit; the vsync thread stays at 60 Hz (Sync 10/11); `hos_realtime_fps`, TICK-014's slot diagnostic and
  `HOS_SPEED_LOG` kept; the present period as ours.
- **hos_thread.c**: upstream's short-sleep timer choice (#2263) with TICK-062's `HOS_SHORT_SLEEP_PAUSE` inside the yield
  path (see below); the job trace marks (TICK-037) with upstream's `sleep_away`.
- **nvn_native.c**: upstream's present trace phases around our TICK-054 held-submit flush and job-trace marks.
- **rx_backend.c**: upstream's checked DLL path and init errors, our TICK-015 / TICK-054 exports and `RX_VK_GONE_RING=1`.
- **hos_update.c**: upstream's tracks, our repository (Sacrappy/HeyListen) and user agent.

### nx2, scripts, builder, tests
- **nx2/build.py**: upstream removed `--opt`, `--no-polls`, `--only`; ours keeps no `-DA64_POLL_CTX` (our loop-header
  polls, Sync 10) and `HeyListen.exe`.
- **nx2/oot_patches.py**: upstream's camera speed, heap retry and movie/menu hooks with our TICK-026 native replacements
  and TICK-047 job order; upstream removed `oot_native_resolution.py` (HOS_SCENE_NATIVE, off since #849).
- **scripts/prof.py & co.**: upstream's `load_symbols` with HeyListen.exe first (`GAME_EXES`).
- **Builder** (WPF, setup.c, portable_home.h, Linux): upstream's code; HeyListen's names (HeyListen Builder,
  `%LOCALAPPDATA%\HeyListenBuilder`, `Games\HeyListen`, HeyListenBuilder.exe, the portable home's folder too), icon and
  URLs. Upstream's portable home and tracks taken.
- **tests/run_all.py**: upstream moved the tables into each test (`RUN_ALL_ARGS`); the fork's own tests got theirs.

## #2419 and TICK-058 / TICK-059
#2419 maps a submission's value to its *last* recording (it kept the first part's: a fence wait returned after the
first ~120 draws). TICK-058's fast poll (`RX_VK_FENCE_POLL_FAST`, off) marks `whole_val` for the values finished in the
recording being submitted, under `rec_map_mu`, after `value_rec` has them: that recording is the last part, so the mark
and `value_rec` agree and the fast poll's `wait_recorded(f, 0)` waits for the whole submission, as the locked path does.
A later recording carrying the same value can only raise `value_rec[f]` for work that is not f's. TICK-059's ticket
(`RX_VK_READBACK_TICKET`, off) orders the readbacks after the replay section and does not read `value_rec`: unchanged.
Both switches stay off. The waits now last as long as the work they stand for (upstream measured lower fps on an
iGPU with no mirrors; with mirrors the early return was hidden).

## Short sleeps (#2263) and HOS_SHORT_SLEEP_PAUSE
#2263 sleeps the game's sub-millisecond sleeps on the high-resolution timer where the process has 6 logical
processors or fewer (`HOS_SHORT_SLEEP_TIMER` over it: 1 all, 0 none). TICK-062's `HOS_SHORT_SLEEP_PAUSE` (off) only
changes the yield loop, which is what remains on bigger PCs: on the 7800X3D (16 threads) nothing changes; on a
≤6-thread PC the timer is chosen and the pause switch has no effect.

## Fixes the tests asked for (second commit)
- **Frame rate limit option id:** 10 -> `HOS_OPT_FPS_LIMIT` (60), because upstream gave 10 to Text size.
- **#2383:** `suyu_rt_fixes` accepts TICK-044's already-fused fma32.
- **Fork on/off switches:** read through `env_on` (upstream's rule, `tests/test_env_on.py`).
- **TICK-033's occlusion report write:** goes through `guest_cpu_write`, so the write-back diff's shadows hear of it.
- **Compaction view-cache bump:** goes through TICK-052's ring.
- **Fork stubs:** upstream's new code-extraction tests got stubs for the fork's code: TICK-037 job trace, TICK-043 scan,
  TICK-052 counters, TICK-038 kinds, TICK-054 present marks, TICK-062 thread cache.
- **Tests of what HeyListen does not take:** upstream's tests of the Game frame rate cap row and the FPS counter levels
  are scoped to what applies. `test_precompile_pipeline_cleanup` is skipped: TICK-008's precompile makes its entries
  before its workers start.
- **Builder:** the portable home is named HeyListenBuilder.
- **run_bench:** reads upstream's 7-column present trace.
- **Anisotropy:** the game's own by default (upstream's 16x cost ~3 fps).
- **#2419:** `RX_VK_VALUE_REC_FIRST=1` restores the first part's mapping (a measurement switch).

## Main merged in (fb6bf47b: TICK-060, TICK-061, TICK-063)
- **Copy helpers:** TICK-060's capture hashes and TICK-061's claimed chunks sit on upstream's per-caller copy helpers.
  There is no global pending count: a claim token is waited for by its users, and a split copy by its caller's own count.
- **Submitted pages:** upstream's 32 submitted-page slots with TICK-060's late-slot reads. A page read from a capture is
  not kept, late or not, which covers TICK-060's rule.
- **Shadow:** TICK-060's deferred late pages are left out of the shadow, together with upstream's full-shadow rule.
- **`wait_inner`:** TICK-060's poll flag sits beside upstream's `FpsRender`.
- **Tests:** the four new fork tests declare their run_all arguments.
- **Build:** engine only (the lift and the profile stay valid).

## Results (details: docs/benchmarks/2026-11-12-sync12.md)

### Tests
- **Before the main merge:** `tests/run_all.py` with llvm-mingw (v0.1.0-rc2's, which has gcc/g++) gave 388 pass, 9 fail,
  65 skip.
- **The 9 failures are not the merge's:**
  - 6 need the .NET SDK, which this PC does not have (the builder fixtures).
  - 1 needs the `zstandard` module.
  - `nx2/test_nup_paths` needs a link privilege and also fails on main. Main's baseline run had 2 nup failures.
- **After the main merge:** see the report.

### Build, lift and profile
- **Lifted code key:** 131df4a9ed.
- **New PGO profile:** recorded on this lift, in `profile/hookshot_pieces.prof`.
- **`--prepare-shaders`:** 0 failed.

### Picture and speed
- **Picture:** the Market is drawn correctly, standing and walking.
- **HOS_SPEED_LOG:** 60.00 on both builds.

### Performance: a regression, not merged
- **castle-move:** the merged sync is 56.2 fps against main's 79.3, interleaved ×3.
- **#2419 (upstream's fence-wait fix):** about -12 fps. Every CPU readback now waits for the submission's last recording
  (6-7 ms each). `RX_VK_VALUE_REC_FIRST=1` gives it back.
- **The rest:** about -10 fps, not isolated. The renderer's replay costs the same; the virtual cores take and wait about
  twice as often. Upstream's vcore and sleep changes are suspects.
- **3-scene ×6 confirm:** not run.
- **Decisions left to the merger:**
  - #2419: upstream's correct wait, its old mapping (`RX_VK_VALUE_REC_FIRST`), or the precise per-recording wait.
  - The remaining 10 fps must be found before this replaces main.
