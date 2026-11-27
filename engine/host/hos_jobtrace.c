// Copyright (C) 2026 Sacrappy

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
   thread pointer, 16 hex digits> <name>" lines, then "<ns> <type> <thread idx> <a hex> <b hex>" (types in hos.h, JT_*).

   HL_STALL_WATCH=<ms> (diagnostic, STALL1 22 Nov 2026): the same records kept all the time in a ring (4 M records, the last
   seconds of a busy game), and a watcher thread that looks at the presenting thread's cadence: when no present has come for
   <ms> (default 40) it takes the process's page-fault count and working set, each thread's CPU time and where each thread is
   running (the instruction pointer: module + offset), and when the present does come it writes the records of the gap (and 3 ms
   before it) to <HL_STALL_WATCH_DIR>/stall_<n>.jt in the HOS_JOBTRACE format and logs the gap's per-thread CPU time, page
   faults and working-set change: what every thread did while the presenting thread's wait lasted. 80 dumps at most, none before
   HL_STALL_WATCH_AFTER seconds (the start-up's gaps).

   HL_STALL_WATCH=rel:<ms> (the relative form; default 2): the threshold follows the frames: a gap counts when it reaches the median of
   the 30 intervals before it plus <ms> and 1.25x that median, never under 4 ms, so a stutter in a 120 fps zone is caught as well as a
   long freeze. The watcher sleeps 1 ms at a time (the system timer's granularity, 1 to 15.6 ms, decides how early a mark is taken
   for the short gaps; the log lines and the dump still carry the true gap). At most 20 gaps a second are looked at (the suspension of
   every thread to read its instruction pointer is the cost, about a millisecond of the watcher's and the threads' time per gap), the
   ones over are counted in the log; HL_STALL_WATCH_MAX (default 80) is the number of dumps. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <immintrin.h>

volatile int hos_jt_on;

typedef struct { int64_t t; uint16_t type, th; uint32_t pad; uint64_t a, b; } JtRec;
#define JT_CAP (4u << 20)   /* (a power of two: the ring of HL_STALL_WATCH masks with it) */
static JtRec *recs;
static volatile LONG64 nrec;
static struct { void *host; char name[64]; DWORD tid; } ths[512];
static volatile LONG nths;
static _Thread_local int my_idx;   /* (index + 1; 0: none yet) */

static int thread_idx(void) {
    if (my_idx) return my_idx - 1;
    const LONG i = InterlockedIncrement(&nths) - 1;
    if (i >= 512) { my_idx = 512; return 511; }
    HosThread *t = hos_self();
    ths[i].host = t;
    ths[i].tid = GetCurrentThreadId();
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

int hos_jt_name_of_tid(unsigned long tid, char *out, int n) {
    const LONG m = nths < 512 ? nths : 512;
    for (LONG i = 0; i < m; i++)
        if (ths[i].tid == tid) { snprintf(out, (size_t)n, "%s", ths[i].name); return 1; }
    return 0;
}

static int sw_ring;   /* (HL_STALL_WATCH: the records wrap) */
static volatile int64_t sw_last_present;
void hos_jt_rec(int type, uint64_t a, uint64_t b) {
    if (!recs) return;
    LONG64 i = InterlockedIncrement64(&nrec) - 1;
    if (sw_ring) i &= JT_CAP - 1;
    else if (i >= JT_CAP) return;
    JtRec *r = &recs[i];
    r->t = hos_now_ns();
    r->type = (uint16_t)type;
    r->th = (uint16_t)thread_idx();
    r->a = a;
    r->b = b;
}
void hos_jt_job(uint64_t obj, uint64_t target, uint64_t site, int begin) {
    HOS_JT(begin ? JT_JOB_B : JT_JOB_E, obj, (target & 0xffffffffu) | site << 32);
}
/* (TICK-054) the renderer's marks (gpu/vk/vk_replay.cpp jt_mark: it looks this up in the exe, as it includes no host header):
   a JT_MARK record of that kind, only while the window is open */
__declspec(dllexport) void hos_jt_mark_ext(int kind, uint64_t v) { HOS_JT(JT_MARK, kind, v); }
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
    {   /* (TICK-054) the GPU's progress while the window is open: a JT_MARK 7 record each time the renderer's timeline
           moves (rx_nvn_replayed: the submissions the GPU has finished), looked at every ~2 us on this thread */
        uint64_t rx_nvn_replayed(void);
        const int64_t until = hos_now_ns() + (int64_t)(secs * 1e9);
        uint64_t last = 0;
        while (hos_now_ns() < until) {
            const uint64_t v = rx_nvn_replayed();
            if (v != last) { HOS_JT(JT_MARK, 7, v); last = v; }
            for (int i = 0; i < 100; i++) _mm_pause();
        }
    }
    hos_jt_on = 0;
    const int64_t t1 = hos_now_ns();
    Sleep(100);   /* (records already begun) */
    LONG64 n = nrec;
    if (n > JT_CAP) n = JT_CAP;
    FILE *f = fopen(path, "w");
    if (!f) { hos_log("jobtrace: cannot write %s", path); return 0; }
    fprintf(f, "# jobtrace window %lld %lld\n", (long long)t0, (long long)t1);
    const LONG m = nths < 512 ? nths : 512;
    /* (the host pointer as 16 hex digits, not %p: glibc prints a null %p as "(nil)", MSVC as zeros) */
    for (LONG i = 0; i < m; i++)
        fprintf(f, "T %ld %016llx %s\n", (long)i, (unsigned long long)(uintptr_t)ths[i].host,
                ths[i].name[0] ? ths[i].name : "?");
    for (LONG64 i = 0; i < n; i++)
        fprintf(f, "%lld %u %u %llx %llx\n", (long long)recs[i].t, recs[i].type, recs[i].th, (unsigned long long)recs[i].a,
                (unsigned long long)recs[i].b);
    fclose(f);
    hos_log("jobtrace: %lld records (%s%.1f s) to %s", (long long)n, nrec > JT_CAP ? "full, " : "", (t1 - t0) / 1e9, path);
    return 0;
}

/* (HL_STALL_WATCH) the presenting thread's present: the watcher's clock */
static volatile int64_t sw_iv[30];   /* (the last 30 intervals between presents, written here so none is missed by the watcher's 1 ms polling) */
static volatile LONG sw_iv_n;
void hos_sw_present(void) {
    if (!sw_ring) return;
    const int64_t t = hos_now_ns(), prev = sw_last_present;
    if (prev) { const LONG n = sw_iv_n; sw_iv[n % 30] = t - prev; sw_iv_n = n + 1; }
    sw_last_present = t;
}

typedef struct { int64_t user, kern; uint64_t cyc; } SwCpu;   /* (GetThreadTimes ticks at 15.6 ms: the cycles are the exact figure) */
static int64_t ft_ns(FILETIME f) { return (int64_t)((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime) * 100; }
static void sw_cpu_all(HANDLE *hs, SwCpu *out, LONG m) {
    for (LONG i = 0; i < m; i++) {
        FILETIME c, e, k, u;
        out[i].user = out[i].kern = -1;
        out[i].cyc = 0;
        if (hs[i]) QueryThreadCycleTime(hs[i], &out[i].cyc);
        if (hs[i] && GetThreadTimes(hs[i], &c, &e, &k, &u)) out[i].user = ft_ns(u), out[i].kern = ft_ns(k);
    }
}
static void sw_where(void *rip, char *out, size_t cap) {
    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod) && mod) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(mod, path, sizeof path);
        const char *base = strrchr(path, '\\');
        snprintf(out, cap, "%s+%#llx", base ? base + 1 : path, (unsigned long long)((char *)rip - (char *)mod));
    } else snprintf(out, cap, "%p", rip);
}
/* HL_STALL_WATCH_STACK=<n> (diagnostic, JOBS1 27 Nov 2026; default 0: the top frame only, as before): at the mark each suspended
   thread's stack is walked n frames deep (at most 16) with the unwind tables of its modules (RtlLookupFunctionEntry +
   RtlVirtualUnwind), and the gap's lines give every frame as module+offset ("<" between frames, the innermost first): the wait
   SITES in our code (HeyListen.exe: its .map names them; hookshot_vk.dll: llvm-nm of the dll at 0x180000000 + offset) under the
   ntdll wait the top frame alone showed. Done while the thread is suspended (the walk reads its stack). Windows only: elsewhere
   the top frame, as before */
