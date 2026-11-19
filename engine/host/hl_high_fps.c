/* (HeyListen, after upstream Hookshot 8fcc8c22b, "keep sound and moving platforms working above a 60 fps cap", and 9df055f23,
   "the sound clock keeps its remainder around 60 fps, and cap 60 runs every phase") The sound manager's job at 60 Hz and the
   physics solver's steps rounded up, under a Frame rate cap above 60: host/oot_high_fps.h has why and the logic.

   Upstream edits the lifted f_80435300, f_838ceb80 and f_838cfc10 in nx2/oot_patches.py. Here the lifted work folder is shared
   by every worktree, so the calls go in the way the step and movie hooks do (host/hl_step.c): lift hooks, HL_SOUND_HOOK and
   HL_SOLVER_HOOK, HL_BGM_HOOK (Longshot eb5ba6e2, Hookshot bf802502 by CraftyCorvid: the music manager's clock and change flags) below with each function's text hash, lines nx2/hl_gates.py puts after anchors in that text, marked
   "hl gate <address + 1>", so a tree that does not ask for them takes them out. The functions' hashes are unchanged by them,
   and an exeFS mod that patches one leaves its hook out. The game's own code still runs: the sound job's step line sets
   vreg 0 again after the game's load, the solver's line sets x1 again after the game's cut.

   Both act only while the host publishes the frame's seconds (hos_frame_seconds_bits: a cap above 60 with the step hooks in).
   HOS_FPS_SOUND=0: the sound job every frame with the game's step; HOS_FPS_SOLVER=0: the game's solver count (comparisons;
   HeyListen's, upstream has no switch). HOS_FPS_LOG (set): what they do, every 600 calls. */
#include "hl_host.h"
#include "oot_high_fps.h"
#include <windows.h>   /* (SRWLOCK, InterlockedIncrement64) */
#include <stdlib.h>

HL_SOUND_HOOK(80435300, "795ee90e082f72a1");    /* the sound manager's job: its head, and its load of the game step */
HL_BGM_HOOK(83373400, "197e674c33455df1");      /* the music manager's update (inside that job): its clock and its change flags */
HL_TURN_HOOK(8263e8e0, "ee4f88d994c3249c");     /* the steering update's saved rate (Gohma: Longshot 3ab8cae6, Hookshot 93a8bff3) */
HL_HORSE_HOOK(82c59eb0, "877587970044fa8a");    /* a horse's jump state machine (Epona's fences: Longshot 8e8a06db) */
HL_FLOOR_HOOK(80159dc0, "5cc81137a91acbd0", "L_8015a39c");   /* a moving floor's carry in three character modules (the Shadow */
HL_FLOOR_HOOK(8015d490, "923be46f8950ebcd", "L_8015dad4");    /* Temple's barge: Longshot 4f755527) */
HL_FLOOR_HOOK(838baa40, "a40b3f20377e9b73", "L_838baf24");
HL_SOLVER_HOOK(838ceb80, "c4203f2465e06081");   /* the solver's step count, the two places the game sets it */
HL_SOLVER_HOOK(838cfc10, "228fd7a71a1ca0f0");

extern volatile uint32_t hos_frame_seconds_bits;   /* (hl_step.c: the frame's seconds, 0 at a cap of 60) */

/* (an env switch read once: 0 turns it off, anything else or unset leaves it on) */
static int switched(int *on, const char *name) {
    int v = __atomic_load_n(on, __ATOMIC_RELAXED);
    if (v < 0) v = !(getenv(name) && !atoi(getenv(name))), __atomic_store_n(on, v, __ATOMIC_RELAXED);
    return v;
}
static int sound_timed(void) {
    static int on = -1;
    return __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0 && switched(&on, "HOS_FPS_SOUND");
}
static int solver_timed(void) {
    static int on = -1;
    return __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0 && switched(&on, "HOS_FPS_SOLVER");
}
static int fps_log(void) {
    static int on = -1;   /* (read by the game's threads) */
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) v = getenv("HOS_FPS_LOG") != NULL, __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    return v;
}

static OotSoundGate sound_gate = OOT_SOUND_GATE_INIT;
static long long sound_calls, sound_runs;   /* (HOS_FPS_LOG's counts, under sound_lock) */
static SRWLOCK sound_lock = SRWLOCK_INIT;   /* (the job's phases may come from different threads) */
static uint64_t bgm_flags_at;   /* (the guest address of the music manager's change flags, from its last read; under sound_lock) */
static uint8_t bgm_held;        /* (the flags seen on frames the gate held the job, since the update last ran; under sound_lock) */
/* shortcut (upstream's): one bit set on two held frames between runs is handled once (bit 6 then counts down once) */
static uint8_t bgm_run;         /* (those, handed to the update's run: its own thread) */

/* f_80435300's head: phase w2, slot the guest address of the pointer to the object whose +0x10 is the game step (the job's own
   load). 0: the job returns at once */
int hos_oot_sound_gate(Ctx *c, uint32_t phase, uint64_t slot) {
    (void)c;
    float step = 1.0f;
    const int t = sound_timed();
    if (t && phase == 0) {
        const uint64_t object = G64(slot);
        const uint64_t time = object ? G64(object) : 0;
        if (time) step = oot_bits_float(G32(time + 0x10));
    }
    long long ran = -1;
    AcquireSRWLockExclusive(&sound_lock);
    const int run = oot_sound_gate(&sound_gate, phase, step, t);
    if (!run && bgm_flags_at) bgm_held |= *(const uint8_t *)GP(bgm_flags_at);   /* (what the update would have read now) */
    if (phase == 0 && fps_log()) {
        sound_runs += run;
        if (!(++sound_calls % 600)) ran = sound_runs, sound_runs = 0;
    }
    ReleaseSRWLockExclusive(&sound_lock);
    if (ran >= 0)
        hos_log("oot: sound job ran %lld of the last 600 frames (step %.3f%s)", ran, step,
                oot_sound_partial(step, t) ? ", held to 60 Hz" : "");
    return run;
}

