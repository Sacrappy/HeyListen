/* (HeyListen, TICK-047) HOS_JOB_ORDER=1: the game's long jobs first. Needs a lift made with OOT_JOB_ORDER=1 (the hooks below;
   the default lift has none, so the switch then does nothing). Not adopted: the A/B is inside the spread (perf-audit.md).

   The BaseProc scheduler (f_8004ab40's object, S) keeps three levels of lock-free ring queues (bounded MPMC, 16-byte cells
   with a sequence word): level L at S + 0x40 + L * 0x50, each with an A ring (+0x00: size, +0x08 cells, +0x10 head, +0x18
   tail) and a B ring (+0x28: size, +0x30 cells, +0x38 head, +0x40 tail). A job is pushed into a B ring by f_8004fa40(S, job)
   when it becomes ready (its last predecessor done: job +0x3c); the job's int16 at +0xc picks the level: -2 level 0, 0 level 2
   (each when that level's B ring exists), anything else level 1. An executor takes the first job it finds from level 0 A,
   0 B, 1 A, 1 B, 2 A, 2 B; FIFO inside a ring. An A ring holds a parallel job (job +0x38: runners left, low 7 bits) pushed back
   after an executor took it, so the next executors join it before they take new work. So inside a level, the jobs of a stage
   start in the order they became ready: their driver's actor-list order. At the Market, ~15 actor calcs are level-0 jobs
   pushed in one burst, Link's last of them (#15, ~0.4 ms after the first), and more are level 1 and 2.

   The switch puts a job whose last runs took longer than HOS_JOB_ORDER_US (default 300 us) into its own level's A ring
   instead of its B ring, so the next free executor takes it before every job of that level already waiting (and after
   every job of the levels above, as before). Only a job that runs once (runners 1) and on any executor (job +0x10 = 0x21; a
   job tied to one executor is forwarded to it from the B ring path, f_810cc640, and the A ring path does not check it): an
   A ring entry with one runner is then taken, run and finished exactly as from the B ring. And only while that A ring is
   under three quarters full: the push returns without pushing when its ring is full (0x8004fc18).

   The patch (nx2/oot_patches.py) is control-flow-free statements, so the lifted functions' PGO records still match (clang's
   profile hash counts branches, loops, labels and comparisons, not calls or assignments; checked: both functions get the
   same profile metadata as unpatched):
     - f_8004ab40, around its two indirect job calls (0x8004b3f0, 0x8004b560): hos_jo_begin() / hos_jo_end(c, rec, t0) time
       each run by its job: the job record's (x21, the pointer the rings hold) first word, its object (tagged in bit 0),
       which stays the same from frame to frame where a record may not.
     - f_8004fa40: x0 -= hos_jo_shift(c, x0, x1, level), one before the branch to each level's push (level 2 at 0x8004fa74,
       level 0 at 0x8004fa84, level 1 at 0x8004fa88; the level-0 and level-2 pushes, 0x8004fd88 / 0x8004fc80, are also
       retry targets with x0 reloaded from the stack, so nothing goes after their labels). The push code addresses the B
       ring by x0 + offsets; x0 - 0x28 makes the same code address that level's A ring. Nothing reads x0 after the push
       (its callers f_8004ab40, f_8389af90, f_838cf8e0 and itself).
   The game's semantics are kept: a job is pushed only once it is ready, as before; the jobs ready together are independent
   (six executors already take them in any order and interleaving); only which ready job an idle executor takes first
   changes.

   HOS_JOB_ORDER_LOG=1: every 10 s, how many pushes were moved, how many jobs are timed, and the longest.
   HOS_JOBTRACE: a JT_MARK for each push seen (a = the job, b = level | moved << 4 | runners << 5 | any executor << 12 |
   the jobs waiting in its B ring << 16). */
#include "hos.h"
#include "../gpu/vk/compat/hookshot_env.h"   /* (env_on) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int jo_on = -1;
static int64_t jo_long_ns = 300000;
static int jo_log;

#define JO_BITS 12
#define JO_N (1u << JO_BITS)
typedef struct { volatile uint64_t key; volatile int64_t ns; } JoEnt;   /* (key: the job's object; ns: smoothed run time) */
static JoEnt jo_tab[JO_N];
static volatile long long jo_moved, jo_pushes;

static int jo_init(void) {
    if (jo_on >= 0) return jo_on;
    const char *e = getenv("HOS_JOB_ORDER"), *us = getenv("HOS_JOB_ORDER_US");
    if (us && atoi(us) > 0) jo_long_ns = (int64_t)atoi(us) * 1000;
    jo_log = env_on("HOS_JOB_ORDER_LOG");
    const int on = e && e[0] == '1';
    static volatile long told;
    if (on && !__sync_lock_test_and_set(&told, 1)) hos_log("joborder: on: jobs over %lld us first in their level", (long long)(jo_long_ns / 1000));
    jo_on = on;
    return on;
}

