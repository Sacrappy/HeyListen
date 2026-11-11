"""TICK-043: the capture copied ahead of its submit (gpu/vk/capture_merge.h, RX_VK_CAPTURE_PRECOPY) holds every page written
since the last submit once, each as it is at the submit: compiled and run against a simulation (no game, no GPU); and
vk_memory.cpp still makes its captures through that rule.

python -I -B tests/test_capture_merge.py --cxx <compiler>
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    src = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for needle in ('#include "capture_merge.h"', 'getenv("RX_VK_CAPTURE_PRECOPY")', "capture_merge::merge(",
                   # (the GPU's progress read before the marks are cleared: a write-back landing after it is copied again)
                   "const uint64_t done = vk_timeline_completed();   // (before the marks are cleared",
                   # (pages not copied go back to the marks, for the submit's scan)
                   "host->mark_written(base + ((uint64_t)pages[i] << kPageShift), 4096);",
                   # (the streaming stores fenced before a capture takes them; the slots written again copied at the submit)
                   "if (stream) stream_fence();   // (the copies visible before the capture that takes them)",
                   "copy_slots(rp.data(), mg.recopy.data(), rp.size(), (uint8_t*)c->map, ok.data());",
                   # (off by default)
                   "const bool on = e && atoi(e) != 0 && capture_on()"):
        if needle not in src:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="capture-merge-") as tmp:
        exe = Path(tmp) / "capture-merge.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_capture_merge.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
