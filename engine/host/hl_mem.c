/* (HeyListen native engine) The OS layer's side of the memory module (engine/native, hl/c/hl_mem.h;
   docs/native-engine/memory-boundary.md, sections 8 and 11): cold pages of CPU-cached GPU pools and of the game's heaps
   leave the process's working set.

   The CPU side of the game's static GPU data (textures, vertex and index data in pools with flags 0x024, shader code in
   0x064: ~3.6 GB of RAM in every scene) stays in guest memory for the resources' lifetime, while the back end has it in
   VRAM. A page trap (section 7) saw game code touch under 1 MB of it in 35 s. Outside the pools, the game's heaps hold
   ~3.5 GB of RAM, nearly all of it resource files the game loaded and keeps (texture files 1.7 GB, models, effects, packs,
   sounds), and 3.2-3.3 GB of it was not touched in 20 s (section 10).
   A thread of its own takes such pages out of the working set, a bounded number a pass: VirtualUnlock on a range that is
   not locked removes its pages from the working set (documented behaviour; checked after each call with
   QueryWorkingSetEx). The pages stay committed and keep their bytes: they go to the standby list (written to the page file
   first when they are dirty), and any later touch, by the game, the back end or the kernel, gets the same bytes back with
   no handler of ours involved. So nothing can read a wrong byte; a page taken out too early costs a soft fault (a hard one
   if Windows had to give its RAM away meanwhile). RAM is freed; the commit charge is not (the pages are backed by the page
   file).
   The native policy (hl::mem::ColdPages, one for the pools and one for the heaps, each with its counters) remembers, per
   64 KB unit, the units that came back into the working set after being taken out: they were needed, and are left in for
   30 s, then 60, 120, and for good after the fourth time, so that pages someone reads all the time are not taken out again
   and again.
   - Pools: those with the chosen flags older than HL_MEM_COLD_AGE seconds (default 10: the back end uploads a new
     resource's data in its first frames).
   - Heaps: every committed page of the heap and of the address regions (where the game's heaps and the resource files'
     heaps live) that is not in a GPU pool (of any flags or age: the pools are the other policy's, or left alone on purpose),
     not in a guest thread's or fiber's stack (hos_guest_stacks). Thread-local blocks, the system region and the image are
     below the heap and never looked at. Memory being filled is left alone by the policy's settle rule: a unit goes only at
     a look 10 s or more after an earlier one, and only if it has not gained resident pages since. (The write-track marks
     cannot say "written lately" here: outside the GPU ranges nobody clears them, so they say "written once since the
     start", and clearing them from here would hide writes from the back end when a pool is made later over that memory: a
     new mirror takes a retired one's pages that its page generations call unchanged, section 11.) And the heaps go quiet
     while memory comes back fast (a scene loading over memory taken out): when 32 MB or more came back (pools and heaps)
     in the last second, no heap page is looked at or taken out for 5 s.
   Never taken out: ranges the back end imported (the driver pins them: hos_noaccess_import tells us before each import),
   anything while HOS_POOL_TRAP measures, the heaps while HOS_HEAP_SPLIT_COLD measures, and nothing with large pages
   (they cannot leave the working set).
     HL_MEM_COLD=1  on (the default)    HL_MEM_COLD=0  off: nothing here runs, as before
     HL_MEM_COLD_HEAPS=1  the heaps too (the default)    HL_MEM_COLD_HEAPS=0  the pools only
     HL_MEM_COLD_FLAGS=<hex,...>  the pool flags (default 24,64)    HL_MEM_COLD_AGE=<seconds>  the pools' age (default 10)
   Every 10 s two lines "hl mem: cold pages (pools): ..." and "(heaps): ..." give the counters (MB out now, taken out, came
   back and how often, the most that came back in 10 s). */
