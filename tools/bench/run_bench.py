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

SCHEMA_VERSION = 1
HARD_LIMIT_S = 120.0
DEFAULT_DURATION_S = 90.0

# Input scripts (HOS_INPUT_SCRIPT syntax) reaching the save from Hookshot's title.
LOAD_SAVE = "30:A,34:A,38:A,42:A,46:A,50:A,54:A,58:A,62:A,66:A,70:A"
HOOKSHOT_SCENES = {
    "kokiri-idle": LOAD_SAVE,  # Link stands at the spawn point
    "kokiri-walk": LOAD_SAVE + ",76:LUP/6000,83:LRIGHT/4000,89:LDOWN/5000",
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
def parse_hookshot_trace(path: Path) -> list[tuple[float, float]]:
    """HOS_PRESENT_TRACE csv (frame,return_ns,interval_ns) -> [(t_s, interval_ms)].

    t_s is seconds since the first present.  Frame 1 (interval 0) and malformed
    lines (e.g. a line cut by the forced kill) are skipped.
    """
    rows: list[tuple[float, float]] = []
    t0 = None
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return rows
    for line in text.splitlines():
        parts = line.strip().split(",")
        if len(parts) != 3 or not parts[0].isdigit():
            continue
        try:
            ret, interval = int(parts[1]), int(parts[2])
        except ValueError:
            continue
        if t0 is None:
            t0 = ret
        if interval > 0:
            rows.append(((ret - t0) / 1e9, interval / 1e6))
    return rows


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
    axis: TimeInSeconds (1.x) or CPUStartTime (2.x), else accumulated intervals.
    Header names are matched case-insensitively.
    """
    raw: list[tuple[float, float]] = []
    try:
        handle = Path(path).open("r", encoding="utf-8", errors="replace", newline="")
    except OSError:
        return []
    with handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if not header:
            return []
        cols = {h.strip().lower(): i for i, h in enumerate(header)}
        fi = next((cols[k] for k in ("msbetweenpresents", "frametime") if k in cols), None)
        ti = next((cols[k] for k in ("timeinseconds", "cpustarttime") if k in cols), None)
        if fi is None:
            return []
        for row in reader:
            try:
                ms = float(row[fi])
                t = float(row[ti]) if ti is not None else float("nan")
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


def run(args: argparse.Namespace) -> dict:
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    build = args.build if args.build != "auto" else detect_build(exe)
    duration = min(float(args.duration), HARD_LIMIT_S - 5.0)
    warnings: list[str] = []
    if duration < args.duration:
        warnings.append(f"duration clamped to {duration} s to respect the {HARD_LIMIT_S:.0f} s limit")

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
                       "--stop_existing_session", "--no_top"]
    out.mkdir(parents=True, exist_ok=True)

    dog = Watchdog(duration + 20.0)
    started = time.monotonic()
    started_utc = utc_now()
    exit_code = None
    log = log_path.open("wb")
    try:
        dog.start()
        game = subprocess.Popen(cmd, cwd=str(exe.parent), env=env, stdout=log, stderr=subprocess.STDOUT)
        dog.add(game)
        if monitor_cmd:
            dog.add(subprocess.Popen(monitor_cmd, cwd=str(out),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        while time.monotonic() - started < duration:
            if game.poll() is not None:
                warnings.append("game exited before the duration was over")
                break
            time.sleep(0.2)
        exit_code = game.poll()
    finally:
        killed = exit_code is None
        dog.stop()  # kills game + monitor if still running, always
        log.close()
    wall = time.monotonic() - started
    if dog.fired:
        warnings.append("hard watchdog fired")

    if build == "hookshot":
        rows = parse_hookshot_trace(trace)
        if rows:
            warnings.append("the last ~1 s of the trace may be lost to the forced kill (buffered writes)")
        else:
            warnings.append("no present trace rows (HOS_PRESENT_TRACE empty or missing)")
        windows = parse_hookshot_log(log_path)
    else:
        rows = parse_presentmon_csv(pm_csv)
        if not rows:
            warnings.append("no PresentMon rows")
        windows = []
    result = {
        "schema_version": SCHEMA_VERSION,
        "build": build,
        "exe": str(exe),
        "scene": args.scene,
        "source": source,
        "started_utc": started_utc,
        "duration_requested_s": duration,
        "wall_clock_s": round(wall, 3),
        "warmup_s": args.warmup_s,
        "killed_by_harness": killed,
        "watchdog_fired": dog.fired,
        "exit_code": exit_code,
        **compute_stats(window(rows, args.warmup_s)),
        "log_window_fps": windows,
        "warnings": warnings,
    }
    (out / "bench.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--exe", required=True, help="hookshot.exe or lichii.exe")
    p.add_argument("--scene", required=True, help="scene name (Hookshot: kokiri-idle, kokiri-walk)")
    p.add_argument("--out", required=True, help="output directory")
    p.add_argument("--build", choices=("auto", "hookshot", "lichii"), default="auto")
    p.add_argument("--duration", type=float, default=DEFAULT_DURATION_S, help="wall clock seconds (default 90, max 115)")
    p.add_argument("--warmup-s", type=float, default=0.0, help="ignore frames in the first N s of the trace")
    p.add_argument("--fps-limit", type=int, default=120, help="Hookshot HOS_FPS_LIMIT")
    p.add_argument("--input-script", help="Hookshot HOS_INPUT_SCRIPT override")
    p.add_argument("--presentmon", help="path to PresentMon (Lichii)")
    p.add_argument("--game", help="Lichii: game file passed with -g (the emulator loads its own save)")
    return p


def main(argv: list[str] | None = None) -> int:
    res = run(build_parser().parse_args(argv))
    print(json.dumps({k: res[k] for k in ("build", "scene", "frames", "fps", "frame_time_ms", "wall_clock_s")}, indent=2))
    return 0 if res["frames"] else 2


if __name__ == "__main__":
    sys.exit(main())
