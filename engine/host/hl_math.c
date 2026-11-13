/* (HeyListen native engine) The OS layer's side of hl::math (engine/native, hl/c/hl_math.h;
   docs/native-engine/physics-loops.md section 8): small numeric kernels of the physics and actor loops answered natively,
   bit for bit, and the differential check that proves it.

   Units (names for HL_MATH_UNITS):
     u2      0x81a040bc  a body's world box from its oriented box (with 0x8170341c), reached through a pointer: HL_ANSWER
     affine  0x80ba6d80  a centre / half-extents box under a 3x4 matrix, called directly: HL_GATE
     u1      0x81746440  the box of a convex vertex set under a precise transform, called directly: HL_GATE
     sphere  0x800a8400  a model's bounding sphere over its bones' spheres, called directly: HL_GATE
   A call takes the native path only when the thread's guest FPCR has no bit but FZ set and the kernel finds every value
   ordinary (hl/math/Exact.h); otherwise ("declined") the translated function runs, as before.

     HL_MATH=0        nothing native
     HL_MATH=1        every validated unit native (the default; a unit is validated by the check below, see `validated`)
     HL_MATH_UNITS=u1,affine   these units native instead of the validated ones (while developing; "all": every unit)
     HL_MATH_CHECK=1  the differential check, on every unit (or those of HL_MATH_UNITS): each call computes the native
                      answer into scratch, then runs the translated function for real (a gate calls the lifted function
                      again under a per-thread bypass; an answered pointer calls hl_translated), and compares the 32 bytes
                      it wrote. The game gets the translated results only. Counts and the first mismatches, with their
                      inputs and both outputs, go to the log
     HL_MATH_LOG=1    the counters every 10 s (always with the check)
   The counters also say which FPCR values the calls came with (the open question of physics-loops.md 4.1). */
#include "hl_host.h"
#include "hl/c/hl_math.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_MATH_DEFAULT 1
#define FPCR_FZ (1ull << 24)
#define FPSR_IXC 0x10ull
#define BOX_BIAS_PTR 0x84ca4680ULL   /* (a global pointer: 0x81a040bc adds the two floats at its +80 to every extent) */
#define VERTEX_START 0x83eb0c20ULL   /* (the starting bound 0x81746440 loads: Havok's large float in four lanes) */
enum { LOG_FIRST = 10, SNAP = 512, SPANS = 6 };

typedef struct { uint64_t at; uint32_t n; } Span;   /* guest bytes a call reads, logged with a mismatch */

enum { WHYS = 13, BODY_DECLINES = 6, OTHER_SHAPES = 8 };
/* (hl::math::Why and hl::math::BodyDecline, in their order) */
static const char *const why_name[WHYS] = {"none", "input-nan-inf", "input-subnormal", "sum-tiny", "sum-overflow",
                                           "mul-underflow", "mul-overflow", "fma-midpoint", "fma-range", "div-range",
                                           "sqrt-range", "narrow-tiny", "narrow-overflow"};
static const char *const body_decline_name[BODY_DECLINES] = {"none", "values", "shape", "cast", "no-ctx", "block-full"};

typedef struct {
    const char *name;
    uint64_t guest;
    int validated;                        /* native under HL_MATH=1: zero mismatches over full runs in the three scenes */
    int (*native)(const Ctx *c, void *out);   /* the 32-byte answer into out: 1, or 0 when the kernel declines */
    uint64_t (*out_at)(const Ctx *c);      /* where the function writes its 32 bytes */
    int (*inputs)(const Ctx *c, Span *s);  /* what it reads (at most SPANS ranges) */
    volatile int *gate;                    /* its gate's switch; NULL: answered through the pointer */
    A64Fn translated;
    int native_on, check_on;
    volatile LONG64 calls, answered, declined_fpcr, declined_inputs, compared, mismatches, fpsr_differs;
    volatile LONG64 why[WHYS];             /* declined by the value rules: which rule (hl_math_last_why) */
    volatile LONG64 body[BODY_DECLINES];   /* (u3) declined, by reason */
    volatile LONG64 other_fn[OTHER_SHAPES][2];   /* (u3) shape box functions not answered: address, calls */
    volatile LONG fpcr_or, fpcr_and, logged, why_logged;
} Unit;