/* The step f_80435300's phase 0 hands to the sound engine (its float's bits) */
uint32_t hos_oot_sound_step(uint32_t step_bits) {
    return oot_sound_step(step_bits, sound_timed());
}

/* The frame's seconds f_83373400 reads, with the step beside it (floats' bits) */
uint32_t hos_oot_sound_seconds(uint32_t seconds_bits, uint32_t step_bits) {
    return oot_sound_seconds(seconds_bits, step_bits, sound_timed());
}

/* f_83373400's head: the change flags held back since its last run are this run's */
void hos_oot_bgm_begin(void) {
    AcquireSRWLockExclusive(&sound_lock);
    bgm_run = bgm_held;
    bgm_held = 0;
    const uint8_t kept = bgm_run;
    ReleaseSRWLockExclusive(&sound_lock);
    if (kept && fps_log()) {   /* (HOS_FPS_LOG: each time the update gets flags its job's held frames saw: the game's own run would have lost them) */
        static volatile LONG64 n;
        const LONG64 k = InterlockedIncrement64(&n);
        if (k <= 200) hos_log("oot: music manager: change flags %#x kept from frames the sound job did not run (bit 5 the battle "
                              "music's name, bit 1 its volume), %lld so far", (unsigned)kept, (long long)k);
    }
}

/* f_83373400's reads of the change flags (at, their guest address; flags, the byte it read): with the ones held back */
uint64_t hos_oot_bgm_flags(uint64_t at, uint64_t flags) {
    AcquireSRWLockExclusive(&sound_lock);
    bgm_flags_at = at;
    const uint8_t run = bgm_run;
    ReleaseSRWLockExclusive(&sound_lock);
    return flags | run;
}

/* The solver steps of f_838ceb80 / f_838cfc10: game, the game's count; scaled_bits, the float it cut; per_frame_bits, the steps
   of a 60 fps frame (a float) */
uint32_t hos_oot_solver_steps(uint32_t game, uint32_t scaled_bits, uint32_t per_frame_bits) {
    const int32_t steps = oot_solver_steps((int32_t)game, scaled_bits, per_frame_bits, solver_timed());
    if (fps_log()) {
        static volatile LONG64 calls;
        const LONG64 n = InterlockedIncrement64(&calls);
        if (n % 600 == 1)
            hos_log("oot: physics solver steps %d (the game's %d: frame step %.3f x %.0f steps a 60 fps frame)", steps, (int32_t)game,
                    (oot_bits_float(scaled_bits) - 0.001f) / oot_bits_float(per_frame_bits), oot_bits_float(per_frame_bits));
    }
    return (uint32_t)steps;
}

/* (the frame's own time is published: a cap above 60 with the step hooks in) */
static int frame_timed(void) {
    return __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0;
}

/* The game's time object's step (this frame's, 1 at 60 fps) behind the pointer slot (the job's: the guest address of the pointer to
   the object whose first word points to it), 0 when there is none */
static uint32_t frame_step_at(uint64_t slot) {
    const uint64_t object = G64(slot);
    const uint64_t time = object ? G64(object) : 0;
    return time ? *(const uint32_t *)GP(time + 0x10) : 0;
}

/* f_82c59eb0's test of its step against 1 (a horse's jump): step_bits its step (a float's bits), slot as above. 1: the step is the
   whole frame's, the branch for steps under 1 is skipped (Longshot 8e8a06db) */
int hos_oot_horse_whole_step(uint32_t step_bits, uint64_t slot) {
    const int t = frame_timed();
    const uint32_t frame = t ? frame_step_at(slot) : 0;
    const int whole = oot_horse_whole_step(step_bits, frame, t);
    if (fps_log()) {
        static volatile LONG64 calls, skipped;
        const LONG64 s = whole ? InterlockedIncrement64(&skipped) : skipped;
        const LONG64 n = InterlockedIncrement64(&calls);
        if (n % 600 == 0)
            hos_log("oot: a horse's jump blend: the whole-frame step skipped the slow-motion branch %lld of %lld times (step %.3f, "
                    "the frame's %.3f)", (long long)s, (long long)n, oot_bits_float(step_bits), oot_bits_float(frame));
    }
    return whole;
}

/* f_80159dc0's, f_8015d490's and f_838baa40's slow-time test before they carry a character by a tenth of its floor's velocity
   (inv_dt_bits: the physics' 1/dt, which was over 100): 1, the tenth (the game's verdict) (Longshot 4f755527) */
int hos_oot_floor_carry_slow(uint32_t inv_dt_bits, uint64_t slot) {
    const int t = frame_timed();
    const uint32_t frame = t ? frame_step_at(slot) : 0;
    const int slow = oot_floor_carry_slow(inv_dt_bits, frame, t);
    if (fps_log()) {
        static volatile LONG64 calls, kept;
        const LONG64 k = slow ? InterlockedIncrement64(&kept) : kept;
        const LONG64 n = InterlockedIncrement64(&calls);
        if (n % 600 == 0)
            hos_log("oot: a moving floor's carry: the slow-time tenth kept %lld of %lld times (1/dt %.1f, the frame's step %.3f)",
                    (long long)k, (long long)n, oot_bits_float(inv_dt_bits), oot_bits_float(frame));
    }
    return slow;
}
