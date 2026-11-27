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
    {   // (mode 4) the slot line: early waits for its slot, a little late keeps the phase, a period late starts again
        Line l;
        Slot s = slot(l, T, P);
        CHECK(s.at == T && !s.late && l.at == T);                                   // (the first frame starts the line)
        s = slot(l, T + 5000000, P);
        CHECK(s.at == T + P && !s.late && l.at == T + P);                           // (early: shown at its slot)
        s = slot(l, T + 2 * P, P);
        CHECK(s.at == T + 2 * P && !s.late);                                        // (exactly on it)
        s = slot(l, T + 3 * P + 4000000, P);
        CHECK(s.at == T + 3 * P + 4000000 && s.late && !s.restarted && l.at == T + 3 * P);   // (4 ms late: at once, the phase kept)
        s = slot(l, T + 3 * P + 9000000, P);
        CHECK(s.at == T + 4 * P && !s.late && l.at == T + 4 * P);                   // (the frame after: its slot, under a period after the late one)
        s = slot(l, T + 6 * P + 1000000, P);
        CHECK(s.late && s.restarted && l.at == T + 6 * P + 1000000 && s.at == l.at);   // (a period or more behind: from it)
        s = slot(l, T + 6 * P + 2000000, P);
        CHECK(s.at == T + 7 * P + 1000000 && !s.late);
        s = slot(l, T, 0);
        CHECK(s.at == T && l.at == 0);                                              // (no pace: the line forgets)
        // the game's wait at its window acquire: until one period after the slot of the frame that used the texture
        CHECK(free_wait(T, T + 1000000, P) == P - 1000000);
        CHECK(free_wait(T, T + P, P) == 0 && free_wait(T, T + 2 * P, P) == 0);
        CHECK(free_wait(0, T, P) == 0 && free_wait(T, T, 0) == 0);
    }
    {   // (mode 4) the time step: the mean of the last five intervals between show times; a gap starts again
        Step st;
        CHECK(step(st, T, 250000000) == 0);
        CHECK(step(st, T + P, 250000000) == P);
        CHECK(step(st, T + 2 * P + 3000000, 250000000) == P + 1500000);
        for (int k = 3; k < 9; k++) step(st, T + (int64_t)k * P, 250000000);
        CHECK(step(st, T + 9 * P, 250000000) == P);                                 // (five even intervals: exactly the period)
        CHECK(step(st, T + 9 * P + 300000000, 250000000) == 0);                     // (a load: again from here)
        CHECK(step(st, T + 9 * P + 300000000 + P, 250000000) == P);
        CHECK(step(st, T, 250000000) == 0);                                         // (a time not after the last one)
    }
    // (mode 4) the whole loop, the game's frames started by the line itself: a 60 limit, 11 ms of work with 0-3 ms of spread, a
    // frame of 30 ms and one of 45 ms. `back` is the host's run-ahead + 1 (HL_PRESENT_PACE_AHEAD).
    for (int back = 2; back <= 3; back++) {
        Line l;
        Step st;
        int64_t slots[16] = {}, ready = T, last = 0, step_ns = 0, at100 = 0, wait_sum = 0;
        uint64_t n = 0;
        int lates = 0, restarts = 0, off = 0, uneven = 0, waits = 0;
        uint32_t seed = 777;
        for (int k = 0; k < 3000; k++) {
            const int64_t slot_back = n >= (uint64_t)back ? slots[(n - (uint64_t)back) & 15] : 0;
            const int64_t start = ready + free_wait(slot_back, ready, P);           // (the window acquire)
            seed = seed * 1103515245u + 12345u;
            int64_t work = 11000000 + (int64_t)(seed >> 8) % 3000000;
            if (k == 1500) work = 30000000;
            if (k == 2200) work = 45000000;
            ready = start + work;                                                   // (the hand-over)
            const Slot s = slot(l, ready, P);
            slots[n++ & 15] = s.at;
            step_ns = step(st, s.at, 250000000);
            if (s.late) lates++, restarts += s.restarted;
            if (k == 100) at100 = s.at;
            const bool calm = k > 100 && (k < 1500 || k > 1516) && (k < 2200 || k > 2216);   // (not the frames around the two slow ones)
            if (calm && last && s.at - last != P) off++;
            if (calm && step_ns != P) uneven++;
            if (calm) wait_sum += s.at - ready, waits++;
            last = s.at;
        }
        std::printf("slot loop, %d frame(s) ahead: %d late (%d restarts), %d intervals off the period, %d uneven steps, phase moved %lld ns, "
                    "the wait from ready to shown %.2f ms\n", back - 1, lates, restarts, off, uneven,
                    (long long)(last - at100 - (int64_t)2899 * P), wait_sum / 1e6 / waits);
        // (every calm frame on its slot, the step exactly the period, the phase kept through both slow frames: the rate holds)
        CHECK(off == 0 && uneven == 0 && restarts <= 1);
        if (back == 2) {
            // (one frame ahead: a frame waits its slack, the period less its work, about 4 ms here; the 30 ms frame and the 45 ms
            // one are shown late, with the one or two after them while the game catches up)
            CHECK(wait_sum / waits > 3000000 && wait_sum / waits < 6000000);
            CHECK(lates >= 2 && lates <= 8);
        } else {
            // (two frames ahead, what three window textures allow: the 30 ms frame is taken by the run-ahead, but every frame
            // waits a period more)
            CHECK(wait_sum / waits > 19000000 && wait_sum / waits < 23000000);
            CHECK(lates >= 1 && lates <= 3 && restarts == 0);
            CHECK(last - at100 == (int64_t)2899 * P);
        }
    }
    {   // (mode 4) whether the line paces: it stops once a quarter of the last 32 paced frames came late, and paces again once 64
        // frames in a row came at the limit's rate on average
        Able a;
        for (int k = 0; k < 200; k++) CHECK(paced(a, k % 20 == 0));        // (one late in twenty: it paces)
        CHECK(a.on);
        int stopped_at = -1;
        for (int k = 0; k < 40 && stopped_at < 0; k++)
            if (!paced(a, k % 2 == 0)) stopped_at = k;                     // (every second frame late: stops at the 8th late one)
        CHECK(stopped_at == 14 && !a.on);
        int64_t t = T;
        for (int k = 0; k < 200; k++) CHECK(!passed(a, t += P * 12 / 10, P));   // (frames 20 % slower than the limit: stays stopped)
        bool on = false;
        int after = 0;
        for (int k = 0; k < 200 && !on; k++, after++) on = passed(a, t += P, P);   // (the limit held again: on within two counts of 64)
        CHECK(on && after <= 129 && a.on && a.late_bits == 0);
    }
    {   // (mode 4) the whole loop on a game that cannot hold its limit (work 10-13 ms against a period of 8.33 ms): with the stop,
        // the frames go out as they come: the rate is the game's own, not lower
        const int64_t P120 = 8333333;
        Line l;
        Able a;
        int64_t ready = T, first = 0, last = 0;
        int shown = 0, passes = 0;
        uint32_t seed = 4242;
        for (int k = 0; k < 2000; k++) {
            seed = seed * 1103515245u + 12345u;
            ready += 10000000 + (int64_t)(seed >> 8) % 3000000;            // (no acquire wait: the game is behind its limit)
            int64_t at = ready;
            if (!a.on) {
                passes++;
                if (passed(a, ready, P120)) l = {};
            } else {
                const Slot s = slot(l, ready, P120);
                at = s.at;
                paced(a, s.late);
            }
            if (k == 100) first = at;
            last = at, shown++;
        }
        const double fps = 1899.0 / ((last - first) / 1e9), own = 1e9 / 11500000.0;
        std::printf("under the limit: %.1f fps shown against the game's own %.1f; %d of 2000 frames went out as they came\n", fps, own, passes);
        CHECK(fps > own * 0.99 && passes > 1900);
    }
    if (fails) return 1;
    std::printf("present pace rule: ok\n");
    return 0;
}