/* ---- the units: where their inputs and outputs are as the function receives them ---- */
static int u2_native(const Ctx *c, void *out) {
    return hl_math_oriented_box_box(GP(c->x[0]), GP(c->x[1]), GP(G64(BOX_BIAS_PTR)), out);
}
static uint64_t u2_out(const Ctx *c) { return c->x[2]; }
static int u2_inputs(const Ctx *c, Span *s) {
    s[0] = (Span){c->x[0] + 104, 4};
    s[1] = (Span){c->x[0] + 128, 64};
    s[2] = (Span){c->x[1], 76};
    s[3] = (Span){G64(BOX_BIAS_PTR) + 80, 8};
    return 4;
}

static int affine_native(const Ctx *c, void *out) { return hl_math_affine_box(GP(c->x[1]), GP(c->x[2]), out); }
static uint64_t affine_out(const Ctx *c) { return c->x[0]; }
static int affine_inputs(const Ctx *c, Span *s) {
    s[0] = (Span){c->x[1], 24};
    s[1] = (Span){c->x[2], 64};
    return 2;
}

static int u1_native(const Ctx *c, void *out) { return hl_math_vertex_set_box(GP(c->x[0]), GP(c->x[1]), GP(VERTEX_START), out); }
static uint64_t u1_out(const Ctx *c) { return c->x[2]; }
static int u1_inputs(const Ctx *c, Span *s) {
    const int32_t off = (int32_t)G32(c->x[0]), n = (int32_t)G32(c->x[0] + 4);
    s[0] = (Span){c->x[0], 8};
    s[1] = (Span){c->x[1], 76};
    s[2] = (Span){VERTEX_START, 16};
    s[3] = (Span){c->x[0] + (int64_t)off, off && n > 0 ? (uint32_t)(n < 32 ? n : 32) * 12 : 0};
    return 4;
}

static int sphere_native(const Ctx *c, void *out) { return hl_math_model_sphere(hl_host(), c->x[0], c->x[1], c->x[31], out); }
static uint64_t sphere_out(const Ctx *c) { return G64(c->x[0] + 56); }
static int sphere_inputs(const Ctx *c, Span *s) {
    const uint64_t d = G64(c->x[0]);
    s[0] = (Span){c->x[0], 64};
    s[1] = (Span){d, 96};
    s[2] = (Span){G64(d + 64), 8 * 16};
    s[3] = (Span){G64(c->x[0] + 56), 32};
    s[4] = (Span){c->x[31] - 28, 12};
    s[5] = (Span){c->x[1] + 32, 8};
    return 6;
}

extern volatile int hl_gate_on_80ba6d80, hl_gate_on_81746440, hl_gate_on_800a8400, hl_gate_on_817d9d80;
enum { U2, AFFINE, U1, SPHERE, U3, UNITS };
/* validated: HL_MATH_CHECK=1 over full runs of kokiri-idle, castle-move and hyrule-walk (PC 3050, 2026-11-13) compared
   0.95 M (u2), 27 M (affine), 2.0 M (u1), 1.9 M (sphere) calls with no mismatch (physics-loops.md section 8) */
static Unit units[UNITS] = {
    [U2] = {.name = "u2", .guest = 0x81a040bcULL, .validated = 1, .native = u2_native, .out_at = u2_out,
            .inputs = u2_inputs},
    [AFFINE] = {.name = "affine", .guest = 0x80ba6d80ULL, .validated = 1, .native = affine_native, .out_at = affine_out,
                .inputs = affine_inputs, .gate = &hl_gate_on_80ba6d80},
    [U1] = {.name = "u1", .guest = 0x81746440ULL, .validated = 1, .native = u1_native, .out_at = u1_out,
            .inputs = u1_inputs, .gate = &hl_gate_on_81746440},
    [SPHERE] = {.name = "sphere", .guest = 0x800a8400ULL, .validated = 1, .native = sphere_native, .out_at = sphere_out,
                .inputs = sphere_inputs, .gate = &hl_gate_on_800a8400},
    /* (writes through pointers it computes: run_body below, not run) */
    [U3] = {.name = "u3", .guest = 0x817d9d80ULL, .validated = 0, .gate = &hl_gate_on_817d9d80},
};