#include "hos.h"
#include "hl/c/hl_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_MEM_COLD_DEFAULT 1
#define HL_MEM_COLD_HEAPS_DEFAULT 1
#define COLD_UNIT_SHIFT 16                  /* (64 KB units: the commit-on-touch granule, 16 pages) */
#define COLD_UNIT_MASK 0xFFFFULL
#define COLD_PASS_MS 500
#define COLD_SCAN_PAGES (64 * 1024)         /* (256 MB looked at a pass: the ~3.6 GB in ~7 s) */
#define COLD_BUDGET_PAGES (16 * 1024)       /* (64 MB taken out a pass at most) */
#define COLD_BACKOFF_PASSES (30000 / COLD_PASS_MS)
#define COLD_GIVE_UP 4
#define COLD_HEAP_QUERIES 1024              /* (VirtualQuery calls a pass at most: the address regions are ~20,000 pieces,
                                               one a resource file, and a call costs tens of microseconds there) */
#define COLD_SETTLE_PASSES (10000 / COLD_PASS_MS)
#define COLD_QUIET_PAGES (8 * 1024)         /* (32 MB come back within the last second: a scene loads) */
#define COLD_QUIET_PASSES (5000 / COLD_PASS_MS)
#define COLD_MAX_EXCLUDED 20480

static volatile LONG cold_on;
static int heaps_on;
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

/* one slice [a, b) of guest pages inside a 1 MB block, whole units: look at each unit's resident pages, take out those
   the policy of `kind` says, and tell it what happened */
static void cold_slice(int kind, uint64_t a, uint64_t b) {
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
        if (hl_mem_cold_look(kind, unit, resident)) {
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
        hl_mem_cold_taken(kind, (a + ((uint64_t)k << 12)) >> COLD_UNIT_SHIFT, left, stayed);
    }
}

/* [a, b) in slices that end at 1 MB boundaries; returns the pages looked at */
static int64_t cold_range(int kind, uint64_t a, uint64_t b) {
    int64_t pages = 0;
    while (a < b) {
        uint64_t end = (a | 0xFFFFF) + 1;
        if (end > b) end = b;
        cold_slice(kind, a, end);
        pages += (int64_t)((end - a) >> 12);
        a = end;
    }
    return pages;
}

/* ---- the pools: those with the chosen flags made HL_MEM_COLD_AGE ago or more, from where the last pass stopped */
static void pools_pass(void) {
    static uint64_t ranges[16384][2];
    static uint64_t cursor;   /* (where the next pass goes on: the scan is spread over passes) */
    const int n = hos_pools_with_flags(cold_flags, cold_nflags, hos_now_ns() - cold_age_ns, ranges, 16384);
    /* (whole pages; a range that starts in the 64 KB unit the one before it ends in starts at the next unit, so that a
       unit is looked at once a pass, with all its eligible pages) */
    for (int k = 0; k < n; k++) {
        ranges[k][0] = (ranges[k][0] + 0xFFF) & ~0xFFFULL, ranges[k][1] &= ~0xFFFULL;
        if (k && ranges[k][0] >> COLD_UNIT_SHIFT == (ranges[k - 1][1] - 1) >> COLD_UNIT_SHIFT)
            ranges[k][0] = ((ranges[k][0] >> COLD_UNIT_SHIFT) + 1) << COLD_UNIT_SHIFT;
    }
    hl_mem_cold_pass(HL_MEM_COLD_POOLS);
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
            cold_slice(HL_MEM_COLD_POOLS, at, end);
            scanned += (int64_t)((end - at) >> 12);
            at = end;
        }
        if (at < hi) { cursor = at; break; }   /* (the looks of this pass are spent inside this range) */
        if (++i == n) i = 0;
        cursor = ranges[i][0];
    }
}

/* ---- the heaps: the committed pages of [HOS_HEAP_BASE, HOS_GUEST_SPACE) outside the pools and the stacks, from where the
   last pass stopped, in whole 64 KB units */
