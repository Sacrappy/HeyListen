"""The FPS box's first sample is not the frames since launch divided by one second.

HeyListen draws the counter with vk_overlay.cpp's fps_tick (the performance overlay, F10), which replaced upstream's fps_take: the
first frame only starts the clock, then a sample is the presents of a window of at least half a second over that window's own
length, so turning the counter on after thousands of frames, or a stall, never publishes a total as a rate.

python -I -B tests/test_fps_rate.py [--cxx g++]
Executes the real fps_tick and its state; no GPU or game data.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_overlay.cpp").read_text(encoding="utf-8")
    if "void fps_tick(" not in source:
        raise SystemExit("the FPS box has no fps_tick: update this check")
    state = source[source.index("uint64_t fps_last_ns"):source.index("void fps_tick(")]
    fn = block(source, "void fps_tick(")
    fixture = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
''' + state + fn + r'''
int main() {
    const uint64_t s = 1000000000ull, frame = 16666667ull;   /* (1/60 s, rounded up: 30 of them are just over half a second) */
    uint64_t t = 400 * s;   /* (a long way into the run, thousands of frames in) */
    /* the first frame starts the clock: nothing is published */
    fps_tick(t);
    CHECK(!fps_text[0] && fps_frames == 0);
    /* a window shorter than half a second publishes nothing yet */
    for (int i = 0; i < 29; i++) fps_tick(t += frame);
    CHECK(!fps_text[0] && fps_frames == 29);
    /* the 30th frame ends the window: 60 fps, 16.7 ms */
    fps_tick(t += frame);
    CHECK(!strcmp(fps_text, "60 fps  16.7 ms  max 16.7 ms"));
    CHECK(fps_frames == 0 && fps_win_ns == 0);
    /* a stall longer than two seconds is not averaged in: the window restarts, the text stays */
    char before[96];
    std::memcpy(before, fps_text, sizeof before);
    fps_tick(t += 3 * s);
    CHECK(fps_frames == 0 && fps_win_ns == 0 && !strcmp(before, fps_text));
    for (int i = 0; i < 30; i++) fps_tick(t += frame * 2);   /* 30 fps, a second long: two windows' worth, published at half */
    CHECK(!strcmp(fps_text, "30 fps  33.3 ms  max 33.3 ms"));
    /* a slow frame inside a window is its max, and counts in the rate */
    for (int i = 0; i < 20; i++) fps_tick(t += frame);
    fps_tick(t += 200000000ull);   /* 20 frames at 60 and one of 200 ms: 21 frames in 533 ms */
    CHECK(!strcmp(fps_text, "39 fps  25.4 ms  max 200.0 ms"));
    std::printf("PASS: the first sample starts the interval, and the next one is a rate\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="fps-rate-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
