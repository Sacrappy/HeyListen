// Copyright (C) 2026 Sacrappy
// The present pace's rule (gpu/vk/present_pace_rule.h), alone: tests/test_present_pace_rule.py compiles and runs this
#include "../gpu/vk/present_pace_rule.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace present_pace_rule;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// a run of frames: each ready `work[i]` after the last hand-over; the hand-over times
static std::vector<int64_t> run(const std::vector<int64_t>& work, int64_t period, int64_t early) {
    Line l;
    std::vector<int64_t> out;
    int64_t t = 1000000;
    for (int64_t w : work) {
        t += w;
        const Hold h = hold(l, t, period, early);
        if (h.until) t = h.until;
        out.push_back(t);
    }
    return out;
}

int main() {
    const int64_t P = 16666666;
    {   // early frames of uneven work: every interval one period (less the wake-up allowance on the first)
        std::vector<int64_t> w;
        for (int i = 0; i < 200; i++) w.push_back(9000000 + (i % 7) * 900000);
        const auto t = run(w, P, 0);
        for (size_t i = 2; i < t.size(); i++) CHECK(t[i] - t[i - 1] == P);
    }
    const int64_t T = 1000000000;   // (a line's point is never 0: 0 is "no line yet")
    {   // the wake-up allowance: the hold ends that much early, the line still moves by whole periods
        Line l;
        hold(l, T, P, 0);
        const Hold h = hold(l, T + 5000000, P, 200000);
        CHECK(h.until == T + P - 200000 && !h.late && l.at == T + P);
        const Hold h2 = hold(l, T + P + 6000000, P, 200000);
        CHECK(h2.until == T + 2 * P - 200000 && l.at == T + 2 * P);
    }
    {   // a late frame: the line starts again from it, and the next early frame waits a whole period (no short catch-up frame)
        Line l;
        hold(l, T, P, 0);
        const Hold late = hold(l, T + P + 5000000, P, 0);
        CHECK(late.late && !late.until && l.at == T + P + 5000000);
        const Hold next = hold(l, T + P + 5000000 + 8000000, P, 0);
        CHECK(next.until == T + 2 * P + 5000000);
    }
    {   // more than a period late (a load's freeze): the line starts again from it
        Line l;
        hold(l, T, P, 0);
        const Hold h = hold(l, T + 900000000, P, 0);
        CHECK(h.late && !h.until && l.at == T + 900000000);
    }
    {   // on time within the allowance: no wait, the line moves by a period
        Line l;
        hold(l, T, P, 0);
        const Hold h = hold(l, T + P - 100000, P, 200000);
        CHECK(!h.until && !h.late && l.at == T + P);
    }
    {   // no period (Unlimited, frame generation): nothing held, no line
        Line l;
        hold(l, T, P, 0);
        const Hold h = hold(l, T + 1000, 0, 0);
        CHECK(!h.until && l.at == 0);
    }
    {   // the later start: the work's high mark ends the margin before the next point; never negative, nothing without a line
        Line l;
        l.at = 100000000;
        CHECK(start_wait(l, 100000000 + 1000000, P, 11000000, 1500000) == P - 11000000 - 1500000 - 1000000);
        CHECK(start_wait(l, 100000000 + 10000000, P, 11000000, 1500000) == 0);
        Line none;
        CHECK(start_wait(none, 5, P, 1, 1) == 0);
        CHECK(start_wait(l, 100000000, P, -1, 0) == 0);
    }
    {   // the high mark: the n-th largest
        const int64_t v[] = {5, 9, 1, 7, 3, 9, 2};
        CHECK(high_mark(v, 7, 1) == 9);
        CHECK(high_mark(v, 7, 2) == 9);
        CHECK(high_mark(v, 7, 3) == 7);
        CHECK(high_mark(v, 7, 99) == 1);
        CHECK(high_mark(v, 0, 1) == -1);
    }
    {   // never two presents closer than a period less the allowance; none before the first
        CHECK(not_before(0, P, 200000) == 0);
        CHECK(not_before(T, P, 450000) == T + P - 450000);
        CHECK(not_before(T, 0, 0) == 0);
    }
    {   // the slew: by the excess over the target, a step at most; nothing at or under it (a late frame's wait of 0)
        CHECK(slew(5000000, 500000, 250000) == 250000);
        CHECK(slew(600000, 500000, 250000) == 100000);
        CHECK(slew(400000, 500000, 250000) == 0);
        CHECK(slew(0, 500000, 250000) == 0);
        const int64_t v[] = {9, 3, 7, 1, 5};
        CHECK(low_mark(v, 5, 1) == 1 && low_mark(v, 5, 2) == 3 && low_mark(v, 5, 9) == 9 && low_mark(v, 0, 1) == -1);
    }
    {   // the queue: two frames at most
        CHECK(!must_wait_for_room(0) && !must_wait_for_room(1) && must_wait_for_room(2) && must_wait_for_room(3));
    }
    {   // the pacer's loop on a game whose frames come on its own 60 Hz clock, 0-3 ms of spread, a late one now and then: every
        // interval at least a period less 0.45 ms, and the wait from ready to present comes down to a few ms (one clock)
        const int64_t early = 200000, step = 250000, target = 500000;
        Line l;
        int64_t last = 0, ring[64], hold_sum = 0, shortest = INT64_MAX;
        int ring_n = 0, ring_at = 0, n = 0;
        uint32_t seed = 12345;
        for (int k = 0; k < 4000; k++) {
            seed = seed * 1103515245u + 12345u;
            int64_t jitter = (int64_t)(seed >> 8) % 3000000;   // (0-3 ms after its grid point)
            if (k % 600 == 0) jitter += 6000000;                 // (a late one, every 10 s)
            const int64_t at = T + (int64_t)k * P + 11000000 + jitter;
            const Hold h = hold(l, at, P, early);
            int64_t shown = std::max(h.until, not_before(last, P, early + step));
            if (shown < at) shown = at;
            shown += 50000;   // (the wake's lateness)
            if (last) shortest = std::min(shortest, shown - last);
            last = shown;
            const int64_t held = shown - at;
            if (k >= 2000) hold_sum += held, n++;
            const int64_t slack = h.until ? std::max<int64_t>(0, h.until + early - at) : 0;   // (as the pacer: how early it came)
            ring[ring_at] = slack, ring_at = (ring_at + 1) % 64, ring_n = ring_n < 64 ? ring_n + 1 : 64;
            if (ring_n >= 16 && l.at) l.at -= slew(low_mark(ring, ring_n, 4), target, step);
        }
        std::printf("loop: shortest interval %.2f ms, mean wait %.2f ms\n", shortest / 1e6, hold_sum / 1e6 / n);
        CHECK(shortest >= P - early - step);
        CHECK(hold_sum / n < 3000000);
    }
    if (fails) return 1;
    std::printf("present pace rule: ok\n");
    return 0;
}
