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
   Also (Longshot d30b9479 and 10010630, both by CraftyCorvid): HL_DRIVE_HOOK, Link's driven body's target while he pulls himself up a
   ledge, and HL_SONG_HOOK, the ocarina playbacks' note flag once a note (host/oot_high_fps.h has both). HOS_FPS_LEDGE=0 and
   HOS_FPS_SONG=0 leave the game's.
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
HL_DRIVE_HOOK(804f5030, "c502d681942b9169");    /* a rigid body's update before the physics step: Link's driven body's target (Longshot d30b9479) */
HL_SONG_HOOK(832b2af0, "46558136b433148c");     /* the song just played, and the ocarina's other melody playback: the note flag */
HL_SONG_HOOK(832b2dc0, "828538cb182b748e");     /* (Longshot 10010630) */

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
static long long sound_job_runs;   /* (phase 0's runs, for the song playbacks' note flag; under sound_lock) */
static OotSongCue song_cue[2];     /* (each playback's last read of it; under sound_lock) */
static long long cue_reads, cue_held;   /* (HOS_FPS_LOG's counts, under sound_lock) */

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
    if (run && phase == 0) sound_job_runs++;
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

/* After f_832b7b50 in a song's playback (site 0: f_832b2af0, the song just played; 1: f_832b2dc0, the ocarina's other melody
   playback): game, its result (bit 0: the note flag is set). Above a cap of 60, bit 0 cleared when the flag was read set before
   and the sound job has not run since (host/oot_high_fps.h). HOS_FPS_SONG=0: the game's */
static int song_timed(void) {
    static int on = -1;
    return __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0 && switched(&on, "HOS_FPS_SONG");
}
uint64_t hos_oot_song_cue(uint32_t site, uint64_t game) {
    const int t = song_timed();
    long long held = -1;
    AcquireSRWLockExclusive(&sound_lock);
    const int cue = oot_song_cue(&song_cue[site & 1], (int)(game & 1), sound_job_runs, t);
    if (t && fps_log()) {
        cue_held += (int)(game & 1) && !cue;
        if (!(++cue_reads % 600)) held = cue_held, cue_held = 0;
    }
    ReleaseSRWLockExclusive(&sound_lock);
    if (held >= 0) hos_log("oot: a song's playback read its note flag set again before the sound job ran %lld of the last 600 reads", held);
    return cue ? game : game & ~(uint64_t)1;
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

/* Link's driven body's target (f_804f5030 on its path for a body driven to a target: host/oot_high_fps.h has why; Longshot
   d30b9479 by CraftyCorvid). HOS_FPS_LEDGE=0: the game's target. */
static int ledge_timed(void) {
    static int on = -1;
    return frame_timed() && switched(&on, "HOS_FPS_LEDGE");
}
static volatile uint64_t link_body;      /* (Link's rigid body: hos_oot_note_link, from his update) */
static volatile uint32_t link_updates;   /* (Link's updates, counted there: a carry needs the update before) */
static OotDriveCarry drive_carry;        /* (under drive_lock) */
static SRWLOCK drive_lock = SRWLOCK_INIT;

/* Link's update (host/hos_main.c, hos_oot_lift_player): his components (actor +624); their +0x50, +0x20, +0x10, +8 is his
   physics character, whose +0x20 is his rigid body. Each pointer is checked to be in guest memory. The count of his updates
   goes first, whatever the walk finds */
void hos_oot_note_link(uint64_t components) {
    __atomic_add_fetch(&link_updates, 1, __ATOMIC_RELAXED);
    uint64_t p = components;
    static const uint32_t walk[5] = {0x50, 0x20, 0x10, 8, 0x20};
    for (int i = 0; i < 5; i++) p = p >= HOS_HEAP_BASE && hos_guest_range(p + walk[i], 8) ? G64(p + walk[i]) : 0;
    if (p >= HOS_HEAP_BASE && hos_guest_range(p + 0x8c, 0x24)) __atomic_store_n(&link_body, p, __ATOMIC_RELAXED);
}

/* f_804f5030, on its path for a body driven to a target, before it reads the target: controller x20 (its target at +0x3c, its
   flags at +0x100), body x23 or x24 (its position at +0x8c, +0x9c, +0xac), slot the guest address of the pointer to the
   game's time object (its +0x10 the frame's step) */
void hos_oot_drive_target(uint64_t controller, uint64_t body, uint64_t slot) {
    if (!body || body != __atomic_load_n(&link_body, __ATOMIC_RELAXED)) return;
    const uint64_t object = G64(slot);
    const uint64_t time = object ? G64(object) : 0;
    const float step = time ? oot_bits_float(*(const uint32_t *)GP(time + 0x10)) : 0.0f;
    const int driven = ledge_timed() && step > 0.0f && step < 1.0f && (G64(controller + 0x100) & 0x180) == 0x180;
    float at[3], target[3], game[3];
    memcpy(&at[0], GP(body + 0x8c), 4);
    memcpy(&at[1], GP(body + 0x9c), 4);
    memcpy(&at[2], GP(body + 0xac), 4);
    memcpy(target, GP(controller + 0x3c), sizeof target);
    memcpy(game, target, sizeof game);
    AcquireSRWLockExclusive(&drive_lock);
    oot_drive_carry(&drive_carry, body, __atomic_load_n(&link_updates, __ATOMIC_RELAXED), driven, at, target);
    ReleaseSRWLockExclusive(&drive_lock);
    if (!driven || !memcmp(game, target, sizeof game)) return;
    memcpy(GP(controller + 0x3c), target, sizeof target);
    if (fps_log()) {
        static volatile LONG64 calls;
        if (InterlockedIncrement64(&calls) % 600 == 0)
            hos_log("oot: Link's driven body: its target carried %.4f m more than the game's (step %.3f)",
                    sqrtf((target[0] - game[0]) * (target[0] - game[0]) + (target[1] - game[1]) * (target[1] - game[1]) +
                          (target[2] - game[2]) * (target[2] - game[2])), step);
    }
}
