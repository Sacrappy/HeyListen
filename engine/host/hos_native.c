/* (hookshot, TICK-026) Native replacements of hot guest code: the host side of nx2/native_replace.py.

   The lifter's patch step puts a gate at the head of each replaced guest block (or function):

       if (hos_nr_mode[ID]) { if (hos_nr_<name>(c)) goto <the block's exit>; }

   and, at the block's exit label(s), `if (hos_nr_mode[ID] == 2) hos_nr_diff_exit(c, ID);`.

   Per replacement one switch, HOS_NR_<NAME> (read once at start):
     unset / 0   the guest code (the default until a replacement is proven; the table below can turn one on)
     1           the native code
     diff        differential mode (debug only): 1 call in HOS_NR_DIFF_EVERY (default 64) runs the native code on a copy of
                 the guest registers first, its memory writes held in a log (not written); then the guest code runs as
                 usual, and at the block's exit the registers (x0-x30, sp, NZCV) and the logged writes are compared with what
                 the guest did. A difference is logged (the first 20 in full), and a summary every 2^k checks and every
                 100,000. Writes the guest makes that the native code does not make are not seen (the log holds the native
                 code's writes only); each replacement says which memory its guest code writes.
   Nothing here changes the guest's state in differential mode: the native code reads guest memory and writes only its
   copy of the registers and its log. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NR_STRCMP_805AFC70, NR_COUNT };

static const struct { const char *name, *env; int default_mode; } nr_info[NR_COUNT] = {
    [NR_STRCMP_805AFC70] = {"strcmp_805afc70", "HOS_NR_STRCMP_805AFC70", 0},
};

volatile int hos_nr_mode[NR_COUNT];
static int nr_every = 64;
static volatile long long nr_calls[NR_COUNT], nr_checks[NR_COUNT], nr_diffs[NR_COUNT];

__attribute__((constructor)) static void nr_init(void) {
    const char *e = getenv("HOS_NR_DIFF_EVERY");
    if (e && atoi(e) > 0) nr_every = atoi(e);
    for (int i = 0; i < NR_COUNT; i++) {
        const char *v = getenv(nr_info[i].env);
        int m = nr_info[i].default_mode;
        if (v) m = !strcmp(v, "diff") ? 2 : atoi(v) ? 1 : 0;
        hos_nr_mode[i] = m;
    }
}

static void nr_announce(void) {
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < NR_COUNT; i++)
        hos_log("native: %s %s", nr_info[i].name, hos_nr_mode[i] == 2 ? "differential (1 call in N)" :
                hos_nr_mode[i] ? "native" : "guest");
    if (nr_every) hos_log("native: differential sampling 1 in %d", nr_every);
}

/* ---- differential state (per guest thread) ---------------------------------------------------------------------- */
#define NR_LOG_MAX 64
typedef struct { uint64_t addr, val; int size; } NrWrite;
typedef struct {
    int pending;               /* the native result below waits for the guest's exit */
    int id;
    Ctx in, out;               /* the registers at entry and the native code's result */
    int nw, overflow;
    NrWrite w[NR_LOG_MAX];     /* the native code's writes (held, not made) */
} NrDiff;
static __thread NrDiff nr_d;
static __thread unsigned nr_tick;
static __thread int nr_logging;   /* (inside a native call in differential mode: stores go to the log) */

/* Memory access for native code: loads read guest memory; stores write it, or in differential mode go to the log.
   (A load in differential mode does not see the native code's own logged stores: native code that reads back what it
   wrote keeps the value in a local.) */
static inline uint8_t nr_ld8(uint64_t a) { return *(const uint8_t *)A64_PTR(a); }
static inline uint64_t nr_ld64(uint64_t a) { uint64_t v; memcpy(&v, A64_PTR(a), 8); return v; }
static inline void nr_st(uint64_t a, uint64_t v, int size) {
    if (nr_logging) {
        if (nr_d.nw < NR_LOG_MAX) nr_d.w[nr_d.nw++] = (NrWrite){a, v, size}; else nr_d.overflow = 1;
        return;
    }
    memcpy(A64_PTR(a), &v, size);
    A64_MARK(a, size);
}

