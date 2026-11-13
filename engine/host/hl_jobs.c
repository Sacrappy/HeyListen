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
   HL_DISPATCH_ALL=1 (the default, with HL_DISPATCH=1): every other caller of the dispatcher runs the native one too, through an entry
   gate on 0x8004ab40 (nx2/hl_gates.py puts it in the lifted code at build time): the stage's owner taking part in the
   stage wait 0x806afc40, the frame's last stage 0x83acfb90, and so every job body that waits for a stage. HL_DISPATCH_ALL=0:
   only the worker message path is native, as before.
   HL_DISPATCH_CHECK=1: its invariants are counted (and, with HL_DISPATCH=0, those that can be read after the translated
   dispatcher), and the counters are logged every 10 s.
   HL_STAGES=1 (the default, with HL_JOBS on): the stage protocol around the dispatcher is native (hl/bridge/Stages.h;
   docs/native-engine/jobs-boundary.md, "The stages' protocol"): entry gates on the registration 0x806b0260, the stage
   wait 0x806afc40 and the frame's last stage 0x83acfb90, together (they share the registered mask and count). The
   worker sets, the scheduler's list, the workers' words and the guest's events stay in place. HL_STAGES=0: the
   translated functions, as before. HL_STAGES_CHECK=1: the protocol's invariants are counted, each registration is
   compared with the translated one on the same set, and the counters are logged every 10 s.
   HL_JOBGROUP=1 (the default, with HL_JOBS on): the module system's job group 0x8007fe60 is native
   (hl/bridge/JobGroup.h; docs/native-engine/jobs-boundary.md, "The module system's job group"): an entry gate on it, which
   both of its callers reach (the module workers' message and the main thread's stage wait twin). The group scheduler, its
   heap, mutex and take count, the contexts' words and the guest's events stay in place. HL_JOBGROUP=0: the translated
   function, as before. HL_JOBGROUP_CHECK=1: its invariants are counted and the counters logged every 10 s.
   HL_MODSTAGES=1 (with HL_JOBS and HL_JOBGROUP on): the module system's twins of the registration 0x800711c0 (with the
   group's seed) and of the stage wait 0x80070b20 are native too (hl/bridge/Stages.h; docs/native-engine/jobs-boundary.md,
   "The module system's twins"), through two entry gates. HL_MODSTAGES=0: the translated twins, as before. Their
   invariants are counted with HL_STAGES_CHECK=1, and each twin registration is compared with the translated twin's. */
#include "hl_host.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_JOBS_DEFAULT HL_JOBS_THROUGH
#define HL_DISPATCH_DEFAULT 1   /* (validated 2026-11-13: invariants clean in 18 runs over the three scenes, two PCs) */
#define HL_DISPATCH_ALL_DEFAULT 1   /* (validated 2026-11-13: invariants clean in 14 runs over the three scenes, two PCs) */
#define HL_STAGES_DEFAULT 1   /* (validated 2026-11-13: invariants clean in 15 runs over the three scenes, two PCs) */
#define HL_JOBGROUP_DEFAULT 1   /* (validated 2026-11-13: invariants clean in 11 runs over the three scenes, two PCs) */
#define HL_MODSTAGES_DEFAULT 0
#define HL_WORKER_MESSAGE 0x8004aae0ULL   /* (hl::bridge::kWorkerMessage: hl_jobs_on checks the two agree) */

