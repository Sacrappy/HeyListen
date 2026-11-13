/* (HeyListen native engine) The OS layer's side of the drop module (engine/native, hl/c/hl_drop.h;
   docs/native-engine/memory-boundary.md section 14): a decoded resource's memory given back to the system for good, and
   made again from its file when anything touches it.

   Cold pages (hl_mem.c) take the CPU side of the game's resources out of the working set, but the pages stay committed:
   the commit charge keeps them, and the page file holds their bytes. A resource file's decoded bytes can be had again
   exactly by decoding the file again (stream-boundary.md section 9: proved byte-exact). So the whole granules of such a
   range (64 KB, the substrate's unit: hos_guestpages.h) that are cold (no page in the working set: cold pages put them
   out and nobody touched them since), were never imported by the back end nor handed to the kernel lately, and are
   unchanged (made read-only, then compared byte for byte with the file decoded again) are dropped: no RAM, no commit. A
   touch, from the game, the back end, the kernel (hos_commit) or the driver (an import), reaches the substrate, which
   calls the owner here: the range's dropped granules are restored from the file, atomically, under the range's lock.
   The policy is hl::mem::Drop (native, no guest knowledge; native/tests/drop_test.cpp).

   What the decision relies on, and how each is held:
   - a writer of a granule being decided faults on its read-only page: the handler here makes it wait for the decision
     (hl_drop_wait_pending), then it runs its instruction again: on the bytes if the granule was kept, through the
     substrate's restore if it was dropped;
   - the kernel is given guest memory through hos_commit, which tells us first (a decision under way there ends first,
     and its granules are not dropped for 5 s: a file read into memory must not meet a read-only page);
   - the back end tells hos_noaccess_import before it imports a range: those granules are never dropped (the driver pins
     imported pages); a dropped one is restored by the substrate before the import;
   - the source never goes away: the RomFS file is read again by its name (the mods' file first, as the game reads it),
     and the dictionary is the host's own copy, made at the decode and kept.
     HL_MEM_DROP=0  off (the default): nothing here runs, as before    HL_MEM_DROP=1  on
     HL_MEM_DROP_AGE=<seconds>  a range is looked at that long after its decode (default 30)
     HL_MEM_DROP_MAX_MB=<MB>  larger decodes are left out (default 32, 0: any): a touch decodes the whole file
     HL_MEM_DROP_CHECK=1  a hash of each granule at its drop, compared at its restore (counted)
     HL_MEM_DROP_LOG=1  the counters every 10 s */
#include "hos.h"
#include "hl/c/hl_drop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "hos_guestpages.h"

#define DROP_PASS_MS 500
#define DROP_GIVE_UP 4

static volatile LONG drop_on;
static int drop_log;
static size_t granule;

/* (hl_stream.c) a decoded file made again from the RomFS, malloc'd; NULL when it cannot be */
uint8_t *hl_stream_decode_again(const char *name, uint32_t dict_id, uint64_t size, uint64_t compressed);

static inline uint64_t guest_of(const void *p) { return (uint64_t)((const uint8_t *)p - (const uint8_t *)GP(0)); }

/* ---- the host callbacks of the policy ---- */
static int cb_is_dropped(void *ctx, uint64_t a) { (void)ctx; return hos_gp_is_dropped(GP(a)); }
static int cb_drop(void *ctx, uint64_t a, uint64_t n) { (void)ctx; return hos_gp_drop(GP(a), (size_t)n); }

typedef struct { hl_drop_fill fill; void *fill_ctx; } FillCall;
static int fill_adapter(void *user, void *buf, void *p, size_t size) {
    const FillCall *f = (const FillCall *)user;
    return f->fill(f->fill_ctx, (uint8_t *)buf, guest_of(p), size);
}
static int cb_restore(void *ctx, uint64_t a, uint64_t n, hl_drop_fill fill, void *fill_ctx) {
    (void)ctx;
    FillCall f = {fill, fill_ctx};
    return hos_gp_restore(GP(a), (size_t)n, fill_adapter, &f);
}

static const uint8_t *cb_source(void *ctx, const char *name, uint32_t dict_id, uint64_t size, uint64_t compressed) {
    (void)ctx;
    return hl_stream_decode_again(name, dict_id, size, compressed);
}
static void cb_release(void *ctx, const uint8_t *bytes) { (void)ctx; free((void *)bytes); }
static const uint8_t *cb_view(void *ctx, uint64_t a) { (void)ctx; return (const uint8_t *)GP(a); }