/* ---- switches ---- */
static int mode = HL_MATH_DEFAULT, check = 0, log_on = 0;
static const char *chosen;   /* HL_MATH_UNITS */

static int listed(const char *list, const char *name) {
    if (!strcmp(list, "all")) return 1;
    const size_t n = strlen(name);
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        size_t k = 0;
        while (p[k] && p[k] != ',' && p[k] != ' ') k++;
        if (k == n && !_strnicmp(p, name, n)) return 1;
        p += k;
    }
    return 0;
}

/* (a constructor: the gates' switches must be up before the game's first call for their units to be asked at all; the
   build and the lifted table are looked at once the game runs, in start()) */
__attribute__((constructor)) static void hl_math_switches(void) {
    const char *e = getenv("HL_MATH"), *k = getenv("HL_MATH_CHECK");
    mode = e && e[0] ? atoi(e) : HL_MATH_DEFAULT;
    check = k && atoi(k) == 1;
    log_on = check || getenv("HL_MATH_LOG") != NULL;
    chosen = getenv("HL_MATH_UNITS");
    if (chosen && !chosen[0]) chosen = NULL;
    for (int i = 0; i < UNITS; i++) {
        Unit *u = &units[i];
        u->check_on = check && (!chosen || listed(chosen, u->name));
        u->native_on = !check && mode != 0 && (chosen ? listed(chosen, u->name) : u->validated);
        u->fpcr_and = -1;
        if (u->gate) *u->gate = u->native_on || u->check_on;
    }
}

static volatile LONG started;   /* 0: not yet, 1: done, 2: being done */
static void start(void) {
    for (;;) {
        const LONG s = started;
        if (s == 1) return;
        if (s == 0 && InterlockedCompareExchange(&started, 2, 0) == 0) break;
        Sleep(0);
    }
    const int supported = hl_supported_build();
    char line[512];
    int n = snprintf(line, sizeof line, "hl math:");
    for (int i = 0; i < UNITS; i++) {
        Unit *u = &units[i];
        if (!supported || !(u->translated = hl_translated(u->guest))) u->native_on = u->check_on = 0;
        if (u->gate) *u->gate = u->native_on || u->check_on;
        n += snprintf(line + n, sizeof line - n, " %s %s;", u->name, u->check_on ? "checked" : u->native_on ? "native" : "off");
    }
    if (!supported) snprintf(line + n, sizeof line - n, " (not the build the native engine's addresses are of)");
    else snprintf(line + n, sizeof line - n, " (HL_MATH=%d%s%s)", mode, chosen ? ", units " : "", chosen ? chosen : "");
    hos_log("%s", line);
    started = 1;
}

/* ---- the counters ---- */
static void report(int force) {
    static volatile LONG64 last;
    const int64_t now = hos_now_ns(), was = last;
    if (!force && (now - was < 10000000000LL || InterlockedCompareExchange64(&last, now, was) != was)) return;
    for (int i = 0; i < UNITS; i++) {
        const Unit *u = &units[i];
        if (!u->calls) continue;
        hos_log("hl math: %s: calls %lld native %lld declined %lld (fpcr %lld, inputs %lld) compared %lld mismatches %lld "
                "fpsr-differs %lld; fpcr bits in every call %#lx, in some %#lx",
                u->name, (long long)u->calls, (long long)u->answered, (long long)(u->declined_fpcr + u->declined_inputs),
                (long long)u->declined_fpcr, (long long)u->declined_inputs, (long long)u->compared,
                (long long)u->mismatches, (long long)u->fpsr_differs, (unsigned long)u->fpcr_and, (unsigned long)u->fpcr_or);
        char line[640];
        int n = 0;
        for (int w = 1; w < WHYS; w++)
            if (u->why[w]) n += snprintf(line + n, sizeof line - n, " %s %lld", why_name[w], (long long)u->why[w]);
        if (n) hos_log("hl math: %s: declined by rule:%s", u->name, line);
        n = 0;
        for (int w = 1; w < BODY_DECLINES; w++)
            if (u->body[w]) n += snprintf(line + n, sizeof line - n, " %s %lld", body_decline_name[w], (long long)u->body[w]);
        for (int k = 0; k < OTHER_SHAPES && n < 500; k++)
            if (u->other_fn[k][0])
                n += snprintf(line + n, sizeof line - n, "%s %#llx x%lld", k ? "," : "; other shape functions",
                              (unsigned long long)u->other_fn[k][0], (long long)u->other_fn[k][1]);
        if (n) hos_log("hl math: %s: declined:%s", u->name, line);
    }
}

