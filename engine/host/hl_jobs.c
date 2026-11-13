/* (HeyListen native engine) The OS layer's side of the native job system's bridge (engine/native, hl/c/hl_bridge.h;
   docs/native-engine/jobs-boundary.md).

   The game's job threads get each stage's work as a message; the function that answers it (guest 0x8004aae0, reached only
   through a pointer) is answered here instead of by its translation (hl_host.h HL_ANSWER: the lifted code is not touched).
   The native job system then runs the stage's work as one of its jobs, under the lane of the worker's core:
     HL_JOBS=0  off: the translated function, as before
     HL_JOBS=1  through (the default): the job is run by the worker's own thread while it waits for it
     HL_JOBS=2  pool: the job is run by a thread of the native pool, which goes on as the worker's guest thread
                (hos_thread_act_as) on that thread's own guest context, while the worker's host thread stands still.
                It ran every stage on the pool in the castle scene, but one run of four stalled: not for use yet
   HL_JOBS_WORKERS=<n>: the pool's threads (default 6). HL_JOBS_LOG=1: the counters every 10 s.
   HL_DISPATCH=1 (the default, with HL_JOBS on): the stage's work is run by the native dispatcher (hl/bridge/Dispatcher.h)
   instead of the translated 0x8004ab40, on the same scheduler (docs/native-engine/jobs-boundary.md, "The dispatcher's
   protocol"). HL_DISPATCH=0: the translated dispatcher, as before.
   HL_DISPATCH_ALL=1 (with HL_DISPATCH=1): every other caller of the dispatcher runs the native one too, through an entry
   gate on 0x8004ab40 (nx2/hl_gates.py puts it in the lifted code at build time): the stage's owner taking part in the
   stage wait 0x806afc40, the frame's last stage 0x83acfb90, and so every job body that waits for a stage. HL_DISPATCH_ALL=0:
   only the worker message path is native, as before.
   HL_DISPATCH_CHECK=1: its invariants are counted (and, with HL_DISPATCH=0, those that can be read after the translated
   dispatcher), and the counters are logged every 10 s. */
#include "hl_host.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_JOBS_DEFAULT HL_JOBS_THROUGH
#define HL_DISPATCH_DEFAULT 1   /* (validated 2026-11-13: invariants clean in 18 runs over the three scenes, two PCs) */
#define HL_DISPATCH_ALL_DEFAULT 0
#define HL_WORKER_MESSAGE 0x8004aae0ULL   /* (hl::bridge::kWorkerMessage: hl_jobs_on checks the two agree) */

static int hl_log_on;
static void hl_report(void) {
    static volatile LONG64 last;
    const int64_t now = hos_now_ns();
    const int64_t was = last;
    if (now - was < 10000000000LL || InterlockedCompareExchange64(&last, now, was) != was) return;
    char line[2048];
    if (hl_bridge_report(line, sizeof line)) hos_log("hl jobs: %s", line);
}

static void hl_worker_message(Ctx *c) {
    hl_bridge_worker_message(c->x[0], c->x[1]);
    if (hl_log_on) hl_report();
}

/* The dispatcher 0x8004ab40(scheduler, context), called directly by the stage wait and the last stage. The native side
   calls guest functions on the thread's own context (hos_ctx()): it must be the one the gate was given, which is so for
   every thread the OS layer runs; otherwise the translated body runs. */
HL_GATE(8004ab40, "4f17bc2e2e505f0f") {
    if (c != hos_ctx()) {
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1)) hos_log("hl jobs: the dispatcher's gate was given another context than the thread's: translated");
        return 0;
    }
    return hl_bridge_dispatch_gate(c->x[0], c->x[1]);
}

/* 1 when the native job system answers the game's worker message (asked once, at the function's first lookup) */
static int hl_jobs_on(void) {
    const char *e = getenv("HL_JOBS"), *w = getenv("HL_JOBS_WORKERS");
    const int mode = e && e[0] ? atoi(e) : HL_JOBS_DEFAULT;
    if (mode == HL_JOBS_OFF) {
        hos_log("hl jobs: off (HL_JOBS=0)");
        return 0;
    }
    if (hl_bridge_worker_message_address() != HL_WORKER_MESSAGE) {
        hos_log("hl jobs: off: the bridge answers %#llx, not the worker message", (unsigned long long)hl_bridge_worker_message_address());
        return 0;
    }
    const char *d = getenv("HL_DISPATCH"), *dc = getenv("HL_DISPATCH_CHECK");
    const int native = d && d[0] ? atoi(d) == 1 : HL_DISPATCH_DEFAULT;
    const int check = dc && dc[0] == '1';
    const int on = hl_bridge_start(hl_host(), mode, w ? (unsigned)atoi(w) : 0,
                                   (native ? HL_DISPATCH_NATIVE : 0) | (check ? HL_DISPATCH_CHECK : 0));
    hl_log_on = getenv("HL_JOBS_LOG") != NULL || check;
    const char *a = getenv("HL_DISPATCH_ALL");
    const int all = on && native && (a && a[0] ? atoi(a) == 1 : HL_DISPATCH_ALL_DEFAULT);
    hl_gate_on_8004ab40 = all;
    char line[2048];
    hl_bridge_report(line, sizeof line);
    hos_log("hl jobs: %s%s: %s", on ? "on" : "not started",
            all ? ", every caller of the dispatcher native (HL_DISPATCH_ALL=1)" : "", line);
    return on;
}

HL_ANSWER(HL_WORKER_MESSAGE, hl_worker_message, hl_jobs_on, "a job thread's stage message")
