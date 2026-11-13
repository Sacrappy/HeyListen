/* (HeyListen) The guest window's substrate: 64 KB granules given back to Windows and restored atomically
   (hos_guestpages.h; docs/native-engine/memory-boundary.md section 13).

   Why: a page decommitted from plain private memory can only be made again by committing it and copying its bytes in,
   and a committed page is visible to every thread at once, as zeros, before the copy ends. A section view is not: its
   pages are filled through a second view nobody else knows, then the view is mapped at the guest address in one call.

   How (measured on the main PC, Windows 11 26200, game-free, before this was written):
   - [HOS_HEAP_BASE, HOS_GUEST_SPACE) (448 GB) is reserved as one placeholder (VirtualAlloc2 MEM_RESERVE_PLACEHOLDER)
     and replaced at once by one private reservation over it (MEM_REPLACE_PLACEHOLDER): commit, decommit, MEM_RESET,
     VirtualProtect, VirtualUnlock, ReadFile into it and commit on first touch all work as on an ordinary reservation.
   - A drop releases the granule back to a placeholder (VirtualFree MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER): Windows
     allows that for any 64 KB-aligned piece of a private allocation made over a placeholder, which splits the
     allocation in two around the hole and keeps the bytes and the commit state of both halves (2.5 us a granule). The
     placeholder has no RAM and no commit charge; any touch of it raises an access violation.
   - A restore creates a page-file-backed section (SEC_COMMIT) the size of the range, maps it privately, has `fill`
     write the bytes there, then maps one view of it per dropped granule at the guest address
     (MapViewOfFile3 MEM_REPLACE_PLACEHOLDER). One section per 64 KB granule cost 36 us a granule, one 8 MB section for
     128 granules 25 us a granule (both with the 64 KB copy). A view cannot be unmapped in part (UnmapViewOfFile2 on
     its second half: ERROR_INVALID_ADDRESS), hence a view per granule; dropping a view again is UnmapViewOfFile2 with
     MEM_PRESERVE_PLACEHOLDER (5 us). The section's commit charge goes when its last view does.
   - What a split reservation changes: one VirtualAlloc(MEM_COMMIT), VirtualFree(MEM_DECOMMIT), MEM_RESET or
     VirtualProtect cannot cross from one piece to the next (ERROR_INVALID_ADDRESS / ERROR_INVALID_PARAMETER);
     VirtualUnlock, QueryWorkingSetEx and the kernel (ReadFile across two views) can. So the OS layer's commits,
     decommits and resets over a range that holds a dropped granule or one apart from the reservation (a view, or a
     forgotten granule: its own 64 KB reservation) go through hos_gp_commit / hos_gp_decommit / hos_gp_reset, which
     work piece by piece; ranges without such granules take the calls of before.

   State: three bitmaps of one bit a granule (dropped, apart, imported) and one of granules being worked on (busy). A
   drop, a restore, a forget or an import claims its granules (waits while any is busy, under one lock and a condition
   variable), does its system calls without the lock, and lets them go. A thread that touches a dropped granule goes to
   the owner, whose restore waits for whatever has the granule; when it gets it, the granule is no longer dropped and
   the restore returns at once. */
#include "hos_guestpages.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hos_log(const char *fmt, ...);

int hos_gp_wanted(void) {
    static int want = -1;
    if (want < 0) {
        const char *e = getenv("HOS_GUEST_PAGES");
        want = e && e[0] == '1';
    }
    return want;
}

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GP_GRAN 0x10000ULL
#define GP_SHIFT 16