/* the value rule a kernel gave up on, counted (and the first few logged with the operation's values) */
static void count_why(Unit *u) {
    uint64_t v[3];
    const int w = hl_math_last_why(v);
    if (w <= 0 || w >= WHYS) return;
    InterlockedIncrement64(&u->why[w]);
    if (InterlockedIncrement(&u->why_logged) <= 8)
        hos_log("hl math: %s declined: %s, operands %#llx %#llx result %#llx", u->name, why_name[w],
                (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2]);
}
static void report_at_exit(void) { report(1); }

static void hex(char *to, size_t cap, const uint8_t *p, size_t n) {
    size_t at = 0;
    for (size_t i = 0; i < n && at + 3 < cap; i++) at += snprintf(to + at, cap - at, "%s%02x", i && !(i & 3) ? " " : "", p[i]);
    to[at] = 0;
}

/* ---- one call ---- */
static _Thread_local int bypass;   /* the check is running the translated function on this thread: its gate says 0 */

/* The check: native answer computed (mine), the translated function run for real, the 32 bytes at out compared. */
static void compare(Unit *u, Ctx *c, uint64_t out, const uint8_t mine[32]) {
    Span s[SPANS];
    uint8_t in[SNAP];
    int ns = 0;
    size_t used = 0;
    if (u->logged < LOG_FIRST) {   /* (the inputs as they were, should this call mismatch) */
        ns = u->inputs(c, s);
        for (int i = 0; i < ns; i++) {
            if (used + s[i].n > SNAP) s[i].n = (uint32_t)(SNAP - used);
            memcpy(in + used, GP(s[i].at), s[i].n);
            used += s[i].n;
        }
    }
    const uint64_t fpcr = c->fpcr, fpsr = c->fpsr | FPSR_IXC;
    bypass = 1;
    u->translated(c);
    bypass = 0;
    InterlockedIncrement64(&u->compared);
    if ((c->fpsr | FPSR_IXC) != fpsr) InterlockedIncrement64(&u->fpsr_differs);
    const uint8_t *theirs = (const uint8_t *)GP(out);
    if (!memcmp(theirs, mine, 32)) return;
    InterlockedIncrement64(&u->mismatches);
    if (!ns || InterlockedIncrement(&u->logged) > LOG_FIRST) return;
    char a[128], b[128], h[1400];
    hex(a, sizeof a, theirs, 32);
    hex(b, sizeof b, mine, 32);
    hos_log("hl math: %s MISMATCH at out %#llx, fpcr %#llx: translated %s / native %s", u->name, (unsigned long long)out,
            (unsigned long long)fpcr, a, b);
    used = 0;
    for (int i = 0; i < ns; i++) {
        hex(h, sizeof h, in + used, s[i].n);
        hos_log("hl math: %s   input %d at %#llx: %s", u->name, i, (unsigned long long)s[i].at, h);
        used += s[i].n;
    }
}

