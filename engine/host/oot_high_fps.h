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
   game's count.

   A driven body's target (f_804f5030, HL_DRIVE_HOOK; Longshot d30b9479 / Hookshot 72f2a07c by CraftyCorvid, issue 1494), a rigid
   body's update before the physics step, on its path for a body driven to a target (the controller's flags +0x100 have 0x80, the
   game's gate to the path, and 0x100, its gate to handing the velocity to the world): while Link pulls himself up a ledge, his
   animation drives his body. Each update the game sets the controller's target (+0x3c) to the body's position plus this frame's
   move and asks the physics for the velocity that reaches it, (target - position) x 1/dt. At 60 fps the body lands on the target;
   above it Havok's constraint solve delivers only part of the move (1.00 at 60, 0.94 at 90, 0.86 at 120, 0.82 at 144, 0.78 at
   170), and the next target starts from where the body ended up, so the shortfall is never made up: the pull-up lifted Link 2.5 m
   instead of 2.8 at 140 fps and he slid back on the lip until he got over, falling back to the hang when forward was let go.
   Above a cap of 60, on a frame shorter than a 60 fps one, Link's driven body is given last update's target plus this frame's move
   instead (its target plus what the physics left of the last one), at most three frames' moves more, so it follows the animation's
   path. A climb starts from the game's own target (the carry needs the update before). At a cap of 60 nothing is carried: the body
   lands on its target there. HOS_FPS_LEDGE=0 leaves the game's target.

   A song's playback after the ocarina (f_832b2af0, the song just played; f_832b2dc0, the ocarina's other melody playback, the
   same code; HL_SONG_HOOK; Longshot 10010630 / Hookshot 23777491 by CraftyCorvid): each update while the melody plays it asks
   f_832b7b50 whether the note flag is set, a word of the melody's player that the sound engine writes on each of its runs, in the
   sound job's phase 0: set on the run where the melody reaches a note, else clear. When it is set the playback goes on to the next
   note. At 60 fps the playback reads it once a run. Above 60 the job runs at 60 Hz, the flag stayed set for two or three updates at
   120 to 165 fps and each one went on a note: the notes came two or three at a time, ahead of the music. Above 60 fps a set flag
   read again before the job has run again is the same note: the playback reads 0. The job's runs are counted at the gate, at phase
   0's head, before the engine writes the flag in that phase. At a cap of 60 the game's value. HOS_FPS_SONG=0: the game's. */
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

/* A driven body's target with last update's shortfall carried. st: the last target given, the game's then, its body and Link's
   update count then; body: the body (its guest address); update: Link's update count now (host/hos_main.c counts them);
   driven: 1 for Link's body driven to a target above a 60 fps cap on a frame under a 60 fps one (0: the game's target is left
   as it is; the next driven call then comes after a gap); at: the body's position now; target: the game's (the position plus
   this frame's move), changed in place.
   - A carry needs the same body and its last driven call in this update or the one before: the path is not reached on the
     updates between two climbs, so a climb starts from the game's own target (the lag carried then, about (1/share - 1) of a
     move, is lost on any gap). The count only finds gaps: a wrong count drops a carry, it never holds a target back.
   - A target bit for bit the game's last (the body update run again before the physics) or the last one given (not renewed)
     gets the last target given, so nothing is carried twice.
   - The carry is at most three of this frame's moves (the physics' share down to a quarter, about 400 fps; above that it
     falls behind), so a body the physics held back (a wall) is not flung when it frees. */
typedef struct { float target[3], game[3]; uint64_t body; uint32_t update; int held; } OotDriveCarry;
static inline void oot_drive_carry(OotDriveCarry *st, uint64_t body, uint32_t update, int driven, const float at[3],
                                   float target[3]) {
    for (int i = 0; i < 3 && driven; i++)
        if (!(target[i] == target[i]) || !(at[i] == at[i]) || fabsf(target[i]) > 1e30f || fabsf(at[i]) > 1e30f) driven = 0;
    if (!driven)
        return;   /* (st->update stays behind: the next driven call is a gap) */
    const int same = st->held && st->body == body && update - st->update <= 1u;
    st->update = update;
    if (same && (!memcmp(st->game, target, sizeof st->game) || !memcmp(st->target, target, sizeof st->target))) {
        memcpy(target, st->target, sizeof st->target);   /* (run again, or not renewed: the target given) */
        return;
    }
    memcpy(st->game, target, sizeof st->game);
    if (same) {
        float carry[3], carried = 0.0f, move = 0.0f;
        for (int i = 0; i < 3; i++) {
            carry[i] = st->target[i] - at[i];
            carried += carry[i] * carry[i];
            move += (target[i] - at[i]) * (target[i] - at[i]);
        }
        const float scale = carried > 9.0f * move ? 3.0f * sqrtf(move / carried) : 1.0f;   /* (at most three moves) */
        for (int i = 0; i < 3 && carried > 0.0f; i++) target[i] += carry[i] * scale;
    }
    memcpy(st->target, target, sizeof st->target);
    st->body = body;
    st->held = 1;
}

typedef struct {
    int set;          /* (the flag at the playback's last read) */
    long long runs;   /* (the sound job's runs then) */
} OotSongCue;

/* The note flag a song's playback reads: set, the game's (1 set, 0 clear); runs, the sound job's runs so far. 1 when the
   playback goes on a note */
static inline int oot_song_cue(OotSongCue *s, int set, long long runs, int timed) {
    const int again = timed && set && s->set && runs == s->runs;   /* (read before the job ran again: the same note) */
    s->set = set;
    s->runs = runs;
    return set && !again;
}