typedef struct { void *va; ULONG_PTR attr; } DropWs;   /* PSAPI_WORKING_SET_EX_INFORMATION (bit 0: valid) */
typedef BOOL(WINAPI *DropQueryFn)(HANDLE, PVOID, DWORD);
static DropQueryFn ws_query;
static uint32_t cb_resident(void *ctx, uint64_t a) {
    (void)ctx;
    DropWs info[16];
    const int m = (int)(granule >> 12) < 16 ? (int)(granule >> 12) : 16;
    for (int k = 0; k < m; k++) info[k].va = GP(a + ((uint64_t)k << 12)), info[k].attr = 0;
    if (!ws_query(GetCurrentProcess(), info, (DWORD)(m * sizeof(DropWs)))) return (uint32_t)m;   /* (unknown: in use) */
    uint32_t n = 0;
    for (int k = 0; k < m; k++) n += (uint32_t)(info[k].attr & 1);
    return n;
}
static int cb_protect(void *ctx, uint64_t a, uint64_t n, int read_only) {
    (void)ctx;
    DWORD was;
    return VirtualProtect(GP(a), (SIZE_T)n, read_only ? PAGE_READONLY : PAGE_READWRITE, &was) ? 0 : 1;
}
static int64_t cb_clock(void *ctx) { (void)ctx; return hos_now_ns(); }
/* committed read-write memory over the whole granule, one allocation (what a lazy commit or hos_commit made) */
static int cb_plain(void *ctx, uint64_t a) {
    (void)ctx;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(GP(a), &mbi, sizeof mbi)) return 0;
    const uint64_t end = guest_of(mbi.BaseAddress) + mbi.RegionSize;
    return mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && end >= a + granule;
}

/* ---- the substrate's hook: a dropped granule touched ---- */
static _Thread_local int in_kernel;   /* (1: hos_commit is handing guest memory to the kernel on this thread; 2: the back end imports it) */
static volatile LONG64 restore_ns_max, restore_ns_sum, restore_count;
static int on_touch(void *p, size_t size, int write) {
    const int path = in_kernel == 2 ? HL_DROP_IMPORT : in_kernel ? HL_DROP_KERNEL
                     : hos_ctx() ? (write ? HL_DROP_GUEST_WRITE : HL_DROP_GUEST_READ)
                                 : (write ? HL_DROP_HOST_WRITE : HL_DROP_HOST_READ);
    const int64_t t0 = hos_now_ns();
    /* (the substrate gives a run of dropped granules, which may belong to several ranges: each one still dropped is
       restored with its range) */
    int ok = 1;
    for (uint64_t a = guest_of(p), end = a + size; a < end && ok; a += granule)
        if (hos_gp_is_dropped(GP(a))) ok = hl_drop_touch(a, path, t0, guest_of(p), guest_of(p) + size);
    const int64_t took = hos_now_ns() - t0;
    InterlockedAdd64(&restore_ns_sum, took);
    InterlockedIncrement64(&restore_count);
    for (LONG64 m = restore_ns_max; took > m;) {
        const LONG64 was = InterlockedCompareExchange64(&restore_ns_max, took, m);
        if (was == m) break;
        m = was;
    }
    if (!ok) {
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 8)
            hos_log("hl drop: a touch of %#llx could not be restored (%s)", (unsigned long long)guest_of(p),
                    hl_drop_ready() ? "not ours, or the source failed" : "off");
    }
    return ok;
}

/* the OS layer unmapped, decommitted or discarded [p, p + size) and the substrate forgot the dropped granules inside
   (a per-file heap's region given back): their records go, and the candidates there with them */
static void on_forget(void *p, size_t size) { hl_drop_forget(guest_of(p), guest_of(p) + size); }

