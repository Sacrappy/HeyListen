"""Compile and run the parallel replay's segment count (vk_replay.cpp replay_handle, TICK-050; no game/GPU required)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The replay still cuts its segments with the rule, and the switches are still read"""
    src = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    for needle in ('#include "segment_plan.h"', "segment_plan::segment_count(total, per, recorders, kMaxBatch, one_round)",
                   'getenv("RX_VK_PARALLEL_ROUNDS")', 'getenv("RX_VK_PARALLEL_SELF")', "if (self_rec) record_job(jobs[0].get());",
                   'getenv("RX_VK_PARALLEL_SEG")'):
        if needle not in src:
            raise SystemExit("vk_replay.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="segment-plan-") as tmp:
        exe = Path(tmp) / "segment-plan"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_segment_plan.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
