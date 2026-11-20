/* (HeyListen, after Longshot 6aa5d653 by cuecuemoar, issue 988) The clock of an animation that loops over one frame or less.
   Such a loop is one pose: the frozen Zoras under the ice in adult Zora's Domain (start 0, end 1) are drawn from it and stand
   still. At the console's 60 fps the step is 1, every update of the clock lands on the same phase and the pose never changes.
   Under a Frame rate cap above 60 the step is the frame's own time (host/hl_step.c, hos_frame_seconds_bits: 0.36 at 165 fps),
   the phase moves by it each frame and wraps, so each frame draws a different phase of the loop: the Zoras twitched.
   Two places make the time a skeleton is drawn at (nx2/hl_gates.py, HL_LOOP_HOOK and HL_POSE_HOOK):
   - loop_step: the clock's step. The loop of one frame or less takes whole frames of it, the fractions carried per animation
     entry, so it moves as often as at 60 fps and by the same whole steps;
   - loop_pose: the time written for the skeleton to be drawn at. The loop's start: the phase a clock moving in whole frames
     stays on.
   Longer loops, and every loop at a cap of 60 (timed 0), keep the step and the time the game has. Not thread-safe by itself:
   the caller holds a lock around loop_step. 4096 entries, found by their address with a short probe: a scene has some hundreds
   at most, and an entry that finds no place starts again from 0 (a pose that stands still, as it did). */
#pragma once
#include <stdint.h>
#include <string.h>

#define LOOP_STEP_SLOTS 4096
#define LOOP_STEP_PROBE 32
typedef struct {
    uint64_t at[LOOP_STEP_SLOTS];
    float rem[LOOP_STEP_SLOTS];
} LoopStep;

/* (a loop of one frame or less: start < end and end - start <= 1; NaN is not) */
static inline int loop_is_pose(uint32_t start_bits, uint32_t end_bits) {
    float start, end;
    memcpy(&start, &start_bits, 4);
    memcpy(&end, &end_bits, 4);
    return end > start && end - start <= 1.0f;
}

/* The step (a float's bits) an entry at `at` takes. `timed`: the frames' own time is the step (a Frame rate cap above 60). */
static inline uint32_t loop_step(LoopStep *s, uint64_t at, uint32_t step_bits, uint32_t start_bits, uint32_t end_bits, int timed) {
    float step;
    memcpy(&step, &step_bits, 4);
    if (!timed || !(step > 0.0f) || !loop_is_pose(start_bits, end_bits)) return step_bits;
    uint32_t h = (uint32_t)((at * 0x9E3779B97F4A7C15ull) >> 40) % LOOP_STEP_SLOTS, k = 0;
    while (k < LOOP_STEP_PROBE && s->at[(h + k) % LOOP_STEP_SLOTS] && s->at[(h + k) % LOOP_STEP_SLOTS] != at) k++;
    const uint32_t i = (h + (k < LOOP_STEP_PROBE ? k : 0)) % LOOP_STEP_SLOTS;
    if (s->at[i] != at) s->at[i] = at, s->rem[i] = 0.0f;
    const float sum = s->rem[i] + step;
    const float whole = sum >= 2147483648.0f ? 2147483648.0f : (float)(uint32_t)sum;
    s->rem[i] = sum - whole;
    memcpy(&step_bits, &whole, 4);
    return step_bits;
}

/* The time (a float's bits) a skeleton is drawn at, for a clock that may only move in [start, end]: the start for a loop of
   one frame or less while `timed`, else the time as it is. */
static inline uint32_t loop_pose(uint32_t time_bits, uint32_t start_bits, uint32_t end_bits, int timed) {
    return timed && loop_is_pose(start_bits, end_bits) ? start_bits : time_bits;
}
