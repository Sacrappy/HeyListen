/* (HeyListen) HOS_MEM_SPLIT=<seconds>: once, that long after the start, where the game's memory in RAM is: the pages of
   the guest window that are resident (in the working set), by part of the window (system region, image, heap, address
   regions), and of them, those inside an NVN memory pool with storage (what the GPU reads: textures, vertex and index
   data, uniform blocks), by the pool's flags. A measurement for docs/native-engine/memory-boundary.md: RAM is the
   resident pages, not the committed ones. Windows only. */
#include "hos.h"
#include "hos_guestpages.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct { uint64_t addr, size; uint32_t flags; int64_t made_ns; } Pool;
static Pool pools[16384];
static int npools;
static SRWLOCK pools_lock = SRWLOCK_INIT;

static int trap_new_pool(uint64_t addr, uint64_t size, uint32_t flags);   /* (HOS_POOL_TRAP_NEW, below) */
static void trap_pool_gone(uint64_t addr, uint64_t size, uint32_t flags);

/* host/nvn_native.c: a pool with storage in guest memory made (add 1) or ended (add 0) */
void hos_memsplit_pool(uint64_t addr, uint64_t size, uint32_t flags, int add) {
    AcquireSRWLockExclusive(&pools_lock);
    if (add) {
        if (npools < (int)(sizeof pools / sizeof pools[0])) pools[npools++] = (Pool){addr, size, flags, hos_now_ns()};
    } else {
        for (int i = 0; i < npools; i++)
            if (pools[i].addr == addr) { pools[i] = pools[--npools]; break; }
    }
    ReleaseSRWLockExclusive(&pools_lock);
    if (add) hl_mem_pool_made(addr, size);
    if (add) trap_new_pool(addr, size, flags);
    else trap_pool_gone(addr, size, flags);
}

static int by_addr(const void *a, const void *b) {
    const uint64_t x = ((const Pool *)a)->addr, y = ((const Pool *)b)->addr;
    return x < y ? -1 : x > y;
}
static int by_lo(const void *a, const void *b) {
    const uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
int hos_pools_with_flags(const uint32_t *flags, int nflags, int64_t made_before_ns, uint64_t (*out)[2], int cap) {
    int n = 0;
    AcquireSRWLockShared(&pools_lock);   /* (copied out, nothing else under the lock: pool creation waits for a memcpy only) */
    for (int i = 0; i < npools && n < cap; i++) {
        if (pools[i].made_ns >= made_before_ns || !pools[i].size) continue;
        if (!flags) { out[n][0] = pools[i].addr, out[n][1] = pools[i].addr + pools[i].size, n++; continue; }
        for (int f = 0; f < nflags; f++)
            if (pools[i].flags == flags[f]) { out[n][0] = pools[i].addr, out[n][1] = pools[i].addr + pools[i].size, n++; break; }
    }
    ReleaseSRWLockShared(&pools_lock);
    qsort(out, (size_t)n, sizeof out[0], by_lo);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && out[i][0] <= out[m - 1][1]) {
            if (out[i][1] > out[m - 1][1]) out[m - 1][1] = out[i][1];
        } else out[m][0] = out[i][0], out[m][1] = out[i][1], m++;
    }
    return m;
}

typedef struct { void *va; ULONG_PTR attr; } WsInfo;   /* PSAPI_WORKING_SET_EX_INFORMATION (bit 0: valid) */
typedef BOOL(WINAPI *QueryWsFn)(HANDLE, PVOID, DWORD);

typedef struct { const char *name; uint64_t lo, hi, committed, resident, in_pools; } Part;