static void nr_report(int id) {
    const long long k = nr_checks[id];
    if ((k & (k - 1)) == 0 || k % 100000 == 0)
        hos_log("native: %s differential: %lld checks, %lld differences (%lld calls)", nr_info[id].name, k,
                (long long)nr_diffs[id], (long long)nr_calls[id]);
}

/* At a replaced block's exit: compare the guest's result with the native one, if one waits. */
void hos_nr_diff_exit(Ctx *c, int id) {
    if (!nr_d.pending || nr_d.id != id) return;
    nr_d.pending = 0;
    const Ctx *o = &nr_d.out;
    char what[512];
    int n = 0, bad = 0;
    for (int r = 0; r < 32; r++)
        if (c->x[r] != o->x[r]) {
            bad = 1;
            if (n < 400) n += snprintf(what + n, sizeof what - n, " x%d guest %#llx native %#llx", r,
                                       (unsigned long long)c->x[r], (unsigned long long)o->x[r]);
        }
    if (c->n != o->n || c->z != o->z || c->c != o->c || c->v != o->v) {
        bad = 1;
        if (n < 400) n += snprintf(what + n, sizeof what - n, " nzcv guest %d%d%d%d native %d%d%d%d",
                                   c->n, c->z, c->c, c->v, o->n, o->z, o->c, o->v);
    }
    for (int i = 0; i < nr_d.nw; i++) {
        uint64_t now = 0;
        memcpy(&now, A64_PTR(nr_d.w[i].addr), nr_d.w[i].size);
        const uint64_t mask = nr_d.w[i].size == 8 ? ~0ULL : (1ULL << (8 * nr_d.w[i].size)) - 1;
        if (now != (nr_d.w[i].val & mask)) {
            bad = 1;
            if (n < 400) n += snprintf(what + n, sizeof what - n, " [%#llx]/%d guest %#llx native %#llx",
                                       (unsigned long long)nr_d.w[i].addr, nr_d.w[i].size, (unsigned long long)now,
                                       (unsigned long long)(nr_d.w[i].val & mask));
        }
    }
    if (nr_d.overflow) { bad = 1; if (n < 400) n += snprintf(what + n, sizeof what - n, " (write log full)"); }
    __atomic_add_fetch(&nr_checks[id], 1, __ATOMIC_RELAXED);
    if (bad) {
        const long long d = __atomic_add_fetch(&nr_diffs[id], 1, __ATOMIC_RELAXED);
        if (d <= 20) hos_log("native: %s DIFFERENCE #%lld:%s", nr_info[id].name, d, what);
    }
    nr_report(id);
}

/* The gate's common part: 1 = the native code ran (the caller jumps to the block's exit), 0 = the guest code runs. In
   differential mode, sampled calls run fn on a copy first and leave the result for hos_nr_diff_exit. */
static inline int nr_gate(Ctx *c, int id, void (*fn)(Ctx *)) {
    const int m = hos_nr_mode[id];
    if (m == 1) { fn(c); return 1; }
    if (m != 2) return 0;
    nr_announce();
    __atomic_add_fetch(&nr_calls[id], 1, __ATOMIC_RELAXED);
    if (++nr_tick % (unsigned)nr_every) return 0;
    memcpy(&nr_d.in, c, sizeof *c);
    memcpy(&nr_d.out, c, sizeof *c);
    nr_d.id = id;
    nr_d.nw = nr_d.overflow = 0;
    nr_logging = 1;
    fn(&nr_d.out);
    nr_logging = 0;
    nr_d.pending = 1;
    return 0;
}