static int hl_log_on;
static void hl_report(void) {
    static volatile LONG64 last;
    const int64_t now = hos_now_ns();
    const int64_t was = last;
    if (now - was < 10000000000LL || InterlockedCompareExchange64(&last, now, was) != was) return;
    char line[2048];
    if (hl_bridge_report(line, sizeof line)) hos_log("hl jobs: %s", line);
    if (hl_bridge_stages_report(line, sizeof line)) hos_log("hl %s", line);
    if (hl_bridge_jobgroup_report(line, sizeof line)) hos_log("hl %s", line);
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

/* The stage protocol's three functions (HL_STAGES), on the thread's own context as the dispatcher's gate. */
static int hl_stage_ctx(Ctx *c) {
    if (c == hos_ctx()) return 1;
    static volatile LONG told;
    if (!InterlockedExchange(&told, 1)) hos_log("hl jobs: a stage gate was given another context than the thread's: translated");
    return 0;
}

HL_GATE(806b0260, "fbe1ec275809759d") {   /* the registration (worker set, send) */
    if (!hl_stage_ctx(c)) return 0;
    const int answered = hl_bridge_stage_register(c->x[0], c->x[1]);
    if (answered && hl_log_on) hl_report();
    return answered;
}

HL_GATE(806afc40, "8a578d7d69ee8980") {   /* the stage wait (worker set) */
    if (!hl_stage_ctx(c)) return 0;
    hl_bridge_stage_wait(c->x[0]);
    return 1;
}

HL_GATE(83acfb90, "e9b61c73cec382d7") {   /* the frame's last stage (actor manager) */
    if (!hl_stage_ctx(c)) return 0;
    hl_bridge_stage_last(c->x[0]);
    return 1;
}

HL_GATE(800711c0, "81fbe08dca8c572d") {   /* the module system's registration twin (worker set, group) */
    if (!hl_stage_ctx(c)) return 0;
    const int answered = hl_bridge_stage_register_module(c->x[0], c->x[1]);
    if (answered && hl_log_on) hl_report();
    return answered;
}

HL_GATE(80070b20, "2920e4fb75af8833") {   /* the module system's stage wait twin (worker set) */
    if (!hl_stage_ctx(c)) return 0;
    hl_bridge_stage_wait_module(c->x[0]);
    return 1;
}

HL_GATE(8007fe60, "677afe69100ea461") {   /* the module system's job group (group, context) */
    if (c != hos_ctx()) {
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1)) hos_log("hl jobs: the job group's gate was given another context than the thread's: translated");
        return 0;
    }
    hl_bridge_jobgroup(c->x[0], c->x[1]);
    if (hl_log_on) hl_report();
    return 1;
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
    const char *a = getenv("HL_DISPATCH_ALL");
    const int all = on && native && (a && a[0] ? atoi(a) == 1 : HL_DISPATCH_ALL_DEFAULT);
    hl_gate_on_8004ab40 = all;
    const char *jg = getenv("HL_JOBGROUP"), *jc = getenv("HL_JOBGROUP_CHECK");
    const int group_check = jc && jc[0] == '1';
    const int group = on && (jg && jg[0] ? atoi(jg) == 1 : HL_JOBGROUP_DEFAULT) &&
                      hl_bridge_jobgroup_start(HL_JOBGROUP_NATIVE | (group_check ? HL_JOBGROUP_CHECK : 0));
    const char *sg = getenv("HL_STAGES"), *sc = getenv("HL_STAGES_CHECK"), *ms = getenv("HL_MODSTAGES");
    const int stage_check = sc && sc[0] == '1';
    const int want = (sg && sg[0] ? atoi(sg) == 1 : HL_STAGES_DEFAULT) * HL_STAGES_NATIVE |
                     (group && (ms && ms[0] ? atoi(ms) == 1 : HL_MODSTAGES_DEFAULT)) * HL_STAGES_MODULE;
    const int started = on && want ? hl_bridge_stages_start(want | (stage_check ? HL_STAGES_CHECK : 0)) : 0;
    const int stages = (started & HL_STAGES_NATIVE) != 0, twins = (started & HL_STAGES_MODULE) != 0;
    hl_log_on = getenv("HL_JOBS_LOG") != NULL || check || ((stages || twins) && stage_check) || (group && group_check);
    /* (the registrations' gates last: a registration answered natively is waited for natively) */
    hl_gate_on_806afc40 = stages;
    hl_gate_on_83acfb90 = stages;
    hl_gate_on_806b0260 = stages;
    hl_gate_on_8007fe60 = group;
    hl_gate_on_80070b20 = twins;
    hl_gate_on_800711c0 = twins;
    char line[2048];
    hl_bridge_report(line, sizeof line);
    hos_log("hl jobs: %s%s%s%s%s: %s", on ? "on" : "not started",
            all ? ", every caller of the dispatcher native (HL_DISPATCH_ALL=1)" : "",
            stages ? ", the stages native (HL_STAGES=1)" : "",
            group ? ", the module system's job group native (HL_JOBGROUP=1)" : "",
            twins ? ", the module system's stage twins native (HL_MODSTAGES=1)" : "", line);
    return on;
}

HL_ANSWER(HL_WORKER_MESSAGE, hl_worker_message, hl_jobs_on, "a job thread's stage message")
