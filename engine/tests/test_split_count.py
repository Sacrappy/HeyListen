"""Compile and run the split counter (gpu/vk/split_count.h, TICK-052; no game/GPU required)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The draws' counters and the phase times are still split counters"""
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    internal = (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8")
    for src, needle in ((draw, "SplitCount st_draws, st_skipped, st_dispatches, st_passes, st_mesh;"),
                        (draw, "SplitCount st_desc_reused;"), (internal, "extern SplitCount prof_ns[PROF_COUNT];")):
        if needle not in src:
            raise SystemExit("the source no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="split-count-") as tmp:
        exe = Path(tmp) / "split-count"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                        str(ROOT / "tests/test_split_count.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