/* 1 when the call was answered (natively, or by the check's run of the translated function); 0: run the translation */
static int run(Unit *u, Ctx *c) {
    if (bypass) return 0;
    if (started != 1) {
        start();
        if (!u->native_on && !u->check_on) return 0;
    }
    InterlockedIncrement64(&u->calls);
    InterlockedOr(&u->fpcr_or, (LONG)c->fpcr);
    InterlockedAnd(&u->fpcr_and, (LONG)c->fpcr);
    /* (the scratch starts as the guest's 32 bytes: a kernel that writes part of them leaves the rest as they are) */
    const uint64_t out = u->out_at(c);
    uint8_t mine[32];
    int answered = 0;
    if (c->fpcr & ~FPCR_FZ) InterlockedIncrement64(&u->declined_fpcr);
    else if (!out || (memcpy(mine, GP(out), 32), !u->native(c, mine))) {
        InterlockedIncrement64(&u->declined_inputs);
        count_why(u);
    } else if (u->check_on) {
        compare(u, c, out, mine);
        answered = 1;
    } else {
        memcpy(GP(out), mine, 32);
        c->fpsr |= FPSR_IXC;   /* (sticky, and the translation sets it on any inexact operation: keeps its fast path open) */
        InterlockedIncrement64(&u->answered);
        answered = 1;
    }
    if (log_on) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1)) atexit(report_at_exit);
        report(0);
    }
    return answered;
}

/* ---- u3: 0x817d9d80, the per-body box update of the last substep (physics-loops.md 6.3) ----
   Called once per body by 0x8182dc20. It writes the body (its transform, quantised speeds, flags and stored box) and
   appends records to the thread's stream blocks, through pointers it computes: the native side makes a plan (what the
   function would leave), which the native run writes and the check compares, byte for byte, after running the
   translation: the body's 192 bytes, the moved-bodies block's header word, and each record and header the plan
   appends. The shape's own box is reached through the shape-type table; the native unit answers the two shapes whose
   box functions are native (u1's convex vertex set through 0x8173e2ac, and u2) and declines the others. */
#define SHAPE_TABLE_PTR 0x84ca78c8ULL   /* (a pointer to the pointer to the shape-type table: 512-byte entries, box function at +24) */
#define CONVEX_BOX_FN 0x8173e2acULL
static int shape_kind(Unit *u, uint64_t body) {
    if ((*(uint8_t *)GP(body + 140)) & 8) return 0;   /* (the inline path: no table) */
    const uint64_t shape = G64(body + 120);
    const uint64_t fn = G64(G64(G64(SHAPE_TABLE_PTR)) + ((uint64_t)(*(uint8_t *)GP(shape + 24)) << 9) + 24);
    if (fn == CONVEX_BOX_FN) return 1;
    if (fn == units[U2].guest) return 2;
    for (int k = 0; k < OTHER_SHAPES; k++) {   /* (counted for the log: which functions the native unit leaves) */
        const LONG64 was = InterlockedCompareExchange64(&u->other_fn[k][0], (LONG64)fn, 0);
        if (was == 0 || (uint64_t)was == fn) {
            InterlockedIncrement64(&u->other_fn[k][1]);
            break;
        }
    }
    return 0;
}

static void body_mismatch(Unit *u, const Ctx *in, const char *what, const uint8_t *theirs, const uint8_t *mine, int n,
                          const uint8_t *before) {
    InterlockedIncrement64(&u->mismatches);
    if (InterlockedIncrement(&u->logged) > LOG_FIRST) return;
    char a[400], b[400], h[400];
    for (int off = 0; off < n; off += 64) {
        const int k = n - off < 64 ? n - off : 64;
        if (!memcmp(theirs + off, mine + off, k)) continue;
        hex(a, sizeof a, theirs + off, k);
        hex(b, sizeof b, mine + off, k);
        hos_log("hl math: u3 MISMATCH %s +%d, fpcr %#llx: translated %s / native %s", what, off,
                (unsigned long long)in->fpcr, a, b);
        if (before) {
            hex(h, sizeof h, before + off, k);
            hos_log("hl math: u3   before %s", h);
        }
    }
}

