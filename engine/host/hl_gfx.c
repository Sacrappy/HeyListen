/* (HeyListen native engine) The OS layer's side of the graphics front end (engine/native, hl/c/hl_gfx.h;
   docs/native-engine/gfx-record-job.md).

   The graphics module's recording job broadcasts to a list of listeners while it holds a global lock; the largest
   listener is the scene's recording pass (guest 0x806602e0, reached only through a pointer), answered here instead of by
   its translation (hl_host.h HL_ANSWER: the lifted code is not touched).
     HL_GFX=0  off: the translated listener, as before
     HL_GFX=1  the native pass (hl::gfx::RecordPass): its own level native, its phases still the translated functions
   HL_GFX_CHECK=1: the pass check. Every NVN procedure the game is given goes through a tap that, while a pass runs on the
   calling thread, adds the call to that pass's recording; with HL_GFX=1 each pass takes the native or the translated path
   at random, and the counters compare the two (hl/gfx/PassCheck.h says what that proves). With HL_GFX=0 every pass is the
   translated one, recorded. The tap is decided when the game asks for its first NVN procedure, so the switch is read
   then; without it the procedures are the NVN layer's own, untouched.
   HL_GFX_CHECK=2: the check's control: the passes drawn for the native path run the translated pass too, so the
   differences it then shows between the paths are the check's own noise.
   HL_GFX_LOG=1: the counters every 10 s (always with the check). */
#include "hl_host.h"
#include "hl/c/hl_gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_GFX_DEFAULT HL_GFX_OFF
#define HL_RECORD_PASS 0x806602e0ULL   /* (hl::bridge::kRecordPass: hl_gfx_on checks the two agree) */

typedef void (*HostFn)(Ctx *c, void *user);

static int check_on = -1;   /* HL_GFX_CHECK, read at the first NVN procedure */
static int check_wanted(void) {
    if (check_on < 0) {
        const char *e = getenv("HL_GFX_CHECK");
        check_on = e ? atoi(e) : 0;
        if (check_on < 0) check_on = 0;
    }
    return check_on;
}

/* ---- the check's tap on the NVN procedures ---- */
typedef struct { HostFn fn; void *user; uint32_t op; } Tap;
static _Thread_local int recording;   /* a pass runs on this thread and is being recorded */
static volatile LONG taps;
enum { TAP_NAMES = 2048 };
static const char *tap_names[TAP_NAMES];

static void tap(Ctx *c, void *user) {
    const Tap *t = (const Tap *)user;
    if (recording) {
        uint64_t x[6];
        uint32_t f[4];
        memcpy(x, c->x, sizeof x);
        for (int i = 0; i < 4; i++) memcpy(&f[i], &c->vreg[i][0], 4);
        hl_gfx_check_call(t->op, x, f);
    }
    t->fn(c, t->user);
}

/* (nvn_native.c, for every procedure it hands the game, once a procedure) */
void hl_gfx_nvn_proc(const char *name, HostFn *fn, void **user) {
    if (!check_wanted()) return;
    Tap *t = (Tap *)malloc(sizeof *t);
    t->fn = *fn;
    t->user = *user;
    t->op = (uint32_t)InterlockedIncrement(&taps) - 1;
    if (t->op < TAP_NAMES) tap_names[t->op] = name;   /* (the NVN layer's table: lives as long as the process) */
    *fn = tap;
    *user = t;
}

/* ---- the pass ---- */
static int log_on, native_on;
static A64Fn translated;
static volatile LONG64 other_ctx;   /* passes left to the translation because the thread's context was not the one given */

static void hl_report(void) {
    static volatile LONG64 last;
    const int64_t now = hos_now_ns();
    const int64_t was = last;
    if (now - was < 10000000000LL || InterlockedCompareExchange64(&last, now, was) != was) return;
    char line[1024];
    if (hl_gfx_report(line, sizeof line, tap_names, TAP_NAMES))
        hos_log("hl gfx: %s; passes on another context %lld", line, (long long)other_ctx);
}

static void hl_gfx_pass(Ctx *A64_NOESC c) {
    const uint64_t listener = c->x[0], args = c->x[1];
    const int label = check_on ? hl_gfx_check_next_native() : native_on;
    int native = check_on == 2 ? 0 : label;
    if (native && hos_ctx() != c) {   /* (the guest calls of the native pass run on hos_ctx(): never seen, kept as a guard) */
        InterlockedIncrement64(&other_ctx);
        native = 0;
    }
    if (check_on) {
        hl_gfx_check_begin(args);
        recording = 1;
    }
    if (native) c->x[0] = hl_gfx_record_pass(listener, args);
    else translated(c);
    if (check_on) {
        recording = 0;
        hl_gfx_check_end(label);
    }
    if (log_on) hl_report();
}

/* 1 when the pass's listener is answered here (asked once, at its first lookup) */
static int hl_gfx_on(void) {
    const char *e = getenv("HL_GFX");
    const int mode = e && e[0] ? atoi(e) : HL_GFX_DEFAULT;
    const int check = check_wanted();
    if (mode == HL_GFX_OFF && !check) {
        hos_log("hl gfx: off (HL_GFX=0)");
        return 0;
    }
    if (hl_gfx_pass_address() != HL_RECORD_PASS || !(translated = hl_translated(HL_RECORD_PASS))) {
        hos_log("hl gfx: off: the bridge answers %#llx, not the recording pass", (unsigned long long)hl_gfx_pass_address());
        return 0;
    }
    native_on = hl_gfx_start(hl_host(), mode, check);
    log_on = check || getenv("HL_GFX_LOG") != NULL;
    hos_log("hl gfx: %s (NVN procedures tapped: %ld)",
            !check ? "the native recording pass"
            : check == 2 ? "the check's control: every pass translated, filed under a path at random"
            : native_on ? "the check: each pass on the native or the translated path at random, recorded"
                        : "the check: the translated recording pass, recorded",
            (long)taps);
    return 1;
}

HL_ANSWER(HL_RECORD_PASS, hl_gfx_pass, hl_gfx_on, "the graphics recording pass's listener")