static int sw_stack_depth(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("HL_STALL_WATCH_STACK");
        v = e && atoi(e) > 0 ? (atoi(e) > 16 ? 16 : atoi(e)) : 0;
    }
    return v;
}
/* (while the thread is suspended: the return addresses only, into pcs; nothing named, logged or allocated, no module lookup: the
   thread may hold the loader's lock) */
static int sw_walk(CONTEXT *cx, int depth, uint64_t *pcs) {
    int f = 0;
#ifdef _WIN32
    for (; f < depth && cx->Rip; f++) {
        pcs[f] = cx->Rip;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(cx->Rip, &base, NULL);
        if (!fn) {   /* (a leaf function: its return address is at the top of the stack) */
            if (!cx->Rsp) break;
            cx->Rip = *(DWORD64 *)(uintptr_t)cx->Rsp;
            cx->Rsp += 8;
        } else {
            PVOID data = NULL;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, cx->Rip, fn, cx, &data, &frame, NULL);
        }
    }
#else
    if (depth > 0) pcs[f++] = (uint64_t)cx->Rip;
#endif
    return f;
}
/* (after the thread is resumed) the frames named, module+offset, innermost first */
static void sw_where_stack(const uint64_t *pcs, int n, char *out, size_t cap) {
    size_t at = 0;
    out[0] = 0;
    for (int f = 0; f < n; f++) {
        char one[96];
        sw_where((void *)(uintptr_t)pcs[f], one, sizeof one);
        const int w = snprintf(out + at, cap - at, "%s%s", f ? " < " : "", one);
        if (w < 0 || (size_t)w >= cap - at) break;
        at += (size_t)w;
    }
}
static void sw_dump(const char *dir, int n, int64_t from, int64_t to) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/stall_%02d.jt", dir, n);
    FILE *f = fopen(path, "w");
    if (!f) { hos_log("stallwatch: cannot write %s", path); return; }
    fprintf(f, "# jobtrace window %lld %lld\n", (long long)from, (long long)to);
    const LONG m = nths < 512 ? nths : 512;
    for (LONG i = 0; i < m; i++)
        fprintf(f, "T %ld %016llx %s\n", (long)i, (unsigned long long)(uintptr_t)ths[i].host, ths[i].name[0] ? ths[i].name : "?");
    LONG64 cnt = 0;
    for (LONG64 i = 0; i < JT_CAP; i++) {
        const JtRec *r = &recs[i];
        if (r->t < from || r->t > to) continue;
        fprintf(f, "%lld %u %u %llx %llx\n", (long long)r->t, r->type, r->th, (unsigned long long)r->a, (unsigned long long)r->b);
        cnt++;
    }
    fclose(f);
    hos_log("stallwatch: %lld records to %s", (long long)cnt, path);
}
static DWORD WINAPI sw_thread(void *p) {
    const char *dir = (const char *)p;
    /* HL_STALL_WATCH=<ms> (diagnostic): a gap of that many ms or more between presents is dumped (default 40); HL_STALL_WATCH=rel:<ms> (default 2):
       a gap that is <ms> and 1.25x over the median of the last 30 intervals (never under 4 ms), at most 20 gaps a second */
    const char *e = getenv("HL_STALL_WATCH");
    const int rel = e && !strncmp(e, "rel", 3);   /* (HL_STALL_WATCH=rel:<ms>: see the top of the file) */
    const double rel_ms = rel && e[3] == ':' && atof(e + 4) > 0 ? atof(e + 4) : 2.0;
    const int thr_ms = !rel && e && atoi(e) > 1 ? atoi(e) : 40;
    int64_t ring[30] = {0}, prev_lp = 0, thr_ns = (int64_t)thr_ms * 1000000LL, win_s = -1, in_win = 0, dropped = 0;
    int rn = 0;
    /* HL_STALL_WATCH_MAX=<n> (diagnostic): the number of stall dumps written (default 80) */
    const char *mx = getenv("HL_STALL_WATCH_MAX");
    const int max_dumps = mx && atoi(mx) > 0 ? atoi(mx) : 80;
    static HANDLE hs[512];
    static SwCpu c0[512], c1[512];
    static CONTEXT ctx[512];
    static char have[512], where[512][1024];   /* (HL_STALL_WATCH_STACK: up to 16 frames of module+offset) */
    const int depth = sw_stack_depth();
    static uint64_t pcs[512][16];
    static int npc[512];
    PROCESS_MEMORY_COUNTERS pm0, pm1;
    memset(&pm0, 0, sizeof pm0);
    int64_t cur = 0;   /* (the present the running gap began at; 0: none) */
    int64_t mark_ns = 0;   /* (the threshold the running gap's mark was taken at) */
    int dumped = 0;
    const char *af = getenv("HL_STALL_WATCH_AFTER");   /* (seconds since the first present before dumps are written: the start-up's gaps fill the quota) */
    const int after_s = af && *af ? atoi(af) : 0;
    int64_t first = 0;
    for (;;) {
        Sleep(1);
        const int64_t now = hos_now_ns(), lp = sw_last_present;
        if (!lp) continue;
        if (!first) first = lp;
        if (rel && lp != prev_lp) {   /* (the interval just ended: the median of the last 30 sets the threshold of the next gap) */
            prev_lp = lp;
            rn = sw_iv_n < 30 ? (int)sw_iv_n : 30;
            for (int i = 0; i < rn; i++) ring[i] = sw_iv[i];
            if (rn >= 10) {
                int64_t t[30];
                for (int i = 0; i < rn; i++) t[i] = ring[i];
                for (int i = 1; i < rn; i++) for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { int64_t x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
                const int64_t med = (rn & 1) ? t[rn / 2] : (t[rn / 2 - 1] + t[rn / 2]) / 2;
                int64_t th = med + (int64_t)(rel_ms * 1e6);
                if (th < med * 5 / 4) th = med * 5 / 4;
                if (th < 4000000LL) th = 4000000LL;
                thr_ns = th;
            } else thr_ns = 40000000LL;   /* (no median yet: the absolute default) */
        }
        const int now_s = (int)((now - first) / 1000000000LL);
        const LONG m = nths < 512 ? nths : 512;
        for (LONG i = 0; i < m; i++)
            if (!hs[i] && ths[i].tid) hs[i] = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ths[i].tid);
        if (cur && lp != cur) {   /* the present came: the gap is over */
            const double ms = (lp - cur) / 1e6;
            sw_cpu_all(hs, c1, m);
            memset(&pm1, 0, sizeof pm1);
            K32GetProcessMemoryInfo(GetCurrentProcess(), &pm1, sizeof pm1);
            hos_log("stallwatch: gap %.1f ms between presents: page faults +%lu, working set %+lld MB since the %d ms mark; each thread's CPU since the mark and where it was then",
                    ms, (unsigned long)(pm1.PageFaultCount - pm0.PageFaultCount),
                    ((long long)pm1.WorkingSetSize - (long long)pm0.WorkingSetSize) / (1 << 20), (int)(mark_ns / 1000000LL));
            for (LONG i = 0; i < m; i++) {
                if (c0[i].user < 0 || c1[i].user < 0) continue;
                const double u = (c1[i].user - c0[i].user) / 1e6, k = (c1[i].kern - c0[i].kern) / 1e6;
                const double mc = (double)(c1[i].cyc - c0[i].cyc) / 1e6;   /* (millions of cycles: about 4.5 per ms) */
                if (u + k >= 0.5 || mc >= 0.5) hos_log("stallwatch:   %-28s %.1f Mcycles (cpu ticks user %.1f kernel %.1f ms); then at %s", ths[i].name, mc, u, k,
                                                       have[i] ? where[i] : "-");
            }
            if (dumped < max_dumps && now_s >= after_s) sw_dump(dir, dumped++, cur - 3000000LL, lp + 1000000LL);
            cur = 0;
        }
        if (!cur && now - lp >= thr_ns) {   /* a gap reached the threshold: where every thread is */
            if (rel) {   /* (the rate cap: 20 gaps a second are looked at) */
                const int64_t sec = now / 1000000000LL;
                if (sec != win_s) {
                    if (dropped) hos_log("stallwatch: %lld gaps over the 20 a second cap were not looked at", (long long)dropped);
                    win_s = sec, in_win = 0, dropped = 0;
                }
                if (in_win >= 20) { dropped++; Sleep(2); continue; }
                in_win++;
            }
            cur = lp;
            mark_ns = thr_ns;
            sw_cpu_all(hs, c0, m);
            memset(&pm0, 0, sizeof pm0);
            K32GetProcessMemoryInfo(GetCurrentProcess(), &pm0, sizeof pm0);
            const DWORD self = GetCurrentThreadId();
            for (LONG i = 0; i < m; i++) {
                have[i] = 0;
                where[i][0] = 0;
                if (!hs[i] || ths[i].tid == self) continue;
                if (SuspendThread(hs[i]) == (DWORD)-1) continue;
                memset(&ctx[i], 0, sizeof ctx[i]);
#ifdef _WIN32
                ctx[i].ContextFlags = depth ? CONTEXT_FULL : CONTEXT_CONTROL;
#else
                ctx[i].ContextFlags = CONTEXT_CONTROL;
#endif
                have[i] = GetThreadContext(hs[i], &ctx[i]) != 0;
                npc[i] = have[i] && depth ? sw_walk(&ctx[i], depth, pcs[i]) : 0;   /* (the stack read while it stands still) */
                ResumeThread(hs[i]);
            }
            for (LONG i = 0; i < m; i++)
                if (have[i] && depth) sw_where_stack(pcs[i], npc[i], where[i], sizeof where[i]);
                else if (have[i]) sw_where((void *)(uintptr_t)ctx[i].Rip, where[i], sizeof where[i]);
        }
    }
    return 0;
}