static uint64_t excluded[COLD_MAX_EXCLUDED][2];
static int by_lo(const void *a, const void *b) {
    const uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
static int heaps_excluded(void) {
    int n = hos_pools_with_flags(NULL, 0, INT64_MAX, excluded, COLD_MAX_EXCLUDED);
    n += hos_guest_stacks(excluded + n, COLD_MAX_EXCLUDED - n);
    /* (a unit with any page of a pool or a stack is not the heaps': out to whole units, sorted, merged) */
    for (int k = 0; k < n; k++)
        excluded[k][0] &= ~COLD_UNIT_MASK, excluded[k][1] = (excluded[k][1] + COLD_UNIT_MASK) & ~COLD_UNIT_MASK;
    qsort(excluded, (size_t)n, sizeof excluded[0], by_lo);
    int m = 0;
    for (int k = 0; k < n; k++) {
        if (m && excluded[k][0] <= excluded[m - 1][1]) {
            if (excluded[k][1] > excluded[m - 1][1]) excluded[m - 1][1] = excluded[k][1];
        } else excluded[m][0] = excluded[k][0], excluded[m][1] = excluded[k][1], m++;
    }
    return m;
}
static int64_t heap_sweeps;
static void heaps_pass(void) {
    static uint64_t at = HOS_HEAP_BASE;   /* (unit aligned) */
    const int ne = heaps_excluded();
    hl_mem_cold_pass(HL_MEM_COLD_HEAPS);
    int e = 0;
    int64_t scanned = 0;
    for (int queries = 0; scanned < COLD_SCAN_PAGES && queries < COLD_HEAP_QUERIES; queries++) {
        if (at >= HOS_GUEST_SPACE) {   /* (a sweep done: the next pass starts again at the bottom) */
            at = HOS_HEAP_BASE;
            heap_sweeps++;
            break;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(GP(at), &mbi, sizeof mbi)) { at = HOS_GUEST_SPACE; continue; }
        uint64_t end = (uint64_t)((uint8_t *)mbi.BaseAddress - (uint8_t *)GP(0)) + mbi.RegionSize;
        if (end > HOS_GUEST_SPACE) end = HOS_GUEST_SPACE;
        uint64_t lo = at;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            if (!(end & COLD_UNIT_MASK)) { at = end; continue; }
            lo = end & ~COLD_UNIT_MASK;   /* (the unit this one ends in may hold committed pages after it: looked at whole) */
        }
        uint64_t hi = (end + COLD_UNIT_MASK) & ~COLD_UNIT_MASK;
        const uint64_t room = (uint64_t)(COLD_SCAN_PAGES - scanned) << 12;
        if (hi - lo > room) hi = lo + room;   /* (room is whole units: the rest of the region next pass) */
        for (uint64_t a = lo; a < hi;) {
            while (e < ne && excluded[e][1] <= a) e++;
            if (e < ne && excluded[e][0] <= a) { a = excluded[e][1]; continue; }
            const uint64_t b = e < ne && excluded[e][0] < hi ? excluded[e][0] : hi;
            scanned += cold_range(HL_MEM_COLD_HEAPS, a, b);
            a = b;
        }
        at = hi;
    }
}

