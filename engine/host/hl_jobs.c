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
   HL_JOBS_WORKERS=<n>: the pool's threads (default 6). HL_JOBS_LOG=1: the counters every 10 s. */
#include "hl_host.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_JOBS_DEFAULT HL_JOBS_THROUGH
#define HL_WORKER_MESSAGE 0x8004aae0ULL   /* (hl::bridge::kWorkerMessage: hl_jobs_on checks the two agree) */

static int hl_log_on;
static void hl_report(void) {
    static volatile LONG64 last;
    const int64_t now = hos_now_ns();
    const int64_t was = last;
    if (now - was < 10000000000LL || InterlockedCompareExchange64(&last, now, was) != was) return;
    char line[512];
    if (hl_bridge_report(line, sizeof line)) hos_log("hl jobs: %s", line);
}

static void hl_worker_message(Ctx *c) {
    hl_bridge_worker_message(c->x[0], c->x[1]);
    if (hl_log_on) hl_report();
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
    const int on = hl_bridge_start(hl_host(), mode, w ? (unsigned)atoi(w) : 0);
    hl_log_on = getenv("HL_JOBS_LOG") != NULL;
    char line[512];
    hl_bridge_report(line, sizeof line);
    hos_log("hl jobs: %s: %s", on ? "on" : "not started", line);
    return on;
}

HL_ANSWER(HL_WORKER_MESSAGE, hl_worker_message, hl_jobs_on, "a job thread's stage message")
