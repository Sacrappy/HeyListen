/* (HeyListen native engine) The OS layer's side of the memory module (engine/native, hl/c/hl_mem.h;
   docs/native-engine/memory-boundary.md, section 8): cold pages of CPU-cached GPU pools leave the process's working set.

   The CPU side of the game's static GPU data (textures, vertex and index data in pools with flags 0x024, shader code in
   0x064: ~3.6 GB of RAM in every scene) stays in guest memory for the resources' lifetime, while the back end has it in
   VRAM. A page trap (section 7) saw game code touch under 1 MB of it in 35 s. A thread of its own takes such pages out of
   the working set, a bounded number a pass: VirtualUnlock on a range that is not locked removes its pages from the working
   set (documented behaviour; checked after each call with QueryWorkingSetEx). The pages stay committed and keep their bytes:
   they go to the standby list (written to the page file first when they are dirty), and any later touch, by the game, the
   back end or the kernel, gets the same bytes back with no handler of ours involved. So nothing can read a wrong byte; a
   page taken out too early costs a soft fault (a hard one if Windows had to give its RAM away meanwhile). RAM is freed;
   the commit charge is not (the pages are backed by the page file).
   The native policy (hl::mem::ColdPages) remembers, per 64 KB unit, the units that came back into the working set after
   being taken out: they were needed, and are left in for 30 s, then 60, 120, and for good after the fourth time, so that
   pages someone reads all the time are not taken out again and again. Never taken out: pools younger than
   HL_MEM_COLD_AGE seconds (default 10: the back end uploads a new resource's data in its first frames), ranges the back
   end imported (the driver pins them: hos_noaccess_import tells us before each import), anything while HOS_POOL_TRAP
   measures, and nothing with large pages (they cannot leave the working set).
     HL_MEM_COLD=1  on (the default)    HL_MEM_COLD=0  off: nothing here runs, as before
     HL_MEM_COLD_FLAGS=<hex,...>  the pool flags (default 24,64)
   Every 10 s a line "hl mem: cold pages: ..." gives the counters (MB out now, taken out, came back and how often). */
#include "hos.h"
#include "hl/c/hl_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_MEM_COLD_DEFAULT 1
#define COLD_UNIT_SHIFT 16                  /* (64 KB units: the commit-on-touch granule, 16 pages) */
#define COLD_PASS_MS 500
#define COLD_SCAN_PAGES (64 * 1024)         /* (256 MB looked at a pass: the ~3.6 GB in ~7 s) */
#define COLD_BUDGET_PAGES (16 * 1024)       /* (64 MB taken out a pass at most) */
#define COLD_BACKOFF_PASSES (30000 / COLD_PASS_MS)
#define COLD_GIVE_UP 4

static volatile LONG cold_on;
static uint32_t cold_flags[8];
static int cold_nflags;
static int64_t cold_age_ns;

void hl_mem_pool_made(uint64_t addr, uint64_t size) {
    if (cold_on && size) hl_mem_cold_forget(addr >> COLD_UNIT_SHIFT, (addr + size + 0xFFFF) >> COLD_UNIT_SHIFT);
}
void hl_mem_imported(uint64_t lo, uint64_t hi) {
    if (cold_on && hi > lo) hl_mem_cold_imported(lo >> COLD_UNIT_SHIFT, (hi + 0xFFFF) >> COLD_UNIT_SHIFT);
}

typedef struct { void *va; ULONG_PTR attr; } ColdWs;   /* PSAPI_WORKING_SET_EX_INFORMATION (bit 0: valid) */
typedef BOOL(WINAPI *ColdQueryFn)(HANDLE, PVOID, DWORD);
static ColdQueryFn cold_query;

/* one slice [a, b) of guest pages inside a 1 MB block of an eligible range: look at each unit's resident pages, take out
   those the policy says, and tell it what happened */
static void cold_slice(uint64_t a, uint64_t b) {
    enum { MAXP = 256 };
    ColdWs info[MAXP], again[MAXP];
    const int m = (int)((b - a) >> 12);
    for (int k = 0; k < m; k++) info[k].va = GP(a + ((uint64_t)k << 12)), info[k].attr = 0;
    if (!cold_query(GetCurrentProcess(), info, (DWORD)(m * sizeof(ColdWs)))) return;
    int took[MAXP >> 4][2], ntook = 0;   /* (first page, end page of each unit taken out) */
    for (int k = 0; k < m;) {
        const uint64_t unit = (a + ((uint64_t)k << 12)) >> COLD_UNIT_SHIFT;
        int e = k;
        while (e < m && ((a + ((uint64_t)e << 12)) >> COLD_UNIT_SHIFT) == unit) e++;
        uint32_t resident = 0;
        for (int j = k; j < e; j++) resident += (uint32_t)(info[j].attr & 1);
        if (hl_mem_cold_look(unit, resident)) {
            for (int j = k; j < e;) {   /* (runs of resident pages: committed, so the call covers them all) */
                if (!(info[j].attr & 1)) { j++; continue; }
                int r = j;
                while (r < e && (info[r].attr & 1)) r++;
                VirtualUnlock(info[j].va, (SIZE_T)(r - j) << 12);   /* (fails with ERROR_NOT_LOCKED, and trims: by design) */
                j = r;
            }
            took[ntook][0] = k, took[ntook][1] = e, ntook++;
        }
        k = e;
    }
    for (int t = 0; t < ntook; t++) {
        const int k = took[t][0], e = took[t][1];
        for (int j = k; j < e; j++) again[j - k].va = info[j].va, again[j - k].attr = 0;
        uint32_t left = 0, stayed = 0;
        if (cold_query(GetCurrentProcess(), again, (DWORD)((e - k) * sizeof(ColdWs))))
            for (int j = k; j < e; j++) {
                if (!(info[j].attr & 1)) continue;
                if (again[j - k].attr & 1) stayed++;
                else left++;
            }
        hl_mem_cold_taken((a + ((uint64_t)k << 12)) >> COLD_UNIT_SHIFT, left, stayed);
    }
}