typedef PVOID(WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL(WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn va2;
static MapViewOfFile3Fn mv3;
static UnmapViewOfFile2Fn uv2;

static uint8_t *gp_base;
static uint64_t gp_count;   /* (granules) */
static volatile LONG gp_on;
static volatile LONG64 *bm_dropped, *bm_apart, *bm_imported, *bm_busy;
static SRWLOCK gp_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE gp_freed = CONDITION_VARIABLE_INIT;
static hos_gp_on_touch volatile gp_owner;
static _Thread_local int gp_path = HOS_GP_PATH_DIRECT;

static volatile LONG64 n_drops, n_restores[HOS_GP_PATHS], n_restore_calls, n_fill_failed, n_map_failed, n_forgets,
    n_declined, n_refused, n_refused_imported, n_drop_failed, n_waits;
static hos_gp_on_forget volatile gp_forget;
/* (splits of the reservation, exclusive: while Windows splits a private allocation around a granule released to a
   placeholder, a VirtualAlloc(MEM_COMMIT) of a reserved page elsewhere in that allocation fails with ERROR_INVALID_ADDRESS:
   84,561 of 479,668 commits in a game-free test with 65,536 splits going on; first touches of committed pages are not
   affected (0 faults). The OS layer's commits and decommits hold it shared: hos_gp_hold) */
static SRWLOCK split_lock = SRWLOCK_INIT;

static inline int bit(volatile LONG64 *bm, uint64_t i) { return (int)((bm[i >> 6] >> (i & 63)) & 1); }
static inline void bit_set(volatile LONG64 *bm, uint64_t i) { InterlockedOr64(&bm[i >> 6], (LONG64)(1ULL << (i & 63))); }
static inline void bit_clear(volatile LONG64 *bm, uint64_t i) { InterlockedAnd64(&bm[i >> 6], ~(LONG64)(1ULL << (i & 63))); }
static int any_bit(volatile LONG64 *bm, uint64_t a, uint64_t b) {
    while (a < b) {
        if (!(a & 63) && b - a >= 64) {
            if (bm[a >> 6]) return 1;
            a += 64;
        } else {
            if (bit(bm, a)) return 1;
            a++;
        }
    }
    return 0;
}
static uint8_t *at(uint64_t i) { return gp_base + (i << GP_SHIFT); }

/* the granules [*a, *b) that [p, p + n) touches, clamped to the substrate's part of the window; 0 when none */
static int span(const void *p, size_t n, uint64_t *a, uint64_t *b) {
    if (!gp_on || !n) return 0;
    const uint8_t *lo = (const uint8_t *)p, *hi = lo + n, *end = gp_base + (gp_count << GP_SHIFT);
    if (hi <= gp_base || lo >= end) return 0;
    if (lo < gp_base) lo = gp_base;
    if (hi > end) hi = end;
    *a = (uint64_t)(lo - gp_base) >> GP_SHIFT;
    *b = ((uint64_t)(hi - gp_base) + GP_GRAN - 1) >> GP_SHIFT;
    return 1;
}

static void claim(uint64_t a, uint64_t b) {
    AcquireSRWLockExclusive(&gp_lock);
    while (any_bit(bm_busy, a, b)) {
        InterlockedIncrement64(&n_waits);
        SleepConditionVariableSRW(&gp_freed, &gp_lock, INFINITE, 0);
    }
    for (uint64_t i = a; i < b; i++) bit_set(bm_busy, i);
    ReleaseSRWLockExclusive(&gp_lock);
}
static void unclaim(uint64_t a, uint64_t b) {
    AcquireSRWLockExclusive(&gp_lock);
    for (uint64_t i = a; i < b; i++) bit_clear(bm_busy, i);
    ReleaseSRWLockExclusive(&gp_lock);
    WakeAllConditionVariable(&gp_freed);
}

size_t hos_gp_granule(void) { return gp_on ? (size_t)GP_GRAN : 0; }
int hos_gp_ready(void) { return gp_on; }
int hos_gp_is_dropped(const void *p) {
    if (!gp_on) return 0;
    const uint8_t *q = (const uint8_t *)p;
    if (q < gp_base || q >= gp_base + (gp_count << GP_SHIFT)) return 0;
    return bit(bm_dropped, (uint64_t)(q - gp_base) >> GP_SHIFT);
}
/* (exported: the Vulkan back end finds it by name, gpu/vk/vk_memory.cpp memory_all_reserved: a dropped granule is
   reserved to VirtualQuery, but its bytes are not zeros) */
__declspec(dllexport) int hos_gp_dropped_in(const void *p, size_t n) {
    uint64_t a, b;
    return span(p, n, &a, &b) && any_bit(bm_dropped, a, b);
}
void hos_gp_set_owner(hos_gp_on_touch cb) { gp_owner = cb; }
void hos_gp_set_on_forget(hos_gp_on_forget cb) { gp_forget = cb; }

int hos_gp_reserve(void *base, size_t size) {
    if (gp_on || !base || !size || ((uintptr_t)base | size) & (GP_GRAN - 1)) return 0;
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    va2 = kb ? (VirtualAlloc2Fn)(void *)GetProcAddress(kb, "VirtualAlloc2") : NULL;
    mv3 = kb ? (MapViewOfFile3Fn)(void *)GetProcAddress(kb, "MapViewOfFile3") : NULL;
    uv2 = kb ? (UnmapViewOfFile2Fn)(void *)GetProcAddress(kb, "UnmapViewOfFile2") : NULL;
    if (!va2 || !mv3 || !uv2) {
        hos_log("mem: guest pages: this Windows has no placeholders (VirtualAlloc2, MapViewOfFile3: Windows 10 1803 or later)");
        return 0;
    }
    const uint64_t count = size >> GP_SHIFT, words = (count + 63) / 64;
    HANDLE self = GetCurrentProcess();
    if (va2(self, base, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0) != base) {
        hos_log("mem: guest pages: the placeholder at %p was not made (error %lu)", base, GetLastError());
        return 0;
    }
    if (va2(self, base, size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != base) {
        hos_log("mem: guest pages: the reservation over the placeholder was not made (error %lu)", GetLastError());
        VirtualFree(base, 0, MEM_RELEASE);
        return 0;
    }
    /* (the bitmaps after the window: made first, they could take its place) */
    volatile LONG64 *maps = (volatile LONG64 *)VirtualAlloc(NULL, (SIZE_T)(words * 8 * 4), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!maps) {
        VirtualFree(base, 0, MEM_RELEASE);
        return 0;
    }
    bm_dropped = maps, bm_apart = maps + words, bm_imported = maps + 2 * words, bm_busy = maps + 3 * words;
    gp_base = (uint8_t *)base, gp_count = count;
    InterlockedExchange(&gp_on, 1);
    return 1;
}

int hos_gp_drop(void *p, size_t size) {
    uint64_t a, b;
    if (!gp_on || !size || ((uintptr_t)p | size) & (GP_GRAN - 1) || (uint8_t *)p < gp_base ||
        (uint8_t *)p + size > gp_base + (gp_count << GP_SHIFT) || !span(p, size, &a, &b)) {
        InterlockedIncrement64(&n_refused);
        return -1;
    }
    claim(a, b);
    if (any_bit(bm_imported, a, b)) {
        unclaim(a, b);
        InterlockedIncrement64(&n_refused_imported);
        return -2;
    }
    HANDLE self = GetCurrentProcess();
    int rc = 0;
    for (uint64_t i = a; i < b; i++) {
        if (bit(bm_dropped, i)) continue;
        /* (the bit first: a thread that faults on the placeholder from the call on finds it) */
        bit_set(bm_dropped, i);
        MEMORY_BASIC_INFORMATION m;
        const int view = VirtualQuery(at(i), &m, sizeof m) && m.Type == MEM_MAPPED;
        BOOL ok;
        if (view) ok = uv2(self, at(i), MEM_PRESERVE_PLACEHOLDER);   /* (a view of its own: nothing else is split) */
        else {
            AcquireSRWLockExclusive(&split_lock);
            ok = VirtualFree(at(i), (SIZE_T)GP_GRAN, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            ReleaseSRWLockExclusive(&split_lock);
        }
        if (!ok) {
            bit_clear(bm_dropped, i);
            if (InterlockedIncrement64(&n_drop_failed) <= 8)
                hos_log("mem: guest pages: the granule at %p was not dropped (%s, error %lu)", at(i), view ? "a view" : "private",
                        GetLastError());
            rc = -1;
            continue;
        }
        bit_set(bm_apart, i);   /* (a placeholder, then a view or a reservation of its own: never the big one again) */
        InterlockedIncrement64(&n_drops);
    }
    unclaim(a, b);
    return rc;
}

int hos_gp_restore(void *p, size_t size, hos_gp_fill fill, void *user) {
    uint64_t a, b;
    if (!gp_on || !size || !fill || ((uintptr_t)p | size) & (GP_GRAN - 1) || (uint8_t *)p < gp_base ||
        (uint8_t *)p + size > gp_base + (gp_count << GP_SHIFT) || !span(p, size, &a, &b))
        return -1;
    const int path = gp_path;
    claim(a, b);
    if (!any_bit(bm_dropped, a, b)) {   /* (restored meanwhile by another thread, or never dropped) */
        unclaim(a, b);
        return 0;
    }
    InterlockedIncrement64(&n_restore_calls);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, (DWORD)((uint64_t)size >> 32),
                                        (DWORD)size, NULL);
    void *buf = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size) : NULL;
    if (!buf) {
        if (InterlockedIncrement64(&n_map_failed) <= 8)
            hos_log("mem: guest pages: no section of %zu bytes for a restore (error %lu)", size, GetLastError());
        if (section) CloseHandle(section);
        unclaim(a, b);
        return -1;
    }
    const int r = fill(user, buf, p, size);
    UnmapViewOfFile(buf);
    if (r) {
        InterlockedIncrement64(&n_fill_failed);
        CloseHandle(section);
        unclaim(a, b);
        return r;
    }
    HANDLE self = GetCurrentProcess();
    int rc = 0;
    for (uint64_t i = a; i < b; i++) {
        if (!bit(bm_dropped, i)) continue;
        if (mv3(section, self, at(i), (i - a) << GP_SHIFT, (SIZE_T)GP_GRAN, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != at(i)) {
            if (InterlockedIncrement64(&n_map_failed) <= 8)
                hos_log("mem: guest pages: the granule at %p was not mapped back (error %lu)", at(i), GetLastError());
            rc = -1;
            continue;
        }
        bit_clear(bm_dropped, i);
        InterlockedIncrement64(&n_restores[path]);
    }
    CloseHandle(section);   /* (the views keep it) */
    unclaim(a, b);
    return rc;
}

/* the owner, for the dropped granules of [a, b) in runs; 1 when all came back */
static int ask_owner(uint64_t a, uint64_t b, int write, int path) {
    int all = 1;
    for (uint64_t i = a; i < b;) {
        if (!bit(bm_dropped, i)) { i++; continue; }
        uint64_t e = i + 1;
        while (e < b && bit(bm_dropped, e)) e++;
        const hos_gp_on_touch cb = gp_owner;
        const int saved = gp_path;
        gp_path = path;
        const int took = cb && cb(at(i), (size_t)((e - i) << GP_SHIFT), write);
        gp_path = saved;
        if (!took) {
            if (InterlockedIncrement64(&n_declined) <= 8)
                hos_log("mem: guest pages: %llu dropped granule(s) at %p %s", (unsigned long long)(e - i), at(i),
                        cb ? "declined by their owner" : "with no owner");
            all = 0;
        }
        i = e;
    }
    return all;
}

int hos_gp_fault(void *p, int write) {
    uint64_t a, b;
    if (!span(p, 1, &a, &b) || !bit(bm_dropped, a)) return 0;
    /* (twice at most: a granule dropped again between the owner's restore and the instruction run again is asked for
       again; one still dropped after that is a failure) */
    for (int tries = 0; tries < 2; tries++) {
        if (!ask_owner(a, b, write, HOS_GP_PATH_HANDLER)) return -1;
        if (!bit(bm_dropped, a)) return 1;
    }
    return -1;
}

void hos_gp_prepare(void *p, size_t n, int path) {
    uint64_t a, b;
    if (span(p, n, &a, &b) && any_bit(bm_dropped, a, b)) ask_owner(a, b, 1, path);
}

int hos_gp_touches(const void *p, size_t n) {
    uint64_t a, b;
    return span(p, n, &a, &b) && (any_bit(bm_dropped, a, b) || any_bit(bm_apart, a, b));
}

/* each piece of [lo, hi) with one state inside one allocation */
typedef int (*PieceFn)(uint8_t *lo, uint8_t *hi, const MEMORY_BASIC_INFORMATION *m);
static int each_piece(uint8_t *lo, uint8_t *hi, PieceFn fn) {
    int ok = 1;
    while (lo < hi) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(lo, &m, sizeof m)) return 0;
        uint8_t *end = (uint8_t *)m.BaseAddress + m.RegionSize;
        if (end > hi) end = hi;
        ok &= fn(lo, end, &m);
        lo = end;
    }
    return ok;
}
static int is_placeholder(const uint8_t *q) { return hos_gp_is_dropped(q); }
static int commit_piece(uint8_t *lo, uint8_t *hi, const MEMORY_BASIC_INFORMATION *m) {
    if (m->State != MEM_RESERVE) return 1;
    if (is_placeholder(lo)) return 0;   /* (the owner did not restore it) */
    return VirtualAlloc(lo, (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE) != NULL;
}
static int decommit_piece(uint8_t *lo, uint8_t *hi, const MEMORY_BASIC_INFORMATION *m) {
    if (m->State != MEM_COMMIT) return 1;
    if (m->Type == MEM_MAPPED) {   /* (a part of a restored granule: its pages cannot be decommitted; zero is what they read) */
        memset(lo, 0, (size_t)(hi - lo));
        return 1;
    }
    return VirtualFree(lo, (SIZE_T)(hi - lo), MEM_DECOMMIT);
}
static int reset_piece(uint8_t *lo, uint8_t *hi, const MEMORY_BASIC_INFORMATION *m) {
    if (m->State != MEM_COMMIT) return 1;
    return VirtualAlloc(lo, (SIZE_T)(hi - lo), MEM_RESET, PAGE_READWRITE) != NULL;
}

void hos_gp_hold(int begin) {
    if (!gp_on) return;
    if (begin) AcquireSRWLockShared(&split_lock);
    else ReleaseSRWLockShared(&split_lock);
}

int hos_gp_commit(void *p, size_t n) {
    uint8_t *lo = (uint8_t *)p, *hi = lo + n;
    hos_gp_prepare(p, n, HOS_GP_PATH_COMMIT);   /* (not held: the owner's restore may wait for a drop) */
    hos_gp_hold(1);
    const int ok = VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) || each_piece(lo, hi, commit_piece);
    hos_gp_hold(0);
    return ok;
}

/* granules wholly inside [lo, hi) that are dropped or views become reservations of their own (they read as zero after
   their next touch); dropped granules the range only cuts are restored by their owner first. Then fn piece by piece. */
static int give_up(void *p, size_t n, PieceFn fn) {
    uint8_t *lo = (uint8_t *)p, *hi = lo + n;
    uint64_t a, b;
    if (!span(p, n, &a, &b)) return each_piece(lo, hi, fn);
    const uint8_t *clo = lo < gp_base ? gp_base : lo, *end = gp_base + (gp_count << GP_SHIFT), *chi = hi > end ? end : hi;
    const uint64_t in_a = ((uint64_t)(clo - gp_base) + GP_GRAN - 1) >> GP_SHIFT, in_b = (uint64_t)(chi - gp_base) >> GP_SHIFT;
    /* (the end granules, when the range only cuts them) */
    if ((a < in_a || a >= in_b) && bit(bm_dropped, a)) ask_owner(a, a + 1, 1, HOS_GP_PATH_COMMIT);
    if (b - 1 > a && b - 1 >= in_b && bit(bm_dropped, b - 1)) ask_owner(b - 1, b, 1, HOS_GP_PATH_COMMIT);
    claim(a, b);
    HANDLE self = GetCurrentProcess();
    uint64_t forgot = 0;
    for (uint64_t i = in_a; i < in_b; i++) {
        if (!bit(bm_apart, i)) continue;
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(at(i), &m, sizeof m)) continue;
        const int dropped = bit(bm_dropped, i), view = m.Type == MEM_MAPPED;
        if (!dropped && !view) continue;   /* (a reservation of its own already: decommitted below as any other) */
        if (view && !uv2(self, at(i), MEM_PRESERVE_PLACEHOLDER)) continue;
        if (va2(self, at(i), (SIZE_T)GP_GRAN, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != at(i)) {
            hos_log("mem: guest pages: the granule at %p was not made a reservation again (error %lu)", at(i), GetLastError());
            if (view) bit_set(bm_dropped, i);   /* (a placeholder now: a touch must find it dropped) */
            continue;
        }
        if (dropped) bit_clear(bm_dropped, i), InterlockedIncrement64(&n_forgets), forgot++;
    }
    hos_gp_hold(1);
    const int ok = each_piece(lo, hi, fn);
    hos_gp_hold(0);
    unclaim(a, b);
    const hos_gp_on_forget cb = gp_forget;
    if (forgot && cb) cb(at(in_a), (size_t)((in_b - in_a) << GP_SHIFT));   /* (no claim held: it may drop or restore) */
    return ok;
}
int hos_gp_decommit(void *p, size_t n) { return give_up(p, n, decommit_piece); }
int hos_gp_reset(void *p, size_t n) { return give_up(p, n, reset_piece); }

void hos_gp_imported(void *p, size_t n) {
    uint64_t a, b;
    if (!span(p, n, &a, &b)) return;
    claim(a, b);   /* (no drop in flight over it: after this, none starts) */
    for (uint64_t i = a; i < b; i++) bit_set(bm_imported, i);
    unclaim(a, b);
    if (any_bit(bm_dropped, a, b)) ask_owner(a, b, 1, HOS_GP_PATH_IMPORT);
}

/* (RX_VK_NO_HOST_IMPORT, stream M2) the back end freed its import of [p, p + n) and no other import holds it: its granules
   may be dropped again */
void hos_gp_unimported(void *p, size_t n) {
    uint64_t a, b;
    if (!span(p, n, &a, &b)) return;
    for (uint64_t i = a; i < b; i++) bit_clear(bm_imported, i);
}

int hos_gp_report(char *out, size_t cap) {
    if (!gp_on) return 0;
    const LONG64 restored = n_restores[0] + n_restores[1] + n_restores[2] + n_restores[3];
    snprintf(out, cap,
             "dropped now %lld MB; drops %lld granules, restores %lld (handler %lld, commit %lld, import %lld, direct %lld) "
             "in %lld calls, forgotten %lld; failed: drops %lld, fills %lld, maps %lld, declined %lld, refused %lld (imported %lld); "
             "waits %lld",
             (long long)((n_drops - restored - n_forgets) >> 4), (long long)n_drops, (long long)restored,
             (long long)n_restores[HOS_GP_PATH_HANDLER], (long long)n_restores[HOS_GP_PATH_COMMIT],
             (long long)n_restores[HOS_GP_PATH_IMPORT], (long long)n_restores[HOS_GP_PATH_DIRECT], (long long)n_restore_calls,
             (long long)n_forgets, (long long)n_drop_failed, (long long)n_fill_failed, (long long)n_map_failed,
             (long long)n_declined, (long long)n_refused, (long long)n_refused_imported,
             (long long)n_waits);
    return 1;
}

#else
/* (Linux and the web: not built yet; mmap could do it: MAP_FIXED of a memfd's filled pages over the range) */
size_t hos_gp_granule(void) { return 0; }
int hos_gp_ready(void) { return 0; }
int hos_gp_drop(void *p, size_t size) { (void)p, (void)size; return -1; }
int hos_gp_restore(void *p, size_t size, hos_gp_fill fill, void *user) { (void)p, (void)size, (void)fill, (void)user; return -1; }
void hos_gp_set_owner(hos_gp_on_touch cb) { (void)cb; }
void hos_gp_set_on_forget(hos_gp_on_forget cb) { (void)cb; }
int hos_gp_is_dropped(const void *p) { (void)p; return 0; }
int hos_gp_reserve(void *base, size_t size) { (void)base, (void)size; return 0; }
int hos_gp_fault(void *p, int write) { (void)p, (void)write; return 0; }
void hos_gp_prepare(void *p, size_t n, int path) { (void)p, (void)n, (void)path; }
int hos_gp_touches(const void *p, size_t n) { (void)p, (void)n; return 0; }
void hos_gp_hold(int begin) { (void)begin; }
int hos_gp_commit(void *p, size_t n) { (void)p, (void)n; return 0; }
int hos_gp_decommit(void *p, size_t n) { (void)p, (void)n; return 0; }
int hos_gp_reset(void *p, size_t n) { (void)p, (void)n; return 0; }
void hos_gp_imported(void *p, size_t n) { (void)p, (void)n; }
void hos_gp_unimported(void *p, size_t n) { (void)p, (void)n; }
int hos_gp_report(char *out, size_t cap) { (void)out, (void)cap; return 0; }
#endif
