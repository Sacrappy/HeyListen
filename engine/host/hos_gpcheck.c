/* (HeyListen) HOS_GUEST_PAGES's counters, and its self-test in the game (hos_guestpages.c; memory-boundary.md section 13).

   With HOS_GUEST_PAGES=1 a line "mem: guest pages: ..." every 10 s gives the substrate's counters (granules dropped,
   restored by path, forgotten, failures).

   HOS_GUEST_PAGES_TEST=1 exercises the whole path in the game without a policy: every 3 s a thread takes granules of GPU
   pool storage (pools with flags 0x024 or 0x064 made 20 s ago or more) that the cold-pages thread has already taken out
   of the working set (none of their 16 pages resident), saves their bytes in a host buffer with a hash, and drops them;
   it is their owner, and restores a granule from its buffer when someone touches it (the game, the back end, a commit
   for the kernel, an import for the driver), then hashes the granule in guest memory against the saved hash. A granule
   is made read-only while it is saved and dropped, so that a write cannot land between the copy and the drop and be
   lost: a writer that faults on it waits until the drop is done, then runs its write again over the dropped granule,
   which restores it first. At most HOS_GUEST_PAGES_TEST_MB (default 256) MB saved at a time, 32 MB dropped a pass.
   Every 10 s "mem: guest pages test: ..." gives the drops, restores, hash mismatches and refusals. */
#include "hos.h"
#include "hos_guestpages.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GRAN 0x10000ULL
#define SLOTS 32768   /* (open addressing, at most half full: 16,384 granules, 1 GB) */

typedef struct { uint8_t *at; uint8_t *bytes; uint64_t hash; } Saved;
static Saved saved[SLOTS];
static SRWLOCK saved_lock = SRWLOCK_INIT;
static int64_t saved_now, saved_cap;
static int64_t sweep_imported, last_sweep_imported = -1, sweep_granules, last_sweep_granules;   /* (check thread only) */
static volatile LONG64 t_drops, t_restored, t_mismatch, t_missing, t_refused, t_refused_imported, t_waits;
static uint8_t *volatile sealing;   /* (the granule being saved and dropped: a write to it waits) */

static uint64_t hash64(const uint8_t *p) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (size_t k = 0; k < GRAN; k += 8) {
        uint64_t w;
        memcpy(&w, p + k, 8);
        h = (h ^ w) * 0x100000001B3ULL;
        h ^= h >> 29;
    }
    return h;
}
static Saved *slot(uint8_t *at, int add) {   /* (saved_lock held) */
    uint64_t i = ((uint64_t)(uintptr_t)at >> 16) * 0x9E3779B97F4A7C15ULL >> 49;
    for (int n = 0; n < SLOTS; n++, i = (i + 1) & (SLOTS - 1)) {
        if (saved[i].at == at) return &saved[i];
        if (!saved[i].at) return add ? &saved[i] : NULL;
    }
    return NULL;
}

/* an entry out of the table, the rest of its cluster moved back over it (saved_lock held) */
static void forget(Saved *s) {
    uint64_t i = (uint64_t)(s - saved), j = i;
    saved[i].at = NULL;
    saved_now -= (int64_t)GRAN;
    for (;;) {
        j = (j + 1) & (SLOTS - 1);
        if (!saved[j].at) break;
        const uint64_t home = ((uint64_t)(uintptr_t)saved[j].at >> 16) * 0x9E3779B97F4A7C15ULL >> 49;
        if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j)) {
            saved[i] = saved[j];
            saved[j].at = NULL;
            i = j;
        }
    }
}

static int fill_saved(void *user, void *buf, void *p, size_t size) {
    (void)user;
    int ok = 1;
    AcquireSRWLockShared(&saved_lock);
    for (size_t off = 0; off < size; off += GRAN) {
        Saved *s = slot((uint8_t *)p + off, 0);
        if (s) memcpy((uint8_t *)buf + off, s->bytes, GRAN);
        else if (hos_gp_is_dropped((uint8_t *)p + off)) ok = 0;   /* (a dropped granule this test never saved) */
    }
    ReleaseSRWLockShared(&saved_lock);
    return ok ? 0 : 1;
}

static int owner(void *p, size_t size, int write) {
    (void)write;
    if (hos_gp_restore(p, size, fill_saved, NULL) != 0) {
        InterlockedIncrement64(&t_missing);
        return 0;
    }
    /* the granules restored: their bytes against the saved hash, then forgotten */
    AcquireSRWLockExclusive(&saved_lock);
    for (size_t off = 0; off < size; off += GRAN) {
        Saved *s = slot((uint8_t *)p + off, 0);
        if (!s || hos_gp_is_dropped(s->at)) continue;
        if (hash64(s->at) != s->hash && InterlockedIncrement64(&t_mismatch) <= 8)
            hos_log("mem: guest pages test: the granule at guest %#llx came back unlike its saved bytes",
                    (unsigned long long)(s->at - (uint8_t *)GP(0)));
        free(s->bytes);
        forget(s);
        InterlockedIncrement64(&t_restored);
    }
    ReleaseSRWLockExclusive(&saved_lock);
    return 1;
}

static LONG CALLBACK sealing_wait(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *p = (uint8_t *)rec->ExceptionInformation[1], *s = sealing;
    if (!s || p < s || p >= s + GRAN) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement64(&t_waits);
    while (sealing == s) Sleep(0);
    return EXCEPTION_CONTINUE_EXECUTION;   /* (the granule is dropped now, or was given back: the write goes on there) */
}

typedef struct { void *va; ULONG_PTR attr; } Ws;
typedef BOOL(WINAPI *QueryWsFn)(HANDLE, PVOID, DWORD);