static int split_no_read(void);   /* (below: HOS_POOL_TRAP on) */
static int page_is_zero(const void *page) {
    const uint64_t *w = (const uint64_t *)page;
    uint64_t any = 0;
    for (int i = 0; i < 512; i++) any |= w[i];
    return !any;
}
static DWORD WINAPI split_main(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    QueryWsFn query = (QueryWsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    if (!query) { hos_log("memsplit: no K32QueryWorkingSetEx"); return 0; }
    static Pool snap[16384];
    AcquireSRWLockShared(&pools_lock);
    const int n = npools;
    memcpy(snap, pools, (size_t)n * sizeof(Pool));
    ReleaseSRWLockShared(&pools_lock);
    qsort(snap, (size_t)n, sizeof(Pool), by_addr);
    Part parts[] = {
        {"system region", HOS_SYSTEM_BASE, HOS_SYSTEM_END, 0, 0, 0},
        {"image", hos_image_base, hos_image_end, 0, 0, 0},
        {"heap", HOS_HEAP_BASE, HOS_HEAP_BASE + hos_heap_size, 0, 0, 0},
        {"address regions", HOS_HEAP_BASE + hos_heap_size, HOS_GUEST_SPACE, 0, 0, 0},
    };
    /* resident bytes inside pools, by flags (the 16 most seen) */
    struct { uint32_t flags; uint64_t size, committed, resident, zero; int count; } byflag[16];
    int nflags = 0;
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < nflags && byflag[k].flags != snap[i].flags) k++;
        if (k == nflags && nflags < 16) { byflag[nflags].flags = snap[i].flags; byflag[nflags].size = byflag[nflags].committed = byflag[nflags].resident = byflag[nflags].zero = 0; byflag[nflags].count = 0; nflags++; }
        if (k < nflags) { byflag[k].size += snap[i].size; byflag[k].count++; }
    }
    enum { BATCH = 4096 };
    static WsInfo info[BATCH];
    const int64_t t0 = hos_now_ns();
    for (int pi = 0; pi < 4; pi++) {
        Part *part = &parts[pi];
        uint64_t a = part->lo;
        int pool_at = 0;
        while (a < part->hi) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(GP(a), &mbi, sizeof mbi)) break;
            const uint64_t base = (uint64_t)((uint8_t *)mbi.BaseAddress - (uint8_t *)GP(0));
            uint64_t end = base + mbi.RegionSize;
            if (end > part->hi) end = part->hi;
            if (mbi.State == MEM_COMMIT) {
                part->committed += end - a;
                for (uint64_t q = a & ~0xFFFULL; q < end;) {
                    int m = 0;
                    for (; m < BATCH && q < end; m++, q += 0x1000) info[m].va = GP(q), info[m].attr = 0;
                    if (!query(GetCurrentProcess(), info, (DWORD)(m * sizeof(WsInfo)))) continue;
                    for (int k = 0; k < m; k++) {
                        const int res = (int)(info[k].attr & 1);
                        if (res) part->resident += 0x1000;
                        const uint64_t g = (uint64_t)((uint8_t *)info[k].va - (uint8_t *)GP(0));
                        while (pool_at < n && snap[pool_at].addr + snap[pool_at].size <= g) pool_at++;
                        if (pool_at < n && snap[pool_at].addr <= g) {
                            if (res) part->in_pools += 0x1000;
                            for (int f = 0; f < nflags; f++)
                                if (byflag[f].flags == snap[pool_at].flags) {
                                    byflag[f].committed += 0x1000;
                                    if (!res) break;
                                    byflag[f].resident += 0x1000;
                                    /* (a resident page that holds only zeros: what decommitting it would give back with the
                                       same contents. Not while HOS_POOL_TRAP has pages NOACCESS) */
                                    if (!split_no_read() && page_is_zero(info[k].va)) byflag[f].zero += 0x1000;
                                    break;
                                }
                        }
                    }
                }
            }
            if (end <= a) break;
            a = end;
        }
    }
    uint64_t resident = 0, in_pools = 0;
    for (int pi = 0; pi < 4; pi++) {
        hos_log("memsplit: %-15s committed %6llu MB, in RAM %6llu MB, of which inside GPU pools %6llu MB", parts[pi].name,
                (unsigned long long)(parts[pi].committed >> 20), (unsigned long long)(parts[pi].resident >> 20),
                (unsigned long long)(parts[pi].in_pools >> 20));
        resident += parts[pi].resident;
        in_pools += parts[pi].in_pools;
    }
    hos_log("memsplit: the game's memory in RAM %llu MB: GPU pools %llu MB (%d pools), the rest %llu MB (%.0f ms to count)",
            (unsigned long long)(resident >> 20), (unsigned long long)(in_pools >> 20), n,
            (unsigned long long)((resident - in_pools) >> 20), (hos_now_ns() - t0) / 1e6);
    for (int f = 0; f < nflags; f++)
        hos_log("memsplit: pools with flags %#06x: %4d, %6llu MB of storage, %6llu MB in RAM (%llu MB of it zeros; %llu MB committed)",
                byflag[f].flags, byflag[f].count, (unsigned long long)(byflag[f].size >> 20), (unsigned long long)(byflag[f].resident >> 20),
                (unsigned long long)(byflag[f].zero >> 20), (unsigned long long)(byflag[f].committed >> 20));
    hos_log_flush();
    return 0;
}

/* ---- (HeyListen, 2026-11-13) HOS_POOL_TRAP=<seconds>: does the game's code touch GPU pool memory once the back end has it?
   A measurement, off unless asked (with the switch off nothing below runs: hos_pool_trap_release is one load and a branch).
   That long after the start, every resident page of the pools with the chosen flags (HOS_POOL_TRAP_FLAGS, hex, comma
   separated: default 24,64,22) that exist for 2 s or more is made PAGE_NOACCESS; HOS_POOL_TRAP_EVERY=<seconds> does it again
   that often (the pages touched since included). The first touch of a trapped page comes to the handler below, which gives the
   page its access back and counts it once: by the pool's flags, read or write, the thread (a guest thread by its guest name,
   a host thread by its description) and the host stack (four frames: offsets in HeyListen.exe or hookshot_vk.dll, which
   tools/bench/pooltrap.py resolves with HeyListen.map into the lifted f_<guest address> or the host function). Touches from
   threads that are not guest threads (the Vulkan back end: captures, mirror syncs, uploads) are counted apart, and so are
   pages handed to the kernel: hos_commit (a file read into a pool, nn_fs.c) and the back end's imports
   (hos_pool_trap_release, exported) give the pages of their range their access back first, as the kernel and the driver do
   not come through the handler (a ReadFile into a NOACCESS page fails).
   HOS_POOL_TRAP_NEW=1 (with HOS_POOL_TRAP_FLAGS, e.g. a1,21): instead, every page of each pool made with those flags from
   the start is trapped when the pool is made (committed first, without being touched), and how many of its pages were in
   RAM already is counted: who brings each page of such a pool into RAM.
   Every 5 s from the arming the totals go to the log ("pooltrap:"); every 10 s, and at HOS_POOL_TRAP_REPORT (default 105 s
   after the start), every counted place goes to HOS_POOL_TRAP_OUT (default pooltrap.txt in the working directory). */
