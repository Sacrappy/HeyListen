/* (HeyListen, TICK-039) HOS_JOBTRACE=<path> (diagnostic): a time-stamped trace of every guest thread's blocking and
   waking for a short window, to rebuild the game's job chain offline (tools/bench/jobtrace.py): when each thread waits
   (light/normal events, condition variables, contended mutexes, message queues, semaphores, the kernel's arbitration
   and address waits), on what (the guest object's address) and from where (the guest caller), when it runs again and
   how (ready, after the 20 us spin, after sleeping), who signals what (and for the kernel's wakes, whom), the guest's
   SleepThread polls, and the waits for a virtual core. Everything between a thread's waits is work (guest code and the
   runtime under it); the trace never counts a copy or a driver call as a wait or the other way round.

   HOS_JOBTRACE_AT=<s> (default 90): seconds after start the window opens; HOS_JOBTRACE_SECS=<s> (default 3) its length.
   Records go to one preallocated array (4 M x 32 bytes, allocated only when the trace is asked for) by an atomic index;
   outside the window a record costs one load and a branch. After the window the file is written: "T <idx> <host
   thread pointer> <name>" lines, then "<ns> <type> <thread idx> <a hex> <b hex>" (types in hos.h, JT_*). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

volatile int hos_jt_on;

typedef struct { int64_t t; uint16_t type, th; uint32_t pad; uint64_t a, b; } JtRec;
#define JT_CAP (4u << 20)
static JtRec *recs;
static volatile LONG64 nrec;
static struct { void *host; char name[64]; } ths[512];
static volatile LONG nths;
static _Thread_local int my_idx;   /* (index + 1; 0: none yet) */

static int thread_idx(void) {
    if (my_idx) return my_idx - 1;
    const LONG i = InterlockedIncrement(&nths) - 1;
    if (i >= 512) { my_idx = 512; return 511; }
    HosThread *t = hos_self();
    ths[i].host = t;
    if (t) snprintf(ths[i].name, sizeof ths[i].name, "%s", hos_thread_name(t));
    else {
        PWSTR d = NULL;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &d)) && d && d[0])
            snprintf(ths[i].name, sizeof ths[i].name, "host:%ls", d);
        else snprintf(ths[i].name, sizeof ths[i].name, "host:%lu", GetCurrentThreadId());
        if (d) LocalFree(d);
    }
    for (char *p = ths[i].name; *p; p++) if (*p == ' ') *p = '_';
    my_idx = i + 1;
    return i;
}

void hos_jt_rec(int type, uint64_t a, uint64_t b) {
    if (!recs) return;
    const LONG64 i = InterlockedIncrement64(&nrec) - 1;
    if (i >= JT_CAP) return;
    JtRec *r = &recs[i];
    r->t = hos_now_ns();
    r->type = (uint16_t)type;
    r->th = (uint16_t)thread_idx();
    r->a = a;
    r->b = b;
}
uint64_t hos_jt_lr(void) {
    Ctx *c = hos_ctx();
    return c ? c->x[30] : 0;
}

static DWORD WINAPI jt_thread(void *p) {
    const char *path = (const char *)p;
    const char *e = getenv("HOS_JOBTRACE_AT"), *s = getenv("HOS_JOBTRACE_SECS");
    const double at = e ? atof(e) : 90.0, secs = s ? atof(s) : 3.0;
    Sleep((DWORD)(at * 1000));
    const int64_t t0 = hos_now_ns();
    hos_jt_on = 1;
    Sleep((DWORD)(secs * 1000));
    hos_jt_on = 0;
    const int64_t t1 = hos_now_ns();
    Sleep(100);   /* (records already begun) */
    LONG64 n = nrec;
    if (n > JT_CAP) n = JT_CAP;
    FILE *f = fopen(path, "w");
    if (!f) { hos_log("jobtrace: cannot write %s", path); return 0; }
    fprintf(f, "# jobtrace window %lld %lld\n", (long long)t0, (long long)t1);
    const LONG m = nths < 512 ? nths : 512;
    for (LONG i = 0; i < m; i++) fprintf(f, "T %ld %p %s\n", (long)i, ths[i].host, ths[i].name[0] ? ths[i].name : "?");
    for (LONG64 i = 0; i < n; i++)
        fprintf(f, "%lld %u %u %llx %llx\n", (long long)recs[i].t, recs[i].type, recs[i].th, (unsigned long long)recs[i].a,
                (unsigned long long)recs[i].b);
    fclose(f);
    hos_log("jobtrace: %lld records (%s%.1f s) to %s", (long long)n, nrec > JT_CAP ? "full, " : "", (t1 - t0) / 1e9, path);
    return 0;
}

void hos_jt_start(void) {
    const char *path = getenv("HOS_JOBTRACE");
    if (!path || !*path) return;
    recs = (JtRec *)VirtualAlloc(NULL, (SIZE_T)JT_CAP * sizeof(JtRec), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!recs) return;
    static char keep[MAX_PATH];
    snprintf(keep, sizeof keep, "%s", path);
    HANDLE h = CreateThread(NULL, 0, jt_thread, keep, 0, NULL);
    if (h) CloseHandle(h);
    hos_log("jobtrace: on, to %s", keep);
}
