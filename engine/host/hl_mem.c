// Copyright (C) 2026 Sacrappy

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
     new mirror takes a retired one's pages that its page generations call unchanged, section 12.) And the heaps go quiet
     while memory comes back fast (a scene loading over memory taken out): when 32 MB or more came back (pools and heaps)
     in the last second, no heap page is looked at or taken out for 5 s.
   Never taken out: ranges the back end imported (the driver pins them: hos_noaccess_import tells us before each import),
   anything while HOS_POOL_TRAP measures, the heaps while HOS_HEAP_SPLIT_COLD measures, and nothing with large pages
   (they cannot leave the working set).
     HL_MEM_COLD=1  on (the default)    HL_MEM_COLD=0  off: nothing here runs, as before
     HL_MEM_COLD_HEAPS=1  the heaps too (the default)    HL_MEM_COLD_HEAPS=0  the pools only
     HL_MEM_COLD_PRESSURE=0  without the back-off: a pass of 500 ms or more stops all trimming for 60 s, and the pools wait out
       the quiet rule too (the default: the system is paging then, and what comes back is read from the page file)
     HL_MEM_COLD_LOWMEM=<MB>  no trimming while the system's available RAM is under that (a quarter more to start again; default 0:
       never held, 3072 proposed). HL_MEM_ACCOUNT=<seconds>  the "hl mem: account:" lines (private commit and working set by owner)
     HL_MEM_COLD_FLAGS=<hex,...>  the pool flags (default 24,64)    HL_MEM_COLD_AGE=<seconds>  the pools' age (default 10)
     HL_MEM_COLD_LOW_MB=<MB> (default 4096) / HL_MEM_COLD_HIGH_MB=<MB> (default 6144)  the whole pass only while the available RAM
       is short: on under LOW, off again from HIGH (hl_mem_coldgate.h); LOW=0: whatever the free RAM, as before (STUT3)
   HL_FAULT_WATCH=<ms>: who takes the page faults, and on what memory (its comment, above hl_mem_init).
   Every 10 s two lines "hl mem: cold pages (pools): ..." and "(heaps): ..." give the counters (MB out now, taken out, came
   back and how often, the most that came back in 10 s). */
#include "hos.h"
#include "hl/c/hl_mem.h"
#include "hl_mem_coldgate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#define HL_MEM_COLD_DEFAULT 1
#define HL_MEM_COLD_HEAPS_DEFAULT 1
#define HL_MEM_COLD_LISTED_DEFAULT 1
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
static int heaps_on, heaps_listed_on;
static uint32_t cold_flags[8];
static int cold_nflags;
static int64_t cold_age_ns;

void hl_mem_pool_made(uint64_t addr, uint64_t size) {
    if (cold_on && size) hl_mem_cold_forget(addr >> COLD_UNIT_SHIFT, (addr + size + 0xFFFF) >> COLD_UNIT_SHIFT);
}
void hl_mem_imported(uint64_t lo, uint64_t hi) {
    if (cold_on && hi > lo) hl_mem_cold_imported(lo >> COLD_UNIT_SHIFT, (hi + 0xFFFF) >> COLD_UNIT_SHIFT);
}
/* (the back end gave its import of [lo, hi) back: the units wholly inside may go out of the working set again) */
void hl_mem_unimported(uint64_t lo, uint64_t hi) {
    if (cold_on && hi > lo) hl_mem_cold_unimported((lo + 0xFFFF) >> COLD_UNIT_SHIFT, hi >> COLD_UNIT_SHIFT);
}

typedef struct { void *va; ULONG_PTR attr; } ColdWs;   /* PSAPI_WORKING_SET_EX_INFORMATION (bit 0: valid) */
static int64_t cold_unlocks, cold_queries;   /* (VirtualUnlock and VirtualQuery calls: a slow pass's line) */
static int64_t cold_max_ws, cold_max_unlock, cold_max_query;   /* (the longest call of each kind in the pass, ns) */
static uint64_t cold_max_query_at, cold_max_query_size;        /* (and that VirtualQuery's address and region) */
typedef BOOL(WINAPI *ColdQueryFn)(HANDLE, PVOID, DWORD);
static ColdQueryFn cold_query;

/* one slice [a, b) of guest pages inside a 1 MB block, whole units: look at each unit's resident pages, take out those
   the policy of `kind` says, and tell it what happened */
