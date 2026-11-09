#!/usr/bin/env python3
"""Markdown table of bench directories (Hookshot present traces, Lichii PresentMon CSVs) (TICK-009, TICK-013).

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
TICK-013 adds the 1% and 0.1% low fps (1000 / mean of the slowest 1% / 0.1% frames)
and the shares of frames over the 120 Hz (8.33 ms) and 144 Hz (6.94 ms) budgets.
All of them are computed from the raw rows, so bench.json schema 1, 2 and 3 all work.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import run_bench as rb  # noqa: E402

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
    return {
        "dir": d.name,
        "build": BUILD_LABEL.get(build, build),
        "scene": scene,
        "source": source,
        "cut": rb.cut_label(warmup, origin, offset),
        "frames": stats["frames"],
        "fps": stats["fps"],
        "fps_1pct_low": stats["fps_1pct_low"],
        "fps_0_1pct_low": stats["fps_0_1pct_low"],
        "ft": stats["frame_time_ms"],
        "over_8_33_pct": stats["over_8_33_ms_pct"],
        "over_6_94_pct": stats["over_6_94_ms_pct"],
        "over_16_7_pct": stats["over_16_7_ms_pct"],
    }


def markdown(results: list[dict]) -> str:
    lines = [
        "| Build | Dir | Scene | Cut | Frames | fps | 1% low | 0.1% low | p50 | p95 | p99 | max "
        "| frames > 8.33 ms (120 Hz) | frames > 6.94 ms (144 Hz) | frames > 16.7 ms |",
        "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in results:
        ft = r["ft"]
        lines.append(
            f"| {r['build']} | {r['dir']} | {r['scene']} | {r['cut']} | {r['frames']} | "
            f"**{r['fps']:.1f}** | {r['fps_1pct_low']:.1f} | {r['fps_0_1pct_low']:.1f} | "
            f"{ft['p50']:.1f} ms | {ft['p95']:.1f} ms | {ft['p99']:.1f} ms | "
            f"{ft['max']:.0f} ms | {r['over_8_33_pct']:.1f}% | {r['over_6_94_pct']:.1f}% | "
            f"{r['over_16_7_pct']:.0f}% |")
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
