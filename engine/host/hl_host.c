/* (HeyListen native engine) The OS layer's common ground for the native modules (hl_host.h): the list of guest functions
   answered natively, and the callbacks the native side works through. docs/native-engine/README.md has the plan. */
#include "hl_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_SUPPORTED_BUILD "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348"

HosThread *hos_thread_act_as(HosThread *t);   /* hos_thread.c */

int hl_supported_build(void) { return !strcmp(hos_main_build_id, HL_SUPPORTED_BUILD); }

/* ---- guest functions answered natively ---- */
typedef struct {
    uint64_t guest;
    A64Fn fn;
    HlOn on;
    const char *what;
    volatile LONG state;   /* 0: not asked yet, 1: off, 2: on, 3: being asked */
} Answer;
enum { ANSWERS = 256 };
static Answer answers[ANSWERS];
static int nanswers;

void hl_answer(uint64_t guest, A64Fn fn, HlOn on, const char *what) {
    if (nanswers >= ANSWERS) return;   /* (constructors: one thread) */
    answers[nanswers] = (Answer){guest, fn, on, what, 0};
    nanswers++;
}

static int answer_on(Answer *a) {
    for (;;) {
        const LONG s = a->state;
        if (s == 1 || s == 2) return s == 2;
        if (s == 0 && InterlockedCompareExchange(&a->state, 3, 0) == 0) break;
        Sleep(0);
    }
    int on = 0;
    if (!hl_supported_build()) {
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1))
            hos_log("hl: off: this is not the build the native engine's addresses are of (%s)", hos_main_build_id);
    } else {
        on = a->on ? a->on() != 0 : 1;
    }
    a->state = on ? 2 : 1;
    return on;
}

/* (the runtime's lookup, once an address: a64_rt.c) */
A64Fn a64_lookup_replaced(uint64_t addr) {
    for (int i = 0; i < nanswers; i++)
        if (answers[i].guest == addr) return answer_on(&answers[i]) ? answers[i].fn : NULL;
    return NULL;
}

/* ---- entry gates in the lifted code (constructors: one thread, before the game starts) ---- */
enum { GATES_IN = 256 };
static uint64_t gates_in[GATES_IN];
static int ngates_in;

void hl_gate_present(unsigned long long guest) {
    if (ngates_in < GATES_IN) gates_in[ngates_in++] = guest;
}

int hl_gates_in(const char *what, const uint64_t *guest, int n) {
    char missing[256];
    int len = 0, all = 1;
    missing[0] = 0;
    for (int i = 0; i < n; i++) {
        int in = 0;
        for (int k = 0; k < ngates_in && !in; k++) in = gates_in[k] == guest[i];
        if (in) continue;
        all = 0;
        if (len < (int)sizeof missing - 20) len += snprintf(missing + len, sizeof missing - (size_t)len, " %#llx", (unsigned long long)guest[i]);
    }
    if (!all) hos_log("hl: %s left off: the entry gates of%s are not in the lifted code (another lift: nx2/hl_gates.py's build log)", what, missing);
    return all;
}

/* (the lifted table: rt/a64_rt.c reads it the same way) */
extern const uint32_t a64_fn_count;
extern const uint32_t a64_fn_addr[];
extern const A64Fn a64_fn_ptr[];
A64Fn hl_translated(uint64_t guest) {
    uint32_t lo = 0, hi = a64_fn_count;
    if (guest >> 32) return NULL;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) >> 1;
        if (a64_fn_addr[mid] < (uint32_t)guest) lo = mid + 1;
        else hi = mid;
    }
    return lo < a64_fn_count && a64_fn_addr[lo] == (uint32_t)guest ? a64_fn_ptr[lo] : NULL;
}

/* ---- what the native side is given ---- */
static void *cb_ptr(void *user, uint64_t guest) { (void)user; return GP(guest); }
static void *cb_current(void *user) { (void)user; return hos_self(); }

uint64_t hl_guest_call(HosThread *as, uint64_t fn, uint64_t a0, uint64_t a1) {
    HosThread *before = as ? hos_thread_act_as(as) : NULL;
    Ctx *c = hos_ctx();
    c->x[0] = a0;
    c->x[1] = a1;
    a64_icall(c, fn);
    const uint64_t r = c->x[0];
    if (as) hos_thread_act_as(before);
    return r;
}
static uint64_t cb_call(void *user, void *as, uint64_t fn, uint64_t a0, uint64_t a1) {
    (void)user;
    return hl_guest_call((HosThread *)as, fn, a0, a1);
}
/* (a frame goes on the guest stack below where the thread stands, and the stack pointer comes back after the call) */
static uint64_t cb_call_with(void *user, void *as, uint64_t fn, const uint64_t *args, unsigned nargs, const void *frame,
                             unsigned frame_size) {
    (void)user;
    HosThread *before = as ? hos_thread_act_as((HosThread *)as) : NULL;
    Ctx *c = hos_ctx();
    const uint64_t sp = c->x[31];
    for (unsigned i = 0; i < nargs && i < 4; i++) c->x[i] = args[i];
    if (frame_size) {
        const uint64_t at = (sp - frame_size) & ~(uint64_t)15;
        memcpy(GP(at), frame, frame_size);
        c->x[31] = at;
        c->x[nargs] = at;
    }
    a64_icall(c, fn);
    const uint64_t r = c->x[0];
    c->x[31] = sp;
    if (as) hos_thread_act_as(before);
    return r;
}
static void cb_log(void *user, const char *line) {
    (void)user;
    hos_log("%s", line);
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

const hl_guest_host *hl_host(void) {
    static const hl_guest_host host = {NULL, cb_ptr, cb_current, cb_call, cb_call_with, cb_log, cb_thread_start, cb_thread_join};
    return &host;
}
