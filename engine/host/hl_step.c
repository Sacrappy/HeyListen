/* (HeyListen) The game's time step under a frame rate cap above 60: the host's measured frame time at the engine's two
   step divides (after upstream Hookshot a2a89ae11, "keep the game at real speed under a Frame rate cap above 60").

   The engine makes its time step in two places from its integer nominal frame rate (the framework's +0x2c8), each a float
   divide "base / nominal": f_80706be0 (the module system's frame: base 60, 30 in the half-rate mode; the frame's step,
   added to the game clock at +0xd58 and handed to the frame-time object, docs/native-engine/main-loop.md) and f_83b6ea90
   (the game's framework's prepare: the global step 60 / nominal, the float at 0x84d61fc8). The engine's own rate is 60
   over a whole number of display intervals, never above 60, and its controller puts 60 back before the steps are made:
   the old way (the measured rate written into +0x2c8, nn_os_sync.c TICK-014) reached the first divide only, and the
   global step stayed a whole 1/60 s a frame: what reads it ran fps / 60 too fast (cutscenes, voice lines, the Deku Tree
   boss above 60). With the hooks the step is base x the frame's seconds at both, whatever the rate.

   The hooks are lines nx2/hl_gates.py puts after the two divides (HL_STEP_HOOK below: both or neither) in the lifted work
   folder, which every worktree shares: a tree without this file asks for none and takes them out, so it links without
   the two symbols. nn_vi.c publishes the frame's seconds (hos_frame_seconds_bits) while the cap is above 60 and the hook
   is in (hos_step_probe set by the first one), and keeps the nominal rate at 60 at most then. HOS_FPS_STEP=0: as before. */
#include "hl_host.h"
#include "frame_whole.h"
#include "loop_step.h"
#include <windows.h>   /* (SRWLOCK: hos_movie_frames, hos_frame_whole) */
#include <stdlib.h>
#include <string.h>

volatile uint32_t hos_frame_seconds_bits = 0;   /* the frame's seconds as a float's bits; 0: the game's own divide */
volatile uint64_t hos_step_probe = 0;           /* f_80706be0's object (its +0xd58: the game clock in 1/60 s) once the hook ran */

HL_STEP_HOOK(80706be0, "9e4c8d7da24dafcf", 8, 1);   /* the frame's step (vreg 8), and the probe */
HL_STEP_HOOK(83b6ea90, "1f02e7327f22f9b9", 0, 0);   /* the global step (vreg 0) */

/* A movie's frame count above a 60 cap (after upstream Hookshot a7cc747d5 and 244a71e00). The game's movie player adds 1 to
   a count at +760 of itself each update (f_83b7c710), and a cutscene that shows a movie takes that count as its timeline's
   frame (f_83b5ec10, f_83b5e9a0): its narration, its rumble and its own end are placed by it. The movie's picture and sound
   follow the clock, so above 60 fps the count ran fps / 60 times too fast: the narration lines started over each other, and
   the opening story movie went black at half its length at a cap of 120 (the cutscene ended, the decoder played on).
   The hook (HL_MOVIE_HOOK below, a line nx2/hl_gates.py puts after that add) makes the step hos_movie_frames(player): how
   many 60ths of a second this update is worth while the host publishes the frame's seconds (hos_frame_seconds_bits: a cap
   above 60 with the step hooks in), the rest carried to that player's next update; 1, the game's own step, otherwise, and
   with HOS_FPS_MOVIE=0 (for comparisons).
   The step is more than 1 when a frame takes longer than a 60th (up to 4, the longest step nn_vi.c publishes), and the
   count then passes values over. The timeline does not need each one: it fires the events whose time lies between its last
   frame and the new one, walking a list sorted by time (f_8124cfc0). Measured upstream at 36 fps under a cap of 120, steps
   of 1 and 2 all the way: every narration line and the rumble as at 60. A step held at 1 would lose that time instead. */
HL_MOVIE_HOOK(83b7c710, "5d406bbf9b83d30a", 760);

uint32_t hos_movie_frames(uint64_t player) {
    enum { PLACES = 16 };
    const uint32_t bits = __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED);   /* (nn_vi.c writes it at each present) */
    if (!bits) return 1;
    static SRWLOCK lock = SRWLOCK_INIT;
    static int timed = -1;   /* (these four under the lock) */
    static struct { uint64_t player, used; double part; } carry[PLACES];
    static uint64_t calls;
    static unsigned made;
    float secs;
    uint32_t n = 1;
    memcpy(&secs, &bits, 4);
    AcquireSRWLockExclusive(&lock);
    if (timed < 0) timed = !(getenv("HOS_FPS_MOVIE") && !atoi(getenv("HOS_FPS_MOVIE")));
    if (timed) {
        int i = 0;
        while (i < PLACES && carry[i].player != player) i++;
        if (i == PLACES) {
            /* (a player not seen yet takes the place used longest ago, so a movie that plays keeps its own while others come
               and go: its count is exact while no more than PLACES players, itself among them, update in turn. It starts at a
               part of a 60th drawn from a hash of how many places were made: with that many players at once every update is a
               first one, and each count then still grows by the frames' time on average instead of standing still) */
            uint32_t x = made++;
            x ^= x >> 16, x *= 0x85ebca6bu, x ^= x >> 13, x *= 0xc2b2ae35u, x ^= x >> 16;   /* (MurmurHash3's last step) */
            i = 0;
            for (int k = 1; k < PLACES; k++)
                if (carry[k].used < carry[i].used) i = k;
            carry[i].player = player, carry[i].part = x / 4294967296.0;
        }
        carry[i].used = ++calls;
        carry[i].part += (double)secs * 60.0;
        n = (uint32_t)carry[i].part;
        carry[i].part -= n;
    }
    ReleaseSRWLockExclusive(&lock);
    return n;
}

