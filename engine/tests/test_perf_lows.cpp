// Copyright (C) 2026 Sacrappy
// perf_lows (gpu/vk/perf_lows.h): the FPS counter's 1% and 0.1% lows. tests/test_perf_lows.py
//   test_perf_lows <file>: reads "n" then n frame times (ms) and prints the two lows, to compare with the percentile's frame
//   test_perf_lows: the window's and the text's own checks
#include "../gpu/vk/perf_lows.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mkwvk::perflows;

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static int window_after(int frames, double frame_ms) {   // frames of this length, the window at the last one's end
    static Ring ring;
    ring.clear();
    static float out[kCap];
    int64_t t = 0;
    for (int i = 0; i < frames; i++) {
        t += (int64_t)(frame_ms * 1e6);
        ring.add((float)frame_ms, t);
    }
    return ring.window(t, out);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::FILE* f = std::fopen(argv[1], "r");
        if (!f) return 2;
        int n = 0;
        if (std::fscanf(f, "%d", &n) != 1) return 2;
        std::vector<float> v((size_t)n);
        for (int i = 0; i < n; i++)
            if (std::fscanf(f, "%f", &v[(size_t)i]) != 1) return 2;
        std::fclose(f);
        double a, b;
        lows(v.data(), n, &a, &b);
        std::printf("%.12f %.12f\n", a, b);
        return 0;
    }
    expect(window_after(1500, 2.0) == 1500, "500 fps: 1500 frames in 3 s: all of them (under 30 s)");
    expect(window_after(3000, 40.0) == 1000, "25 fps: 1000 frames reach back 40 s, longer than 30 s: 1000");
    expect(window_after(3000, 20.0) >= 1499 && window_after(3000, 20.0) <= 1502, "50 fps: 30 s of frames, about 1500");
    expect(window_after(9000, 1000.0 / 240) >= 7199 && window_after(9000, 1000.0 / 240) <= 7202, "240 fps: 30 s of frames, about 7200");
    expect(window_after(300, 16.0) == 300, "fewer frames than that: all of them");
    expect(window_after(kCap + 100, 0.5) == kCap, "a ring that wrapped keeps its capacity");
    {   // a slow stretch at 10 fps: 1000 frames reach back 100 s but the ring has 40 only
        expect(window_after(40, 100.0) == 40, "40 frames of 100 ms: the 40");
    }
    char b[32];
    text(b, sizeof b, 57.4, 50);
    expect(!std::strcmp(b, "-"), "under 100 frames: a dash");
    text(b, sizeof b, 57.4, 500);
    expect(!std::strcmp(b, "57 FPS"), "the number, rounded, with its unit");
    text(b, sizeof b, 0, 500);
    expect(!std::strcmp(b, "-"), "no number: a dash");
    {   // a known series: 1000 frames of 10 ms, 10 of them 50 ms: the 10th slowest is a 50 ms one
        std::vector<float> v(1000, 10.0f);
        for (int i = 0; i < 10; i++) v[(size_t)i * 90] = 50.0f;
        double a, c;
        lows(v.data(), 1000, &a, &c);
        expect(std::abs(a - 1000.0 / 50.0) < 1e-9, "1%: the 10th slowest frame is a 50 ms one: 20 fps");
        expect(std::abs(c - 1000.0 / 50.0) < 1e-9, "0.1%: the slowest frame: 20 fps");
    }
    {   // what the change is for: 2400 frames of 12.5 ms (80 fps, 30 s), twenty-three of 20 ms and ONE stall of 500 ms: the 1% low
        // is the 24th slowest frame, 12.5 ms: 80 fps (the mean of the slowest 24 would read 25 fps); the 0.1% low is the 3rd slowest
        std::vector<float> v(2400, 12.5f);
        for (int i = 0; i < 23; i++) v[(size_t)i * 100 + 7] = 20.0f;
        v[1234] = 500.0f;
        double a, c;
        lows(v.data(), 2400, &a, &c);
        expect(std::abs(a - 1000.0 / 20.0) < 1e-9, "1%: one stall does not move it: the 24th slowest is a 20 ms frame, 50 fps");
        expect(std::abs(c - 1000.0 / 20.0) < 1e-9, "0.1%: the 3rd slowest frame, 20 ms: 50 fps");
        expect(slowest(2400, 0.01) == 24 && slowest(2400, 0.001) == 3 && slowest(50, 0.001) == 1 && slowest(0, 0.01) == 0, "how many frames a fraction takes");
    }
    if (failures) return 1;
    std::printf("perf_lows: all checks passed\n");
    return 0;
}