static uint64_t *trap_bits;   /* (a bit a guest page: trapped and not touched since) */
static int64_t trap_t0;   /* (hos_now_ns at the start) */
static volatile LONG trap_mode;   /* (0 off, 1 armed by time, 2 new pools) */
static uint32_t trap_flag_list[16];
static int trap_nflags;
enum { TC_GUEST, TC_HOST, TC_KERNEL, TC_N };   /* (a guest thread, a host thread, handed to the kernel or the driver) */
static const char *const trap_class_name[TC_N] = {"guest", "host", "kernel"};
static volatile LONG64 trap_count[16][TC_N][2];   /* (pages, by flag index, class, read 0 / write 1) */
static volatile LONG64 trap_armed_pages[16], trap_armed_imported[16], trap_new_resident[16], trap_new_pages[16];
static volatile LONG64 trap_faults, trap_races;
static uintptr_t trap_exe_lo, trap_exe_hi, trap_vk_lo, trap_vk_hi;

static int split_no_read(void) { return trap_mode != 0; }
int hos_pool_trap_active(void) { return trap_mode != 0; }
static int trap_flag_index(uint32_t flags) {
    for (int i = 0; i < trap_nflags; i++)
        if (trap_flag_list[i] == flags) return i;
    return -1;
}
static inline int trap_test(uint64_t page) { return (int)((__atomic_load_n(&trap_bits[page >> 6], __ATOMIC_ACQUIRE) >> (page & 63)) & 1); }
static inline void trap_set(uint64_t page) { __atomic_fetch_or(&trap_bits[page >> 6], 1ull << (page & 63), __ATOMIC_RELEASE); }
static inline int trap_take(uint64_t page) {   /* (1: this caller cleared the bit) */
    const uint64_t bit = 1ull << (page & 63);
    return (__atomic_fetch_and(&trap_bits[page >> 6], ~bit, __ATOMIC_ACQ_REL) & bit) != 0;
}
static volatile LONG64 trap_gone[16];   /* (trapped pages whose pool ended untouched: given their access back, counted apart) */
static void trap_pool_gone(uint64_t addr, uint64_t size, uint32_t flags) {
    if (!trap_mode || !size || addr >= HOS_GUEST_SPACE || size > HOS_GUEST_SPACE - addr) return;
    const int fi = trap_flag_index(flags);
    for (uint64_t page = addr >> 12; page < (addr + size + 0xFFF) >> 12; page++) {
        if (!trap_test(page)) continue;
        DWORD old;
        VirtualProtect(GP(page << 12), 0x1000, PAGE_READWRITE, &old);
        if (trap_take(page) && fi >= 0) InterlockedIncrement64(&trap_gone[fi]);
    }
}
/* (the pool flags of a guest address: the live list, under its lock; -1 when it is in none of the chosen ones) */
static int trap_flags_of(uint64_t g) {
    int k = -1;
    AcquireSRWLockShared(&pools_lock);
    for (int i = 0; i < npools; i++)
        if (g >= pools[i].addr && g - pools[i].addr < pools[i].size) {
            k = trap_flag_index(pools[i].flags);
            if (k >= 0) break;
        }
    ReleaseSRWLockShared(&pools_lock);
    return k;
}

/* the places counted: class, flags, read/write, thread, four host frames */
typedef struct {
    uint64_t key;
    int cls, fi, rw;
    char thread[40];
    uintptr_t frame[4];
    int64_t pages;
} TrapSite;
enum { TRAP_SITES = 8192 };
static TrapSite trap_sites[TRAP_SITES];
static int trap_nsites;
static SRWLOCK trap_sites_lock = SRWLOCK_INIT;