static void cold_slice(int kind, uint64_t a, uint64_t b) {
    enum { MAXP = 256 };
    ColdWs info[MAXP], again[MAXP];
    const int m = (int)((b - a) >> 12);
    for (int k = 0; k < m; k++) info[k].va = GP(a + ((uint64_t)k << 12)), info[k].attr = 0;
    const int64_t q0 = hos_now_ns();
    const BOOL got = cold_query(GetCurrentProcess(), info, (DWORD)(m * sizeof(ColdWs)));
    if (hos_now_ns() - q0 > cold_max_ws) cold_max_ws = hos_now_ns() - q0;
    if (!got) return;
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
                const int64_t u0 = hos_now_ns();
                VirtualUnlock(info[j].va, (SIZE_T)(r - j) << 12);   /* (fails with ERROR_NOT_LOCKED, and trims: by design) */
                if (hos_now_ns() - u0 > cold_max_unlock) cold_max_unlock = hos_now_ns() - u0;
                cold_unlocks++;
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

/* (HL_MEM_COLD_LISTED=1, the default) The heaps' ranges from the OS layer's own books: the heap, then the address regions
   above it, ascending, adjacent ones merged; their pages are looked at with QueryWorkingSetEx alone, a page not committed
   reads as not resident. The walk before it found the committed pages with VirtualQuery, and one VirtualQuery over a large
   lazily committed range walks its page tables to find where the range ends, with the process's address space lock held:
   on the main PC, passes of 300-430 ms with ~205 queries, and in each of them a game thread committing memory
   (NtAllocateVirtualMemory: AllocateMemoryPages, a first touch) sat that long with its virtual core, and every thread
   waiting for that core ran without one after 250 ms: the 165-317 ms frames of steady play (temple of time, 3 runs of 3) */
#define COLD_MAX_LISTED 32768
#define COLD_LIST_SCAN_PAGES (128 * 1024)   /* (512 MB looked at a pass: pages not committed cost a look too) */
static uint64_t listed[COLD_MAX_LISTED][2];
static int heaps_listed(void) {
    int n = 0;
    if (hos_heap_size) listed[n][0] = HOS_HEAP_BASE, listed[n][1] = HOS_HEAP_BASE + hos_heap_size, n++;
    n += hos_regions_above(HOS_HEAP_BASE, listed + n, COLD_MAX_LISTED - n);
    int m = 0;   /* (whole units, merged) */
    for (int k = 0; k < n; k++) {
        const uint64_t lo = listed[k][0] & ~COLD_UNIT_MASK, hi = (listed[k][1] + COLD_UNIT_MASK) & ~COLD_UNIT_MASK;
        if (hi > HOS_GUEST_SPACE || hi <= lo) continue;
        if (m && lo <= listed[m - 1][1]) {
            if (hi > listed[m - 1][1]) listed[m - 1][1] = hi;
        } else listed[m][0] = lo, listed[m][1] = hi, m++;
    }
    return m;
}
static void heaps_pass_listed(void) {
    static uint64_t at = HOS_HEAP_BASE;   /* (unit aligned) */
    const int ne = heaps_excluded(), nl = heaps_listed();
    hl_mem_cold_pass(HL_MEM_COLD_HEAPS);
    int e = 0, l = 0;
    while (l < nl && listed[l][1] <= at) l++;
    int64_t scanned = 0;
    for (; scanned < COLD_LIST_SCAN_PAGES; l++) {
        if (l >= nl) {   /* (a sweep done: the next pass starts again at the bottom) */
            at = HOS_HEAP_BASE;
            heap_sweeps++;
            break;
        }
        const uint64_t lo = listed[l][0] > at ? listed[l][0] : at;
        uint64_t hi = listed[l][1];
        const uint64_t room = (uint64_t)(COLD_LIST_SCAN_PAGES - scanned) << 12;
        if (hi - lo > room) hi = lo + room;   /* (room is whole units: the rest of the range next pass) */
        for (uint64_t a = lo; a < hi;) {
            while (e < ne && excluded[e][1] <= a) e++;
            if (e < ne && excluded[e][0] <= a) { a = excluded[e][1]; continue; }
            const uint64_t b = e < ne && excluded[e][0] < hi ? excluded[e][0] : hi;
            scanned += cold_range(HL_MEM_COLD_HEAPS, a, b);
            a = b;
        }
        at = hi;
        if (hi < listed[l][1]) break;   /* (the pass's looks are spent inside this range) */
    }
}

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
        cold_queries++;
        const int64_t v0 = hos_now_ns();
        const SIZE_T vq = VirtualQuery(GP(at), &mbi, sizeof mbi);
        if (hos_now_ns() - v0 > cold_max_query)
            cold_max_query = hos_now_ns() - v0, cold_max_query_at = at, cold_max_query_size = vq ? mbi.RegionSize : 0;
        if (!vq) { at = HOS_GUEST_SPACE; continue; }
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

/* ---- HL_MEM_ACCOUNT=<seconds> (off by default): where the process's private commit and its working set are, a line
   "hl mem: account:" every <seconds> (MEM1, 22 Nov 2026: the owner's 15 minutes grew from 7.5 to 29.2 GB of private commit and
   frames of 0.4-1.2 s came with an OS trim). A sweep of the address space with VirtualQuery (the address space lock is held
   for each call, so a pass is for diagnosis only) sums the committed bytes of the guest space by part (the heap, the address
   regions, the part below the heap) and of the rest of the process by kind (private, mapped, image) and by the size of the
   allocation they are in; QueryWorkingSet says how many resident pages are the guest's (in a GPU pool, outside) and how many
   the host's. The system's side: available RAM, the commit charge against its limit, the pages the whole system read from
   disk since the last line (the hard faults), and the process's own page faults. */
typedef LONG(NTAPI *AcctNtQuerySys)(ULONG, PVOID, ULONG, PULONG);
typedef BOOL(WINAPI *AcctQueryWs)(HANDLE, PVOID, DWORD);
typedef struct { uint64_t base, reserved, committed; } AcctAlloc;
static int acct_by_committed(const void *a, const void *b) {
    const uint64_t x = ((const AcctAlloc *)a)->committed, y = ((const AcctAlloc *)b)->committed;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int acct_bucket(uint64_t r) {
    return r >= (1ULL << 30) ? 0 : r >= (256ULL << 20) ? 1 : r >= (64ULL << 20) ? 2 : r >= (16ULL << 20) ? 3 : r >= (2ULL << 20) ? 4 : 5;
}
static DWORD WINAPI account_main(void *arg) {
    const int secs = (int)(intptr_t)arg;
    AcctNtQuerySys nt_query = (AcctNtQuerySys)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    AcctQueryWs query_ws = (AcctQueryWs)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSet");
    uint32_t last_reads = 0, last_faults = 0, have_last = 0;
    const uint64_t g0 = (uint64_t)(uintptr_t)GP(0);
    const uint64_t heap_lo = g0 + HOS_HEAP_BASE;
    static uint64_t pools[COLD_MAX_EXCLUDED][2];
    static AcctAlloc allocs[1 << 16];
    for (;;) {
        Sleep((DWORD)secs * 1000);
        const int64_t t0 = hos_now_ns();
        const uint64_t heap_hi = heap_lo + hos_heap_size;
        uint64_t guest_heap = 0, guest_regions = 0, guest_low = 0, other_private = 0, mapped = 0, image = 0;
        uint64_t bucket[6] = {0, 0, 0, 0, 0, 0};   /* (>=1 GB, 256 MB-1 GB, 64-256 MB, 16-64 MB, 2-16 MB, <2 MB: by allocation size) */
        int nalloc = 0;
        AcctAlloc cur = {0, 0, 0};
        uint64_t at = 0;
        MEMORY_BASIC_INFORMATION mbi;
        while (VirtualQuery((void *)(uintptr_t)at, &mbi, sizeof mbi)) {
            const uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, sz = mbi.RegionSize;
            const uint64_t ab = (uint64_t)(uintptr_t)mbi.AllocationBase;
            const int in_guest = lo >= g0 && lo < g0 + HOS_GUEST_SPACE;
            if (mbi.State != MEM_FREE && !in_guest && mbi.Type == MEM_PRIVATE) {
                if (cur.base != ab) {
                    if (cur.base) {
                        bucket[acct_bucket(cur.reserved)] += cur.committed;
                        if (nalloc < (1 << 16)) allocs[nalloc++] = cur;
                    }
                    cur.base = ab, cur.reserved = 0, cur.committed = 0;
                }
                cur.reserved += sz;
            }
            if (mbi.State == MEM_COMMIT) {
                if (mbi.Type == MEM_PRIVATE) {
                    if (in_guest) {
                        if (lo >= heap_hi) guest_regions += sz;
                        else if (lo >= heap_lo) guest_heap += sz;
                        else guest_low += sz;
                    } else other_private += sz, cur.committed += sz;
                } else if (mbi.Type == MEM_MAPPED) mapped += sz;
                else image += sz;
            }
            at = lo + sz;
            if (at >= 0x7FFFFFFE0000ULL) break;
        }
        if (cur.base) {
            bucket[acct_bucket(cur.reserved)] += cur.committed;
            if (nalloc < (1 << 16)) allocs[nalloc++] = cur;
        }
        qsort(allocs, (size_t)nalloc, sizeof allocs[0], acct_by_committed);
        /* (the working set: every resident page, by where it is) */
        uint64_t ws_guest_pool = 0, ws_guest_other = 0, ws_host = 0, ws_shared = 0;
        if (query_ws) {
            DWORD cap = 64u << 20;
            ULONG_PTR *buf = (ULONG_PTR *)malloc(cap);
            while (buf && !query_ws(GetCurrentProcess(), buf, cap)) {
                if (GetLastError() != ERROR_BAD_LENGTH) { free(buf); buf = NULL; break; }
                cap = (DWORD)((buf[0] + 4096) * sizeof(ULONG_PTR) + sizeof(ULONG_PTR));
                free(buf);
                buf = (ULONG_PTR *)malloc(cap);
            }
            if (buf) {
                const int np = hos_pools_with_flags(NULL, 0, INT64_MAX, pools, COLD_MAX_EXCLUDED);
                qsort(pools, (size_t)np, sizeof pools[0], by_lo);
                int pi = 0;
                for (ULONG_PTR i = 0; i < buf[0]; i++) {
                    const ULONG_PTR e = buf[1 + i];
                    const uint64_t va = (uint64_t)(e & ~(ULONG_PTR)0xFFF);
                    if (e & 0x100) ws_shared += 4096;   /* (the Shared bit) */
                    if (va >= g0 && va < g0 + HOS_GUEST_SPACE) {
                        const uint64_t ga = va - g0;
                        while (pi < np && pools[pi][1] <= ga) pi++;
                        if (pi < np && pools[pi][0] <= ga) ws_guest_pool += 4096;
                        else ws_guest_other += 4096;
                    } else ws_host += 4096;
                }
                free(buf);
            }
        }
        PROCESS_MEMORY_COUNTERS_EX pm;
        memset(&pm, 0, sizeof pm);
        pm.cb = sizeof pm;
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        MEMORYSTATUSEX ms;
        memset(&ms, 0, sizeof ms);
        ms.dwLength = sizeof ms;
        GlobalMemoryStatusEx(&ms);
        uint32_t sys[128];
        memset(sys, 0, sizeof sys);
        ULONG got = 0;
        uint32_t reads = 0;
        if (nt_query && nt_query(2, sys, sizeof sys, &got) >= 0) reads = sys[20];   /* (SystemPerformanceInformation.PageReadCount) */
        const uint32_t d_reads = have_last ? reads - last_reads : 0, d_faults = have_last ? (uint32_t)pm.PageFaultCount - last_faults : 0;
        last_reads = reads, last_faults = (uint32_t)pm.PageFaultCount, have_last = 1;
        const unsigned long long MB = 1ull << 20;
        hos_log("hl mem: account: private %llu MB (queried commit %llu MB: guest heap %llu, guest regions %llu, guest low %llu, other "
                "%llu; mapped %llu, image %llu); other private by allocation size: >=1 GB %llu, 256 MB-1 GB %llu, 64-256 MB %llu, "
                "16-64 MB %llu, 2-16 MB %llu, <2 MB %llu (%d allocations)",
                (unsigned long long)pm.PrivateUsage / MB,
                (unsigned long long)(guest_heap + guest_regions + guest_low + other_private) / MB, guest_heap / MB, guest_regions / MB,
                guest_low / MB, other_private / MB, mapped / MB, image / MB, bucket[0] / MB, bucket[1] / MB, bucket[2] / MB,
                bucket[3] / MB, bucket[4] / MB, bucket[5] / MB, nalloc);
        hos_log("hl mem: account: working set %llu MB (guest pools %llu, guest outside pools %llu, host %llu; shared %llu); the system: "
                "%llu MB of %llu MB RAM free, commit %llu of %llu MB, %u pages read from disk since the last line, %u page faults "
                "of the process; the pass took %.0f ms",
                (unsigned long long)pm.WorkingSetSize / MB, ws_guest_pool / MB, ws_guest_other / MB, ws_host / MB, ws_shared / MB,
                (unsigned long long)ms.ullAvailPhys / MB, (unsigned long long)ms.ullTotalPhys / MB,
                (unsigned long long)(ms.ullTotalPageFile - ms.ullAvailPageFile) / MB, (unsigned long long)ms.ullTotalPageFile / MB,
                d_reads, d_faults, (hos_now_ns() - t0) / 1e6);
        {   /* (STALL2: who holds the rest of the PC's RAM: the five largest working sets of the other processes) */
            DWORD pids[2048], got_b = 0;
            if (K32EnumProcesses(pids, sizeof pids, &got_b)) {
                struct { uint64_t ws; char name[40]; } top[5];
                memset(top, 0, sizeof top);
                uint64_t others = 0;
                for (DWORD i = 0; i < got_b / sizeof(DWORD); i++) {
                    if (pids[i] == GetCurrentProcessId()) continue;
                    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
                    if (!ph) continue;
                    PROCESS_MEMORY_COUNTERS opm;
                    char nm[MAX_PATH] = "?";
                    DWORD nl = sizeof nm;
                    if (K32GetProcessMemoryInfo(ph, &opm, sizeof opm)) {
                        QueryFullProcessImageNameA(ph, 0, nm, &nl);
                        const char *b = strrchr(nm, 0x5c);
                        others += opm.WorkingSetSize;
                        int at = -1;
                        for (int k = 0; k < 5; k++)
                            if (opm.WorkingSetSize > top[k].ws && (at < 0 || top[k].ws < top[at].ws)) at = k;
                        if (at >= 0) top[at].ws = opm.WorkingSetSize, snprintf(top[at].name, sizeof top[at].name, "%s", b ? b + 1 : nm);
                    }
                    CloseHandle(ph);
                }
                hos_log("hl mem: account: other processes' working sets %llu MB in all (those this process may query); the largest: %s %llu, %s %llu, "
                        "%s %llu, %s %llu, %s %llu MB", (unsigned long long)(others / MB), top[0].name, (unsigned long long)(top[0].ws / MB), top[1].name,
                        (unsigned long long)(top[1].ws / MB), top[2].name, (unsigned long long)(top[2].ws / MB), top[3].name, (unsigned long long)(top[3].ws / MB),
                        top[4].name, (unsigned long long)(top[4].ws / MB));
            }
        }
        for (int k = 0; k < 6 && k < nalloc; k++) {
            char tagname[48] = "no tag";
            extern int hos_mem_tag_name(uint64_t addr, char *out, int n);
            hos_mem_tag_name(allocs[k].base, tagname, sizeof tagname);
            /* (FOOT2: how the allocation looks inside: its sub-regions by state, and where its resident pages are, in 8 slices of
               its reserved size, sampled every 16th page) */
            int nreg = 0, nres = 0;
            uint64_t comm_bytes = 0, res_bytes = 0, slice_res[8] = {0};
            char first[120] = "";
            if (allocs[k].reserved >= (128ull << 20)) {
                uint64_t q = allocs[k].base;
                const uint64_t end = allocs[k].base + allocs[k].reserved;
                MEMORY_BASIC_INFORMATION rb;
                while (q < end && VirtualQuery((void *)(uintptr_t)q, &rb, sizeof rb)) {
                    if (nreg < 4) {
                        const size_t at = strlen(first);
                        snprintf(first + at, sizeof first - at, " [+%llu MB %s %llu MB]", (unsigned long long)((q - allocs[k].base) / MB),
                                 rb.State == MEM_COMMIT ? "commit" : rb.State == MEM_RESERVE ? "reserve" : "free", (unsigned long long)rb.RegionSize / MB);
                    }
                    nreg++;
                    if (rb.State == MEM_COMMIT) comm_bytes += rb.RegionSize;
                    q += rb.RegionSize;
                }
                typedef BOOL(WINAPI * AcctQueryWsEx)(HANDLE, PVOID, DWORD);
                AcctQueryWsEx query_ex = (AcctQueryWsEx)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
                if (query_ex && allocs[k].reserved <= (4096ull << 20)) {
                    const uint64_t pages = allocs[k].reserved >> 12, n = pages / 16;
                    ULONG_PTR *w = (ULONG_PTR *)malloc((size_t)n * 2 * sizeof(ULONG_PTR));
                    if (w) {
                        for (uint64_t i = 0; i < n; i++) w[2 * i] = (ULONG_PTR)(allocs[k].base + (i << 16)), w[2 * i + 1] = 0;
                        if (query_ex(GetCurrentProcess(), w, (DWORD)(n * 2 * sizeof(ULONG_PTR))))
                            for (uint64_t i = 0; i < n; i++)
                                if (w[2 * i + 1] & 1) slice_res[i * 8 / n] += 16 * 4096, res_bytes += 16 * 4096;
                        free(w);
                    }
                }
                (void)nres;
            }
            hos_log("hl mem: account:   host allocation %#llx: %llu MB reserved, %llu MB committed; tag: %s; %d regions%s; resident (sampled) %llu MB in 8 slices: "
                    "%llu %llu %llu %llu %llu %llu %llu %llu",
                    (unsigned long long)allocs[k].base, (unsigned long long)allocs[k].reserved / MB, (unsigned long long)allocs[k].committed / MB,
                    tagname, nreg, first, (unsigned long long)res_bytes / MB, (unsigned long long)slice_res[0] / MB, (unsigned long long)slice_res[1] / MB,
                    (unsigned long long)slice_res[2] / MB, (unsigned long long)slice_res[3] / MB, (unsigned long long)slice_res[4] / MB,
                    (unsigned long long)slice_res[5] / MB, (unsigned long long)slice_res[6] / MB, (unsigned long long)slice_res[7] / MB);
        }
    }
    return 0;
}

static int pressure_on = 1;
static uint64_t lowmem_mb = 3072;   /* (HL_MEM_COLD_LOWMEM=<MB>: available RAM under which nothing is taken out; 0: never held. Default 3072 since STALL1, 22 Nov: the PC 3050's hog run with it had 56 frames over 50 ms against 77 with the pages taken out as before) */
static int lowmem_held;
/* (STUT3, hl_mem_coldgate.h) the pass only while the PC is short of RAM: HL_MEM_COLD_LOW_MB / HL_MEM_COLD_HIGH_MB */
static uint64_t gate_low_mb = HL_COLDGATE_LOW_DEFAULT_MB, gate_high_mb = HL_COLDGATE_HIGH_DEFAULT_MB;
static int gate_on = 0, gate_told = 0;
static int64_t lowmem_since, lowmem_ns, lowmem_passes;
static int64_t pressure_until;
static DWORD WINAPI cold_main(void *unused) {
    {
        const char *pe = getenv("HL_MEM_COLD_PRESSURE");
        pressure_on = pe && pe[0] ? atoi(pe) != 0 : 1;
    }
    (void)unused;
    {
        const char *lm = getenv("HL_MEM_COLD_LOWMEM");
        if (lm && lm[0]) lowmem_mb = (uint64_t)atoll(lm);
        /* (STUT3, HL_MEM_COLD_LOW_MB=<MB>, default 4096; HL_MEM_COLD_HIGH_MB=<MB>, default 6144) the pass, pools and heaps, runs only
           while the PC is short of RAM: it starts when the available RAM falls under the LOW line and stops once it is back at the
           HIGH line (never under LOW). HL_MEM_COLD_LOW_MB=0: whatever the free RAM, as before. On the PC 3050 (32 GB, 4.4 GB free at
           the least in play) the pass took 5-6 GB of pool pages and ~3 GB of heap pages out in a warp tour and 4-5 GB came back;
           with the pass off the frames over 100 ms fell by 31 percent in one series. A pool-only gate moved the churn to the heaps
           (6 GB out, 2.2 GB back): so the whole pass */
        const char *pl = getenv("HL_MEM_COLD_LOW_MB");
        gate_low_mb = pl ? (uint64_t)strtoull(pl, NULL, 10) : 4096;
        const char *ph = getenv("HL_MEM_COLD_HIGH_MB");
        gate_high_mb = ph ? (uint64_t)strtoull(ph, NULL, 10) : 6144;
    }
    double worst_ms[2] = {0, 0};
    int64_t next_report = hos_now_ns() + 10000000000LL;
    uint64_t back_before[2] = {0, 0};   /* (come-backs of every kind, one and two passes ago: the quiet rule) */
    int quiet_left = 0, quiet_passes = 0, quiet_passes_all = 0, passes = 0;
    for (;;) {
        Sleep(COLD_PASS_MS);
        if (hos_pool_trap_active()) continue;
        /* (STUT1, HL_MEM_COLD_PRESSURE=0: as before) A pass that took 500 ms or more, or memory coming back fast, stops the
           trimming for a while, the pools' too: the v0.4.2 log of issue 67 (16 GB, 10.3 GB used, an RX 580) took 8.7 GB of pool
           pages out and 6.8 GB came back, 113,504 units, and one working-set query took 6.8 s with game threads 2-11 s without a
           core: the system was paging, and each page taken out was one more to read back */
        if (pressure_on && hos_now_ns() < pressure_until) continue;
        /* (MEM1, HL_MEM_COLD_LOWMEM=<MB>) The system's free RAM is low: every page taken out now goes to the standby or the
           modified list of a machine that is about to reuse them, the dirty ones are written to the page file first, and what
           the game or the back end reads next comes back from the disk. Nothing is taken out until the free RAM is a quarter
           above the line again. */
        if (lowmem_mb) {
            MEMORYSTATUSEX ms;
            memset(&ms, 0, sizeof ms);
            ms.dwLength = sizeof ms;
            if (GlobalMemoryStatusEx(&ms)) {
                const uint64_t avail_mb = ms.ullAvailPhys >> 20;
                if (!lowmem_held && avail_mb < lowmem_mb) {
                    lowmem_held = 1, lowmem_since = hos_now_ns();
                    hos_log("hl mem: cold pages: held, %llu MB of RAM free (under %llu MB; HL_MEM_COLD_LOWMEM=0: not)", (unsigned long long)avail_mb, (unsigned long long)lowmem_mb);
                }
                else if (lowmem_held && avail_mb >= lowmem_mb + lowmem_mb / 4) {
                    lowmem_held = 0;
                    lowmem_ns += hos_now_ns() - lowmem_since;
                    hos_log("hl mem: cold pages: going on, %llu MB of RAM free; held %.1f s in all", (unsigned long long)avail_mb, lowmem_ns / 1e9);
                }
                if (lowmem_held) lowmem_passes++;
                {   /* (the performance summary's memory record, gpu/vk/perf_parts.h kOptColdHeld / kOptColdPasses: tenths of a second
                       the pages were held in all, and the passes left out; HL_TEL_MEMORY=0: not pushed) */
                    void rx_option_set(int id, int value);
                    static int push_on = -1;
                    if (push_on < 0) {
                        const char *e = getenv("HL_TEL_MEMORY");
                        push_on = !(e && e[0] == '0' && !e[1]);
                    }
                    if (push_on) {
                        const int64_t held_ns = lowmem_ns + (lowmem_held ? hos_now_ns() - lowmem_since : 0);
                        rx_option_set(98, (int)(held_ns / 100000000LL));
                        rx_option_set(99, (int)lowmem_passes);
                    }
                }
                if (lowmem_held) continue;
            }
        }
        {   /* (STUT3) the whole pass only while the PC is short of RAM (hl_mem_coldgate.h), its state logged at each change */
            MEMORYSTATUSEX gm;
            memset(&gm, 0, sizeof gm);
            gm.dwLength = sizeof gm;
            if (GlobalMemoryStatusEx(&gm)) {
                const uint64_t avail_mb = gm.ullAvailPhys >> 20;
                const int was = gate_on;
                gate_on = hl_coldgate_next(gate_on, avail_mb, gate_low_mb, gate_high_mb);
                if (gate_on != was || !gate_told) {
                    gate_told = 1;
                    if (!gate_low_mb)
                        hos_log("hl mem: cold pages: on whatever the free RAM (HL_MEM_COLD_LOW_MB=0)");
                    else if (gate_on)
                        hos_log("hl mem: cold pages: on, %llu MB of RAM available (under %llu MB, HL_MEM_COLD_LOW_MB; off again from %llu MB)",
                                (unsigned long long)avail_mb, (unsigned long long)gate_low_mb,
                                (unsigned long long)hl_coldgate_high(gate_low_mb, gate_high_mb));
                    else
                        hos_log("hl mem: cold pages: off, %llu MB of RAM available (pages are taken out only under %llu MB, HL_MEM_COLD_LOW_MB; "
                                "off from %llu MB, HL_MEM_COLD_HIGH_MB; 0: always)", (unsigned long long)avail_mb,
                                (unsigned long long)gate_low_mb, (unsigned long long)hl_coldgate_high(gate_low_mb, gate_high_mb));
                }
            } else {
                gate_on = 1;   /* (no answer: the pass as before) */
            }
            if (!gate_on) continue;   /* (RAM to spare: nothing is taken out, pools or heaps) */
        }
        const int64_t t0 = hos_now_ns();
        const int64_t unlocks0 = cold_unlocks, queries0 = cold_queries;
        cold_max_ws = cold_max_unlock = cold_max_query = 0;
        {
            const uint64_t back = hl_mem_cold_back_pages(HL_MEM_COLD_POOLS) + hl_mem_cold_back_pages(HL_MEM_COLD_HEAPS);
            if (++passes > 2 && back - back_before[1] >= COLD_QUIET_PAGES) quiet_left = COLD_QUIET_PASSES;
            back_before[1] = back_before[0], back_before[0] = back;
        }
        if (pressure_on && quiet_left > 0) hl_mem_cold_pass(HL_MEM_COLD_POOLS);   /* (the pools wait too, holds count on) */
        else pools_pass();
        const int64_t t1 = hos_now_ns(), unlocks1 = cold_unlocks;
        if (heaps_on) {
            if (quiet_left > 0) quiet_left--, quiet_passes++, hl_mem_cold_pass(HL_MEM_COLD_HEAPS);   /* (holds count on) */
            else if (heaps_listed_on) heaps_pass_listed();
            else heaps_pass();
        } else if (quiet_left > 0) quiet_left--;
        const int64_t t2 = hos_now_ns();
        if (pressure_on && t2 - t0 >= 500000000LL) {
            pressure_until = t2 + 60000000000LL;
            static int told;
            if (told++ < 8) hos_log("hl mem: cold pages: a pass took %.0f ms (working-set query %.0f ms): no trimming for 60 s (HL_MEM_COLD_PRESSURE=0: not)", (t2 - t0) / 1e6, cold_max_ws / 1e6);
        }
        /* (a pass of 100 ms or more, alone, with its calls: such passes came with the 250 ms frames of the v0.4.0
           benchmark, the game's Resource Memory thread holding its core meanwhile) */
        if (t2 - t0 >= 100000000LL)
            hos_log("hl mem: a slow pass: pools %.1f ms (%lld unlocks), heaps %.1f ms (%lld unlocks, %lld queries); the longest "
                    "calls: working-set query %.1f ms, unlock %.1f ms, VirtualQuery %.1f ms (at %#llx, a region of %llu MB)",
                    (t1 - t0) / 1e6, (long long)(unlocks1 - unlocks0), (t2 - t1) / 1e6,
                    (long long)(cold_unlocks - unlocks1), (long long)(cold_queries - queries0), cold_max_ws / 1e6,
                    cold_max_unlock / 1e6, cold_max_query / 1e6, (unsigned long long)cold_max_query_at,
                    (unsigned long long)(cold_max_query_size >> 20));
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
            if (lowmem_mb)
                hos_log("hl mem: cold pages: free RAM line %llu MB (HL_MEM_COLD_LOWMEM): %s now, %lld passes left out in all, held %.1f s in all",
                        (unsigned long long)lowmem_mb, lowmem_held ? "held" : "not held", (long long)lowmem_passes,
                        (lowmem_ns + (lowmem_held ? hos_now_ns() - lowmem_since : 0)) / 1e9);
            quiet_passes = 0;
            worst_ms[0] = worst_ms[1] = 0;
            next_report = hos_now_ns() + 10000000000LL;
        }
    }
    return 0;
}

/* ---- HL_FAULT_WATCH=<poll ms> (diagnostic, off by default; STALL2, 22 Nov 2026): who takes the page faults, and on what memory.
   The owner's 23 minutes had 31 million faults (123 GB of 4 KB pages for a working set of 8 GB) and 132 stutters of 50 ms or more
   that were page faults at 9 to 12 us each; nothing said which thread faulted or which memory (the stall watch has each thread's CPU,
   not its faults). InitializeProcessForWsWatch + GetWsChangesEx hand back, for every page that was added to the working set, the
   faulting instruction, the address and the thread. Each second with at least 2000 such pages, one line: the pages (and MB), by
   thread (the job trace's names: run with HL_STALL_WATCH), by memory (guest heap, address regions, below the heap, GPU pools; outside
   the guest window: the tagged ranges of HOS_HOST_SPLIT / RX_VK_LEDGER by name, private with no owner, mapped, images) and by the
   module of the faulting instruction, with the discards of freed guest pages of that second (hos_mem.c discard_pages: count, MB, ms).
   Every 30 s the cells (thread x memory) with the most pages. "ns" is hos_now_ns, the clock of present_trace.csv. Pages that were
   already in the working set (a touch of a resident page) are not faults and not listed. */
typedef struct { void *pc, *va; ULONG_PTR tid, flags; } FwEntry;
typedef BOOL(WINAPI *FwInitFn)(HANDLE);
typedef BOOL(WINAPI *FwGetFn)(HANDLE, FwEntry *, DWORD *);
enum { FW_MAX_THREADS = 256, FW_MAX_CLASSES = 40 };
static volatile LONG64 fw_disc_bytes, fw_disc_ns, fw_disc_n;
void hl_fw_note_discard(uint64_t bytes, int64_t ns) {
    InterlockedAdd64(&fw_disc_bytes, (LONG64)bytes);
    InterlockedAdd64(&fw_disc_ns, ns);
    InterlockedIncrement64(&fw_disc_n);
}
static struct { DWORD tid; char name[40]; } fw_th[FW_MAX_THREADS];
static int fw_nth;
static char fw_cls[FW_MAX_CLASSES][48];
static int fw_ncls;
static int fw_class(const char *name) {
    for (int i = 0; i < fw_ncls; i++)
        if (!strcmp(fw_cls[i], name)) return i;
    if (fw_ncls == FW_MAX_CLASSES) return 0;
    snprintf(fw_cls[fw_ncls], sizeof fw_cls[0], "%s", name);
    return fw_ncls++;
}
static int fw_thread(DWORD tid) {
    for (int i = 0; i < fw_nth; i++)
        if (fw_th[i].tid == tid) return i;
    if (fw_nth == FW_MAX_THREADS) return 0;
    fw_th[fw_nth].tid = tid;
    if (!hos_jt_name_of_tid(tid, fw_th[fw_nth].name, sizeof fw_th[0].name)) {
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        PWSTR d = NULL;
        if (h && SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0]) snprintf(fw_th[fw_nth].name, sizeof fw_th[0].name, "host:%ls", d);
        else snprintf(fw_th[fw_nth].name, sizeof fw_th[0].name, "tid %lu", (unsigned long)tid);
        if (d) LocalFree(d);
        if (h) CloseHandle(h);
    }
    return fw_nth++;
}
static uint64_t fw_cell_win[FW_MAX_THREADS][FW_MAX_CLASSES], fw_cell_30[FW_MAX_THREADS][FW_MAX_CLASSES];
static uint64_t fw_mod_win[FW_MAX_CLASSES];
static char fw_mod[FW_MAX_CLASSES][40];
static int fw_nmod;
static int fw_module(void *pc) {
    static uint64_t last_lo, last_hi;
    static int last_id;
    const uint64_t a = (uint64_t)(uintptr_t)pc;
    if (a >= last_lo && a < last_hi) return last_id;
    HMODULE m = NULL;
    char path[260], *base = "?";
    MEMORY_BASIC_INFORMATION mbi;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) &&
        GetModuleFileNameA(m, path, sizeof path)) {
        base = strrchr(path, '\\');
        base = base ? base + 1 : path;
    } else if (a >= (uint64_t)(uintptr_t)GP(0) && a < (uint64_t)(uintptr_t)GP(0) + HOS_GUEST_SPACE) base = "guest window";
    int id = -1;
    for (int i = 0; i < fw_nmod; i++)
        if (!strcmp(fw_mod[i], base)) id = i;
    if (id < 0 && fw_nmod < FW_MAX_CLASSES) snprintf(fw_mod[id = fw_nmod++], sizeof fw_mod[0], "%s", base);
    if (id < 0) id = 0;
    if (m && VirtualQuery(pc, &mbi, sizeof mbi)) last_lo = (uint64_t)(uintptr_t)mbi.BaseAddress, last_hi = last_lo + mbi.RegionSize, last_id = id;
    return id;
}
static int fw_cmp(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}
/* the k largest of a counter list, as "name n, name n" */
static void fw_top(char *out, size_t cap, const uint64_t *cnt, int n, const char *(*name)(int), int k) {
    uint64_t pr[FW_MAX_THREADS + FW_MAX_CLASSES][2];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (cnt[i]) pr[m][0] = cnt[i], pr[m][1] = (uint64_t)i, m++;
    qsort(pr, (size_t)m, sizeof pr[0], fw_cmp);
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < m && i < k && len + 90 < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, "%s%s %llu", i ? ", " : "", name((int)pr[i][1]), (unsigned long long)pr[i][0]);
}
static const char *fw_name_th(int i) { return fw_th[i].name; }
static const char *fw_name_cls(int i) { return fw_cls[i]; }
static const char *fw_name_mod(int i) { return fw_mod[i]; }
static DWORD WINAPI fault_main(void *arg) {
    const int poll_ms = (int)(intptr_t)arg;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FwInitFn init = (FwInitFn)(void *)GetProcAddress(k32, "K32InitializeProcessForWsWatch");
    FwGetFn get = (FwGetFn)(void *)GetProcAddress(k32, "K32GetWsChangesEx");
    if (!init || !get || !init(GetCurrentProcess())) {
        hos_log("hl fault: HL_FAULT_WATCH: no working-set watch here (error %lu): off", GetLastError());
        return 0;
    }
    hos_log("hl fault: on: every %d ms the pages added to the working set are read with the faulting thread and address; a line a second "
            "with 2000 pages or more, the cells of the last 30 s every 30 s (HL_FAULT_WATCH=0: off)", poll_ms);
    DWORD cap_entries = 1u << 18;
    FwEntry *buf = (FwEntry *)malloc((size_t)cap_entries * sizeof *buf);
    const uint64_t g0 = (uint64_t)(uintptr_t)GP(0), heap_lo = g0 + HOS_HEAP_BASE;
    static uint64_t pools[COLD_MAX_EXCLUDED][2];
    const int64_t t_start = hos_now_ns();
    int64_t next_line = t_start + 1000000000LL, next_30 = t_start + 30000000000LL, pools_at = 0;
    uint64_t total_win = 0, lost = 0, total_all = 0;
    LONG64 d_bytes0 = 0, d_ns0 = 0, d_n0 = 0;
    int npools = 0;
    uint64_t memo_lo = 0, memo_hi = 0;
    int memo_cls = -1;
    for (;;) {
        Sleep((DWORD)poll_ms);
        DWORD cb = cap_entries * (DWORD)sizeof *buf;
        memset(buf, 0, (size_t)cb);
        if (get(GetCurrentProcess(), buf, &cb)) {
            if (hos_now_ns() - pools_at > 2000000000LL) {   /* (the pools move rarely: looked at again every 2 s) */
                npools = hos_pools_with_flags(NULL, 0, INT64_MAX, pools, COLD_MAX_EXCLUDED);
                pools_at = hos_now_ns();
            }
            const uint64_t heap_hi = heap_lo + hos_heap_size;
            for (DWORD i = 0; i < cap_entries && buf[i].pc; i++) {
                const uint64_t va = (uint64_t)(uintptr_t)buf[i].va;
                int cls;
                if (memo_cls >= 0 && va >= memo_lo && va < memo_hi) cls = memo_cls;
                else {
                    char nm[48], t[48];
                    memo_lo = va & ~0xFFFFULL, memo_hi = memo_lo + 0x10000;
                    if (va >= g0 && va < g0 + HOS_GUEST_SPACE) {
                        const uint64_t ga = va - g0;
                        int pool = 0;
                        for (int p = 0; p < npools; p++)
                            if (pools[p][0] <= ga && ga < pools[p][1]) { pool = 1; break; }
                        snprintf(nm, sizeof nm, "%s", pool ? "guest: GPU pool" : va >= heap_hi ? "guest: address regions" : va >= heap_lo ? "guest: heap" : "guest: below the heap");
                    } else {
                        MEMORY_BASIC_INFORMATION mbi;
                        if (hos_mem_tag_name(va, t, sizeof t)) snprintf(nm, sizeof nm, "host tag: %s", t);
                        else if (VirtualQuery((void *)(uintptr_t)va, &mbi, sizeof mbi))
                            snprintf(nm, sizeof nm, "%s", mbi.Type == MEM_IMAGE ? "host: image" : mbi.Type == MEM_MAPPED ? "host: mapped" :
                                     mbi.RegionSize >= (64u << 20) ? "host: private, 64 MB+" : "host: private, no owner");
                        else snprintf(nm, sizeof nm, "host: unknown");
                    }
                    cls = fw_class(nm);
                    memo_cls = cls;
                }
                const int th = fw_thread((DWORD)buf[i].tid);
                fw_cell_win[th][cls]++;
                fw_cell_30[th][cls]++;
                fw_mod_win[fw_module(buf[i].pc)]++;
                total_win++;
                total_all++;
            }
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_INSUFFICIENT_BUFFER || err == ERROR_MORE_DATA) {   /* (the kernel keeps what did not fit: the next call is bigger) */
                lost++;
                if (cap_entries < (1u << 22)) {
                    cap_entries *= 2;
                    free(buf);
                    buf = (FwEntry *)malloc((size_t)cap_entries * sizeof *buf);
                }
            }
        }
        if (hos_now_ns() >= next_line) {
            next_line = hos_now_ns() + 1000000000LL;
            if (total_win >= 2000) {
                uint64_t by_th[FW_MAX_THREADS] = {0}, by_cls[FW_MAX_CLASSES] = {0};
                for (int t = 0; t < fw_nth; t++)
                    for (int c = 0; c < fw_ncls; c++) by_th[t] += fw_cell_win[t][c], by_cls[c] += fw_cell_win[t][c];
                char a[420], b[420], m[240];
                fw_top(a, sizeof a, by_th, fw_nth, fw_name_th, 6);
                fw_top(b, sizeof b, by_cls, fw_ncls, fw_name_cls, 6);
                fw_top(m, sizeof m, fw_mod_win, fw_nmod, fw_name_mod, 4);
                const LONG64 db = fw_disc_bytes, dn = fw_disc_ns, dc = fw_disc_n;
                hos_log("hl fault: t=%.0f s ns=%lld: %llu pages added to the working set (%llu MB); by thread: %s; by memory: %s; by code in: %s; "
                        "discards of freed guest pages this second: %lld (%lld MB, %.1f ms)",
                        (hos_now_ns() - t_start) / 1e9, (long long)hos_now_ns(), (unsigned long long)total_win, (unsigned long long)(total_win >> 8),
                        a, b, m, (long long)(dc - d_n0), (long long)((db - d_bytes0) >> 20), (dn - d_ns0) / 1e6);
            }
            d_bytes0 = fw_disc_bytes, d_ns0 = fw_disc_ns, d_n0 = fw_disc_n;
            memset(fw_cell_win, 0, sizeof fw_cell_win);
            memset(fw_mod_win, 0, sizeof fw_mod_win);
            total_win = 0;
        }
        if (hos_now_ns() >= next_30) {
            next_30 = hos_now_ns() + 30000000000LL;
            static uint64_t flat[FW_MAX_THREADS * FW_MAX_CLASSES][2];
            int n = 0;
            uint64_t sum = 0;
            for (int t = 0; t < fw_nth; t++)
                for (int c = 0; c < fw_ncls; c++)
                    if (fw_cell_30[t][c]) flat[n][0] = fw_cell_30[t][c], flat[n][1] = (uint64_t)(t * FW_MAX_CLASSES + c), n++, sum += fw_cell_30[t][c];
            qsort(flat, (size_t)n, sizeof flat[0], fw_cmp);
            char line[900];
            size_t len = 0;
            line[0] = 0;
            for (int i = 0; i < n && i < 10 && len + 130 < sizeof line; i++)
                len += (size_t)snprintf(line + len, sizeof line - len, "%s%s on %s %llu", i ? "; " : "", fw_th[flat[i][1] / FW_MAX_CLASSES].name,
                                        fw_cls[flat[i][1] % FW_MAX_CLASSES], (unsigned long long)flat[i][0]);
            hos_log("hl fault: 30 s: %llu pages (%llu MB), %llu in all so far, %llu polls found the kernel's buffer too small; the most: %s",
                    (unsigned long long)sum, (unsigned long long)(sum >> 8), (unsigned long long)total_all, (unsigned long long)lost, line);
            memset(fw_cell_30, 0, sizeof fw_cell_30);
        }
    }
    return 0;
}