static DWORD WINAPI cold_main(void *unused) {
    (void)unused;
    static uint64_t ranges[16384][2];
    uint64_t cursor = 0;   /* (where the next pass goes on: the scan is spread over passes) */
    double worst_ms = 0;
    int64_t next_report = hos_now_ns() + 10000000000LL;
    for (;;) {
        Sleep(COLD_PASS_MS);
        const int64_t t0 = hos_now_ns();
        if (hos_pool_trap_active()) continue;
        const int n = hos_pools_with_flags(cold_flags, cold_nflags, t0 - cold_age_ns, ranges, 16384);
        /* (whole pages; a range that starts in the 64 KB unit the one before it ends in starts at the next unit, so that a
           unit is looked at once a pass, with all its eligible pages) */
        for (int k = 0; k < n; k++) {
            ranges[k][0] = (ranges[k][0] + 0xFFF) & ~0xFFFULL, ranges[k][1] &= ~0xFFFULL;
            if (k && ranges[k][0] >> COLD_UNIT_SHIFT == (ranges[k - 1][1] - 1) >> COLD_UNIT_SHIFT)
                ranges[k][0] = ((ranges[k][0] >> COLD_UNIT_SHIFT) + 1) << COLD_UNIT_SHIFT;
        }
        hl_mem_cold_pass();
        int i = 0;
        while (i < n && ranges[i][1] <= cursor) i++;
        if (i == n) i = 0, cursor = 0;
        int64_t scanned = 0;
        for (int visited = 0; n && visited <= n && scanned < COLD_SCAN_PAGES; visited++) {
            const uint64_t hi = ranges[i][1];
            uint64_t at = ranges[i][0] > cursor ? ranges[i][0] : cursor;
            while (at < hi && scanned < COLD_SCAN_PAGES) {
                uint64_t end = (at | 0xFFFFF) + 1;   /* (to the next 1 MB boundary: whole units in a slice) */
                if (end > hi) end = hi;
                cold_slice(at, end);
                scanned += (int64_t)((end - at) >> 12);
                at = end;
            }
            if (at < hi) { cursor = at; break; }   /* (the looks of this pass are spent inside this range) */
            if (++i == n) i = 0;
            cursor = ranges[i][0];
        }
        const double ms = (hos_now_ns() - t0) / 1e6;
        if (ms > worst_ms) worst_ms = ms;
        if (hos_now_ns() >= next_report) {
            char line[512];
            if (hl_mem_cold_report(line, sizeof line))
                hos_log("hl mem: cold pages: %s; a pass %.1f ms at most", line, worst_ms);
            worst_ms = 0;
            next_report = hos_now_ns() + 10000000000LL;
        }
    }
    return 0;
}

void hl_mem_init(void) {
    const char *e = getenv("HL_MEM_COLD");
    const int want = e && e[0] ? atoi(e) != 0 : HL_MEM_COLD_DEFAULT;
    if (!want) {
        hos_log("hl mem: cold pages: off (HL_MEM_COLD=0)");
        return;
    }
    extern int hos_large_pages;
    cold_query = (ColdQueryFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    const char *why = hos_large_pages ? "large pages" : hos_pool_trap_active() ? "HOS_POOL_TRAP measures" : !cold_query ? "no QueryWorkingSetEx" : NULL;
    if (why) {
        hos_log("hl mem: cold pages: off (%s)", why);
        return;
    }
    const char *fl = getenv("HL_MEM_COLD_FLAGS"), *age = getenv("HL_MEM_COLD_AGE");
    if (!fl || !*fl) fl = "24,64";
    for (const char *s = fl; *s && cold_nflags < 8;) {
        char *end;
        const unsigned long v = strtoul(s, &end, 16);
        if (end == s) break;
        cold_flags[cold_nflags++] = (uint32_t)v;
        s = *end == ',' ? end + 1 : end;
    }
    cold_age_ns = (int64_t)(age && *age ? atoi(age) : 10) * 1000000000LL;
    const hl_mem_cold_config config = {HOS_GUEST_SPACE >> COLD_UNIT_SHIFT, 1u << (COLD_UNIT_SHIFT - 12), 4096,
                                       COLD_BACKOFF_PASSES, COLD_GIVE_UP, COLD_BUDGET_PAGES};
    if (!cold_nflags || !hl_mem_cold_start(&config)) {
        hos_log("hl mem: cold pages: off (no flags, or the policy could not be made)");
        return;
    }
    cold_on = 1;
    hos_log("hl mem: cold pages: on: pages of pools with flags %s older than %lld s leave the working set (%d MB a pass at "
            "most, every %d ms; units that come back are left in 30 s, longer each time) (HL_MEM_COLD=0: not)",
            fl, (long long)(cold_age_ns / 1000000000LL), COLD_BUDGET_PAGES >> 8, COLD_PASS_MS);
    CloseHandle(CreateThread(NULL, 0, cold_main, NULL, 0, NULL));
}
#else
void hl_mem_pool_made(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void hl_mem_imported(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hl_mem_init(void) {}
#endif
