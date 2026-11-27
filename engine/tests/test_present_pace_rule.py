# Copyright (C) 2026 Sacrappy
"""Compile and run the present pace's rule (gpu/vk/present_pace_rule.h; no game/GPU): at a frame rate limit each frame is handed
over on an even line (an early frame waits for its point, a late one starts the line again: no short frame after it); and by the
text, that vk_replay.cpp holds at present()'s start, before the order lock, and only where the pace applies."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    src = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    for needle in ['#include "present_pace_rule.h"',
                   "present_pace_rule::hold(line, now, period,",
                   "present_pace_rule::start_wait(line, now, period, work,",
                   "if (limit <= 0 || framegen_on()) return 0;",
                   "if (dev.present_mode_name && dev.present_mode_name[0] == 'f') return 0;"]:
        if needle not in src:
            raise SystemExit("gpu/vk/vk_replay.cpp no longer has: " + needle)
    i = src.index("void present(const nvn_tex* t, const int crop[4]) {")
    head = src[i:i + 400]
    if "    present_pace_begin();" not in head or head.index("present_pace_begin") > head.index("perf_parts::Scope"):
        raise SystemExit("present() must begin with present_pace_begin(), before the renderer's own part and any lock")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="present-pace-rule-") as tmp:
        exe = Path(tmp) / "present-pace-rule"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_present_pace_rule.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
