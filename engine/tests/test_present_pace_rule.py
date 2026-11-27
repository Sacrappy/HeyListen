# Copyright (C) 2026 Sacrappy
"""Compile and run the present pace's rule (gpu/vk/present_pace_rule.h; no game/GPU): at a frame rate limit each frame is handed
over on an even line (an early frame waits for its point, a late one starts the line again: no short frame after it); and by the
text, that vk_replay.cpp holds at present()'s start, before the order lock, and only where the pace applies; and that the pacer
thread (=3) keeps two presents a period apart, moves its line earlier, and bounds its queue at two."""
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
                   "if (dev.present_mode_name && dev.present_mode_name[0] == 'f') return 0;",
                   "present_pace_rule::not_before(t_last, j.period, early + 250000)",
                   "present_pace_rule::slew(present_pace_rule::low_mark(t_ring, t_ring_n, 4), target, 250000)",
                   "const int64_t slack = h.until ? std::max<int64_t>(0, h.until + early - j.at) : 0;",
                   "if (present_pace_rule::must_wait_for_room((int)t_q.size())) {",
                   "res = pace_thread_push(dev.swapchain, index);"]:
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