static void trap_module_ranges(void) {
    HMODULE mods[2] = {GetModuleHandleA(NULL), GetModuleHandleA("hookshot_vk.dll")};
    for (int m = 0; m < 2; m++) {
        if (!mods[m]) continue;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mods[m];
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)mods[m] + dos->e_lfanew);
        const uintptr_t lo = (uintptr_t)mods[m], hi = lo + nt->OptionalHeader.SizeOfImage;
        if (m == 0) trap_exe_lo = lo, trap_exe_hi = hi;
        else trap_vk_lo = lo, trap_vk_hi = hi;
    }
}
/* (a frame as written out: e+<offset> in HeyListen.exe, v+<offset> in hookshot_vk.dll, the module's name+offset otherwise) */
static void trap_frame_text(uintptr_t a, char *out, size_t n) {
    if (!trap_vk_lo) trap_module_ranges();   /* (the back end loads after the trap starts) */
    if (a >= trap_exe_lo && a < trap_exe_hi) { snprintf(out, n, "e+%#llx", (unsigned long long)(a - trap_exe_lo)); return; }
    if (a >= trap_vk_lo && a < trap_vk_hi) { snprintf(out, n, "v+%#llx", (unsigned long long)(a - trap_vk_lo)); return; }
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    const char *b = strrchr(name, '\\');
    snprintf(out, n, "%s+%#llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
}

static void trap_note(int cls, int fi, int rw, const char *thread, const uintptr_t frame[4]) {
    InterlockedIncrement64(&trap_count[fi][cls][rw]);
    uint64_t key = 1469598103934665603ull;
    const uint64_t parts[7] = {(uint64_t)cls, (uint64_t)fi, (uint64_t)rw, frame[0], frame[1], frame[2], frame[3]};
    for (int i = 0; i < 7; i++) key = (key ^ parts[i]) * 1099511628211ull;
    for (const char *s = thread; *s; s++) key = (key ^ (uint8_t)*s) * 1099511628211ull;
    AcquireSRWLockExclusive(&trap_sites_lock);
    int i = 0;
    while (i < trap_nsites && trap_sites[i].key != key) i++;
    if (i == trap_nsites && trap_nsites < TRAP_SITES) {
        TrapSite *s = &trap_sites[trap_nsites++];
        s->key = key, s->cls = cls, s->fi = fi, s->rw = rw, s->pages = 0;
        snprintf(s->thread, sizeof s->thread, "%s", thread);
        memcpy(s->frame, frame, sizeof s->frame);
    }
    if (i < trap_nsites) trap_sites[i].pages++;
    ReleaseSRWLockExclusive(&trap_sites_lock);
}

/* (the host stack from the fault: x64 unwind data; a frame with none is a leaf, its return address at rsp) */
static void trap_stack(const CONTEXT *from, uintptr_t frame[4]) {
    CONTEXT ctx = *from;
    for (int i = 0; i < 4; i++) {
        frame[i] = (uintptr_t)ctx.Rip;
        if (!ctx.Rip || i == 3) {
            for (int k = i + 1; k < 4; k++) frame[k] = 0;
            break;
        }
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        if (!f) {
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void *hd;
        DWORD64 fr;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &hd, &fr, NULL);
    }
}

/* (no heap call in the handler: a host thread is named by its id here, by its description in the report) */
static void trap_thread_name(char *out, size_t n) {
    HosThread *t = hos_self();
    if (t) snprintf(out, n, "%s", hos_thread_name(t));
    else snprintf(out, n, "host %lu", GetCurrentThreadId());
}
static void trap_thread_desc(const char *in, char *out, size_t n) {
    snprintf(out, n, "%s", in);
    unsigned long id;
    if (sscanf(in, "host %lu", &id) != 1) return;
    typedef HRESULT(WINAPI *GetDescFn)(HANDLE, PWSTR *);
    GetDescFn get = (GetDescFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    WCHAR *w = NULL;
    if (h && get && SUCCEEDED(get(h, &w)) && w && w[0]) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    if (w) LocalFree(w);
    if (h) CloseHandle(h);
}

/* (the first faults, raw, for HOS_POOL_TRAP_OUT's tail: a diagnosis of the trap itself) */
static struct { uint64_t guest; uintptr_t rip; DWORD tid; int rw; } trap_first[64];
static volatile LONG trap_nfirst;

static LONG CALLBACK trap_handler(EXCEPTION_POINTERS *info) {
    static _Thread_local int inside;
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (inside || rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t *p = (const uint8_t *)rec->ExceptionInformation[1];
    if (p < A64_MEM || p >= A64_MEM + HOS_GUEST_SPACE) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t guest = (uint64_t)(p - A64_MEM), page = guest >> 12;
    inside = 1;
    {
        const LONG k = InterlockedIncrement(&trap_nfirst) - 1;
        if (k < 64) trap_first[k].guest = guest, trap_first[k].rip = (uintptr_t)info->ContextRecord->Rip,
                    trap_first[k].tid = GetCurrentThreadId(), trap_first[k].rw = (int)rec->ExceptionInformation[0];
    }
    if (!trap_test(page)) {
        /* (another thread is giving it back right now, or the re-arming raced a give-back: a NOACCESS page of a chosen
           pool is ours either way) */
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Protect != PAGE_NOACCESS ||
            trap_flags_of(guest) < 0) {
            inside = 0;
            return EXCEPTION_CONTINUE_SEARCH;
        }
        InterlockedIncrement64(&trap_races);
    }
    DWORD old;
    if (!VirtualProtect(GP(page << 12), 0x1000, PAGE_READWRITE, &old)) { inside = 0; return EXCEPTION_CONTINUE_SEARCH; }
    if (trap_take(page)) {
        InterlockedIncrement64(&trap_faults);
        const int fi = trap_flags_of(guest);
        char thread[40];
        trap_thread_name(thread, sizeof thread);
        uintptr_t frame[4];
        trap_stack(info->ContextRecord, frame);
        trap_note(hos_self() ? TC_GUEST : TC_HOST, fi < 0 ? 15 : fi, rec->ExceptionInformation[0] == 1, thread, frame);
    }
    inside = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* the pages of [addr, addr + size) given their access back before the kernel or the driver uses them (hos_commit, the back
   end's imports); counted as the kernel's, by who asked */
__declspec(dllexport) void hos_pool_trap_release(uint64_t addr, uint64_t size, const char *who) {
    if (!trap_mode || !size || addr >= HOS_GUEST_SPACE) return;
    const uint64_t end = size > HOS_GUEST_SPACE - addr ? HOS_GUEST_SPACE : addr + size;
    const uintptr_t frame[4] = {(uintptr_t)who, 0, 0, 0};
    for (uint64_t page = addr >> 12; page < (end + 0xFFF) >> 12; page++) {
        if (!(page & 63) && !trap_bits[page >> 6] && page + 64 <= (end + 0xFFF) >> 12) { page += 63; continue; }
        if (!trap_test(page)) continue;
        DWORD old;
        VirtualProtect(GP(page << 12), 0x1000, PAGE_READWRITE, &old);
        if (trap_take(page)) {
            const int fi = trap_flags_of(page << 12);
            trap_note(TC_KERNEL, fi < 0 ? 15 : fi, 1, who, frame);
        }
    }
}

/* a run of pages trapped: the bits first, then the protection (a fault always finds the bit) */
static int64_t trap_run(uint64_t lo_page, uint64_t n) {
    for (uint64_t k = 0; k < n; k++) trap_set(lo_page + k);
    DWORD old;
    if (VirtualProtect(GP(lo_page << 12), (SIZE_T)(n << 12), PAGE_NOACCESS, &old)) return (int64_t)n;
    int64_t done = 0;
    for (uint64_t k = 0; k < n; k++) {
        if (VirtualProtect(GP((lo_page + k) << 12), 0x1000, PAGE_NOACCESS, &old)) done++;
        else trap_take(lo_page + k);
    }
    return done;
}

static int trap_new_pool(uint64_t addr, uint64_t size, uint32_t flags) {
    if (trap_mode != 2) return 0;
    const int fi = trap_flag_index(flags);
    if (fi < 0 || (addr & 0xFFF) || !size) return 0;
    QueryWsFn query = (QueryWsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    const uint64_t n = (size + 0xFFF) >> 12;
    static WsInfo info[4096];
    static SRWLOCK lock = SRWLOCK_INIT;   /* (info is shared) */
    AcquireSRWLockExclusive(&lock);
    int64_t resident = 0;
    for (uint64_t q = 0; q < n && query;) {
        int m = 0;
        for (; m < 4096 && q < n; m++, q++) info[m].va = GP(addr + (q << 12)), info[m].attr = 0;
        if (query(GetCurrentProcess(), info, (DWORD)(m * sizeof(WsInfo))))
            for (int k = 0; k < m; k++) resident += info[k].attr & 1;
    }
    ReleaseSRWLockExclusive(&lock);
    InterlockedAdd64(&trap_new_resident[fi], resident);
    InterlockedAdd64(&trap_new_pages[fi], (LONG64)n);
    VirtualAlloc(GP(addr), (SIZE_T)(n << 12), MEM_COMMIT, PAGE_READWRITE);   /* (no RAM until touched) */
    InterlockedAdd64(&trap_armed_pages[fi], trap_run(addr >> 12, n));
    return 1;
}

static int trap_by_count(const void *a, const void *b) {
    const int64_t x = ((const TrapSite *)a)->pages, y = ((const TrapSite *)b)->pages;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void trap_dump(void) {
    static TrapSite snap[TRAP_SITES];
    AcquireSRWLockShared(&trap_sites_lock);
    const int n = trap_nsites;
    memcpy(snap, trap_sites, (size_t)n * sizeof(TrapSite));
    ReleaseSRWLockShared(&trap_sites_lock);
    qsort(snap, (size_t)n, sizeof(TrapSite), trap_by_count);
    const char *path = getenv("HOS_POOL_TRAP_OUT");
    FILE *out = fopen(path && *path ? path : "pooltrap.txt", "w");
    if (!out) return;
    fprintf(out, "# pooltrap at %.1f s: pages class flags rw thread frames\n", (hos_now_ns() - trap_t0) / 1e9);
    for (int f = 0; f < trap_nflags; f++) {
        int64_t touched = 0;
        for (int c = 0; c < TC_N; c++) touched += trap_count[f][c][0] + trap_count[f][c][1];
        fprintf(out, "total %#x trapped %lld imported %lld gone %lld untouched %lld", trap_flag_list[f], (long long)trap_armed_pages[f],
                (long long)trap_armed_imported[f], (long long)trap_gone[f], (long long)(trap_armed_pages[f] - touched - trap_gone[f]));
        for (int c = 0; c < TC_N; c++)
            fprintf(out, " %s_r %lld %s_w %lld", trap_class_name[c], (long long)trap_count[f][c][0], trap_class_name[c],
                    (long long)trap_count[f][c][1]);
        if (trap_mode == 2) fprintf(out, " made %lld resident_when_made %lld", (long long)trap_new_pages[f], (long long)trap_new_resident[f]);
        fprintf(out, "\n");
    }
    for (int i = 0; i < n; i++) {
        const TrapSite *s = &snap[i];
        char desc[80];
        trap_thread_desc(s->thread, desc, sizeof desc);
        fprintf(out, "site %lld %s %#x %s \"%s\"", (long long)s->pages, trap_class_name[s->cls],
                s->fi < trap_nflags ? trap_flag_list[s->fi] : 0, s->rw ? "w" : "r", desc);
        for (int k = 0; k < 4; k++) {
            if (s->cls == TC_KERNEL) break;
            char t[MAX_PATH + 32];
            trap_frame_text(s->frame[k], t, sizeof t);
            fprintf(out, " %s", s->frame[k] ? t : "-");
        }
        fprintf(out, "\n");
    }
    for (int i = 0; i < trap_nfirst && i < 64; i++) {
        char t[MAX_PATH + 32];
        trap_frame_text(trap_first[i].rip, t, sizeof t);
        fprintf(out, "first %#llx %s thread %lu %d\n", (unsigned long long)trap_first[i].guest, t, trap_first[i].tid, trap_first[i].rw);
    }
    fclose(out);
}

static void trap_report(int full) {
    static const char *const rwn[2] = {"read", "write"};
    for (int f = 0; f < trap_nflags; f++) {
        int64_t touched = 0;
        char line[512];
        int at = 0;
        for (int c = 0; c < TC_N; c++)
            for (int rw = 0; rw < 2; rw++) {
                touched += trap_count[f][c][rw];
                at += snprintf(line + at, sizeof line - at, " %s %s %lld MB;", trap_class_name[c], rwn[rw],
                               (long long)(trap_count[f][c][rw] >> 8));
            }
        hos_log("pooltrap: flags %#05x: %lld MB trapped (%lld MB of it in imported chunks), touched since%s pool ended %lld MB; "
                "untouched %lld MB", trap_flag_list[f], (long long)(trap_armed_pages[f] >> 8), (long long)(trap_armed_imported[f] >> 8),
                line, (long long)(trap_gone[f] >> 8), (long long)((trap_armed_pages[f] - touched - trap_gone[f]) >> 8));
        if (trap_mode == 2)
            hos_log("pooltrap: flags %#05x: %lld MB of pools made, %lld MB of their pages in RAM when made", trap_flag_list[f],
                    (long long)(trap_new_pages[f] >> 8), (long long)(trap_new_resident[f] >> 8));
    }
    hos_log("pooltrap: %lld first touches, %lld races, %d places", (long long)trap_faults, (long long)trap_races, trap_nsites);
    hos_log_flush();
    if (full) trap_dump();
}

static void trap_arm(void) {
    QueryWsFn query = (QueryWsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    if (!query) return;
    int rx_guest_imported(uint64_t lo, uint64_t hi);   /* (rx_backend.c) */
    static Pool snap[16384];
    AcquireSRWLockShared(&pools_lock);
    const int n = npools;
    memcpy(snap, pools, (size_t)n * sizeof(Pool));
    ReleaseSRWLockShared(&pools_lock);
    const int64_t now = hos_now_ns(), t0 = now;
    enum { BATCH = 4096 };
    static WsInfo info[BATCH];
    int64_t armed = 0;
    for (int i = 0; i < n; i++) {
        const int fi = trap_flag_index(snap[i].flags);
        if (fi < 0 || now - snap[i].made_ns < 2000000000ll || (snap[i].addr & 0xFFF)) continue;
        const uint64_t lo = snap[i].addr >> 12, hi = (snap[i].addr + snap[i].size) >> 12;
        for (uint64_t q = lo; q < hi;) {
            int m = 0;
            const uint64_t first = q;
            for (; m < BATCH && q < hi; m++, q++) info[m].va = GP(q << 12), info[m].attr = 0;
            if (!query(GetCurrentProcess(), info, (DWORD)(m * sizeof(WsInfo)))) continue;
            for (int k = 0; k < m;) {
                if (!(info[k].attr & 1) || trap_test(first + k)) { k++; continue; }
                int e = k;
                while (e < m && (info[e].attr & 1) && !trap_test(first + e)) e++;
                const int64_t got = trap_run(first + k, (uint64_t)(e - k));
                InterlockedAdd64(&trap_armed_pages[fi], got);
                armed += got;
                /* (in a chunk the back end imported: the GPU has this memory in a buffer, mirrored when bound as one) */
                if (rx_guest_imported((first + k) << 12, (first + e) << 12)) InterlockedAdd64(&trap_armed_imported[fi], got);
                k = e;
            }
        }
    }
    hos_log("pooltrap: %lld MB of resident pool pages trapped (%.0f ms)", (long long)(armed >> 8), (hos_now_ns() - t0) / 1e6);
    hos_log_flush();
}

static DWORD WINAPI trap_main(void *p) {
    const int at = (int)(uintptr_t)p;
    const char *ev = getenv("HOS_POOL_TRAP_EVERY"), *rp = getenv("HOS_POOL_TRAP_REPORT");
    const int every = ev ? atoi(ev) : 0, report_at = rp ? atoi(rp) : 105;
    const int64_t t0 = trap_t0;   /* (the times are from the start: hos_now_ns counts from the PC's) */
    int64_t next_arm = t0 + (int64_t)at * 1000000000ll, next_sum = next_arm + 5000000000ll, next_full = next_arm + 10000000000ll;
    int reported = 0;
    for (;;) {
        Sleep(250);
        const int64_t now = hos_now_ns();
        if (trap_mode == 1 && now >= next_arm) {
            trap_arm();
            next_arm = every > 0 ? now + (int64_t)every * 1000000000ll : INT64_MAX;
        }
        if (now >= next_sum) { trap_dump(); trap_report(0); next_sum = now + 5000000000ll; }
        if (now >= next_full || (!reported && now >= t0 + (int64_t)report_at * 1000000000ll)) {
            trap_report(1);
            if (now >= t0 + (int64_t)report_at * 1000000000ll) reported = 1;
            next_full = now + 10000000000ll;
        }
    }
    return 0;
}

static void trap_init(void) {
    const char *e = getenv("HOS_POOL_TRAP");
    const char *nw = getenv("HOS_POOL_TRAP_NEW");
    const int fresh = nw && atoi(nw) != 0;
    if (!fresh && !(e && atoi(e) > 0)) return;
    const char *fl = getenv("HOS_POOL_TRAP_FLAGS");
    if (!fl || !*fl) fl = "24,64,22";
    for (const char *s = fl; *s && trap_nflags < 15;) {
        char *end;
        const unsigned long v = strtoul(s, &end, 16);
        if (end == s) break;
        trap_flag_list[trap_nflags++] = (uint32_t)v;
        s = *end == ',' ? end + 1 : end;
    }
    trap_t0 = hos_now_ns();
    trap_bits = (uint64_t *)calloc((size_t)(HOS_GUEST_SPACE >> 12) / 64, sizeof(uint64_t));
    if (!trap_bits) return;
    trap_module_ranges();
    AddVectoredExceptionHandler(1, trap_handler);   /* (first: ahead of commit_on_touch) */
    trap_mode = fresh ? 2 : 1;
    hos_log("pooltrap: %s, flags %s%s", fresh ? "every page of new pools trapped" : "resident pool pages trapped", fl,
            fresh ? "" : " (HOS_POOL_TRAP)");
    CloseHandle(CreateThread(NULL, 0, trap_main, (void *)(uintptr_t)(e ? atoi(e) : 0), 0, NULL));
}

/* ---- (HeyListen, 2026-11-13) pools the game made without CPU access keep no zero pages in RAM. HOS_NOACCESS_RAM=0: off.
   NVN's CPU_NO_ACCESS pools (flag 0x1: 0x0a1 and 0x021 in OoT, ~550 + ~290 MB of storage) are memory the game's CPU code never
   reads or writes after nvnMemoryPoolInitialize: that is NVN's contract, and a trap of every page of them (HOS_POOL_TRAP_NEW,
   above; castle, 2026-11-13) saw no touch from lifted code. They were in RAM all the same (417 MB of the 0x0a1 pools on a castle
   walk, every byte of it zero) because the back end touched them: texture uploads read the storage of render targets the game
   never wrote (a read of a page nobody wrote gives it a zero page), and imports of 16 MB chunks around a neighbouring pool's
   binding had the driver lock them in. The back end now leaves them alone where it can (gpu/vk: no upload of texture storage
   that is all reserved, no import of a no-access pool's pages for another pool's binding), and this thread gives back to
   Windows, every 5 s, the pages of those pools that are in RAM and hold only zeros: decommitted, they read as zeros again (a
   touch commits a zero page: mkw_glue.c commit_on_touch), so what anyone reads there is what it was. What this relies on:
   - the game does not write those pools while they live (NVN's contract). The pages are checked for zeros, and decommitted
     under the pool list's lock, so a pool that ends meanwhile (its memory going back to the game's heap) waits;
   - the back end writes guest memory through nvn_gpu_write (texture write-backs, replay writes), which takes discard_lock
     shared: a page is checked and decommitted under it exclusive, so a write cannot land between the check and the decommit;
   - pages the back end imported (the driver locks them: a decommit would leave the GPU on other pages than the CPU) are never
     decommitted: an import adds its range to a list first (hos_noaccess_import, exported, called by the back end under
     discard_lock shared before it commits and imports), and pages in any range ever imported are left alone;
   - the heap and the address regions only (where commit_on_touch commits on first touch), and not with large pages. */
static SRWLOCK discard_lock = SRWLOCK_INIT;
static int discard_state = -1;   /* (-1 not read yet, 0 off, 1 on) */
static uint64_t (*imported_ranges)[2];
static int nimported, cap_imported;
static volatile LONG64 discard_pages, discard_rounds;
static int discard_on(void) {
    if (discard_state < 0) {
        const char *e = getenv("HOS_NOACCESS_RAM");
        extern int hos_large_pages;
        discard_state = !(e && e[0] == '0') && hos_lazy_commit() && !hos_large_pages;
    }
    return discard_state;
}
/* (the back end, before it commits and imports [lo, hi)) */
__declspec(dllexport) void hos_noaccess_import(uint64_t lo, uint64_t hi) {
    hl_mem_imported(lo, hi);   /* (HL_MEM_COLD: pinned pages are never taken out of the working set) */
    hl_drop_import(lo, hi);    /* (HL_MEM_DROP: never dropped; a decision under way there ends first) */
    /* (HOS_GUEST_PAGES: pinned granules are never dropped, and dropped ones are restored before the driver locks them) */
    if (hos_gp_ready() && hi > lo) hos_gp_imported(GP(lo), (size_t)(hi - lo));
    if (!discard_on() || hi <= lo) return;
    AcquireSRWLockExclusive(&discard_lock);   /* (the list changes: exclusive; imports are rare) */
    int merged = 0;
    for (int i = 0; i < nimported && !merged; i++)
        if (lo <= imported_ranges[i][1] && hi >= imported_ranges[i][0]) {
            if (lo < imported_ranges[i][0]) imported_ranges[i][0] = lo;
            if (hi > imported_ranges[i][1]) imported_ranges[i][1] = hi;
            merged = 1;
        }
    if (!merged) {
        if (nimported == cap_imported) {
            cap_imported = cap_imported ? cap_imported * 2 : 256;
            imported_ranges = (uint64_t(*)[2])realloc(imported_ranges, sizeof *imported_ranges * (size_t)cap_imported);
        }
        imported_ranges[nimported][0] = lo, imported_ranges[nimported][1] = hi;
        nimported++;
    }
    ReleaseSRWLockExclusive(&discard_lock);
}
static int ever_imported(uint64_t lo, uint64_t hi) {   /* (discard_lock held) */
    for (int i = 0; i < nimported; i++)
        if (lo < imported_ranges[i][1] && hi > imported_ranges[i][0]) return 1;
    return 0;
}
/* (nvn_native.c nvn_gpu_write: the back end's writes into guest memory, around their copy) */
void hos_noaccess_write(int begin) {
    if (discard_state <= 0) return;   /* (not on, or not started: the thread reads the switch before its first decommit) */
    if (begin) AcquireSRWLockShared(&discard_lock);
    else ReleaseSRWLockShared(&discard_lock);
}
static int pool_still(uint64_t addr, uint64_t size, uint32_t flags) {   /* (pools_lock held) */
    for (int i = 0; i < npools; i++)
        if (pools[i].addr == addr && pools[i].size == size && pools[i].flags == flags) return 1;
    return 0;
}
static DWORD WINAPI discard_main(void *unused) {
    (void)unused;
    QueryWsFn query = (QueryWsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    if (!query) return 0;
    static Pool snap[16384];
    enum { SLICE = 256 };   /* (pages a look: 1 MB) */
    WsInfo info[SLICE];
    for (;;) {
        Sleep(5000);
        if (trap_mode) continue;   /* (HOS_POOL_TRAP has pages NOACCESS: not read here) */
        AcquireSRWLockShared(&pools_lock);
        int n = 0;
        for (int i = 0; i < npools; i++)
            if ((pools[i].flags & 0x1) && pools[i].addr >= HOS_HEAP_BASE && !(pools[i].addr & 0xFFF)) snap[n++] = pools[i];
        ReleaseSRWLockShared(&pools_lock);
        const int64_t t0 = hos_now_ns();
        int64_t got = 0;
        for (int i = 0; i < n; i++) {
            const uint64_t lo = snap[i].addr >> 12, hi = (snap[i].addr + snap[i].size) >> 12;
            for (uint64_t q = lo; q < hi; q += SLICE) {
                const int m = (int)(hi - q < SLICE ? hi - q : SLICE);
                for (int k = 0; k < m; k++) info[k].va = GP((q + k) << 12), info[k].attr = 0;
                if (!query(GetCurrentProcess(), info, (DWORD)(m * sizeof(WsInfo)))) continue;
                int any = 0;   /* (pages are read only under the locks below: the pool may end meanwhile, and its memory with it) */
                for (int k = 0; k < m && !any; k++) any = (int)(info[k].attr & 1);
                if (!any) continue;
                AcquireSRWLockExclusive(&discard_lock);
                AcquireSRWLockShared(&pools_lock);
                if (pool_still(snap[i].addr, snap[i].size, snap[i].flags)) {
                    for (int k = 0; k < m;) {   /* (runs of resident zero pages, outside anything imported) */
                        if (!(info[k].attr & 1) || !page_is_zero(info[k].va)) { k++; continue; }
                        int e = k + 1;
                        while (e < m && (info[e].attr & 1) && page_is_zero(info[e].va)) e++;
                        const uint64_t a = (q + k) << 12, b = (q + e) << 12;
                        if (!ever_imported(a, b) && (hos_gp_ready() && hos_gp_touches(GP(a), (size_t)(b - a))
                                                         ? hos_gp_decommit(GP(a), (size_t)(b - a))
                                                         : VirtualFree(GP(a), (SIZE_T)(b - a), MEM_DECOMMIT)))
                            got += e - k;
                        k = e;
                    }
                }
                ReleaseSRWLockShared(&pools_lock);
                ReleaseSRWLockExclusive(&discard_lock);
            }
        }
        InterlockedAdd64(&discard_pages, got);
        const LONG64 rounds = InterlockedIncrement64(&discard_rounds);
        if (got || rounds <= 2)
            hos_log("mem: %lld MB of zero pages of %d pools without CPU access given back (%lld MB so far, %.1f ms)",
                    (long long)(got >> 8), n, (long long)(discard_pages >> 8), (hos_now_ns() - t0) / 1e6);
    }
    return 0;
}

void hos_memsplit_init(void) {
    const char *e = getenv("HOS_MEM_SPLIT");
    if (e && atoi(e) > 0) CloseHandle(CreateThread(NULL, 0, split_main, (void *)(uintptr_t)atoi(e), 0, NULL));
    trap_init();
    if (discard_on()) {
        hos_log("mem: zero pages of pools without CPU access given back every 5 s (HOS_NOACCESS_RAM=0: not)");
        CloseHandle(CreateThread(NULL, 0, discard_main, NULL, 0, NULL));
    }
    hl_mem_init();   /* (HL_MEM_COLD, hl_mem.c: after the trap, which it stays off for) */
    if (!hos_gp_wanted()) hl_drop_init();   /* (HL_MEM_DROP, hl_drop.c: says it is off; with the substrate it starts once the window is reserved) */
}
#else
void hos_memsplit_pool(uint64_t addr, uint64_t size, uint32_t flags, int add) { (void)addr; (void)size; (void)flags; (void)add; }
void hos_memsplit_init(void) {}
int hos_pools_with_flags(const uint32_t *flags, int nflags, int64_t made_before_ns, uint64_t (*out)[2], int cap) {
    (void)flags; (void)nflags; (void)made_before_ns; (void)out; (void)cap;
    return 0;
}
int hos_pool_trap_active(void) { return 0; }
void hos_pool_trap_release(uint64_t addr, uint64_t size, const char *who) { (void)addr; (void)size; (void)who; }
void hos_noaccess_import(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void hos_noaccess_write(int begin) { (void)begin; }
#endif