/* ---- HL_MEM_TRIM_ARENAS=<seconds> (FOOT2, 23 Nov 2026; off by default): the graphics driver's big private arenas leave the working set.
   nvoglv64 keeps, from the start-up precompile on, 6 GB of private memory in five allocations (2 GB and four of 1 GB: the cache file's
   data it was given, 2.1 GB, and the 8,569 pipelines it built from it), written whole in the first 40 s and not read again by anyone
   (the GPU runs from VRAM). Left alone they sit in the working set until Windows trims them under pressure (5.8 GB resident at 41 s,
   1.1 GB at 565 s in the 12-minute tour), taking the RAM other pages needed meanwhile. Every <seconds> a pass looks at the private
   allocations outside the guest window of 512 MB or more (reserved) that nobody tagged, and takes their committed pages out of the
   working set with VirtualUnlock on a range that is not locked (as the cold pass does); they stay committed and keep their bytes, and
   a later touch is a soft fault (a hard one if Windows needed the RAM). */
static DWORD WINAPI arenas_main(void *arg) {
    const int secs = (int)(intptr_t)arg;
    const uint64_t g0 = (uint64_t)(uintptr_t)GP(0);
    for (;;) {
        Sleep((DWORD)secs * 1000);
        MEMORY_BASIC_INFORMATION mbi;
        struct { uint64_t base, reserved; } cur = {0, 0};
        static uint64_t starts[64], sizes[64];
        int n = 0;
        uint64_t at = 0, trimmed = 0;
        /* (pass 1: the allocations of 512 MB and more; pass 2 trims their committed runs) */
        while (VirtualQuery((void *)(uintptr_t)at, &mbi, sizeof mbi)) {
            const uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, sz = mbi.RegionSize, ab = (uint64_t)(uintptr_t)mbi.AllocationBase;
            if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE && !(lo >= g0 && lo < g0 + HOS_GUEST_SPACE)) {
                if (cur.base != ab) {
                    if (cur.base && cur.reserved >= (512ULL << 20) && n < 64) starts[n] = cur.base, sizes[n] = cur.reserved, n++;
                    cur.base = ab, cur.reserved = 0;
                }
                cur.reserved += sz;
            }
            at = lo + sz;
            if (at >= 0x7FFFFFFE0000ULL) break;
        }
        if (cur.base && cur.reserved >= (512ULL << 20) && n < 64) starts[n] = cur.base, sizes[n] = cur.reserved, n++;
        for (int k = 0; k < n; k++) {
            char nm[48];
            if (hos_mem_tag_name(starts[k], nm, sizeof nm)) continue;   /* (ours: the table's own owner decides) */
            uint64_t a = starts[k];
            while (a < starts[k] + sizes[k] && VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) {
                const uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, sz = mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE))
                    for (uint64_t o = 0; o < sz; o += (16ULL << 20)) {
                        const uint64_t len = sz - o < (16ULL << 20) ? sz - o : (16ULL << 20);
                        VirtualUnlock((void *)(uintptr_t)(lo + o), (SIZE_T)len);
                        trimmed += len;
                    }
                a = lo + sz;
            }
        }
        hos_log("hl mem: arenas: %d private allocations of 512 MB and more looked at, %llu MB of committed pages taken out of the working set", n,
                (unsigned long long)(trimmed >> 20));
    }
    return 0;
}