/* The game's counters that add the frame step, or an amount of it, cut to whole frames, above a 60 cap (after upstream
   Hookshot f915af93, #2006). At 60 fps the step is 1; above a 60 cap it is the frame's own time, under 1 (0.42 at 144 fps),
   and the cut is 0 every frame, so the counter stands still. A whole hook (HL_WHOLE_HOOK below: the function, its text hash,
   the register the cut goes into, the guest address the remainder is kept for) is a line nx2/hl_gates.py puts after that
   cut, making the count hos_frame_whole(key, the amount, the game's count): the amount added to that counter's own
   remainder and its whole frames handed out (host/frame_whole.h) while the host publishes the frame's seconds; the game's
   own count, exactly, otherwise and with HOS_FPS_WHOLE=0 (comparisons; HeyListen's, upstream has no switch). A site is
   added once it is seen to misbehave in game:
   - f_80a87e50, the pick-up indicator above Link's head (a rupee, a Deku Stick): the count it shows (+864) moves toward the
     wallet's (+860) by (int)(step * max(distance / 50, 1)) and the indicator fades when they meet. Cut to 0, it stayed at
     the old count until a door or a talk replaced it (upstream issue 1832).
   - f_803edf70, the time of day (after upstream f0c40fc96, #2093): each frame it adds (int)(2 * step) game seconds (+60) to
     the seconds of the day (+40, a day of 86400, then the day count +44). Cut, it added 1 a frame below 120 fps (the day
     slower: 83% at 100) and 0 above 120, where the clock stood still and the sun swung back and forth over the frozen hour.
     The function cuts into x8 the same way once more, in the branch that sets the time from another source: that one stays
     the game's (the hook's anchor is the cut stored to +60).
   - f_800672e0, the water renderer (after upstream 6df80a8d, #2409): its ripple simulation runs (int)step times the water's
     substeps (+76 of the pass) each frame. Cut to 0, the rings Link and the actors make in water stood still above 60 fps.
     Whole frames carried, a frame ran all its substeps or none (0 or 4 at 108 fps), and the rings stood still for a frame and then
     jumped (Longshot 01d0263d by Jeff Aigner, issue 1199): the hook's "t8" hands out the whole substeps instead (hos_frame_whole_times,
     the load of the substeps' count in x8 right before the cut; x8 then 1), 2 or 3 every frame; HOS_FPS_WATER=0 keeps the whole
     frames carried and multiplies them, as before. Its remainder is kept per water object (x21). Found by atk3001 for Lichii (zortos293/Lichii#1152), ported to Fritchii
     by Frizti (Frizti/Fritchii d389685a4, 063dbb741), then to Hookshot by hckrman101.
   - f_80652cb0, the Deku Tree's floor web (after upstream db0dd7140, #2929): it cuts the elapsed time divided by 1/60 to
     whole simulation steps, then clamps them to 0..3. Cut to 0, the web did not react when Link jumped on it above 60 fps.
     Its remainder is kept per web object (x19 + 2296), before the clamp, which stays: a long pause leaves no backlog.
   - f_832b32b0, Scarecrow's Song playback, f_832b6840, the eight-note recording (mode 10) and f_832b2690, its completion
     (after upstream Hookshot 2d8044fb, Longshot 08771847, #2906, #2828, Longshot #87 and #294): the elapsed frames (+0x265fc)
     advance the saved note delays, are reset between the notes and gaps and are saved as their lengths, and the eighth note's
     length is written when it ends. Cut to 0 above 60 fps, playback waited on its first note, and a recording lost its
     rhythm (the notes' lengths 0). The three share the one counter, so one remainder (the cuts are unsigned, the third
     argument of the hook; f_832b6840 has a second unsigned cut, that of the other recording mode (13), left as it is, so
     its hook is the one before the addition to x10: "u10"). A song whose rhythm was lost is not restored: record it again. */
