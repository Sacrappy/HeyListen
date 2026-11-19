/* (HeyListen, after upstream Hookshot 8fcc8c22b and 9df055f23) Two places in the game that need whole 60 fps frames, under a
   Frame rate cap above 60, where the frame step is the frame's own time (host/hl_step.c, hos_frame_seconds_bits: 0.5 at 120
   fps). nx2/hl_gates.py hooks them (host/hl_high_fps.c: HL_SOUND_HOOK, HL_SOLVER_HOOK), and host/hl_high_fps.c holds the
   state. At a cap of 60 (no frame time given: timed 0) both return what the game computed, so nothing changes.

   The sound manager's job (f_80435300, its phase in w2): phase 0 hands the game step to the sound engine, whose fade and delay
   timers and parameter ramps count updates of at least one whole step. Fed less, some sounds are not released when an area
   unloads, the voices in use climb until the game's voice budget is full, and new sounds (sword, voices), then the area music,
   stop until a restart. Above 60 fps the job runs at 60 Hz: phase 0 adds the step to a remainder and runs once a whole step has
   built up, with a step of 1 (sound_step), keeping the rest; the job's other phases follow that frame's choice. Sound keeps the
   clock on average. A step of 1 or more runs as the game has it and keeps the remainder: around 60 fps (a cap above 60 with
   the game near 60, or VRR) steps of 0.98 and 1.02 alternate, and the 0.98 that did not run comes back as a whole step later
   (dropped, the sound clock ran at half the real one). A step of 0 or less (or NaN), and every phase at a cap of 60, run as the
   game has them, and clear the remainder.
   (Upstream after zortos293/Lichii PR #1289 by auz466, GPL-3.0: the same gate as a code patch for the emulator.)

   The music manager (game::Bgm, its update f_83373400, HL_BGM_HOOK; Longshot eb5ba6e2, Hookshot bf802502 by CraftyCorvid) runs
   inside that job, so at 60 Hz too, but reads the frame step and the frame's seconds from the game's time object itself (+16 and
   +24), not from phase 0: held to 60 Hz and handed the frame's own time its clock ran at 60 / fps of the real one (0.56 at 120
   fps: its timers, fades, stop delays and rest counter at half speed). Its loads go through sound_step and sound_seconds: while
   the gate holds the job, a whole step and that step's seconds (the frame's seconds over its step). It also reads a byte of
   change flags the battle manager sets for one game frame ([[0x84c9dda0]]+0x4898: bit 5 the battle music's name changed, bit 1
   its distance volume, bits 2 and 6, 6 running a count down) and syncs a property for each bit set. Held to 60 Hz it saw only
   the frames it ran: at 120 fps the end of a fight was lost half the time, and the battle music played on until the next fight
   or a reload (or stayed at a lower volume). On a frame the gate holds the job it ORs the byte, as the update would have read
   it then, into a latch (host/hl_high_fps.c); the update's next run reads its flags with them. Two settings of one bit within
   one held stretch are handled once.

   A horse's jump (f_82c59eb0, HL_HORSE_HOOK; Longshot 8e8a06db by CraftyCorvid, Epona gallops through fences above 60 fps): while a
   horse gets ready to jump an obstacle the state machine sets each frame the blend frames of the move into its JumpReady
   animation, so the blend ends at takeoff. With a step under 1 (slow motion at 60 fps) it takes last frame's plus the step
   instead; above 60 fps every step is under 1, the blend frames grew each frame (25 at 60 fps, 100 to 1500 above) and the jump
   animation never blended in. The step is compared with this frame's own step (the game's time object, +0x10) instead: a step
   that is the whole frame's is not slow motion, one under it still takes the game's branch; at a cap of 60 and on a frame
   slower than 60 fps the game's own test decides.

   A moving floor's carry (f_80159dc0, f_8015d490 and f_838baa40, HL_FLOOR_HOOK; Longshot 4f755527 by CraftyCorvid, issue 439): a
   character on a moving floor is carried by its velocity, but each module first tests the physics time object's 1/dt (+0x28)
   and, above 100 (a frame under 10 ms: at 60 fps, time slowed below 0.6), carries it by a tenth. Above 100 fps every frame is
   under 10 ms: on the Shadow Temple's barge Link and the enemies slid to its back. Above a cap of 60, on a frame shorter than a
   60 fps one, 1/dt times the frame's step is tested instead (the game's test as at 60 fps); at a cap of 60 and on a frame of a
   60 fps one or longer the game's own test decides.

   The physics world's solver steps: the game sets them each update to (int)(frame step * the steps of a 60 fps frame + 0.001)
   (f_838ceb80 and f_838cfc10, into the solver's +712; 8 a 60 fps frame in the places seen). A step under 1 cuts that short,
   and as the measured step wanders around its mean the count changes from frame to frame (8 x 0.49 is 3, 8 x 0.5 is 4 at 120
   fps); 4 steps a 60 fps frame would be 0 above 240. A moving platform (the stone one under the spikes in the Deku Tree)
   then slides out from under Link (upstream issue 1798). While the step is under 1 the count is rounded up instead, so a
   solver step never covers more time than at 60 fps, give or take 10% (the frame time's jitter, so a steady rate keeps a
   steady count), and it is at least 1 and never under the game's. A step of 1 or more (a frame slower than 60 fps) keeps the
   game's count. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float part;   /* (the steps added up since the job last ran, under 1) */
    int run;      /* (phase 0's choice, for the other phases of the same frame) */
} OotSoundGate;
#define OOT_SOUND_GATE_INIT {0.0f, 1}   /* (every phase runs until a phase 0 has chosen) */