void hl_mem_init(void) {
    {
        const char *ta = getenv("HL_MEM_TRIM_ARENAS");
        if (ta && atoi(ta) > 0) CloseHandle(CreateThread(NULL, 0, arenas_main, (void *)(intptr_t)atoi(ta), 0, NULL));
        const char *ac = getenv("HL_MEM_ACCOUNT");
        if (ac && atoi(ac) > 0) CloseHandle(CreateThread(NULL, 0, account_main, (void *)(intptr_t)atoi(ac), 0, NULL));
    }
    {
        const char *fw = getenv("HL_FAULT_WATCH");
        if (fw && atoi(fw) > 0) CloseHandle(CreateThread(NULL, 0, fault_main, (void *)(intptr_t)atoi(fw), 0, NULL));
    }
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
    const char *li = getenv("HL_MEM_COLD_LISTED");
    heaps_listed_on = li && li[0] ? atoi(li) != 0 : HL_MEM_COLD_LISTED_DEFAULT;
    cold_on = 1;
    hos_log("hl mem: cold pages: on: pages of pools with flags %s older than %lld s leave the working set (%d MB a pass at "
            "most, every %d ms; units that come back are left in 30 s, longer each time) (HL_MEM_COLD=0: not)",
            fl, (long long)(cold_age_ns / 1000000000LL), COLD_BUDGET_PAGES >> 8, COLD_PASS_MS);
    if (heaps_on)
        hos_log("hl mem: cold pages: on for the heaps too: their committed pages outside pools and stacks, a unit seen still "
                "for %d s or more (%d MB a pass at most; none while 32 MB a second come back) (HL_MEM_COLD_HEAPS=0: not); their "
                "ranges %s",
                COLD_SETTLE_PASSES * COLD_PASS_MS / 1000, COLD_BUDGET_PAGES >> 8,
                heaps_listed_on ? "from the heap and the address regions, no VirtualQuery (HL_MEM_COLD_LISTED=0: found with it)"
                                : "found with VirtualQuery (HL_MEM_COLD_LISTED=0)");
    else
        hos_log("hl mem: cold pages: not for the heaps (%s)", !want_heaps ? "HL_MEM_COLD_HEAPS=0"
                                                                 : split_cold ? "HOS_HEAP_SPLIT_COLD measures"
                                                                              : "the policy could not be made");
    CloseHandle(CreateThread(NULL, 0, cold_main, NULL, 0, NULL));
}
#else
void hl_mem_pool_made(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void hl_mem_imported(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hl_mem_unimported(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hl_mem_init(void) {}
void hl_fw_note_discard(uint64_t bytes, int64_t ns) { (void)bytes; (void)ns; }
#endif