/* a writer's fault on a granule being decided (read-only for the decision): it waits, then runs its instruction again */
static LONG CALLBACK pending_handler(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || rec->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t *p = (const uint8_t *)rec->ExceptionInformation[1];
    if (p < (const uint8_t *)GP(0) || p >= (const uint8_t *)GP(HOS_GUEST_SPACE)) return EXCEPTION_CONTINUE_SEARCH;
    return hl_drop_wait_pending(guest_of(p)) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/* (mkw_glue.c hos_commit) the kernel is about to be given [lo, hi) */
void hl_drop_kernel_begin(uint64_t lo, uint64_t hi) {
    if (!drop_on) return;
    hl_drop_kernel_use(lo, hi, hos_now_ns());
    in_kernel = 1;
}
void hl_drop_kernel_end(void) { in_kernel = 0; }
/* (hos_memsplit.c hos_noaccess_import) the back end imports [lo, hi) */
void hl_drop_import(uint64_t lo, uint64_t hi) {
    if (!drop_on) return;
    hl_drop_imported(lo, hi);
    in_kernel = 2;
}
void hl_drop_import_end(void) { in_kernel = 0; }
/* (hos_memsplit.c hos_noaccess_unimport) the back end gave its import of [lo, hi) back */
void hl_drop_unimport(uint64_t lo, uint64_t hi) {
    if (drop_on) hl_drop_unimported(lo, hi);
}
/* (hl_stream.c) a decode wrote [dest, dest + size) */
void hl_drop_decode(uint64_t dest, uint64_t size, uint64_t compressed, uint32_t dict_id, const char *name) {
    if (drop_on) hl_drop_decoded(dest, size, compressed, dict_id, name, hos_now_ns());
}

static DWORD WINAPI drop_main(void *unused) {
    (void)unused;
    int64_t next_report = hos_now_ns() + 10000000000LL;
    double worst_ms = 0;
    for (;;) {
        Sleep(DROP_PASS_MS);
        const int64_t t0 = hos_now_ns();
        hl_drop_pass(t0);
        const double ms = (hos_now_ns() - t0) / 1e6;
        if (ms > worst_ms) worst_ms = ms;
        if (hos_now_ns() < next_report) continue;
        hl_drop_reconcile();
        if (drop_log) {
            char line[1024];
            if (hl_drop_report(line, sizeof line, hos_now_ns())) {
                const LONG64 n = InterlockedExchange64(&restore_count, 0), sum = InterlockedExchange64(&restore_ns_sum, 0),
                             mx = InterlockedExchange64(&restore_ns_max, 0);
                hos_log("hl drop: %s; a touch took %.2f ms on average, %.2f ms at most (since the last line); a pass %.1f ms at "
                        "most", line, n ? sum / 1e6 / n : 0.0, mx / 1e6, worst_ms);
            }
        }
        worst_ms = 0;
        next_report = hos_now_ns() + 10000000000LL;
    }
    return 0;
}

void hl_drop_init(void) {
    const char *e = getenv("HL_MEM_DROP");
    if (!(e && atoi(e) == 1)) return;
    extern int hos_large_pages;
    ws_query = (DropQueryFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    granule = hos_gp_granule();
    const char *why = hos_large_pages ? "large pages" : hos_pool_trap_active() ? "HOS_POOL_TRAP measures"
                      : !ws_query ? "no QueryWorkingSetEx" : !hos_gp_ready() || !granule ? "no substrate (HOS_GUEST_PAGES=1)" : NULL;
    if (why) {
        hos_log("hl drop: off (%s)", why);
        return;
    }
    const char *age = getenv("HL_MEM_DROP_AGE"), *chk = getenv("HL_MEM_DROP_CHECK"), *lg = getenv("HL_MEM_DROP_LOG");
    const char *cold = getenv("HL_MEM_COLD");
    hl_drop_config config = {0};
    config.granule = granule;
    config.age_ns = (int64_t)(age && *age ? atoi(age) : 30) * 1000000000LL;
    config.retry_ns = 10000000000LL;
    config.hold_ns = 60000000000LL;
    config.kernel_ns = 5000000000LL;
    config.give_up = DROP_GIVE_UP;
    config.pass_bytes = 64ull << 20;
    /* (cold pages off: nothing leaves the working set, so "cold" cannot be told; the age alone then) */
    config.require_cold = !(cold && cold[0] == '0');
    config.check = chk && atoi(chk) == 1;
    /* (a touch decodes the whole file on the touching thread, ~1.8 ms a MB on the PC 3050: one of 836 ms at the castle's
       load. Larger files are left out: HL_MEM_DROP_MAX_MB, default 32, 0 for any) */
    {
        const char *mx = getenv("HL_MEM_DROP_MAX_MB");
        config.max_range_bytes = (uint64_t)(mx && *mx ? atoi(mx) : 32) << 20;
    }
    config.pass_ns = 250000000;   /* (a pass once took 19 s on the PC 3050 at a scene's load: pages read back, files decoded) */
    const hl_drop_host host = {NULL, cb_is_dropped, cb_drop, cb_restore, cb_source, cb_release, cb_view, cb_resident,
                               cb_protect, cb_plain, cb_clock};
    if (!hl_drop_start(&config, &host)) {
        hos_log("hl drop: off (the policy could not be made)");
        return;
    }
    drop_log = config.check || (lg && atoi(lg) == 1);
    AddVectoredExceptionHandler(1, pending_handler);
    hos_gp_set_owner(on_touch);
    hos_gp_set_on_forget(on_forget);
    drop_on = 1;
    hos_log("hl drop: on: whole %zu KB granules of decoded resources, %lld s old or more%s, never imported, unchanged "
            "(compared with the file decoded again), given back for good; restored from the file on touch%s "
            "(HL_MEM_DROP=0: not)", granule >> 10, (long long)(config.age_ns / 1000000000LL),
            config.require_cold ? ", out of the working set" : "", config.check ? "; HL_MEM_DROP_CHECK=1: every restore hashed" : "");
    CloseHandle(CreateThread(NULL, 0, drop_main, NULL, 0, NULL));
}
#else
void hl_drop_kernel_begin(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hl_drop_kernel_end(void) {}
void hl_drop_import(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hl_drop_import_end(void) {}
void hl_drop_unimport(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hl_drop_decode(uint64_t dest, uint64_t size, uint64_t compressed, uint32_t dict_id, const char *name) {
    (void)dest; (void)size; (void)compressed; (void)dict_id; (void)name;
}
void hl_drop_init(void) {}
#endif
