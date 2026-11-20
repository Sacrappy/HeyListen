"""The Frame generation row stops at DLSS Frame Generation's runtime while DLSS FG makes the frames.

NGX tells the most frames its runtime makes between two of the game's (MultiFrameCountMax: 5 on an RTX 50's, 1 on an
RTX 40's). Past it NGX was asked for no set at all (vk_dlss.cpp dlss_fg_frames) and the renderer's own frame generation made every
frame, with DLSS FG chosen: 7x and 8x on an RTX 50 were never DLSS FG. Now the fixed multiplier frame generation runs at,
framegen_now (gpu/vk/vk_framegen.cpp), is the row's, but at most the runtime's most + 1 while DLSS FG makes the frames; Off as it
is; a saved value past it is kept (another engine runs it) and is shown and run as that top. (Hookshot ec76f696, pmwork-personal;
there it was framegen_capped, which is framegen_rows.h's clamp here, with the other modes' limits as well: tests/test_framegen_rows.py.)

The real framegen_rows::clamp is compiled and run for DLSS FG: 6x at most on a runtime that makes 5, 2x on one that makes 1, the
row's value while the most is not known (0: NGX not asked yet, or another engine). The text checks: vk_dlss.cpp's dlss_fg_most gives
the runtime's most only while DLSS-G is asked for and can run, from a temporal Upscaler's inputs (0 in a build without it);
multiplier() takes framegen_now().

    python -I -B tests/test_framegen_dlss_cap.py [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

MAIN = r"""
#include <cstdio>
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using mkwvk::framegen_rows::BY_DLSS;
using mkwvk::framegen_rows::clamp;
int main() {
    /* an RTX 50's runtime: 5 made frames at most, so 6x */
    CHECK(clamp(BY_DLSS, 5, 8) == 6 && clamp(BY_DLSS, 5, 7) == 6 && clamp(BY_DLSS, 5, 6) == 6);
    CHECK(clamp(BY_DLSS, 5, 5) == 5 && clamp(BY_DLSS, 5, 2) == 2);
    /* an RTX 40's: 1 at most, so 2x */
    CHECK(clamp(BY_DLSS, 1, 8) == 2 && clamp(BY_DLSS, 1, 4) == 2 && clamp(BY_DLSS, 1, 2) == 2);
    /* not known yet, or another engine: the row's */
    CHECK(clamp(BY_DLSS, 0, 8) == 8 && clamp(BY_DLSS, 0, 3) == 3);
    /* Off as it is */
    CHECK(clamp(BY_DLSS, 5, 0) == 0 && clamp(BY_DLSS, 1, 0) == 0);
    if (failures) return 1;
    std::puts("PASS: DLSS FG's multiplier stops at its runtime's most + 1");
    return 0;
}
"""


def check_sources(framegen, dlss):
    mult = block(framegen, "int multiplier() {", where="gpu/vk/vk_framegen.cpp")
    assert "framegen_now()" in mult and "opts.framegen" not in mult, \
        "multiplier: frame generation runs at framegen_now(), the row's as the mode that makes the frames supports it"
    now = block(framegen, "int framegen_now() {", where="gpu/vk/vk_framegen.cpp")
    assert "framegen_rows::clamp(framegen_by_now(), dlss_fg_most(), opts.framegen)" in now, \
        "framegen_now: the row's value, capped by the mode (DLSS FG's runtime, AMD FSR 3.1's 2x)"
    most = [b for b in dlss.split("int dlss_fg_most() {")[1:]]
    assert len(most) == 2, "vk_dlss.cpp: dlss_fg_most with NVIDIA's SDK and without it"
    real = block(dlss, "int dlss_fg_most() {\n    if (!dlss_fg_wanted()", where="gpu/vk/vk_dlss.cpp")
    for part in ("dlss_fg_wanted()", "!temporal_wanted()", "most_known"):
        assert part in real, "dlss_fg_most: " + part
    assert "int dlss_fg_most() { return 0; }" in dlss, "vk_dlss.cpp: 0 in a build without NVIDIA's SDK"
    frames = block(dlss, "int dlss_fg_frames(VkCommandBuffer cmd,", where="gpu/vk/vk_dlss.cpp")
    too_many = frames[frames.index("if ((unsigned)count > g.most) {"):]
    assert too_many.index("if (!temporal_wanted()) return 0;") < too_many.index("g.too_many++"), \
        "dlss_fg_frames: without a temporal Upscaler (no cap) the reason is the inputs, not the runtime's most"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    vk = ROOT / "gpu" / "vk"
    framegen = (vk / "vk_framegen.cpp").read_text(encoding="utf-8")
    dlss = (vk / "vk_dlss.cpp").read_text(encoding="utf-8")
    rows = (vk / "framegen_rows.h").read_text(encoding="utf-8").replace("#pragma once", "")
    fixture = rows + MAIN
    check_sources(framegen, dlss)
    with tempfile.TemporaryDirectory(prefix="framegen-dlss-cap-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