static inline uint32_t jo_hash(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> (64 - JO_BITS)); }

uint64_t hos_jo_begin(void) {
    if (__builtin_expect(jo_on == 0, 1)) return 0;
    if (!jo_init()) return 0;
    return (uint64_t)hos_now_ns();
}

static void jo_report(int64_t now) {
    static int64_t last;
    if (!last) { last = now; return; }
    if (now - last < 10000000000LL) return;
    last = now;
    int timed = 0, longs = 0;
    uint64_t top = 0; int64_t topns = 0;
    for (unsigned i = 0; i < JO_N; i++) {
        if (!jo_tab[i].key) continue;
        timed++;
        if (jo_tab[i].ns >= jo_long_ns) longs++;
        if (jo_tab[i].ns > topns) topns = jo_tab[i].ns, top = jo_tab[i].key;
    }
    hos_log("joborder: %lld of %lld pushes moved; %d jobs timed, %d long; longest %llx %lld us", jo_moved, jo_pushes, timed,
            longs, (unsigned long long)top, (long long)(topns / 1000));
}

void hos_jo_end(Ctx *c, uint64_t rec, uint64_t t0) {
    if (!t0) return;
    const uint64_t job = recomp_load64(c, rec);
    const int64_t now = hos_now_ns(), d = now - (int64_t)t0;
    uint32_t h = jo_hash(job);
    for (int probe = 0; probe < 8; probe++, h = (h + 1) & (JO_N - 1)) {
        const uint64_t k = jo_tab[h].key;
        if (k == job) {   /* (smoothed: half the last run, half the history; a job long once stays long a few frames) */
            jo_tab[h].ns = (jo_tab[h].ns + d) / 2;
            break;
        }
        if (!k) {
            if (__sync_bool_compare_and_swap(&jo_tab[h].key, 0, job)) { jo_tab[h].ns = d; break; }
            if (jo_tab[h].key == job) break;
        }
    }
    if (jo_log) {
        static volatile long lock;
        if (!__sync_lock_test_and_set(&lock, 1)) { jo_report(now); __sync_lock_release(&lock); }
    }
}

/* In f_8004fa40, before the branch to the push of `level`: 0x28 puts the push into that level's A ring (x0 -= 0x28), 0 keeps it.
   The level-2 and level-0 statements run before the job's level is known (only when that level's B ring exists): the job goes
   there only when its level field is 0 / -2 */
uint64_t hos_jo_shift(Ctx *c, uint64_t sched, uint64_t rec, int level) {
    if (__builtin_expect(jo_on == 0, 1)) return 0;
    if (!jo_init()) return 0;
    const int16_t field = (int16_t)recomp_load16(c, rec + 0xc);
    if ((level == 2 && field != 0) || (level == 0 && field != -2)) return 0;
    const uint64_t job = recomp_load64(c, rec);
    if (jo_log) __sync_fetch_and_add(&jo_pushes, 1);
    uint32_t h = jo_hash(job);
    int64_t ns = -1;
    for (int probe = 0; probe < 8; probe++, h = (h + 1) & (JO_N - 1)) {
        const uint64_t k = jo_tab[h].key;
        if (k == job) { ns = jo_tab[h].ns; break; }
        if (!k) break;
    }
    uint64_t shift = 0;
    const uint64_t a = sched + 0x40 + (uint64_t)level * 0x50;   /* (the level's A ring) */
    if (jo_log) {
        static volatile long told[3];
        if (!__sync_lock_test_and_set(&told[level], 1))
            hos_log("joborder: level %d: A ring %u cells, B ring %u", level, recomp_load32(c, a), recomp_load32(c, a + 0x28));
    }
    const unsigned runners = recomp_load16(c, rec + 0x38) & 0x7f, any = recomp_load32(c, rec + 0x10) == 0x21;
    /* (the A ring must have room: a push into a full ring returns without pushing (0x8004fc18), and the A rings are sized
       for the parallel jobs the game puts back there, not for these; a quarter of it left free or the job stays in B) */
    const uint32_t asize = recomp_load32(c, a);
    const uint64_t aused = recomp_load64(c, a + 0x18) - recomp_load64(c, a + 0x10);
    if (ns >= jo_long_ns && runners == 1 && any && asize && recomp_load64(c, a + 8) && aused < asize - asize / 4) {
        shift = 0x28;
        if (jo_log) __sync_fetch_and_add(&jo_moved, 1);
    }
    if (__builtin_expect(hos_jt_on, 0)) {
        const uint64_t waiting = recomp_load64(c, a + 0x40) - recomp_load64(c, a + 0x38);
        hos_jt_rec(JT_MARK, job, (uint64_t)level | (shift ? 16 : 0) | runners << 5 | (uint64_t)any << 12 | (waiting & 0xffffff) << 16);
    }
    return shift;
}

