#!/usr/bin/env python3
"""Markdown table of bench directories (Hookshot present traces, Lichii PresentMon CSVs) (TICK-009).

    python tools/bench/summarize.py [--warmup-s N] [--origin launch|first_present] <dir> [<dir> ...]

A directory holds present_trace.csv (Hookshot) or presentmon.csv (Lichii), and
optionally bench.json (written by run_bench.py).  The warm-up of each directory is
taken from its bench.json (warmup_s, warmup_origin, launch_offset_s); directories
written before schema 2 have no origin and count from the first present.
--warmup-s and --origin override the warm-up of every directory given (the old
Hookshot captures: --warmup-s 45 --origin first_present reproduces
docs/benchmarks/2026-11-baseline.md).
Columns follow that baseline: fps = frames / total frame time, percentiles are
linear interpolations, and "> 16.7 ms" is the share of frames over one 60 Hz frame.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import run_bench as rb  # noqa: E402

FRAME_BUDGET_MS = 16.7
BUILD_LABEL = {"hookshot": "Hookshot", "lichii": "Lichii"}


def summarize_dir(d: Path, warmup_override: float | None, origin_override: str | None = None) -> dict:
    d = Path(d)
    meta: dict = {}
    bench = d / "bench.json"
    if bench.is_file():
        meta = json.loads(bench.read_text(encoding="utf-8"))
    trace, pm = d / "present_trace.csv", d / "presentmon.csv"
    if trace.is_file():
        rows, source, build = rb.parse_hookshot_trace(trace), "hos_present_trace", "hookshot"
    elif pm.is_file():
        rows, source, build = rb.parse_presentmon_csv(pm), "presentmon", "lichii"
    else:
        raise SystemExit(f"{d}: neither present_trace.csv nor presentmon.csv")
    build = meta.get("build", build)
    scene = meta.get("scene", d.name)
    warmup = float(warmup_override) if warmup_override is not None else float(meta.get("warmup_s", 0.0))
    origin = origin_override or meta.get("warmup_origin", "first_present")
    offset = meta.get("launch_offset_s")
    skip = rb.skip_from_first_present(warmup, origin, offset)
    vals = rb.window(rows, skip)
    stats = rb.compute_stats(vals)
    over = sum(1 for ms in vals if ms > FRAME_BUDGET_MS)
    return {
        "dir": d.name,
        "build": BUILD_LABEL.get(build, build),
        "scene": scene,
        "source": source,
        "cut": rb.cut_label(warmup, origin, offset),
        "frames": stats["frames"],
        "fps": stats["fps"],
        "ft": stats["frame_time_ms"],
        "over_pct": 100.0 * over / len(vals) if vals else 0.0,
    }


def markdown(results: list[dict]) -> str:
    lines = [
        "| Build | Dir | Scene | Cut | Frames | fps | p50 | p95 | p99 | max | frames > 16.7 ms |",
        "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in results:
        ft = r["ft"]
        lines.append(
            f"| {r['build']} | {r['dir']} | {r['scene']} | {r['cut']} | {r['frames']} | "
            f"**{r['fps']:.1f}** | {ft['p50']:.1f} ms | {ft['p95']:.1f} ms | {ft['p99']:.1f} ms | "
            f"{ft['max']:.0f} ms | {r['over_pct']:.0f}% |")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("dirs", nargs="+", help="bench directories")
    p.add_argument("--warmup-s", type=float, default=None,
                   help="warm-up in seconds for every directory (default: each bench.json's)")
    p.add_argument("--origin", choices=("launch", "first_present"), default=None,
                   help="count the warm-up from launch or from the first present (default: each bench.json's)")
    args = p.parse_args(argv)
    results = [summarize_dir(Path(d), args.warmup_s, args.origin) for d in args.dirs]
    print(markdown(results))
    return 0 if all(r["frames"] for r in results) else 2


if __name__ == "__main__":
    sys.exit(main())
