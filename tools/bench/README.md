# tools/bench - frametime benchmark harness (TICK-001)

```
python tools/bench/run_bench.py --exe <hookshot.exe|lichii.exe> --scene <name> --out <dir>
        [--duration 90] [--warmup-s N] [--repeat N] [--fps-limit 120] [--input-script S]
        [--presentmon <PresentMon.exe>] [--game <file for lichii -g>] [--build auto|hookshot|lichii]
python tools/bench/summarize.py [--warmup-s N] [--origin launch|first_present] <dir> [<dir> ...]
```

Runs one build, collects per-frame present intervals, writes `<dir>/bench.json`
(plus the raw `present_trace.csv` / `presentmon.csv` and `game_log.txt`).
Exit code 0 when frames were recorded, 2 when none were.

`--repeat N` (TICK-009) runs N launches back to back into `<dir>/run1` .. `run<N>`
(each with its own watchdog, the game killed before the next one starts) and writes
`<dir>/repeat.json`: fps min/max/mean and `spread_pct` = (max - min) / mean. The
"within 5%" check is `within_limit`. `--repeat 1` (default) writes to `<dir>` directly.

`summarize.py <dir> ...` prints the Markdown table of `docs/benchmarks/2026-11-baseline.md`
(fps, p50/p95/p99/max, share of frames > 16.7 ms) for Hookshot traces and PresentMon CSVs.
Each directory's warm-up comes from its `bench.json`. Directories without one (the
TICK-004 captures) count from the first present. `--warmup-s 45 --origin first_present`
reproduces the baseline's Hookshot rows; Lichii rows are the whole capture.

## Warm-up (TICK-009)

`--warmup-s` is counted from **game launch** (wall clock), not from the first present.
Default: **60 s for Hookshot**, 0 for Lichii (the baseline measured the whole capture).

How the default was chosen, from the TICK-004 traces (`build/bench/hookshot-*`, log
windows of 120 presents, trace times converted with the launch offset below):

- Kokiri Forest: fps drops from the capped title (120) to 45-60 at about 15.7-18.4 s
  after the first present, i.e. about 29-30 s after launch. Scene-entry hitches (up to
  ~500 ms) are over by ~18 s after the first present.
- Hyrule Field: the drop is at 41.3-44.6 s after the first present (about 53-57 s after
  launch). The last scene-entry hitch (~650 ms) is at 42.0-42.8 s after the first present.
- 60 s after launch covers both entries with a few seconds of margin and leaves
  about 30 s of gameplay in a 90 s run, or ~55 s in the 115 s maximum.

These are from the log and trace timing only, not a repeat of the baseline runs.

### Launch offset

`bench.json` records `launch_offset_s`, the time from the game launch to the first
present. `run_bench.py` takes `time.perf_counter_ns()` just before starting the game
and subtracts it from the first trace row's `return_ns`. Checked on the TICK-004 runs:
the trace clock is QPC since boot, which is what `perf_counter_ns` reads on Windows
(boot time from `Win32_OperatingSystem.LastBootUpTime` plus `return_ns` gives a first
present 11-13 s after the recorded `started_utc`, which is truncated to whole seconds;
the ~9 s shader precompile in the game log is most of that).

If the offset is implausible (outside 0-120 s, i.e. another clock), the offset is the
time the first trace row was seen on disk (polled during the run). That is an upper
bound because the file is buffered. If neither works, `launch_offset_s` is null and
`warmup_origin` is `first_present`, with a warning. Lichii has no launch offset (the
PresentMon capture starts after launch and its clock is not measured), so its warm-up
counts from the first PresentMon row.

## Watchdog

The game (and PresentMon) are launched as children of the script. They are killed
(`taskkill /F /T`) when the duration is over, by a timer thread at `duration + 20 s`
(never more than 120 s after launch), in a `finally` block on any exception, and by
`atexit`. `--duration` is clamped to 115 s. Killing the script itself with SIGKILL or
Task Manager is the one case this cannot cover.

## Hookshot

Same launch as `hookshot-fork/scripts/walk-test.sh`: cwd = the exe folder, env
`HOS_FPS_LIMIT`, `HOS_AUDIO=0`, `HOS_WINDOW_NOACTIVATE=1`, `HOS_INPUT_SCRIPT` (title
-> save via scripted A presses). Scenes: `kokiri-idle` (Link stands at the spawn point),
`kokiri-walk` (adds a walk at 76/83/89 s). `--input-script` overrides.

Frame times come from `HOS_PRESENT_TRACE=<csv>` (native present-call returns,
`frame,return_ns,interval_ns`; not GPU completion or display times). The first
row (interval 0) is dropped. A forced kill may lose the last ~1 s of buffered rows.
The 120-present `vk: N presents, X fps` lines of the game log are copied to
`log_window_fps` as a cross-check only (they are one number per 120 frames).
The save is reached from the title, which takes a while; the default warm-up (60 s from
launch, see above) skips it. Use `--duration 115` to keep a useful measurement window
after the warm-up.

## Lichii (Eden fork)

No per-frame log exists: the emulator log (`%APPDATA%\eden\log\eden_log.txt`) has only
settings and no frame times. The source is **PresentMon** (not installed on this
machine at the time of writing). Without it the script exits before launching anything.
It runs `PresentMon --process_name lichii.exe --output_file presentmon.csv --timed N
--terminate_after_timed --stop_existing_session --no_console_stats` (PresentMon 2.6; 1.x used `--no_top`) and reads `msBetweenPresents`
(1.x) or `FrameTime` (2.x). Needs an elevated shell or Performance Log Users
membership for ETW. The scene is not controllable: Lichii has no input script, so
`--scene` is only a label and the user positions the game; this is a gap. These are
PresentMon's present intervals on the host swap chain, not guest (Switch) frames.

## bench.json (schema_version 2)

| key | type | meaning |
|---|---|---|
| schema_version | int | 2 (1 = warm-up counted from the first present) |
| build | "hookshot"/"lichii" | |
| exe, scene | string | inputs |
| source | "hos_present_trace"/"presentmon" | where frame times came from |
| started_utc | string | ISO 8601 |
| duration_requested_s, wall_clock_s | number | requested vs actual run time |
| warmup_s | number | frames before this many seconds are ignored (see Warm-up) |
| warmup_origin | "launch"/"first_present" | what warmup_s is counted from (absent in schema 1 = first_present) |
| launch_offset_s | number/null | game launch to first present (see Warm-up) |
| killed_by_harness | bool | game still running at the end and was killed |
| watchdog_fired | bool | the hard timer (not the normal end) killed it |
| exit_code | int/null | null when killed |
| frames | int | frames counted |
| fps | number | frames / total frame time = 1000 / mean |
| frame_time_ms | object | `mean`, `p50`, `p95`, `p99`, `max` (percentiles interpolated linearly) |
| log_window_fps | [number] | Hookshot only, see above |
| warnings | [string] | caveats of this run |

## Tests

`python -m pytest -q tools/bench` - parsers on sample files in `testdata/`, the warm-up and
launch-offset logic, the fps spread, `summarize.py` on synthetic directories, the
watchdog, and end-to-end runs (`--repeat 2` included) against a fake game process. No real
game is started.
