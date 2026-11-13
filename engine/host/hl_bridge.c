/* (HeyListen native engine) The OS layer's side of the native job system's bridge (engine/native, hl/c/hl_bridge.h;
   docs/native-engine/jobs-boundary.md).

   The game's job threads get each stage's work as a message; the function that answers it (guest 0x8004aae0, reached only
   through a pointer) is answered here instead of by its translation (a64_lookup_replaced: the lifted code is not touched).
   The native job system then runs the stage's work as one of its jobs, under the lane of the worker's core:
     HL_JOBS=0  off: the translated function, as before
     (default: 1. 2 ran every stage on the pool in the castle scene but one run of four stalled: not for use yet)
     HL_JOBS=1  through: the job is run by the worker's own thread while it waits for it
     HL_JOBS=2  pool: the job is run by a thread of the native pool, which goes on as the worker's guest thread
                (hos_thread_act_as) on that thread's own guest context, while the worker's host thread stands still
   HL_JOBS_WORKERS=<n>: the pool's threads (default 6). HL_JOBS_LOG=1: the counters every 10 s.

   Only for the build the addresses are of (hos_main_build_id); any other game or update runs as before. */
#include "hos.h"
#include "hl/c/hl_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_JOBS_DEFAULT HL_JOBS_THROUGH
#define HL_SUPPORTED_BUILD "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348"

HosThread *hos_thread_act_as(HosThread *t);   /* hos_thread.c */

static void *cb_ptr(void *user, uint64_t guest) { (void)user; return GP(guest); }
static void *cb_current(void *user) { (void)user; return hos_self(); }

static void cb_call(void *user, void *as, uint64_t fn, uint64_t a0, uint64_t a1) {
    (void)user;
    HosThread *before = hos_thread_act_as((HosThread *)as);
    Ctx *c = hos_ctx();
    c->x[0] = a0;
    c->x[1] = a1;
    a64_icall(c, fn);
    hos_thread_act_as(before);
}

typedef struct { void (*entry)(void *); void *arg; } Start;
static DWORD WINAPI pool_main(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.entry(s.arg);
    return 0;
}
static void *cb_thread_start(void *user, void (*entry)(void *), void *arg, unsigned index) {
    (void)user;
    Start *s = (Start *)malloc(sizeof *s);
    s->entry = entry;
    s->arg = arg;
    /* (the host stack lifted code needs: a C call per guest call, as hos_thread_start gives guest threads) */
    HANDLE h = CreateThread(NULL, 64 << 20, pool_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) hos_fatal(NULL, "hl: CreateThread failed for pool worker %u", index);
    wchar_t name[32];
    swprintf(name, 32, L"hl-worker-%u", index);
    SetThreadDescription(h, name);
    return h;
}
static void cb_thread_join(void *user, void *handle) {
    (void)user;
    WaitForSingleObject((HANDLE)handle, INFINITE);
    CloseHandle((HANDLE)handle);
}

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

/* 1 when the native job system answers the game's worker message (decided once, at the first lookup) */
static int hl_on(void) {
    static volatile LONG state;   /* 0: not decided, 1: off, 2: on, 3: being decided */
    for (;;) {
        const LONG s = state;
        if (s == 1 || s == 2) return s == 2;
        if (s == 0 && InterlockedCompareExchange(&state, 3, 0) == 0) break;
        Sleep(0);
    }
    const char *e = getenv("HL_JOBS"), *w = getenv("HL_JOBS_WORKERS");
    const int mode = e && e[0] ? atoi(e) : HL_JOBS_DEFAULT;
    int on = 0;
    if (mode == HL_JOBS_OFF) {
        hos_log("hl jobs: off (HL_JOBS=0)");
    } else if (strcmp(hos_main_build_id, HL_SUPPORTED_BUILD)) {
        hos_log("hl jobs: off: this is not the build the bridge's addresses are of (%s)", hos_main_build_id);
    } else {
        static hl_guest_host host;
        host.ptr = cb_ptr;
        host.current = cb_current;
        host.call = cb_call;
        host.thread_start = cb_thread_start;
        host.thread_join = cb_thread_join;
        on = hl_bridge_start(&host, mode, w ? (unsigned)atoi(w) : 0);
        hl_log_on = getenv("HL_JOBS_LOG") != NULL;
        char line[512];
        hl_bridge_report(line, sizeof line);
        hos_log("hl jobs: %s: %s", on ? "on" : "not started", line);
    }
    state = on ? 2 : 1;
    return on;
}

A64Fn a64_lookup_replaced(uint64_t addr) {
    if (addr != hl_bridge_worker_message_address()) return NULL;
    return hl_on() ? hl_worker_message : NULL;
}
