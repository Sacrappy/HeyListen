/* (HeyListen, after upstream Hookshot f915af93) Whole frames out of a frame step that can be under one, for the game's counters
   that add the step, or the step times a whole number, cut to whole frames (nx2/hl_gates.py HL_WHOLE_HOOK, host/hl_step.c
   hos_frame_whole). At the console's 60 fps the step is 1 and they count frames. Under a Frame rate cap above 60 the step is
   the frame's own time (host/hl_step.c, hos_frame_seconds_bits): 0.42 at 144 fps, cut to 0 every frame, and the counter never
   moves, so what waits on it never comes: the pick-up indicator above Link's head counts the rupees shown up to the wallet's
   (f_80a87e50: the shown +864 moves by (int)(step * the distance / 50, at least 1)) and fades only when they meet; it stayed
   at the old count until a door or a talk replaced it. While the frames' own time is the step (carry), the amount is added to
   the counter's own remainder and the whole frames in it are handed out, so the counter moves at the game's rate whatever the
   frame rate. Otherwise it is cut as the game cuts it: 60 fps as before. One remainder per counter address (or per object:
   each water surface keeps its own), reused oldest first. 64 slots: a scene's water surfaces each take one every frame, and
   more of them than slots would evict each other in turn, every one starting again from 0 and standing still (upstream
   6df80a8d). Not thread-safe by itself: the caller holds a lock. */
#pragma once
#include <stdint.h>
#include <string.h>

#define FRAME_WHOLE_SLOTS 64
typedef struct {
    uint64_t at[FRAME_WHOLE_SLOTS];
    float rem[FRAME_WHOLE_SLOTS];
    uint32_t used[FRAME_WHOLE_SLOTS];   /* (the call count at the slot's last use: the oldest is reused) */
    uint32_t calls;
} FrameWhole;

/* (as the lifted code cuts it: toward zero, NaN and below zero to 0, at most 0x7fffffff: the counts are small and positive) */
static inline uint32_t frame_whole_cut(float f) {
    if (!(f > 0.0f)) return 0;
    if (f >= 2147483648.0f) return 0x7fffffffu;
    return (uint32_t)f;
}

static inline uint32_t frame_whole(FrameWhole *w, uint64_t at, uint32_t amount_bits, int carry) {
    float amount;
    memcpy(&amount, &amount_bits, 4);
    if (!carry || !(amount > 0.0f) || amount >= 2147483648.0f) return frame_whole_cut(amount);
    w->calls++;
    int slot = -1, oldest = 0;
    for (int i = 0; i < FRAME_WHOLE_SLOTS; i++) {
        if (w->at[i] == at) { slot = i; break; }
        if (w->used[i] < w->used[oldest]) oldest = i;
    }
    if (slot < 0) slot = oldest, w->at[slot] = at, w->rem[slot] = 0.0f;
    w->used[slot] = w->calls;
    const float sum = w->rem[slot] + amount;
    const uint32_t whole = frame_whole_cut(sum);
    w->rem[slot] = sum - (float)whole;
    return whole;
}

/* (the whole frames of the amount, times a whole number of substeps each, for a counter that runs that many substeps a frame:
   the water's ripple simulation, f_800672e0; Longshot 01d0263d by Jeff Aigner, issue 1199). The game cuts the amount and then
   multiplies, so above 60 fps a frame ran all its substeps or none, and the ripples stood still for a frame and then jumped.
   With the carry the substeps are handed out instead, their fractions kept per counter: 2 or 3 of 4 a frame at 108 fps.
   Without it, cut as the game cuts it. */
static inline uint32_t frame_whole_times(FrameWhole *w, uint64_t at, uint32_t amount_bits, uint32_t times, int carry) {
    float amount;
    memcpy(&amount, &amount_bits, 4);
    if (!carry) return frame_whole_cut(amount) * times;
    const float scaled = amount * (float)times;
    uint32_t scaled_bits;
    memcpy(&scaled_bits, &scaled, 4);
    return frame_whole(w, at, scaled_bits, 1);
}