static inline float oot_bits_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
static inline uint32_t oot_float_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

/* (whether the step is a part of a frame: the only case the gate and the feed change) */
static inline int oot_sound_partial(float step, int timed) {
    return timed && step > 0.0f && step < 1.0f;
}

/* 1: run the job now, 0: return from it at once */
static inline int oot_sound_gate(OotSoundGate *g, uint32_t phase, float step, int timed) {
    if (!timed) {   /* (a cap of 60: every phase of every frame, whatever came before) */
        g->part = 0.0f;
        g->run = 1;
        return 1;
    }
    if (phase != 0) return g->run;
    if (!(step > 0.0f)) {   /* (0, below 0, NaN: as the game has it) */
        g->part = 0.0f;
        g->run = 1;
        return 1;
    }
    if (step >= 1.0f) {   /* (a whole step or more: runs with it, the remainder kept for the next partial ones) */
        g->run = 1;
        return 1;
    }
    g->part += step;
    g->run = g->part >= 1.0f;
    if (g->run) g->part -= 1.0f;
    return g->run;
}

/* The step phase 0 hands to the sound engine: a whole one while the gate holds the job to whole steps */
static inline uint32_t oot_sound_step(uint32_t step_bits, int timed) {
    return oot_sound_partial(oot_bits_float(step_bits), timed) ? 0x3f800000u : step_bits;
}

/* The frame's seconds the music manager reads beside the step: a whole step's while the gate holds the job (with the step: the
   game's own base, 60 or the half-rate mode's 30, stays in it) */
static inline uint32_t oot_sound_seconds(uint32_t seconds_bits, uint32_t step_bits, int timed) {
    const float step = oot_bits_float(step_bits);
    return oot_sound_partial(step, timed) ? oot_float_bits(oot_bits_float(seconds_bits) / step) : seconds_bits;
}

/* The solver's steps: game (the game's own count), scaled_bits (frame step * steps a 60 fps frame + 0.001, the float it cut),
   per_frame_bits (the steps of a 60 fps frame, a float) */
static inline int32_t oot_solver_steps(int32_t game, uint32_t scaled_bits, uint32_t per_frame_bits, int timed) {
    const float scaled = oot_bits_float(scaled_bits) - 0.001f, per_frame = oot_bits_float(per_frame_bits);
    if (!timed || !(scaled > 0.0f) || !(per_frame > 0.0f) || !(scaled < per_frame) || per_frame > 1000.0f) return game;
    int32_t steps = (int32_t)ceilf(scaled - 0.1f);
    if (steps < 1) steps = 1;
    return steps > game ? steps : game;
}

/* A horse's jump: 1 when f_82c59eb0's step (step_bits) is under 1 only because the frame is short, so its branch for steps
   under 1 (slow motion) is skipped; 0 when the game's own test (step < 1) decides. frame_step_bits: this frame's step from the
   game's time object (0 when it has none). The step is the frame's own when within 0.1% of it */
static inline int oot_horse_whole_step(uint32_t step_bits, uint32_t frame_step_bits, int timed) {
    const float step = oot_bits_float(step_bits), frame = oot_bits_float(frame_step_bits);
    if (!timed || !(frame > 0.0f) || !(frame < 1.0f) || !(step > 0.0f)) return 0;
    return step >= frame * 0.999f;
}

/* A moving floor's carry: 1 when the modules' slow-time test stands (asked once the game's, 1/dt over 100, held), 0 when the
   frame is short and not slow, so the floor's velocity carries in full. inv_dt_bits: the physics time object's 1/dt;
   frame_step_bits: this frame's step from the game's time object (0 when it has none). On a frame of a 60 fps one or longer
   (a step of 1 or more) 1/dt times the step is at least 1/dt: the game's verdict */
static inline int oot_floor_carry_slow(uint32_t inv_dt_bits, uint32_t frame_step_bits, int timed) {
    const float inv_dt = oot_bits_float(inv_dt_bits), step = oot_bits_float(frame_step_bits);
    if (!timed || !(step > 0.0f)) return 1;   /* (the game's own test decides) */
    return inv_dt * step > 100.0f;
}
