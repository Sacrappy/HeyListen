// Copyright (C) 2026 Sacrappy
#pragma once
// (ARR1, 27 Nov 2026) The present pace's rule, apart from the renderer so that tests/test_present_pace_rule.py runs it alone.
// At a frame rate limit the game's window acquire waits for its frame's place (host/nn_vi.c), at the START of the game's frame:
// the present then comes one period after the one before plus the difference of the two frames' work, and a mailbox swapchain
// (V-Sync off) shows that difference. Here the frame is handed over on an even line instead: early, it waits for the line's next
// point; on time (within the wake-up's allowance), the line moves on by a period; late, the line starts again from it, so a late
// frame is never followed by a short one to catch up.
#include <algorithm>
#include <cstdint>

namespace present_pace_rule {

struct Line {
    int64_t at = 0;   // (the line's last point, ns; 0: none yet)
};

struct Hold {
    int64_t until = 0;   // (> 0: wait until then)
    bool late = false;   // (the frame came after its point: the line moved to it)
};

/// The frame handed over at `now`: the hold, and the line moved on. `early_ns`: the hold ends that much before the point (the
/// timer's wake-up); the line moves by whole periods, never by the wake-up's lateness
inline Hold hold(Line& l, int64_t now, int64_t period, int64_t early_ns) {
    Hold h;
    if (period <= 0) {
        l.at = 0;
        return h;
    }
    if (l.at && now < l.at + period - early_ns) {
        h.until = l.at + period - early_ns;
        l.at += period;
    } else if (l.at && now < l.at + 2 * period) {
        l.at += period;
        if (now > l.at) l.at = now, h.late = true;
    } else {
        h.late = l.at != 0;
        l.at = now;
    }
    return h;
}

/// (HL_PRESENT_PACE=2) After the present: how long the game's next frame may still wait before it starts, so that its work
/// (`work_ns`, a high mark of the recent frames') ends `margin_ns` before the line's next point. The input is then read later
/// and the hold above is short: the delay the hold adds between a frame's end and its present is not added to the input's age
inline int64_t start_wait(const Line& l, int64_t now, int64_t period, int64_t work_ns, int64_t margin_ns) {
    if (!l.at || period <= 0 || work_ns < 0) return 0;
    const int64_t start = l.at + period - work_ns - margin_ns;
    return start > now ? start - now : 0;
}

/// The high mark of the last frames' work: the n-th largest of `v[0..count)` (n from 1), the ring's values in any order
inline int64_t high_mark(const int64_t* v, int count, int n) {
    if (count <= 0) return -1;
    if (count > 64) count = 64;
    if (n < 1) n = 1;
    if (n > count) n = count;
    int64_t t[64];
    for (int i = 0; i < count; i++) t[i] = v[i];
    std::nth_element(t, t + (n - 1), t + count, [](int64_t a, int64_t b) { return a > b; });
    return t[n - 1];
}

}  // namespace present_pace_rule
