#!/usr/bin/env python3
"""Frametime benchmark harness for Hookshot and Lichii builds (TICK-001).

Starts one build for a fixed wall-clock time, collects per-frame present
intervals and writes p50/p95/p99/max + fps to <out>/bench.json.
The game process (and PresentMon) are ALWAYS killed by a hard watchdog at
most 120 s after launch, also when this script raises.  See README.md.
"""
from __future__ import annotations

import argparse
import atexit
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = 2  # v2: warm-up counted from launch (warmup_origin, launch_offset_s)
HARD_LIMIT_S = 120.0
DEFAULT_DURATION_S = 90.0
# Hookshot, measured on the TICK-004 traces (see README "Warm-up"): gameplay starts
# ~29 s after launch in Kokiri and ~53-57 s in Hyrule Field, and the Hyrule scene-entry
# hitches end ~55 s after launch.  60 s from launch covers both.  Lichii: no warm-up
# (the baseline measured the whole capture).
HOOKSHOT_DEFAULT_WARMUP_S = 60.0
LICHII_DEFAULT_WARMUP_S = 0.0
# A launch->first present offset outside this range means the trace clock is not the
# perf_counter clock (the offset is then taken from when the first row appears).
OFFSET_MAX_S = 120.0
MEASURE_MIN_S = 30.0  # warn when less than this is left to measure after the warm-up
REPEAT_SPREAD_LIMIT_PCT = 5.0

# Input scripts (HOS_INPUT_SCRIPT syntax) reaching the save from Hookshot's title.
LOAD_SAVE = "30:A,34:A,38:A,42:A,46:A,50:A,54:A,58:A,62:A,66:A,70:A"
HOOKSHOT_SCENES = {
    "kokiri-idle": LOAD_SAVE,  # Link stands at the spawn point
    "kokiri-walk": LOAD_SAVE + ",76:LUP/6000,83:LRIGHT/4000,89:LDOWN/5000",
    # Same inputs; the scene is whichever save the title selects (the last one used).
    "hyrule-idle": LOAD_SAVE,
    "hyrule-walk": LOAD_SAVE + ",76:LUP/6000,83:LRIGHT/4000,89:LDOWN/5000",
}