HL_WHOLE_HOOK(80a87e50, "7464f66dbdfb2fd2", 12, "c->x[19] + 864");
HL_WHOLE_HOOK(803edf70, "29ce7b235d0d6bfd", 8, "c->x[19] + 60");
HL_WHOLE_HOOK(800672e0, "feef3d08fe1f642d", 9, "c->x[21]", "t8");
HL_WHOLE_HOOK(80652cb0, "9593be89631328d3", 22, "c->x[19] + 2296");
HL_WHOLE_HOOK(832b32b0, "5b78765ca27d412c", 8, "c->x[19] + 0x265fc", "u");
HL_WHOLE_HOOK(832b6840, "51843fdaa0ed4e2d", 8, "c->x[9]", "u10");
HL_WHOLE_HOOK(832b2690, "25068bf751cb70b1", 8, "c->x[19] + 0x265fc", "u");

uint32_t hos_frame_whole(uint64_t at, uint32_t amount_bits, uint32_t game) {
    static FrameWhole state;   /* (these two under the lock: the game's update threads) */
    static SRWLOCK lock = SRWLOCK_INIT;
    static int on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) v = !(getenv("HOS_FPS_WHOLE") && !atoi(getenv("HOS_FPS_WHOLE"))), __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    if (!v || !__atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED)) return game;
    AcquireSRWLockExclusive(&lock);
    const uint32_t whole = frame_whole(&state, at, amount_bits, 1);
    ReleaseSRWLockExclusive(&lock);
    return whole;
}

/* The water's ripple simulation's whole substeps (hl_gates' "t8" of the water hook above): the count of its substeps a frame in
   `times`, game the game's whole frames, which the game multiplies by them. HOS_FPS_WATER=0, or no frame time published, the
   whole frames (carried while the frame time is published, HOS_FPS_WHOLE) times the substeps, as the game would. */
uint32_t hos_frame_whole_times(uint64_t at, uint32_t amount_bits, uint32_t times) {
    static FrameWhole state;   /* (under the lock: the game's update threads) */
    static SRWLOCK lock = SRWLOCK_INIT;
    static int on = -1, water = -1;
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED), w = __atomic_load_n(&water, __ATOMIC_RELAXED);
    if (v < 0) v = !(getenv("HOS_FPS_WHOLE") && !atoi(getenv("HOS_FPS_WHOLE"))), __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    if (w < 0) w = !(getenv("HOS_FPS_WATER") && !atoi(getenv("HOS_FPS_WATER"))), __atomic_store_n(&water, w, __ATOMIC_RELAXED);
    const int timed = __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0;
    AcquireSRWLockExclusive(&lock);
    uint32_t whole;
    if (v && timed && !w) {   /* (the whole frames carried per counter, then multiplied: the substeps all or none) */
        whole = frame_whole(&state, at, amount_bits, 1) * times;
    } else {
        whole = frame_whole_times(&state, at, amount_bits, times, v && timed);
    }
    ReleaseSRWLockExclusive(&lock);
    return whole;
}

/* A looping animation one frame long (the frozen Zoras under the ice in adult Zora's Domain: start 0, end 1) is one pose
   (host/loop_step.h; Longshot 6aa5d653 by cuecuemoar, issue 988). Two lift hooks (nx2/hl_gates.py) make the time a skeleton is
   drawn at:
   - HL_LOOP_HOOK, f_802cfca0 advances the clock of each animation entry ([x20]: flags +0, time +4, speed +12, start +16, end +20)
     and copies it to the skeleton's time. Its two advances multiply the step (vreg 8) by the definition's speed first: a loop
     of one frame or less takes the step as whole frames, carried per entry (hos_loop_step); longer loops keep the fractional step.
   - HL_POSE_HOOK, f_80944aa0 writes the time the skeletons are drawn at (x22: the value +0, the range it is kept in +4, +8)
     from a clock of its own that also moved by the fractional step: a range of one frame or less is drawn at its start
     (hos_loop_pose), the phase of a clock that moves in whole frames.
   A cap of 60 (hos_frame_seconds_bits 0) leaves both as the game has them. HOS_FPS_LOOP=0: the game's, both places. */
HL_LOOP_HOOK(802cfca0, "9111a12ac4e739a5");
HL_POSE_HOOK(80944aa0, "7554cb8a60fc4426");

static int loop_timed(void) {
    static int on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) v = !(getenv("HOS_FPS_LOOP") && !atoi(getenv("HOS_FPS_LOOP"))), __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    return v && __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0;
}

uint32_t hos_loop_step(uint64_t at, uint32_t step_bits, uint32_t start_bits, uint32_t end_bits) {
    static LoopStep state;   /* (the game's update threads: under the lock) */
    static SRWLOCK lock = SRWLOCK_INIT;
    if (!loop_timed() || !loop_is_pose(start_bits, end_bits)) return step_bits;   /* (most entries: no lock) */
    AcquireSRWLockExclusive(&lock);
    const uint32_t whole = loop_step(&state, at, step_bits, start_bits, end_bits, 1);
    ReleaseSRWLockExclusive(&lock);
    return whole;
}

uint32_t hos_loop_pose(uint32_t time_bits, uint32_t start_bits, uint32_t end_bits) {
    return loop_pose(time_bits, start_bits, end_bits, loop_timed());
}
