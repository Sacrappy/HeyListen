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

/// (the pacer thread, HL_PRESENT_PACE=3) The earliest the next present may go out: one period after the last one, less the
/// wake-up's allowance. Never two presents closer than that, whatever the line says (a queued frame, a restarted line)
inline int64_t not_before(int64_t last_present, int64_t period, int64_t early_ns) {
    return last_present && period > 0 ? last_present + period - early_ns : 0;
}

/// (one clock) The line only moves later by itself (a late frame restarts it from itself), so it settles where every frame waits
/// almost a period: the game's own clock (its window acquire, host/nn_vi.c) and the line drift apart in phase. Here the line moves
/// EARLIER, by the excess of the recent frames' smallest waits (`low_hold`, a low mark of them) over `target_ns`, `max_step_ns` a
/// frame at most: the presents stay even (each interval shortened by that step at most) and the wait comes down to about the
/// frames' own spread. A late frame gives a wait of 0: the low mark falls under the target and the line stops moving
inline int64_t slew(int64_t low_hold, int64_t target_ns, int64_t max_step_ns) {
    const int64_t x = low_hold - target_ns;
    return x > 0 ? std::min(x, max_step_ns) : 0;
}

/// The low mark of the last frames' waits: the n-th smallest of `v[0..count)` (n from 1); -1 for none
inline int64_t low_mark(const int64_t* v, int count, int n) {
    if (count <= 0) return -1;
    if (count > 64) count = 64;
    if (n < 1) n = 1;
    if (n > count) n = count;
    int64_t t[64];
    for (int i = 0; i < count; i++) t[i] = v[i];
    std::nth_element(t, t + (n - 1), t + count);
    return t[n - 1];
}

/// The queue between the game's hand-over and the pacer thread: two frames at most (the game one frame ahead, never more). A third
/// waits for room (the hand-over blocks, a period at most): no frame is dropped and none is shown early
constexpr int kQueueMost = 2;
inline bool must_wait_for_room(int queued) { return queued >= kQueueMost; }

/// (HL_PRESENT_PACE=4, the slot line) The rule of MacNativeGames' nvn_pace.h, which Rekarina paces with too, written again here:
/// a frame's slot is the slot before plus one period, given when the frame is HANDED OVER (not when the pacer thread takes it).
/// Early: the frame is shown at its slot. Late by less than a period: shown at once and the line KEEPS ITS PHASE (the slot stays
/// where it was, so the frame after is shown a little less than a period later and the rate holds at the limit). A period or more
/// behind: the line starts again from the frame, with no burst to catch up. One line for everything: the pacer thread shows at
/// the slot, the game's window acquire waits on the slots (free_wait) and its time step follows them (Step)
struct Slot {
    int64_t at = 0;           // (when the frame is shown: its slot, or `now` for a late one)
    bool late = false;        // (the frame came after its slot)
    bool restarted = false;   // (a period or more behind: the line starts again from it)
};
inline Slot slot(Line& l, int64_t now, int64_t period) {
    Slot s;
    s.at = now;
    if (period <= 0) {
        l.at = 0;
        return s;
    }
    if (!l.at) {
        l.at = now;
        return s;
    }
    const int64_t next = l.at + period;
    if (now <= next) {
        l.at = s.at = next;
        return s;
    }
    s.late = true;
    if (now - next < period) l.at = next;
    else l.at = now, s.restarted = true;
    return s;
}

/// (the slot line) Whether the line paces at all. Where the game cannot hold its limit the line only costs frames: a frame
/// faster than the period waits for its slot, a slower one starts the line again, and nothing is caught up (the Market in DLAA at
/// a 120 limit this PC cannot hold: 93.8 fps without the pacer, 64.6 with it; at a 90 limit 80.4 against 72.5). So the line STOPS
/// pacing once a quarter of the last 32 paced frames came late (paced), and while it is stopped the frames go out as they come
/// and the game's own wait holds the limit (host/nn_vi.c); it paces AGAIN once 64 frames in a row came at the limit's rate on
/// average, within 1 % (passed): the game holds its limit again
struct Able {
    uint32_t late_bits = 0;          // (the last 32 paced frames, a bit each: late)
    int64_t last = 0, sum = 0;       // (stopped: the last hand-over, and the sum of the intervals since the count began)
    int n = 0;
    bool on = true;
};
inline int ones32(uint32_t v) {
    int c = 0;
    for (; v; v &= v - 1) c++;
    return c;
}
/// a frame the line paced, late or not: whether the line still paces
inline bool paced(Able& a, bool late) {
    a.late_bits = a.late_bits << 1 | (late ? 1u : 0u);
    if (ones32(a.late_bits) >= 8) a = Able{}, a.on = false;
    return a.on;
}
/// a frame handed over at `now` while the line is stopped: whether it paces again from the next frame
inline bool passed(Able& a, int64_t now, int64_t period) {
    if (a.last && now > a.last) a.sum += now - a.last, a.n++;
    a.last = now;
    if (a.n >= 64) {
        if (period > 0 && a.sum <= (int64_t)64 * period * 101 / 100) a = Able{};
        else a.sum = 0, a.n = 0;
    }
    return a.on;
}

/// (the slot line) The game's wait at its window acquire: the window texture it takes is free one period after the slot of the
/// frame that used it (`slot_back`: the slot of the frame as many hand-overs back as the window has textures; 0: none yet). The
/// game's frames start on the line's own clock, so the line never has to follow another one
inline int64_t free_wait(int64_t slot_back, int64_t now, int64_t period) {
    return slot_back && period > 0 && slot_back + period > now ? slot_back + period - now : 0;
}

/// (the slot line) The game's time step: the mean of the last intervals between the frames' show times, five at most, so the
/// game advances by what is shown, the pacer's waits included (a steady line: exactly the period). A gap over `gap_ns` (a
/// load) or a time not after the last one starts again: 0 until there is an interval
struct Step {
    int64_t last = 0, d[5] = {};
    int n = 0, at = 0;
};
inline int64_t step(Step& s, int64_t shown_at, int64_t gap_ns) {
    if (!s.last || shown_at <= s.last || shown_at - s.last > gap_ns) {
        s.last = shown_at, s.n = s.at = 0;
        return 0;
    }
    s.d[s.at] = shown_at - s.last, s.at = (s.at + 1) % 5, s.last = shown_at;
    if (s.n < 5) s.n++;
    int64_t sum = 0;
    for (int i = 0; i < s.n; i++) sum += s.d[i];
    return sum / s.n;
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