/* ===================================================================================================================
   NR_STRCMP_805AFC70: the string compare inside the binary search of f_805aeea0 (guest 0x805afc70-0x805afda0; the
   search loop is 0x805afbc0-0x805afda4, its key compare is this block). On entry x10 = key + 3 and x9 = the table
   entry's name + 3 (both C strings), x11 = 0x400000 (a byte budget, counted down 8 per group), x27 the sign constant
   of the search. The guest compares 8 bytes a group (unrolled, a byte at a time): at the first byte where the key's
   byte a is 0 or differs from the entry's byte b, x8 = 0 when both are 0, else (a < b ? x27 : -x27) as 32 bits;
   x12 = a, x13 = b, x14 = (a >= b ? -x27 : x27); x9 / x10 / x11 are those of the group's start; C = (a >= b), Z as the
   guest's last compare leaves it (0 after a mismatch or a terminator at byte 0, (b == 0) at bytes 1-7), N and V
   untouched. All groups equal for the whole budget: x8 = 0, x11 = 0, x9 / x10 past the last group, x12-x14 of its last byte, Z = C = 1. Every way out
   goes to 0x805afbc0, the search loop's head, which reads x8 only. The block writes no memory.
   (Written from the lifted code's behaviour; the guest's own A64_POLL per byte is not made: the loop is bounded.) */
static void nr_strcmp_805afc70(Ctx *c) {
    uint64_t p = c->x[10] - 3, q = c->x[9] - 3, budget = c->x[11];
    const uint32_t k = (uint32_t)c->x[27], neg = (uint32_t)(0 - c->x[27]);
    const uint64_t ones = 0x0101010101010101ULL, highs = 0x8080808080808080ULL;
    for (;;) {
        /* a whole group at once when neither 8-byte span crosses a 4 KB page (the guest reads at least its first
           byte, so the page is mapped) and the key's 8 bytes hold no 0 and equal the entry's */
        if (((uint32_t)p & 0xfff) <= 0xff8 && ((uint32_t)q & 0xfff) <= 0xff8) {
            const uint64_t A = nr_ld64(p), B = nr_ld64(q);
            if (A == B && !((A - ones) & ~A & highs)) {
                budget -= 8;
                if (budget == 0) {   /* (the whole budget equal: the guest's last group, all 8 bytes equal) */
                    c->x[12] = A >> 56; c->x[13] = B >> 56; c->x[14] = neg;   /* (a >= b) */
                    c->x[8] = 0; c->x[9] = q + 11; c->x[10] = p + 11; c->x[11] = 0;   /* (the guest steps both past the group before it tests the budget) */
                    c->z = 1; c->c = 1;
                    return;
                }
                p += 8; q += 8;
                continue;
            }
        }
        for (int i = 0; i < 8; i++) {
            const uint32_t a = nr_ld8(p + i), b = nr_ld8(q + i);
            if (a == 0 || a != b) {
                c->x[12] = a; c->x[13] = b;
                c->x[14] = a >= b ? neg : k;
                c->x[8] = (a == 0 && b == 0) ? 0 : c->x[14];
                c->x[9] = q + 3; c->x[10] = p + 3; c->x[11] = budget;
                c->c = a >= b;
                c->z = (a == 0 && i > 0) ? (b == 0) : 0;
                return;
            }
        }
        budget -= 8;
        if (budget == 0) {
            c->x[12] = nr_ld8(p + 7); c->x[13] = nr_ld8(q + 7); c->x[14] = neg;
            c->x[8] = 0; c->x[9] = q + 11; c->x[10] = p + 11; c->x[11] = 0;   /* (the guest steps both past the group before it tests the budget) */
            c->z = 1; c->c = 1;
            return;
        }
        p += 8; q += 8;
    }
}

int hos_nr_strcmp_805afc70(Ctx *c) { return nr_gate(c, NR_STRCMP_805AFC70, nr_strcmp_805afc70); }
