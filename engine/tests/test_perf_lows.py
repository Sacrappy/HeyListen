# Copyright (C) 2026 Sacrappy
"""The FPS counter's 1% and 0.1% lows (gpu/vk/perf_lows.h): the 99th and the 99.9th percentile's frame, as the usual tools show them
(not the benchmark's mean of the slowest 1 %, tools/bench/run_bench.py low_fps, which they were until 27 Nov 2026), against the same
selection written here on the same series; the window's and the text's own checks; and the wiring: the overlay's lines, the frame
times fed to the ring, nothing computed unless the full overlay shows."""
import argparse
import math
import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def percentile_low_fps():
    """The reference: 1000 / the frame time of the ceil(fraction * N)-th slowest frame, the slowest one at least"""
    def low_fps(sorted_vals, fraction):
        if not sorted_vals:
            return 0.0
        k = max(1, math.ceil(fraction * len(sorted_vals) - 1e-9))
        return 1000.0 / sorted_vals[-k]
    return low_fps, "the percentile's frame"


def need(path, needles):
    src = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in src:
            raise SystemExit(path + " no longer has: " + needle)


def check_source():
    need("gpu/vk/vk_overlay.cpp", ['#include "perf_lows.h"', "void perf_lows_draw(", "perf_lows(&a, &b, &n);", "    perf_lows_draw(fps_c);\n",
                                   '"1% low (shown frames)"', '"0.1% low"'])
    src = (ROOT / "gpu/vk/vk_overlay.cpp").read_text(encoding="utf-8")
    full = src[src.index("void perf_full_draw("):src.index("void perf_fps_draw(")]
    if "perf_lows_draw(" not in full:
        raise SystemExit("the lows are drawn by the full overlay")
    fps = src[src.index("void perf_fps_draw("):src.index("} // namespace\n")]
    if "perf_lows" in fps:
        raise SystemExit("the compact FPS box must not compute or show the lows")
    need("gpu/vk/vk_perf.cpp", ['#include "perf_lows.h"', "lows_ring.add((float)((now - last_frame_ns) / 1e6), now);", "lows_ring.clear();",
                                "perflows::lows(lows_scratch, n, low1, low01);"])
    need("gpu/vk/vk_menu.cpp", ["if (opts.show_fps) perf_note_frame();"])
    need("gpu/vk/vk_perf.h", ["void perf_lows(double* low1, double* low01, int* frames);"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    low_fps, which = percentile_low_fps()
    with tempfile.TemporaryDirectory(prefix="perf-lows-") as tmp:
        exe = Path(tmp) / "perf-lows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_perf_lows.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        rng = random.Random(25)
        series = [[16.5], [10.0, 40.0], [16.0] * 99, [rng.choice([8.0, 8.25, 16.5, 33.0]) for _ in range(100)],
                  [rng.choice([8.0, 8.25, 16.5, 33.0]) for _ in range(999)], [rng.choice([8.0, 8.25, 16.5, 33.0]) for _ in range(1000)],
                  [rng.choice([8.0, 8.25, 16.5, 33.0]) for _ in range(1001)], [4.0] * 5000,
                  [4.0 + (i % 7) * 0.25 for i in range(2400)], [rng.randrange(40, 400) / 8 for _ in range(16000)],
                  [16.0] * 1000 + [250.0, 90.0, 60.0]]
        for n, vals in enumerate(series):   # (float32-exact frame times: the C++ and the bench's sums agree to rounding)
            f = Path(tmp) / ("s%d.txt" % n)
            f.write_text(str(len(vals)) + "\n" + "\n".join("%.4f" % v for v in vals), encoding="ascii")
            out = subprocess.run([str(exe), str(f)], check=True, capture_output=True, text=True).stdout.split()
            ref = sorted(vals)
            want = (low_fps(ref, 0.01), low_fps(ref, 0.001))
            for got, w, name in zip((float(out[0]), float(out[1])), want, ("1% low", "0.1% low")):
                if abs(got - w) > 1e-6 * max(1.0, w):
                    raise SystemExit("series %d (%d frames): %s is %.9f here and %.9f in %s" % (n, len(vals), name, got, w, which))
    print("perf lows: all checks passed (compared with %s)" % which)


if __name__ == "__main__":
    main()