static DWORD WINAPI cold_main(void *unused) {
    (void)unused;
    double worst_ms[2] = {0, 0};
    int64_t next_report = hos_now_ns() + 10000000000LL;
    uint64_t back_before[2] = {0, 0};   /* (come-backs of every kind, one and two passes ago: the quiet rule) */
    int quiet_left = 0, quiet_passes = 0, quiet_passes_all = 0, passes = 0;
    for (;;) {
        Sleep(COLD_PASS_MS);
        if (hos_pool_trap_active()) continue;
        const int64_t t0 = hos_now_ns();
        pools_pass();
        const int64_t t1 = hos_now_ns();
        if (heaps_on) {
            const uint64_t back = hl_mem_cold_back_pages(HL_MEM_COLD_POOLS) + hl_mem_cold_back_pages(HL_MEM_COLD_HEAPS);
            if (++passes > 2 && back - back_before[1] >= COLD_QUIET_PAGES) quiet_left = COLD_QUIET_PASSES;
            back_before[1] = back_before[0], back_before[0] = back;
            if (quiet_left > 0) quiet_left--, quiet_passes++, hl_mem_cold_pass(HL_MEM_COLD_HEAPS);   /* (holds count on) */
            else heaps_pass();
        }
        const int64_t t2 = hos_now_ns();
        if ((t1 - t0) / 1e6 > worst_ms[0]) worst_ms[0] = (t1 - t0) / 1e6;
        if ((t2 - t1) / 1e6 > worst_ms[1]) worst_ms[1] = (t2 - t1) / 1e6;
        if (hos_now_ns() >= next_report) {
            char line[640];
            if (hl_mem_cold_report(HL_MEM_COLD_POOLS, line, sizeof line))
                hos_log("hl mem: cold pages (pools): %s; a pass %.1f ms at most", line, worst_ms[0]);
            if (heaps_on && hl_mem_cold_report(HL_MEM_COLD_HEAPS, line, sizeof line)) {
                quiet_passes_all += quiet_passes;
                hos_log("hl mem: cold pages (heaps): %s; quiet %.1f s since the last line (%.1f s in all), %lld sweeps; a "
                        "pass %.1f ms at most", line, quiet_passes * COLD_PASS_MS / 1000.0,
                        quiet_passes_all * COLD_PASS_MS / 1000.0, (long long)heap_sweeps, worst_ms[1]);
            }
            quiet_passes = 0;
            worst_ms[0] = worst_ms[1] = 0;
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
    hl_mem_cold_config config = {HOS_GUEST_SPACE >> COLD_UNIT_SHIFT, 1u << (COLD_UNIT_SHIFT - 12), 4096,
                                 COLD_BACKOFF_PASSES, COLD_GIVE_UP, COLD_BUDGET_PAGES, 0};
    if (!cold_nflags || !hl_mem_cold_start(HL_MEM_COLD_POOLS, &config)) {
        hos_log("hl mem: cold pages: off (no flags, or the policy could not be made)");
        return;
    }
    /* the heaps: not while hos_heapsplit.c measures their coldness itself (HOS_HEAP_SPLIT with HOS_HEAP_SPLIT_COLD) */
    const char *he = getenv("HL_MEM_COLD_HEAPS"), *hs = getenv("HOS_HEAP_SPLIT"), *hc = getenv("HOS_HEAP_SPLIT_COLD");
    const int want_heaps = he && he[0] ? atoi(he) != 0 : HL_MEM_COLD_HEAPS_DEFAULT;
    const int split_cold = hs && atoi(hs) > 0 && hc && atoi(hc) > 0;
    config.settle_passes = COLD_SETTLE_PASSES;
    heaps_on = want_heaps && !split_cold && hl_mem_cold_start(HL_MEM_COLD_HEAPS, &config);
    cold_on = 1;
    hos_log("hl mem: cold pages: on: pages of pools with flags %s older than %lld s leave the working set (%d MB a pass at "
            "most, every %d ms; units that come back are left in 30 s, longer each time) (HL_MEM_COLD=0: not)",
            fl, (long long)(cold_age_ns / 1000000000LL), COLD_BUDGET_PAGES >> 8, COLD_PASS_MS);
    if (heaps_on)
        hos_log("hl mem: cold pages: on for the heaps too: their committed pages outside pools and stacks, a unit seen still "
                "for %d s or more (%d MB a pass at most; none while 32 MB a second come back) (HL_MEM_COLD_HEAPS=0: not)",
                COLD_SETTLE_PASSES * COLD_PASS_MS / 1000, COLD_BUDGET_PAGES >> 8);
    else
        hos_log("hl mem: cold pages: not for the heaps (%s)", !want_heaps ? "HL_MEM_COLD_HEAPS=0"
                                                                 : split_cold ? "HOS_HEAP_SPLIT_COLD measures"
                                                                              : "the policy could not be made");
    CloseHandle(CreateThread(NULL, 0, cold_main, NULL, 0, NULL));
}
#else
void hl_mem_pool_made(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void hl_mem_imported(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hl_mem_init(void) {}
#endif
