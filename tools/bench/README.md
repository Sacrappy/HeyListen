# tools/bench - frametime benchmark harness (TICK-001)

```
python tools/bench/run_bench.py --exe <hookshot.exe|lichii.exe> --scene <name> --out <dir>
        [--duration 90] [--warmup-s 0] [--fps-limit 120] [--input-script S]
        [--presentmon <PresentMon.exe>] [--game <file for lichii -g>] [--build auto|hookshot|lichii]
```

Runs one build, collects per-frame present intervals, writes `<dir>/bench.json`
(plus the raw `present_trace.csv` / `presentmon.csv` and `game_log.txt`).
Exit code 0 when frames were recorded, 2 when none were.

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
The save is reached from the title, which takes ~72 s; use `--warmup-s 75` to measure
gameplay only (then lengthen `--duration`, max 115) - the default measures everything.

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

## bench.json (schema_version 1)

| key | type | meaning |
|---|---|---|
| schema_version | int | 1 |
| build | "hookshot"/"lichii" | |
| exe, scene | string | inputs |
| source | "hos_present_trace"/"presentmon" | where frame times came from |
| started_utc | string | ISO 8601 |
| duration_requested_s, wall_clock_s | number | requested vs actual run time |
| warmup_s | number | frames earlier than this (s after the first frame) are ignored |
| killed_by_harness | bool | game still running at the end and was killed |
| watchdog_fired | bool | the hard timer (not the normal end) killed it |
| exit_code | int/null | null when killed |
| frames | int | frames counted |
| fps | number | frames / total frame time = 1000 / mean |
| frame_time_ms | object | `mean`, `p50`, `p95`, `p99`, `max` (percentiles interpolated linearly) |
| log_window_fps | [number] | Hookshot only, see above |
| warnings | [string] | caveats of this run |

## Tests

`python -m pytest tools/bench/test_parse.py` - parsers on sample files in `testdata/`,
watchdog, and an end-to-end run against a fake game process. No real game is started.