# ---------------------------------------------------------------- statistics
def percentile(sorted_vals: list[float], q: float) -> float:
    """Linear interpolation between closest ranks (numpy default)."""
    if not sorted_vals:
        return 0.0
    pos = (len(sorted_vals) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def compute_stats(intervals_ms: list[float]) -> dict:
    vals = sorted(v for v in intervals_ms if v > 0)
    if not vals:
        return {"frames": 0, "fps": 0.0,
                "frame_time_ms": {k: 0.0 for k in ("mean", "p50", "p95", "p99", "max")}}
    mean = sum(vals) / len(vals)
    return {
        "frames": len(vals),
        "fps": 1000.0 / mean,
        "frame_time_ms": {
            "mean": mean,
            "p50": percentile(vals, 0.50),
            "p95": percentile(vals, 0.95),
            "p99": percentile(vals, 0.99),
            "max": vals[-1],
        },
    }


# ------------------------------------------------------------------- parsers
def _trace_samples(path: Path) -> list[tuple[int, int]]:
    """Valid (return_ns, interval_ns) pairs of a HOS_PRESENT_TRACE csv, in file order.

    Malformed lines (e.g. a line cut by the forced kill) are skipped.
    """
    out: list[tuple[int, int]] = []
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return out
    for line in text.splitlines():
        parts = line.strip().split(",")
        if len(parts) != 3 or not parts[0].isdigit():
            continue
        try:
            out.append((int(parts[1]), int(parts[2])))
        except ValueError:
            continue
    return out


def parse_hookshot_trace(path: Path) -> list[tuple[float, float]]:
    """HOS_PRESENT_TRACE csv (frame,return_ns,interval_ns) -> [(t_s, interval_ms)].

    t_s is seconds since the first present.  Frame 1 (interval 0) and malformed
    lines (e.g. a line cut by the forced kill) are skipped.
    """
    samples = _trace_samples(path)
    if not samples:
        return []
    t0 = samples[0][0]
    return [((ret - t0) / 1e9, interval / 1e6) for ret, interval in samples if interval > 0]


def hookshot_first_return_ns(path: Path) -> int | None:
    """return_ns of the first valid trace row (absolute game clock), or None."""
    samples = _trace_samples(path)
    return samples[0][0] if samples else None


def trace_has_row(path: Path) -> bool:
    """True once the trace file holds at least one complete data row (cheap, for polling)."""
    try:
        with Path(path).open("rb") as f:
            head = f.read(4096).decode("utf-8", errors="replace")
    except OSError:
        return False
    return any(len(p) == 3 and p[0].isdigit() for p in (ln.strip().split(",") for ln in head.splitlines()))


_LOG_WINDOW = re.compile(r"vk:\s*\d+\s+presents,\s*([0-9.]+)\s+fps;")


def parse_hookshot_log(path: Path) -> list[float]:
    """fps of each 120-present window from the game log ('vk: N presents, X fps; ...')."""
    out: list[float] = []
    try:
        for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
            m = _LOG_WINDOW.search(line)
            if m:
                out.append(float(m.group(1)))
    except OSError:
        pass
    return out


def parse_presentmon_csv(path: Path) -> list[tuple[float, float]]:
    """PresentMon csv -> [(t_s, interval_ms)].

    Interval column: msBetweenPresents (1.x) or FrameTime (2.x console); time
    axis: TimeInSeconds (1.x), CPUStartTime (2.x) or TimeInMs (2.6), else accumulated intervals.
    Header names are matched case-insensitively.
    """
    raw: list[tuple[float, float]] = []
    try:
        handle = Path(path).open("r", encoding="utf-8-sig", errors="replace", newline="")
    except OSError:
        return []
    with handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if not header:
            return []
        cols = {h.strip().lower(): i for i, h in enumerate(header)}
        fi = next((cols[k] for k in ("msbetweenpresents", "frametime") if k in cols), None)
        ti = next((cols[k] for k in ("timeinseconds", "cpustarttime", "timeinms") if k in cols), None)
        t_scale = 0.001 if ti is not None and header[ti].strip().lower() == "timeinms" else 1.0  # 2.6 CSV
        if fi is None:
            return []
        for row in reader:
            try:
                ms = float(row[fi])
                t = float(row[ti]) * t_scale if ti is not None else float("nan")
            except (ValueError, IndexError):
                continue
            if ms > 0:
                raw.append((t, ms))
    if not raw:
        return []
    if raw[0][0] == raw[0][0]:  # not NaN
        t0 = raw[0][0]
        return [(t - t0, ms) for t, ms in raw]
    rows, acc = [], 0.0
    for _, ms in raw:
        acc += ms / 1000.0
        rows.append((acc, ms))
    return rows


def window(rows: list[tuple[float, float]], skip_first_s: float) -> list[float]:
    return [ms for t, ms in rows if t >= skip_first_s]


def skip_from_first_present(warmup_s: float, origin: str, launch_offset_s: float | None) -> float:
    """Seconds after the first present before measuring starts.

    origin "launch": warmup_s is counted from the game launch, so subtract the
    launch -> first present offset (never below 0: a warm-up shorter than the
    offset discards nothing).  origin "first_present" (or no offset known): as is.
    """
    if origin == "launch" and launch_offset_s is not None:
        return max(0.0, warmup_s - launch_offset_s)
    return max(0.0, warmup_s)


def cut_label(warmup_s: float, origin: str, launch_offset_s: float | None) -> str:
    if warmup_s <= 0:
        return "whole capture"
    if origin == "launch" and launch_offset_s is not None:
        return f"{warmup_s:g} s after launch"
    return f"{warmup_s:g} s after first present"


def resolve_launch_offset(trace: Path, launch_ns: int, first_seen_ns: int | None,
                          ) -> tuple[float | None, str | None]:
    """Launch -> first present offset in seconds, and a note when it is a fallback.

    The game's return_ns is the same clock as time.perf_counter_ns() on Windows (QPC):
    on the TICK-004 runs, boot time + return_ns gives a first present 11-13 s after the
    recorded started_utc (the ~9 s shader precompile is most of it), so the offset is
    first_return_ns - launch_ns.  If that is implausible (another clock), the
    offset is the time the first trace row was seen on disk (polled during the run;
    an upper bound, the file is buffered).
    """
    first_ret = hookshot_first_return_ns(trace)
    if first_ret is not None:
        off = (first_ret - launch_ns) / 1e9
        if 0.0 <= off <= OFFSET_MAX_S:
            return off, None
    if first_seen_ns is not None:
        off = (first_seen_ns - launch_ns) / 1e9
        if 0.0 <= off <= OFFSET_MAX_S:
            return off, ("trace clock is not the launch clock; launch offset taken from the "
                         "time the first trace row was seen (upper bound, buffered writes)")
    return None, "launch offset unknown; warm-up counted from the first present"


def fps_spread(fps_values: list[float]) -> dict:
    """Spread between repeated runs: (max - min) / mean, in percent."""
    vals = [v for v in fps_values if v > 0]
    if not vals:
        return {"fps_mean": 0.0, "fps_min": 0.0, "fps_max": 0.0,
                "spread_pct": None, "within_limit": False}
    mean = sum(vals) / len(vals)
    spread = 100.0 * (max(vals) - min(vals)) / mean
    return {"fps_mean": mean, "fps_min": min(vals), "fps_max": max(vals),
            "spread_pct": spread if len(vals) > 1 else None,
            "within_limit": len(vals) > 1 and spread <= REPEAT_SPREAD_LIMIT_PCT}


# ------------------------------------------------------------------ watchdog
def kill_tree(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        else:
            proc.kill()
    except Exception:
        pass
    try:
        proc.kill()
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except Exception:
        pass


class Watchdog:
    """Owns every child process; kills their trees at the deadline, at exit, on exceptions."""

    def __init__(self, hard_limit_s: float) -> None:
        self.hard_limit_s = min(hard_limit_s, HARD_LIMIT_S)
        self.procs: list[subprocess.Popen] = []
        self.fired = False
        self._timer: threading.Timer | None = None
        self._lock = threading.Lock()
        atexit.register(self.kill_all)

    def start(self) -> None:
        self._timer = threading.Timer(self.hard_limit_s, self._fire)
        self._timer.daemon = True
        self._timer.start()

    def _fire(self) -> None:
        self.fired = True
        self.kill_all()

    def add(self, proc: subprocess.Popen) -> None:
        with self._lock:
            self.procs.append(proc)

    def kill_all(self) -> None:
        with self._lock:
            procs = list(self.procs)
        for p in procs:
            kill_tree(p)

    def stop(self) -> None:
        if self._timer:
            self._timer.cancel()
        self.kill_all()


# ------------------------------------------------------------------ the run
def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def find_presentmon(explicit: str | None) -> str | None:
    if explicit:
        return explicit if Path(explicit).is_file() else None
    for name in ("PresentMon", "PresentMon.exe", "PresentMon-2.exe"):
        found = shutil.which(name)
        if found:
            return found
    return None


def detect_build(exe: Path) -> str:
    n = exe.name.lower()
    return "lichii" if any(t in n for t in ("lichii", "eden")) else "hookshot"


def resolve_warmup(warmup_arg: float | None, build: str) -> float:
    """--warmup-s if given, else the per-build default (counted from launch for Hookshot)."""
    if warmup_arg is not None:
        return max(0.0, float(warmup_arg))
    return HOOKSHOT_DEFAULT_WARMUP_S if build == "hookshot" else LICHII_DEFAULT_WARMUP_S


def run(args: argparse.Namespace) -> dict:
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    build = args.build if args.build != "auto" else detect_build(exe)
    duration = min(float(args.duration), HARD_LIMIT_S - 5.0)
    warnings: list[str] = []
    if duration < args.duration:
        warnings.append(f"duration clamped to {duration} s to respect the {HARD_LIMIT_S:.0f} s limit")
    warmup = resolve_warmup(args.warmup_s, build)
    if duration - warmup < MEASURE_MIN_S:
        warnings.append(f"only {max(0.0, duration - warmup):.0f} s left to measure after the "
                        f"{warmup:g} s warm-up; use a longer --duration (max {HARD_LIMIT_S - 5:.0f})")

    env = os.environ.copy()
    cmd = [str(exe)]
    monitor_cmd: list[str] | None = None
    trace = out / "present_trace.csv"
    pm_csv = out / "presentmon.csv"
    log_path = out / "game_log.txt"

    # Everything that can refuse happens before anything is launched.
    if build == "hookshot":
        source = "hos_present_trace"
        script = args.input_script or HOOKSHOT_SCENES.get(args.scene)
        if script is None:
            raise SystemExit(f"unknown scene {args.scene!r}; known: {sorted(HOOKSHOT_SCENES)} (or pass --input-script)")
        env.update({"HOS_PRESENT_TRACE": str(trace), "HOS_FPS_LIMIT": str(args.fps_limit),
                    "HOS_AUDIO": "0", "HOS_WINDOW_NOACTIVATE": "1", "HOS_INPUT_SCRIPT": script})
    else:
        source = "presentmon"
        pm = find_presentmon(args.presentmon)
        if not pm:
            raise SystemExit("Lichii has no built-in frametime log; PresentMon is required "
                             "(install it or pass --presentmon <path>). Nothing was launched.")
        if args.game:
            cmd += ["-g", args.game]
        monitor_cmd = [pm, "--process_name", exe.name, "--output_file", str(pm_csv),
                       "--timed", str(int(duration)), "--terminate_after_timed",
                       "--stop_existing_session", "--no_console_stats"]
    out.mkdir(parents=True, exist_ok=True)

    dog = Watchdog(duration + 20.0)
    started = time.monotonic()
    started_utc = utc_now()
    exit_code = None
    launch_ns = None
    first_seen_ns = None
    next_check = 0.0
    log = log_path.open("wb")
    try:
        dog.start()
        launch_ns = time.perf_counter_ns()  # same clock as the game's return_ns (see resolve_launch_offset)
        game = subprocess.Popen(cmd, cwd=str(exe.parent), env=env, stdout=log, stderr=subprocess.STDOUT)
        dog.add(game)
        if monitor_cmd:
            dog.add(subprocess.Popen(monitor_cmd, cwd=str(out),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        while time.monotonic() - started < duration:
            if game.poll() is not None:
                warnings.append("game exited before the duration was over")
                break
            if build == "hookshot" and first_seen_ns is None and time.monotonic() >= next_check:
                next_check = time.monotonic() + 0.5
                if trace_has_row(trace):
                    first_seen_ns = time.perf_counter_ns()
            time.sleep(0.2)
        exit_code = game.poll()
    finally:
        killed = exit_code is None
        dog.stop()  # kills game + monitor if still running, always
        log.close()
    wall = time.monotonic() - started
    if dog.fired:
        warnings.append("hard watchdog fired")

    offset, origin = None, "first_present"
    if build == "hookshot":
        rows = parse_hookshot_trace(trace)
        if rows:
            warnings.append("the last ~1 s of the trace may be lost to the forced kill (buffered writes)")
        else:
            warnings.append("no present trace rows (HOS_PRESENT_TRACE empty or missing)")
        windows = parse_hookshot_log(log_path)
        if launch_ns is not None:
            offset, note = resolve_launch_offset(trace, launch_ns, first_seen_ns)
            if note:
                warnings.append(note)
        if offset is not None:
            origin = "launch"
    else:
        rows = parse_presentmon_csv(pm_csv)
        if not rows:
            warnings.append("no PresentMon rows")
        windows = []
        warnings.append("warm-up counted from the first PresentMon row (no launch offset for Lichii)")
    skip = skip_from_first_present(warmup, origin, offset)
    result = {
        "schema_version": SCHEMA_VERSION,
        "build": build,
        "exe": str(exe),
        "scene": args.scene,
        "source": source,
        "started_utc": started_utc,
        "duration_requested_s": duration,
        "wall_clock_s": round(wall, 3),
        "warmup_s": warmup,
        "warmup_origin": origin,
        "launch_offset_s": None if offset is None else round(offset, 3),
        "killed_by_harness": killed,
        "watchdog_fired": dog.fired,
        "exit_code": exit_code,
        **compute_stats(window(rows, skip)),
        "log_window_fps": windows,
        "warnings": warnings,
    }
    if rows and not result["frames"]:
        result["warnings"].append(f"no frames after the {warmup:g} s warm-up")
    (out / "bench.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def run_repeat(args: argparse.Namespace) -> list[dict]:
    """--repeat N: N runs back to back into <out>/run1..runN, then <out>/repeat.json.

    Each run is its own launch with its own warm-up and watchdog; the game is killed
    at the end of each run before the next one starts.  N = 1 writes to <out> directly.
    """
    n = int(args.repeat)
    if n < 1:
        raise SystemExit("--repeat must be >= 1")
    if n == 1:
        return [run(args)]
    out = Path(args.out).resolve()
    results = []
    for i in range(1, n + 1):
        results.append(run(argparse.Namespace(**{**vars(args), "out": str(out / f"run{i}")})))
    spread = fps_spread([r["fps"] for r in results])
    summary = {
        "schema_version": SCHEMA_VERSION,
        "build": results[0]["build"],
        "scene": args.scene,
        "limit_pct": REPEAT_SPREAD_LIMIT_PCT,
        **spread,
        "runs": [{"run": i + 1, "dir": f"run{i + 1}", "fps": r["fps"], "frames": r["frames"],
                  "p50_ms": r["frame_time_ms"]["p50"], "p95_ms": r["frame_time_ms"]["p95"],
                  "p99_ms": r["frame_time_ms"]["p99"], "max_ms": r["frame_time_ms"]["max"]}
                 for i, r in enumerate(results)],
    }
    (out / "repeat.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return results


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--exe", required=True, help="hookshot.exe or lichii.exe")
    p.add_argument("--scene", required=True, help="scene name (Hookshot: kokiri-idle, kokiri-walk)")
    p.add_argument("--out", required=True, help="output directory")
    p.add_argument("--build", choices=("auto", "hookshot", "lichii"), default="auto")
    p.add_argument("--duration", type=float, default=DEFAULT_DURATION_S, help="wall clock seconds (default 90, max 115)")
    p.add_argument("--warmup-s", type=float, default=None,
                   help="ignore frames before N s after game LAUNCH (Hookshot default 60; Lichii default 0)")
    p.add_argument("--repeat", type=int, default=1, help="N back-to-back runs into <out>/run1..N + repeat.json")
    p.add_argument("--fps-limit", type=int, default=120, help="Hookshot HOS_FPS_LIMIT")
    p.add_argument("--input-script", help="Hookshot HOS_INPUT_SCRIPT override")
    p.add_argument("--presentmon", help="path to PresentMon (Lichii)")
    p.add_argument("--game", help="Lichii: game file passed with -g (the emulator loads its own save)")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    results = run_repeat(args)
    for res in results:
        print(json.dumps({k: res[k] for k in ("build", "scene", "frames", "fps", "frame_time_ms",
                                              "wall_clock_s", "warmup_origin", "launch_offset_s")}, indent=2))
    if len(results) > 1:
        spread = fps_spread([r["fps"] for r in results])
        sp = "n/a" if spread["spread_pct"] is None else f"{spread['spread_pct']:.1f}%"
        print(f"repeat: fps {spread['fps_min']:.1f}-{spread['fps_max']:.1f} "
              f"(mean {spread['fps_mean']:.1f}), spread {sp}, "
              f"{'within' if spread['within_limit'] else 'NOT within'} {REPEAT_SPREAD_LIMIT_PCT:g}%")
    return 0 if all(r["frames"] for r in results) else 2


if __name__ == "__main__":
    sys.exit(main())
