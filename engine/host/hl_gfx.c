/* (HeyListen native engine) The OS layer's side of the graphics front end (engine/native, hl/c/hl_gfx.h;
   docs/native-engine/gfx-record-job.md).

   The graphics module's recording job broadcasts to a list of listeners while it holds a global lock; the largest
   listener is the scene's recording pass (guest 0x806602e0, reached only through a pointer), answered here instead of by
   its translation (hl_host.h HL_ANSWER: the lifted code is not touched).
     HL_GFX=0  off: the translated listener, as before
     HL_GFX=1  the native pass (hl::gfx::RecordPass; the default): its own level native, its phases still translated
   HL_GFX_CHECK=1: the pass check. Every NVN procedure the game is given goes through a tap that, while a pass runs on the
   calling thread, adds the call to that pass's recording; with HL_GFX=1 each pass takes the native or the translated path
   at random, and the counters compare the two (hl/gfx/PassCheck.h says what that proves). With HL_GFX=0 every pass is the
   translated one, recorded. The tap is decided when the game asks for its first NVN procedure, so the switch is read
   then; without it the procedures are the NVN layer's own, untouched.
   HL_GFX_CHECK=2: the check's control: the passes drawn for the native path run the translated pass too, so the
   differences it then shows between the paths are the check's own noise.
   HL_GFX_LOG=1: the counters every 10 s (always with the check).

   The draw path's render-state function (guest 0x80077d60, called directly from 254 functions) is answered through an
   entry gate (HL_GATE below; nx2/hl_gates.py puts it in the lifted function at build time):
     HL_GFX_STATE=0  off: the translated function, as before
     HL_GFX_STATE=1  (the default) the native function (hl::gfx::RenderState): it writes the block and calls the NVN procedures itself,
                     through the table of NVN entry points below (the procedures the game's own globals hold, checked
                     once against them); a call that reaches the material's state callback is left to the translation
   With HL_GFX_CHECK=1 (or 2) the gate checks instead, call by call (hl/gfx/StateCheck.h): the native version runs into a
   recording, the translated body runs for real under the tap, and the two lists of NVN calls and the block's bytes are
   compared; the first mismatches are logged in full. */
#include "hl_host.h"
#include "hl/c/hl_gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_GFX_DEFAULT HL_GFX_SHELL   /* (validated in the three scenes, check and shots clean: gfx-record-job.md section 7) */
#define HL_RECORD_PASS 0x806602e0ULL   /* (hl::bridge::kRecordPass: hl_gfx_on checks the two agree) */
#define HL_GFX_STATE_DEFAULT 1   /* (validated 2026-11-13 on both PCs: the exact check at 0 mismatches over 4.2 M NVN calls in three scenes, native runs clean) */
#define HL_STATE_FN 0x80077d60ULL      /* (hl::bridge::kApplyRenderState: state_init checks the two agree) */

typedef void (*HostFn)(Ctx *c, void *user);
uint64_t nvn_native_proc(const char *name);                                  /* nvn_native.c */
uint64_t hos_hostcall_target(uint64_t addr, HostFn *fn, void **user);         /* hos_hostcall.c */

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
typedef struct { HostFn fn; void *user; uint32_t op; uint32_t proc; } Tap;   /* proc: the native numbering */
static _Thread_local int recording;         /* a pass runs on this thread and is being recorded */
static _Thread_local int state_recording;   /* the render-state function's translated body runs here, compared */
static volatile LONG taps;
enum { TAP_NAMES = 2048 };
static const char *tap_names[TAP_NAMES];

static void tap(Ctx *c, void *user) {
    const Tap *t = (const Tap *)user;
    if (recording | state_recording) {
        uint64_t x[6];
        uint32_t f[4];
        memcpy(x, c->x, sizeof x);
        for (int i = 0; i < 4; i++) memcpy(&f[i], &c->vreg[i][0], 4);
        if (recording) hl_gfx_check_call(t->op, x, f);
        if (state_recording) hl_gfx_state_check_call(t->proc, x, f);
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
    const uint32_t n = hl_gfx_nvn_count();
    for (t->proc = 0; t->proc < n && strcmp(hl_gfx_nvn_name(t->proc), name); t->proc++) {}
    if (t->op < TAP_NAMES) tap_names[t->op] = name;   /* (the NVN layer's table: lives as long as the process) */
    *fn = tap;
    *user = t;
}

/* ---- the counters ---- */
static int log_on;
static volatile LONG64 other_ctx;   /* passes left to the translation because the thread's context was not the one given */

static void hl_report(void) {
    static volatile LONG64 last;
    const int64_t now = hos_now_ns();
    const int64_t was = last;
    if (now - was < 10000000000LL || InterlockedCompareExchange64(&last, now, was) != was) return;
    char line[1024];
    if (hl_gfx_report(line, sizeof line, tap_names, TAP_NAMES))
        hos_log("hl gfx: %s; passes on another context %lld", line, (long long)other_ctx);
    if (hl_gfx_state_report(line, sizeof line)) hos_log("hl gfx: %s", line);
}

/* ---- the pass ---- */
static int native_on;
static A64Fn translated;

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
    log_on |= check || getenv("HL_GFX_LOG") != NULL;
    hos_log("hl gfx: %s (NVN procedures tapped: %ld)",
            !check ? "the native recording pass"
            : check == 2 ? "the check's control: every pass translated, filed under a path at random"
            : native_on ? "the check: each pass on the native or the translated path at random, recorded"
                        : "the check: the translated recording pass, recorded",
            (long)taps);
    return 1;
}

