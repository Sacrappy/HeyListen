// Copyright (C) 2026 Sacrappy

// The 1% low and 0.1% low of the FPS counter's full overlay (F10), as the usual tools show them (RTSS, MSI Afterburner): the
// frame rate of the 99th and of the 99.9th percentile's frame time, that is 1000 / the frame time of the ceil(fraction * N)-th
// slowest frame of the window, at least the slowest one. Until 27 Nov 2026 they were the benchmark's (tools/bench/run_bench.py
// low_fps: the MEAN frame time of the slowest 1 %) over the last 10 s or 1000 frames: at 80 fps the 1 % was 10 frames, so one
// stall of half a second took the line from 50 to 14 fps for 12 s, far under what players read in other tools at the same
// moment. The benchmark keeps its own definition (one long stall counts for its length there). The window is the last 30 s or
// the last 1000 frames, whichever is longer (the ring's capacity at most: 30 s up to 546 frames a second). The frames are the
// ones the counter counts: every frame shown (with frame generation the made ones too). tests/test_perf_lows.py
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace mkwvk {
namespace perflows {

constexpr int kCap = 16384;                    // frame times kept: 30 s up to 546 shown frames a second
constexpr int kLeast = 1000;                   // the window holds at least this many frames...
constexpr int64_t kWindowNs = 30000000000LL;   // ...and at least this much time
constexpr int kShowFrom = 100;                 // fewer frames than this in the window: the lines read "-"

/// A ring of the latest frame times (ms) and when each ended (ns). Static storage, nothing allocated while frames come in
struct Ring {
    float ms[kCap];
    int64_t at[kCap];
    int pos, n;
    void clear() { pos = n = 0; }
    void add(float frame_ms, int64_t now) {
        ms[pos] = frame_ms, at[pos] = now;
        pos = (pos + 1) % kCap;
        if (n < kCap) n++;
    }
    /// The window's frame times, newest first, copied to out (room for kCap); how many
    int window(int64_t now, float* out) const {
        int c = 0;
        while (c < n) {
            const int k = (pos - 1 - c + kCap) % kCap;
            if (c >= kLeast && now - at[k] > kWindowNs) break;
            out[c++] = ms[k];
        }
        return c;
    }
};

/// How many of the n slowest frames the fraction takes: ceil(fraction * n), one at least
inline int slowest(int n, double fraction) { return n > 0 ? std::max(1, (int)std::ceil(fraction * n - 1e-9)) : 0; }
/// The low for `fraction` of v[0..n) (reordered): 1000 / the frame time of the slowest(n, fraction)-th slowest frame, the
/// percentile's own frame. 0 for no frames
inline double low_fps(float* v, int n, double fraction) {
    if (n <= 0) return 0.0;
    const int k = slowest(n, fraction);
    std::nth_element(v, v + (n - k), v + n);   // (the k slowest end up in v[n-k..n), the k-th slowest at v[n-k])
    return v[n - k] > 0 ? 1000.0 / v[n - k] : 0.0;
}
/// Both lows from one window (reordered): the 1% selection first, the 0.1% one inside it
inline void lows(float* v, int n, double* low1, double* low01) {
    *low1 = low_fps(v, n, 0.01);
    const int k1 = slowest(n, 0.01);   // (low_fps just left the k1 slowest in v[n-k1..n))
    *low01 = k1 > 0 ? low_fps(v + (n - k1), k1, 0.001 * n / k1) : 0.0;   // (the same frame as slowest(n, 0.001) of the whole window)
}
/// The line's number: "-" while the window holds too few frames to say
inline void text(char* b, size_t cap, double fps, int frames) {
    if (frames < kShowFrom || fps <= 0) std::snprintf(b, cap, "-");
    else std::snprintf(b, cap, "%.0f FPS", fps);
}

}  // namespace perflows
}  // namespace mkwvk