/* one granule: read-only, all 16 pages still out of the working set and committed, saved, dropped */
static int take(uint8_t *g, QueryWsFn query) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(g, &m, sizeof m) || m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE ||
        (uint8_t *)m.BaseAddress + m.RegionSize < g + GRAN)
        return 0;
    Ws ws[16];
    for (int k = 0; k < 16; k++) ws[k].va = g + k * 0x1000, ws[k].attr = 0;
    if (!query(GetCurrentProcess(), ws, sizeof ws)) return 0;
    for (int k = 0; k < 16; k++)
        if (ws[k].attr & 1) return 0;   /* (resident: in use, or not taken out yet) */
    uint8_t *bytes = (uint8_t *)malloc(GRAN);
    if (!bytes) return 0;
    sealing = g;
    DWORD old;
    if (!VirtualProtect(g, GRAN, PAGE_READONLY, &old)) {
        sealing = NULL;
        free(bytes);
        return 0;
    }
    memcpy(bytes, g, GRAN);
    const uint64_t h = hash64(bytes);
    AcquireSRWLockExclusive(&saved_lock);
    Saved *s = slot(g, 1);
    if (s) s->at = g, s->bytes = bytes, s->hash = h, saved_now += (int64_t)GRAN;
    ReleaseSRWLockExclusive(&saved_lock);
    const int rc = s ? hos_gp_drop(g, GRAN) : -1, dropped = rc == 0;
    if (!dropped) {
        VirtualProtect(g, GRAN, PAGE_READWRITE, &old);
        if (s) {   /* (looked up again: a restore meanwhile may have moved it) */
            AcquireSRWLockExclusive(&saved_lock);
            Saved *again = slot(g, 0);
            if (again) forget(again);
            ReleaseSRWLockExclusive(&saved_lock);
        }
        free(bytes);
        InterlockedIncrement64(rc == -2 ? &t_refused_imported : &t_refused);
    } else
        InterlockedIncrement64(&t_drops);
    sealing = NULL;
    return dropped;
}

static DWORD WINAPI check_main(void *arg) {
    const int test = (int)(uintptr_t)arg;
    QueryWsFn query = (QueryWsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    static uint64_t ranges[16384][2];
    static const uint32_t flags[2] = {0x24, 0x64};
    uint64_t cursor = 0;
    int64_t next_report = hos_now_ns() + 10000000000LL;
    for (;;) {
        Sleep(test ? 3000 : 10000);
        if (test && query && !hos_pool_trap_active()) {
            const int n = hos_pools_with_flags(flags, 2, hos_now_ns() - 20000000000LL, ranges, 16384);
            int64_t budget = 32 << 20, looked = 0;
            for (int i = 0; i < n && budget > 0 && looked < 8192; i++) {
                uint64_t lo = (ranges[i][0] + GRAN - 1) & ~(GRAN - 1), hi = ranges[i][1] & ~(GRAN - 1);
                if (hi <= cursor) continue;
                if (lo < cursor) lo = cursor;
                for (uint64_t a = lo; a < hi && budget > 0 && looked < 8192; a += GRAN, looked++) {
                    cursor = a + GRAN;
                    if (saved_now + (int64_t)GRAN > saved_cap) { budget = 0; break; }
                    sweep_granules++;
                    if (hos_gp_is_imported(GP(a))) { sweep_imported++; continue; }   /* (the driver holds it: never dropped) */
                    if (hos_gp_is_dropped(GP(a))) continue;
                    if (take((uint8_t *)GP(a), query)) budget -= (int64_t)GRAN;
                }
            }
            if (looked < 8192 && budget > 0) {   /* (the end of the pools: again from the bottom) */
                cursor = 0;
                last_sweep_imported = sweep_imported, last_sweep_granules = sweep_granules;
                sweep_imported = sweep_granules = 0;
            }
        }
        if (hos_now_ns() >= next_report) {
            char line[512];
            if (hos_gp_report(line, sizeof line)) hos_log("mem: guest pages: %s", line);
            if (test)
                hos_log("mem: guest pages test: dropped %lld granules, restored %lld, saved now %lld MB; hash mismatches %lld, "
                        "restores refused %lld, drops refused %lld (imported %lld), writers held during a drop %lld; the last whole "
                        "sweep of the pools: %lld MB of granules, %lld MB of them imported (not droppable)",
                        (long long)t_drops, (long long)t_restored, (long long)(saved_now >> 20), (long long)t_mismatch,
                        (long long)t_missing, (long long)t_refused, (long long)t_refused_imported, (long long)t_waits,
                        (long long)(last_sweep_granules >> 4), (long long)(last_sweep_imported >> 4));
            next_report = hos_now_ns() + 10000000000LL;
        }
    }
    return 0;
}

void hos_gp_check_start(void) {
    const char *e = getenv("HOS_GUEST_PAGES_TEST"), *mb = getenv("HOS_GUEST_PAGES_TEST_MB");
    const int test = e && e[0] == '1';
    if (test) {
        saved_cap = (int64_t)(mb && atoi(mb) > 0 ? atoi(mb) : 256) << 20;
        if (saved_cap > (int64_t)(SLOTS / 2) * (int64_t)GRAN) saved_cap = (int64_t)(SLOTS / 2) * (int64_t)GRAN;
        AddVectoredExceptionHandler(1, sealing_wait);   /* (in front of commit_on_touch) */
        hos_gp_set_owner(owner);
        hos_log("mem: guest pages test: on: granules of pools 0x024/0x064 out of the working set dropped every 3 s (%lld MB "
                "saved at most) and restored from their saved bytes on touch", (long long)(saved_cap >> 20));
    }
    CloseHandle(CreateThread(NULL, 0, check_main, (void *)(uintptr_t)test, 0, NULL));
}
#else
void hos_gp_check_start(void) {}
#endif