HL_ANSWER(HL_RECORD_PASS, hl_gfx_pass, hl_gfx_on, "the graphics recording pass's listener")

/* ---- the render-state function ---- */

/* The NVN entry points native code calls (hl_gfx_host.nvn), in the native numbering: the host function and its data
   behind the host call the game's own global holds. */
enum { STATE_PROCS = 64 };
static HostFn nvn_fn[STATE_PROCS];
static void *nvn_user[STATE_PROCS];
static _Thread_local Ctx *t_ctx;   /* the guest context of the call the gate is answering on this thread */

static void nvn_call(void *user, uint32_t proc, const uint64_t *x, uint32_t nx, const uint32_t *f, uint32_t nf) {
    (void)user;
    Ctx *c = t_ctx;
    for (uint32_t i = 0; i < nx; i++) c->x[i] = x[i];
    for (uint32_t i = 0; i < nf; i++) {   /* (as the lifted code loads a single: the rest of the register cleared) */
        c->vreg[i][0] = f[i];
        c->vreg[i][1] = 0;
    }
    nvn_fn[proc](c, nvn_user[proc]);
}
static void gfx_written(void *user, uint64_t guest, uint64_t n) {
    (void)user, (void)guest, (void)n;   /* (unused when the build tracks no writes) */
    HOS_WRITTEN(GP(guest), (size_t)n);
}
static void gfx_log(void *user, const char *line) {
    (void)user;
    hos_log("hl gfx: render state %s", line);
}
static const hl_gfx_host gfx_host = {NULL, nvn_call, gfx_written, gfx_log};

/* 1 when every procedure resolves to a host call and the game's global holds that same host call */
static int nvn_resolve(void) {
    const uint32_t n = hl_gfx_nvn_count();
    if (n > STATE_PROCS) {
        hos_log("hl gfx: render state off: %u NVN procedures, room for %d", n, STATE_PROCS);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        const char *name = hl_gfx_nvn_name(i);
        const uint64_t a = nvn_native_proc(name);
        uint64_t game;
        memcpy(&game, GP(hl_gfx_nvn_global(i)), 8);
        if (!hos_hostcall_target(a, &nvn_fn[i], &nvn_user[i]) || game != a) {
            hos_log("hl gfx: render state off: %s is %#llx for the game (its global %#llx), %#llx in the NVN layer", name,
                    (unsigned long long)game, (unsigned long long)hl_gfx_nvn_global(i), (unsigned long long)a);
            return 0;
        }
    }
    return 1;
}

static int state_check;          /* HL_GFX_CHECK set: compare each call, the translation answering */
static A64Fn state_translated;
static volatile LONG state_ready;   /* 0: not asked yet, 1: off, 2: on, 3: being asked */
static _Thread_local int t_state_inside;   /* the gate's own call of the translated body runs on this thread */

/* Asked at the gate's first call (the game's NVN procedures are resolved by then): 1 when the gate answers */
static int state_init(void) {
    for (;;) {
        const LONG s = state_ready;
        if (s == 1 || s == 2) return s == 2;
        if (s == 0 && InterlockedCompareExchange(&state_ready, 3, 0) == 0) break;
        Sleep(0);
    }
    const char *e = getenv("HL_GFX_STATE");
    const int mode = e && e[0] ? atoi(e) : HL_GFX_STATE_DEFAULT;
    state_check = check_wanted() != 0;
    int on = 0;
    if (mode != 1 && !state_check) {
        hos_log("hl gfx: render state: the translated function (HL_GFX_STATE=%d)", mode);
    } else if (!hl_supported_build()) {
        hos_log("hl gfx: render state off: not the supported build");
    } else if (hl_gfx_state_address() != HL_STATE_FN || !(state_translated = hl_translated(HL_STATE_FN))) {
        hos_log("hl gfx: render state off: the bridge answers %#llx", (unsigned long long)hl_gfx_state_address());
    } else if (nvn_resolve() && hl_gfx_state_start(hl_host(), &gfx_host, state_check)) {
        on = 1;
        log_on |= state_check || getenv("HL_GFX_LOG") != NULL;
        hos_log("hl gfx: render state: %s",
                state_check ? "the check: each call compared with the translated function, which answers"
                            : "the native function (HL_GFX_STATE=1)");
    }
    state_ready = on ? 2 : 1;
    return on;
}

/* (switched on from the start so that the first call asks state_init; off for good once it says so) */
HL_GATE(80077d60, "31664901fd8bc782") {
    if (t_state_inside) return 0;
    if (state_ready != 2 && !state_init()) {
        hl_gate_on_80077d60 = 0;
        return 0;
    }
    Ctx *before = t_ctx;
    t_ctx = c;
    int answered;
    if (state_check) {
        answered = hl_gfx_state_check_begin(c->x[0], c->x[1], c->x[31]);
        if (answered) {
            t_state_inside = 1;
            state_recording = 1;
            state_translated(c);
            state_recording = 0;
            t_state_inside = 0;
            hl_gfx_state_check_end();
        }
    } else {
        answered = hl_gfx_state_apply(c->x[0], c->x[1], c->x[31]);
    }
    t_ctx = before;
    if (log_on) hl_report();
    return answered;
}
__attribute__((constructor)) static void hl_gfx_state_gate(void) { hl_gate_on_80077d60 = 1; }