void hos_jt_start(void) {
    {   /* (HL_STALL_WATCH=<ms> or rel:<ms>: the ring and the watcher; see the top of the file) */
        const char *w = getenv("HL_STALL_WATCH");
        if (w && *w && (atoi(w) != 0 || !strncmp(w, "rel", 3))) {
            recs = (JtRec *)VirtualAlloc(NULL, (SIZE_T)JT_CAP * sizeof(JtRec), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!recs) return;
            hos_mem_tag(recs, (uint64_t)JT_CAP * sizeof(JtRec), "job trace ring (HL_STALL_WATCH)");
            static char dir[MAX_PATH];
            const char *d = getenv("HL_STALL_WATCH_DIR");
            snprintf(dir, sizeof dir, "%s", d && *d ? d : "stall_watch");
            CreateDirectoryA(dir, NULL);
            sw_ring = 1, hos_jt_on = 1;
            HANDLE h = CreateThread(NULL, 0, sw_thread, dir, 0, NULL);
            if (h) CloseHandle(h);
            if (!strncmp(w, "rel", 3)) hos_log("stallwatch: on, a gap over the median of the last 30 by %g ms and 1.25x is dumped to %s (HL_STALL_WATCH=rel)", w[3] == ':' && atof(w + 4) > 0 ? atof(w + 4) : 2.0, dir);
            else hos_log("stallwatch: on, a gap of %d ms or more between presents is dumped to %s (HL_STALL_WATCH)", atoi(w) > 1 ? atoi(w) : 40, dir);
            return;
        }
    }
    const char *path = getenv("HOS_JOBTRACE");
    if (!path || !*path) return;
    recs = (JtRec *)VirtualAlloc(NULL, (SIZE_T)JT_CAP * sizeof(JtRec), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!recs) return;
    hos_mem_tag(recs, (uint64_t)JT_CAP * sizeof(JtRec), "job trace records (HOS_JOBTRACE)");
    static char keep[MAX_PATH];
    snprintf(keep, sizeof keep, "%s", path);
    HANDLE h = CreateThread(NULL, 0, jt_thread, keep, 0, NULL);
    if (h) CloseHandle(h);
    hos_log("jobtrace: on, to %s", keep);
}