static int run_body(Unit *u, Ctx *c) {
    if (bypass) return 0;
    if (started != 1) {
        start();
        if (!u->native_on && !u->check_on) return 0;
    }
    InterlockedIncrement64(&u->calls);
    InterlockedOr(&u->fpcr_or, (LONG)c->fpcr);
    InterlockedAnd(&u->fpcr_and, (LONG)c->fpcr);
    if (log_on) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1)) atexit(report_at_exit);
        report(0);
    }
    if (c->fpcr & ~FPCR_FZ) {
        InterlockedIncrement64(&u->declined_fpcr);
        return 0;
    }
    const hl_math_body_args args = {.ctx = c->x[0], .motion = c->x[1], .record = c->x[2], .prev = c->x[3],
                                    .body = c->x[4], .state = c->x[5], .global = G64(BOX_BIAS_PTR),
                                    .start = VERTEX_START, .shape_kind = shape_kind(u, c->x[4])};
    static _Thread_local hl_math_body_plan plan;
    if (!hl_math_body_box(hl_host(), &args, &plan)) {
        InterlockedIncrement64(&u->declined_inputs);
        if (plan.decline > 0 && plan.decline < BODY_DECLINES) InterlockedIncrement64(&u->body[plan.decline]);
        if (plan.decline == 1) count_why(u);
        return 0;
    }
    uint8_t *body = (uint8_t *)GP(args.body);
    if (u->check_on) {
        uint8_t before[192];
        memcpy(before, body, 192);
        const uint64_t block1 = args.ctx ? G64(args.ctx + 1568) : 0;
        uint32_t want1 = block1 ? G32(block1) : 0;
        for (int i = 0; i < plan.appends; i++)
            if (plan.add[i].block == block1) want1 = plan.add[i].header;
        const uint64_t fpsr = c->fpsr | FPSR_IXC;
        bypass = 1;
        u->translated(c);
        bypass = 0;
        InterlockedIncrement64(&u->compared);
        if ((c->fpsr | FPSR_IXC) != fpsr) InterlockedIncrement64(&u->fpsr_differs);
        if (memcmp(body, plan.body, 192)) body_mismatch(u, c, "body", body, plan.body, 192, before);
        else if (block1 && G32(block1) != want1) {
            const uint32_t got = G32(block1);
            body_mismatch(u, c, "block header", (const uint8_t *)&got, (const uint8_t *)&want1, 4, NULL);
        } else
            for (int i = 0; i < plan.appends; i++) {
                const hl_math_body_append *a = &plan.add[i];
                const uint8_t *at = (const uint8_t *)GP(a->block);
                if (G32(a->block) != a->header || memcmp(at + a->at, a->rec, a->len)) {
                    uint8_t mine[20], theirs[20];
                    memcpy(mine, &a->header, 4);
                    memcpy(mine + 4, a->rec, 16);
                    memcpy(theirs, at, 4);
                    memcpy(theirs + 4, at + a->at, 16);
                    body_mismatch(u, c, i ? "append 2" : "append 1", theirs, mine, 4 + (int)a->len, NULL);
                    break;
                }
            }
        return 1;
    }
    for (int i = 0; i < 192;) {   /* (the bytes the function stores, in runs) */
        if (!plan.written[i]) { i++; continue; }
        int j = i;
        while (j < 192 && plan.written[j]) j++;
        memcpy(body + i, plan.body + i, j - i);
        HOS_WRITTEN(body + i, j - i);
        i = j;
    }
    for (int i = 0; i < plan.appends; i++) {
        const hl_math_body_append *a = &plan.add[i];
        uint8_t *at = (uint8_t *)GP(a->block);
        memcpy(at + a->at, a->rec, a->len);
        memcpy(at, &a->header, 4);
        HOS_WRITTEN(at + a->at, a->len);
        HOS_WRITTEN(at, 4);
    }
    c->fpsr |= FPSR_IXC;
    InterlockedIncrement64(&u->answered);
    return 1;
}

static int u2_on(void) {
    start();
    return units[U2].native_on || units[U2].check_on;
}
static void u2_answer(Ctx *A64_NOESC c) {
    if (!run(&units[U2], c)) units[U2].translated(c);
}
HL_ANSWER(0x81a040bcULL, u2_answer, u2_on, "a body's world box from its oriented box")

HL_GATE(80ba6d80, "fc814e67d0dad1eb") { return run(&units[AFFINE], c); }
HL_GATE(81746440, "851bc99bf31be697") { return run(&units[U1], c); }
HL_GATE(800a8400, "42b7877d24ec9c57") { return run(&units[SPHERE], c); }
HL_GATE(817d9d80, "fff55648d2f990a0") { return run_body(&units[U3], c); }
