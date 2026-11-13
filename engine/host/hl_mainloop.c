/* (HeyListen native engine) The OS layer's side of the native main loop (engine/native hl/bridge/FrameLoop.h;
   docs/native-engine/main-loop.md).

   The game's main thread ends in the framework's main loop (guest 0x809667b0), which calls the framework's frame function
   forever. HL_MAINLOOP=1: an entry gate on that loop hands the main thread to the native main loop (hl::app::MainLoop
   through hl::bridge::FrameLoop): the iteration and the order of the frame's steps are native, each step calls the
   game's own functions, the frame's time step stays the game's and the pacing the OS layer's (nn_vi.c, nvn_native.c).
   It needs the job bridge on (HL_JOBS, the default), whose native job system the loop is given. HL_MAINLOOP=0 (the
   default until validated): the translated loop, as before.
   HL_MAINLOOP_CHECK=1: the loop's invariants are counted (each phase once a frame in the game's order; the module
   system's frame 0x80706be0, observed through a second entry gate that runs it translated, run at most once a frame and
   its frame counter counted up by one) and logged every 10 s with the phases' functions. HL_MAINLOOP_LOG=1: the
   counters alone every 10 s. */
#include "hl_host.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_MAINLOOP_DEFAULT 0
#define HL_JOBS_WORKER_MESSAGE 0x8004aae0ULL   /* (hl_jobs.c: answered when the job bridge is on) */

A64Fn a64_lookup_replaced(uint64_t addr);   /* hl_host.c */
extern volatile int hl_gate_on_80706be0;     /* (the check's gate, below) */

static const uint64_t loop_gates[] = {0x809667b0};
static const uint64_t check_gates[] = {0x80706be0};
#define GATES_IN(what, list) hl_gates_in(what, list, (int)(sizeof list / sizeof list[0]))

static int env_flag(const char *name, int dflt) {
    const char *e = getenv(name);
    return e && e[0] ? atoi(e) : dflt;
}

/* (decided once, when the main thread enters the loop: the game's build, the job bridge and the gates are known then) */
static int mainloop_on(void) {
    if (!hl_supported_build() || !GATES_IN("HL_MAINLOOP", loop_gates)) return 0;
    if (!a64_lookup_replaced(HL_JOBS_WORKER_MESSAGE)) {
        hos_log("hl mainloop: off: the job bridge is off (HL_JOBS=0), the native loop needs its job system");
        return 0;
    }
    const int check = env_flag("HL_MAINLOOP_CHECK", 0) == 1 && GATES_IN("HL_MAINLOOP_CHECK", check_gates);
    const int log = check || getenv("HL_MAINLOOP_LOG") != NULL;
    const uint64_t scratch = hos_sys_alloc(32, 16);
    if (!hl_bridge_mainloop_start(HL_MAINLOOP_NATIVE | (check ? HL_MAINLOOP_CHECK : 0) | (log ? HL_MAINLOOP_LOG : 0), scratch)) {
        hos_log("hl mainloop: off: the native loop did not start");
        return 0;
    }
    hl_gate_on_80706be0 = check;
    hos_log("hl mainloop: on (HL_MAINLOOP=1): the framework's main loop is the native loop's%s",
            check ? ", its invariants counted (HL_MAINLOOP_CHECK=1)" : "");
    return 1;
}

/* The framework's main loop (framework): never returns when answered. */
HL_GATE(809667b0, "553871e59ba31a50") {
    static volatile LONG decided;
    static int on;
    if (!InterlockedExchange(&decided, 1)) on = mainloop_on();
    if (!on || c != hos_ctx()) return 0;
    hl_bridge_mainloop_run(c->x[0]);
    return 1;
}

/* (HL_MAINLOOP_CHECK) The module system's frame (object), run translated and observed around. */
static _Thread_local int calc_through;
HL_GATE(80706be0, "9e4c8d7da24dafcf") {
    if (calc_through) {
        calc_through = 0;
        return 0;
    }
    static A64Fn translated;
    if (!translated) translated = hl_translated(0x80706be0);
    if (!translated) return 0;
    const uint64_t object = c->x[0];
    hl_bridge_mainloop_calc_begin(object);
    calc_through = 1;
    translated(c);
    hl_bridge_mainloop_calc_end(object);
    return 1;
}

/* (the loop's gate is armed from the start when the switch says so; the gate itself decides the rest, once) */
__attribute__((constructor)) static void hl_mainloop_switch(void) {
    hl_gate_on_809667b0 = env_flag("HL_MAINLOOP", HL_MAINLOOP_DEFAULT) == 1;
}
