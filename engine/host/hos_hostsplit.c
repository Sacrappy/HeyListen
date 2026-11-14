/* (HeyListen) HOS_HOST_SPLIT=<seconds>[,<every>]: where the process's memory is outside the game's own. The game's memory (the
   guest window) has its census in hos_memsplit.c (HOS_MEM_SPLIT); this is the rest: that long after the start, and then every
   <every> seconds (default: once), and at a normal exit, the whole address space is walked (VirtualQuery) and every committed
   page is given an owner and counted as committed and as resident (QueryWorkingSetEx). Owners, strongest first:
   - a tag: a range this engine named when it made it (hos_mem_tag, exported: the Vulkan back end finds it by name as it does
     hos_noaccess_import). One load and a branch when the switch is off;
   - a large block of a C or Windows heap (256 KB and up: the engine's big malloc and calloc buffers), then the heap's own
     regions (HeapWalk), by heap;
   - the stack of a host thread (guest threads' stacks are in the window);
   - an image, by module, and by section for the game's executable and the back end (static tables live there);
   - the guest window, as one line;
   - what is left: files mapped (by name), then private allocations and mapped sections with no owner, by the size of their
     allocation. This is where a driver's memory shows: it makes its allocations itself, and what called it is not known here
     (no VirtualAlloc hook: nothing says which call of the back end made which allocation).
   One table, the largest allocations with their addresses, and a line of totals that reconciles the pages counted with the
   process's working set and private commit, saying what it could not explain. Windows only. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

typedef struct { uint64_t lo, size; char name[40]; } Tag;
enum { MAX_TAGS = 8192 };
static Tag tags[MAX_TAGS];
static int ntags, tags_dropped;
static SRWLOCK tags_lock = SRWLOCK_INIT;
static int hs_first = -1, hs_every, hs_ledger;   /* (seconds of the first census, then between; -1: not read yet, 0: off) */

/* the tags are kept when either reads them: this census, or the back end's ledger (RX_VK_LEDGER), which asks for the census's
   totals itself (hos_hostsplit_totals) */
static int hs_enabled(void) {
    if (hs_first < 0) {
        const char *e = getenv("HOS_HOST_SPLIT");
        hs_first = e && atoi(e) > 0 ? atoi(e) : 0;
        const char *c = e ? strpbrk(e, ",:") : NULL;   /* (a batch file splits at a comma: HOS_HOST_SPLIT=60:60 there) */
        hs_every = c ? atoi(c + 1) : 0;
        const char *l = getenv("RX_VK_LEDGER");
        hs_ledger = l && atoi(l) > 0;
    }
    return hs_first > 0 || hs_ledger;
}

/* a range of this process's memory that this engine made, named for the census (the name is copied: 39 characters at most) */
__declspec(dllexport) void hos_mem_tag(const void *ptr, uint64_t size, const char *name) {
    if (!ptr || !size || !hs_enabled()) return;
    AcquireSRWLockExclusive(&tags_lock);
    int i = 0;
    while (i < ntags && tags[i].lo != (uint64_t)(uintptr_t)ptr) i++;
    if (i == ntags && ntags < MAX_TAGS) ntags++;
    if (i < ntags) {
        tags[i].lo = (uint64_t)(uintptr_t)ptr, tags[i].size = size;
        snprintf(tags[i].name, sizeof tags[i].name, "%s", name ? name : "tagged");
    } else tags_dropped++;
    ReleaseSRWLockExclusive(&tags_lock);
}
/* its memory was freed: the range is no longer named (an address the system hands out again is not this one's) */
__declspec(dllexport) void hos_mem_untag(const void *ptr) {
    if (!ptr || !hs_enabled()) return;
    AcquireSRWLockExclusive(&tags_lock);
    for (int i = 0; i < ntags; i++)
        if (tags[i].lo == (uint64_t)(uintptr_t)ptr) { tags[i] = tags[--ntags]; break; }
    ReleaseSRWLockExclusive(&tags_lock);
}

typedef struct { void *va; ULONG_PTR attr; } WsInfo;
typedef BOOL(WINAPI *QueryWsFn)(HANDLE, PVOID, DWORD);
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
                 QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage; } MemCounters;

/* ---- owners */
enum { G_GUEST, G_TAG, G_BAR, G_HEAP, G_STACK, G_IMAGE, G_DRIVER_IMAGE, G_MAPPED, G_OTHER, G_N };
static const char *const group_name[G_N] = {"guest window (see HOS_MEM_SPLIT)", "tagged by this engine", "tagged: VRAM mapped through the BAR (not RAM)", "C and Windows heaps",
                                            "host thread stacks", "images (exe, back end, system DLLs)", "images of graphics drivers",
                                            "mapped sections and files", "private, no owner (the driver's, or no one known)"};
typedef struct { char name[72]; int group; uint64_t committed, resident, count, last_alloc; } Owner;
enum { MAX_OWNERS = 512 };
static Owner owners[MAX_OWNERS];
static int nowners;
static int owner_of(const char *name, int group) {
    for (int i = 0; i < nowners; i++)
        if (!strcmp(owners[i].name, name)) return i;
    if (nowners == MAX_OWNERS) return 0;   /* (the first one catches the overflow: the guest window's, never an image) */
    Owner *o = &owners[nowners];
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->group = group;
    return nowners++;
}

/* ---- spans: [lo, hi) with an owner, sorted, not overlapping; a later paint takes what it covers */
typedef struct { uint64_t lo, hi; int owner; } Span;
static Span *spans, *spans2;
static int nspans, capspans;
static void paint(uint64_t lo, uint64_t hi, int owner) {
    if (hi <= lo) return;
    if (nspans + 3 > capspans) {
        capspans = capspans ? capspans * 2 : 4096;
        spans = (Span *)realloc(spans, sizeof(Span) * (size_t)capspans);
        spans2 = (Span *)realloc(spans2, sizeof(Span) * (size_t)capspans);
    }
    int n = 0, i = 0;
    for (; i < nspans && spans[i].hi <= lo; i++) spans2[n++] = spans[i];
    if (i < nspans && spans[i].lo < lo) spans2[n++] = (Span){spans[i].lo, lo, spans[i].owner};
    spans2[n++] = (Span){lo, hi, owner};
    for (; i < nspans && spans[i].hi <= hi; i++) {}
    if (i < nspans && spans[i].lo < hi) spans2[n++] = (Span){hi, spans[i].hi, spans[i].owner}, i++;
    for (; i < nspans; i++) spans2[n++] = spans[i];
    Span *t = spans;
    spans = spans2, spans2 = t;
    nspans = n;
}

/* ---- what the process says of itself */
static const char *const driver_names[] = {"nvoglv64", "nvvk", "nvlddmkm", "nvapi64", "nvwgf2umx", "nvcuda", "nvd3dum", "nvngx", "amdvlk64", "amdxx64",
                                           "atiadlxx", "igvk64", "ig9icd64", "igd", "vulkan-1", "nvml", "libnvidia", NULL};
static int is_driver(const char *n) {
    for (int i = 0; driver_names[i]; i++)
        if (strstr(n, driver_names[i])) return 1;
    return 0;
}
static void paint_modules(void) {
    typedef BOOL(WINAPI * EnumFn)(HANDLE, HMODULE *, DWORD, LPDWORD);
    typedef DWORD(WINAPI * NameFn)(HANDLE, HMODULE, LPSTR, DWORD);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    EnumFn enum_mods = (EnumFn)(void *)GetProcAddress(k, "K32EnumProcessModules");
    NameFn base_name = (NameFn)(void *)GetProcAddress(k, "K32GetModuleBaseNameA");
    static HMODULE mods[2048];
    DWORD got = 0;
    if (!enum_mods || !base_name || !enum_mods(GetCurrentProcess(), mods, sizeof mods, &got)) return;
    const HMODULE exe = GetModuleHandleA(NULL), vk = GetModuleHandleA("hookshot_vk.dll");
    for (DWORD i = 0; i < got / sizeof(HMODULE) && i < 2048; i++) {
        char name[64] = "?";
        base_name(GetCurrentProcess(), mods[i], name, sizeof name);
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mods[i];
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)mods[i] + dos->e_lfanew);
        const uint64_t base = (uint64_t)(uintptr_t)mods[i];
        char label[72];
        snprintf(label, sizeof label, "image %s", name);
        paint(base, base + nt->OptionalHeader.SizeOfImage, owner_of(label, is_driver(name) ? G_DRIVER_IMAGE : G_IMAGE));
        if (mods[i] != exe && mods[i] != vk) continue;
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; s++) {   /* (static tables live in the sections: by section) */
            char sn[9] = {0};
            memcpy(sn, sec[s].Name, 8);
            snprintf(label, sizeof label, "image %s %s", name, sn);
            paint(base + sec[s].VirtualAddress, base + sec[s].VirtualAddress + sec[s].Misc.VirtualSize, owner_of(label, G_IMAGE));
        }
    }
}

typedef struct { LONG ExitStatus; PVOID Teb; PVOID Pid, Tid; ULONG_PTR Affinity; LONG Priority, BasePriority; } ThreadBasic;
static int paint_stacks(void) {
    typedef LONG(NTAPI * QueryThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
    QueryThreadFn qt = (QueryThreadFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!qt || snap == INVALID_HANDLE_VALUE) return 0;
    const int stacks = owner_of("host thread stacks", G_STACK);
    THREADENTRY32 te = {sizeof te};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        ThreadBasic tb;
        if (h && qt(h, 0, &tb, sizeof tb, NULL) >= 0 && tb.Teb) {
            const NT_TIB *tib = (const NT_TIB *)tb.Teb;
            const uint64_t dealloc = (uint64_t)(uintptr_t) * (void *const *)((const uint8_t *)tb.Teb + 0x1478);   /* (TEB.DeallocationStack) */
            const uint64_t top = (uint64_t)(uintptr_t)tib->StackBase;
            if (dealloc && top > dealloc && top - dealloc < (1ull << 32)) paint(dealloc, top, stacks), n++;
        }
        if (h) CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

/* heaps: each one's regions, and its large busy blocks apart. Reported against HeapSummary (the engine's own "C heaps" line) */
typedef struct { uint64_t allocated, committed, region_committed, large_bytes; int large_n; } HeapNote;
static HeapNote heap_notes[64];
/* (the heaps' blocks over 256 KB: by size, and the biggest few: what the engine's own big malloc and calloc buffers are) */
enum { BIG_BINS = 5, BIG_TOP = 8 };
static uint64_t big_bytes[BIG_BINS], big_count[BIG_BINS], big_top[BIG_TOP];
static const char *const big_bin_name[BIG_BINS] = {"256 KB to 1 MB", "1 to 4 MB", "4 to 16 MB", "16 to 64 MB", "64 MB and up"};
static void big_note(uint64_t size) {
    const int k = size >= (64u << 20) ? 4 : size >= (16u << 20) ? 3 : size >= (4u << 20) ? 2 : size >= (1u << 20) ? 1 : 0;
    big_bytes[k] += size, big_count[k]++;
    int at = BIG_TOP;
    while (at > 0 && size > big_top[at - 1]) at--;
    if (at < BIG_TOP) {
        memmove(&big_top[at + 1], &big_top[at], (BIG_TOP - 1 - at) * sizeof big_top[0]);
        big_top[at] = size;
    }
}
static int nheaps;
static int64_t heap_ms;
static void paint_heaps(void) {
    typedef struct { DWORD cb; SIZE_T allocated, committed, reserved, max_reserve; } Summary;
    typedef BOOL(WINAPI * SumFn)(HANDLE, DWORD, Summary *);
    SumFn sum = (SumFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapSummary");
    HANDLE hs[64];
    nheaps = (int)GetProcessHeaps(64, hs);
    if (nheaps > 64) nheaps = 64;
    const int64_t t0 = hos_now_ns();
    memset(big_bytes, 0, sizeof big_bytes), memset(big_count, 0, sizeof big_count), memset(big_top, 0, sizeof big_top);
    for (int i = 0; i < nheaps; i++) {
        char label[72];
        snprintf(label, sizeof label, "C/Win heap %d (%s)", i, hs[i] == GetProcessHeap() ? "process heap" : "other");
        const int region_owner = owner_of(label, G_HEAP);
        snprintf(label, sizeof label, "C/Win heap %d, blocks over 256 KB", i);
        const int large_owner = owner_of(label, G_HEAP);
        memset(&heap_notes[i], 0, sizeof heap_notes[i]);
        Summary su = {sizeof su};
        if (sum && sum(hs[i], 0, &su)) heap_notes[i].allocated = su.allocated, heap_notes[i].committed = su.committed;
        if (!HeapLock(hs[i])) continue;   /* (nothing between the lock and the unlock allocates or logs) */
        PROCESS_HEAP_ENTRY e;
        memset(&e, 0, sizeof e);
        while (HeapWalk(hs[i], &e)) {
            if (e.wFlags & PROCESS_HEAP_REGION) {
                const uint64_t lo = (uint64_t)(uintptr_t)e.lpData;
                paint(lo, lo + e.Region.dwCommittedSize + e.Region.dwUnCommittedSize, region_owner);
                heap_notes[i].region_committed += e.Region.dwCommittedSize;
            } else if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) && e.cbData >= (256u << 10)) {
                const uint64_t lo = (uint64_t)(uintptr_t)e.lpData;
                paint(lo, lo + e.cbData, large_owner);
                heap_notes[i].large_bytes += e.cbData, heap_notes[i].large_n++;
                big_note(e.cbData);
            }
        }
        HeapUnlock(hs[i]);
    }
    heap_ms = (hos_now_ns() - t0) / 1000000;
}

/* ---- the walk */
typedef struct { uint64_t base, size, abase; DWORD state, protect, type; int alloc; } Reg;
typedef struct { uint64_t base, committed, resident; DWORD type, protect; int owner_default, owner_first; } Alloc;
static int64_t start_ns;   /* (hos_now_ns counts from the PC's start) */
static Reg *regs;
static Alloc *allocs;
static int nregs, capregs, nallocs, capallocs;
static SRWLOCK census_lock = SRWLOCK_INIT;

static const char *prot_name(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return "---";
    case PAGE_READONLY: return "r--";
    case PAGE_READWRITE: return "rw-";
    case PAGE_WRITECOPY: return "rwc";
    case PAGE_EXECUTE: return "--x";
    case PAGE_EXECUTE_READ: return "r-x";
    case PAGE_EXECUTE_READWRITE: return "rwx";
    default: return "???";
    }
}
static const char *size_class(uint64_t committed) {
    const uint64_t mb = committed >> 20;
    return mb >= 512 ? "512 MB and up" : mb >= 128 ? "128-512 MB" : mb >= 64 ? "64-128 MB" : mb >= 32 ? "32-64 MB" : mb >= 8 ? "8-32 MB" : mb >= 1 ? "1-8 MB" : "under 1 MB";
}
static int by_committed_desc(const void *a, const void *b) {
    const Alloc *x = (const Alloc *)a, *y = (const Alloc *)b;
    return x->committed < y->committed ? 1 : x->committed > y->committed ? -1 : 0;
}
static int by_resident_desc(const void *a, const void *b) {
    const Owner *x = (const Owner *)a, *y = (const Owner *)b;
    return x->resident < y->resident ? 1 : x->resident > y->resident ? -1 : 0;
}
#define MB(x) ((unsigned long long)((x) >> 20))

/* (the ledger's numbers out of a census: see hos_hostsplit_totals) */
enum { T_WS, T_PRIVATE, T_RAM_COUNTED, T_WIN_RES, T_WIN_COM, T_BAR, T_GROUP_COM, T_GROUP_RES = T_GROUP_COM + G_N, T_BIG_RES = T_GROUP_RES + G_N,
       T_BIG_COM, T_WALK_MS, T_N };
static void census(const char *why, uint64_t *totals) {
    if (totals) AcquireSRWLockExclusive(&census_lock);
    else if (!TryAcquireSRWLockExclusive(&census_lock)) return;
    typedef BOOL(WINAPI * MappedFn)(HANDLE, LPVOID, LPSTR, DWORD);
    typedef BOOL(WINAPI * MemInfoFn)(HANDLE, MemCounters *, DWORD);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    QueryWsFn query = (QueryWsFn)(void *)GetProcAddress(k, "K32QueryWorkingSetEx");
    MappedFn mapped_name = (MappedFn)(void *)GetProcAddress(k, "K32GetMappedFileNameA");
    MemInfoFn mem_info = (MemInfoFn)(void *)GetProcAddress(k, "K32GetProcessMemoryInfo");
    if (!query) { hos_log("hostsplit: no K32QueryWorkingSetEx"); ReleaseSRWLockExclusive(&census_lock); return; }
    const int64_t t0 = hos_now_ns();
    nowners = 0, nspans = 0, nregs = 0, nallocs = 0;
    owner_of("guest window", G_GUEST);   /* (owner 0) */

    /* the owners, weakest paint first */
    const uint64_t win_lo = (uint64_t)(uintptr_t)GP(0);
    paint(win_lo, win_lo + HOS_GUEST_SPACE, 0);
    paint_modules();
    const int nthreads = paint_stacks();
    paint_heaps();
    AcquireSRWLockShared(&tags_lock);
    for (int i = 0; i < ntags; i++) paint(tags[i].lo, tags[i].lo + tags[i].size, owner_of(tags[i].name, strstr(tags[i].name, ", BAR") ? G_BAR : G_TAG));
    ReleaseSRWLockShared(&tags_lock);

    /* pass 1: every region, grouped by allocation */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t reserved_total = 0;
    for (uint64_t a = 0; a < (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        const uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_RESERVE) reserved_total += size;
        if (mbi.State == MEM_COMMIT) {
            if (nregs == capregs) { capregs = capregs ? capregs * 2 : 8192; regs = (Reg *)realloc(regs, sizeof(Reg) * (size_t)capregs); }
            const uint64_t abase = (uint64_t)(uintptr_t)mbi.AllocationBase;
            if (!nallocs || allocs[nallocs - 1].base != abase) {
                if (nallocs == capallocs) { capallocs = capallocs ? capallocs * 2 : 4096; allocs = (Alloc *)realloc(allocs, sizeof(Alloc) * (size_t)capallocs); }
                allocs[nallocs++] = (Alloc){abase, 0, 0, mbi.Type, mbi.Protect, -1, -1};
            }
            allocs[nallocs - 1].committed += size;
            regs[nregs++] = (Reg){base, size, abase, mbi.State, mbi.Protect, mbi.Type, nallocs - 1};
        }
        if (base + size <= a) break;
        a = base + size;
    }
    /* the default owner of an allocation no span covers: a mapped file by name, or a class by the allocation's size */
    for (int i = 0; i < nallocs; i++) {
        Alloc *a = &allocs[i];
        char label[96];
        if (a->type == MEM_IMAGE) a->owner_default = owner_of("image (no module)", G_IMAGE);
        else if (a->type == MEM_MAPPED) {
            char path[MAX_PATH] = "";
            const char *b = NULL;
            if (mapped_name && mapped_name(GetCurrentProcess(), (void *)(uintptr_t)a->base, path, sizeof path)) {
                b = strrchr(path, '\\');
                b = b ? b + 1 : path;
            }
            if (b && *b) snprintf(label, sizeof label, "mapped file %s", b);
            else snprintf(label, sizeof label, "mapped section, no file, %s", size_class(a->committed));
            a->owner_default = owner_of(label, G_MAPPED);
        } else {
            snprintf(label, sizeof label, "private %s", size_class(a->committed));
            a->owner_default = owner_of(label, G_OTHER);
        }
    }

    /* pass 2: residency, page by page in address order; the spans' cursor only moves forward */
    enum { BATCH = 4096 };
    static WsInfo info[BATCH];
    int si_cursor = 0;
    uint64_t total_resident = 0, total_committed = 0, private_committed = 0;
    for (int ri = 0; ri < nregs; ri++) {
        const Reg *r = &regs[ri];
        Alloc *al = &allocs[r->alloc];
        for (uint64_t q = r->base; q < r->base + r->size;) {
            int m = 0;
            for (; m < BATCH && q < r->base + r->size; m++, q += 0x1000) info[m].va = (void *)(uintptr_t)q, info[m].attr = 0;
            const BOOL ok = query(GetCurrentProcess(), info, (DWORD)(m * sizeof(WsInfo)));
            for (int p = 0; p < m; p++) {
                const uint64_t a = (uint64_t)(uintptr_t)info[p].va;
                while (si_cursor < nspans && spans[si_cursor].hi <= a) si_cursor++;
                const int o = si_cursor < nspans && spans[si_cursor].lo <= a ? spans[si_cursor].owner : al->owner_default;
                const int res = ok && (info[p].attr & 1);
                if (al->owner_first < 0) al->owner_first = o;
                Owner *ow = &owners[o];
                ow->committed += 0x1000;
                total_committed += 0x1000;
                if (r->type == MEM_PRIVATE) private_committed += 0x1000;
                if (res) ow->resident += 0x1000, al->resident += 0x1000, total_resident += 0x1000;
                if (ow->last_alloc != al->base) ow->last_alloc = al->base, ow->count++;
            }
        }
    }

    /* the report */
    MemCounters mc = {sizeof mc};
    if (mem_info) mem_info(GetCurrentProcess(), &mc, sizeof mc);
    const int64_t walk_ms = (hos_now_ns() - t0) / 1000000;
    if (totals) {   /* (the ledger's census: the numbers, no report) */
        memset(totals, 0, sizeof(uint64_t) * T_N);
        for (int i = 0; i < nowners; i++) {
            totals[T_GROUP_COM + owners[i].group] += owners[i].committed, totals[T_GROUP_RES + owners[i].group] += owners[i].resident;
            if (!strcmp(owners[i].name, "private 512 MB and up") || !strcmp(owners[i].name, "private 128-512 MB"))
                totals[T_BIG_RES] += owners[i].resident, totals[T_BIG_COM] += owners[i].committed;
        }
        totals[T_WS] = mc.WorkingSetSize, totals[T_PRIVATE] = mc.PrivateUsage;
        totals[T_BAR] = totals[T_GROUP_RES + G_BAR];
        totals[T_RAM_COUNTED] = total_resident - totals[T_BAR];
        totals[T_WIN_RES] = owners[0].resident, totals[T_WIN_COM] = owners[0].committed;
        totals[T_WALK_MS] = (uint64_t)walk_ms;
        (void)why;
        ReleaseSRWLockExclusive(&census_lock);
        return;
    }
    hos_log("hostsplit: --- %s, %.0f s after the start (%lld ms to walk: %d regions, %d allocations, %d host threads) ---", why,
            (double)(hos_now_ns() - start_ns) / 1e9, (long long)walk_ms, nregs, nallocs, nthreads);
    uint64_t gc[G_N] = {0}, gr[G_N] = {0}, gn[G_N] = {0};
    for (int i = 0; i < nowners; i++) gc[owners[i].group] += owners[i].committed, gr[owners[i].group] += owners[i].resident, gn[owners[i].group] += owners[i].count;
    hos_log("hostsplit: %-52s %9s %9s %6s", "group", "commit MB", "in RAM MB", "count");
    for (int g = 0; g < G_N; g++)
        hos_log("hostsplit: %-52s %9llu %9llu %6llu", group_name[g], MB(gc[g]), MB(gr[g]), (unsigned long long)gn[g]);
    /* the owners one by one (those of 16 MB or more, in RAM or committed), most resident first */
    static Owner sorted[MAX_OWNERS];
    memcpy(sorted, owners, sizeof(Owner) * (size_t)nowners);
    qsort(sorted, (size_t)nowners, sizeof(Owner), by_resident_desc);
    hos_log("hostsplit: -- owners of 16 MB or more (by RAM) --");
    for (int i = 0; i < nowners; i++)
        if (sorted[i].committed >= (16u << 20) || sorted[i].resident >= (16u << 20))
            hos_log("hostsplit: %-52s %9llu %9llu %6llu", sorted[i].name, MB(sorted[i].committed), MB(sorted[i].resident), (unsigned long long)sorted[i].count);
    hos_log("hostsplit: -- heaps: HeapSummary (the C heaps line's numbers) against what the walk painted (%lld ms) --", (long long)heap_ms);
    for (int i = 0; i < nheaps; i++)
        if (heap_notes[i].committed >= (1u << 20))
            hos_log("hostsplit: heap %d: %llu MB in use of %llu MB committed; walk: regions %llu MB committed, %d blocks over 256 KB of %llu MB", i,
                    MB(heap_notes[i].allocated), MB(heap_notes[i].committed), MB(heap_notes[i].region_committed), heap_notes[i].large_n, MB(heap_notes[i].large_bytes));
    {
        char top[160] = "";
        int at = 0;
        for (int k = 0; k < BIG_TOP && big_top[k] && at < (int)sizeof top - 12; k++) at += snprintf(top + at, sizeof top - (size_t)at, " %llu", MB(big_top[k]));
        hos_log("hostsplit: heap blocks over 256 KB, MB (count): %s %llu (%llu), %s %llu (%llu), %s %llu (%llu), %s %llu (%llu), %s %llu (%llu); the largest, MB:%s",
                big_bin_name[0], MB(big_bytes[0]), (unsigned long long)big_count[0], big_bin_name[1], MB(big_bytes[1]), (unsigned long long)big_count[1],
                big_bin_name[2], MB(big_bytes[2]), (unsigned long long)big_count[2], big_bin_name[3], MB(big_bytes[3]), (unsigned long long)big_count[3],
                big_bin_name[4], MB(big_bytes[4]), (unsigned long long)big_count[4], top);
    }
    /* the largest allocations outside the window */
    qsort(allocs, (size_t)nallocs, sizeof(Alloc), by_committed_desc);
    hos_log("hostsplit: -- the 30 largest allocations outside the guest window: base, committed, in RAM, protection, type, owner of its first page --");
    int shown = 0;
    for (int i = 0; i < nallocs && shown < 30; i++) {
        const Alloc *a = &allocs[i];
        if (a->owner_first == 0) continue;
        hos_log("hostsplit: %#016llx %6llu MB %6llu MB %s %-7s %s", (unsigned long long)a->base, MB(a->committed), MB(a->resident), prot_name(a->protect),
                a->type == MEM_IMAGE ? "image" : a->type == MEM_MAPPED ? "mapped" : "private", owners[a->owner_first].name);
        shown++;
    }
    /* private allocations by size class: how many, how much */
    {
        uint64_t n[8] = {0}, c[8] = {0}, r[8] = {0};
        for (int i = 0; i < nallocs; i++) {
            const Alloc *a = &allocs[i];
            if (a->type != MEM_PRIVATE || a->owner_first == 0) continue;
            const uint64_t mb = a->committed >> 20;
            const int k = mb >= 512 ? 6 : mb >= 128 ? 5 : mb >= 64 ? 4 : mb >= 32 ? 3 : mb >= 8 ? 2 : mb >= 1 ? 1 : 0;
            n[k]++, c[k] += a->committed, r[k] += a->resident;
        }
        static const char *const cls[7] = {"under 1 MB", "1-8 MB", "8-32 MB", "32-64 MB", "64-128 MB", "128-512 MB", "512 MB and up"};
        hos_log("hostsplit: -- every private allocation outside the window, by the size of the allocation --");
        for (int k = 6; k >= 0; k--) hos_log("hostsplit: %-52s %9llu %9llu %6llu", cls[k], MB(c[k]), MB(r[k]), (unsigned long long)n[k]);
    }
    const uint64_t window_res = owners[0].resident, window_com = owners[0].committed, bar_res = gr[G_BAR];
    /* (pages of VRAM that the driver mapped into the process: QueryWorkingSetEx calls them valid, the working set does not count them) */
    const uint64_t ram_counted = total_resident - bar_res;
    const int64_t gap_ws = (int64_t)mc.WorkingSetSize - (int64_t)ram_counted;
    const int64_t gap_pr = (int64_t)mc.PrivateUsage - (int64_t)private_committed;
    hos_log("hostsplit: totals: RAM counted %llu MB (guest window %llu, the rest %llu; %llu MB of VRAM mapped through the BAR left out) against the working set %llu MB (peak %llu): working set less counted %lld MB",
            MB(ram_counted), MB(window_res), MB(ram_counted - window_res), MB(bar_res), MB(mc.WorkingSetSize), MB(mc.PeakWorkingSetSize), (long long)(gap_ws / (1 << 20)));
    hos_log("hostsplit: totals: private pages committed %llu MB (guest window %llu, the rest %llu) against the process's private commit %llu MB: commit charge less pages counted %lld MB; all committed %llu MB, address space reserved with nothing committed %llu MB",
            MB(private_committed), MB(window_com), MB(private_committed - window_com), MB(mc.PrivateUsage), (long long)(gap_pr / (1 << 20)), MB(total_committed), MB(reserved_total));
    {   /* (one line a census, to follow a run: RAM first, then what is only committed) */
        static const char *const classes[] = {"private 512 MB and up", "private 128-512 MB", "private 64-128 MB", "private 32-64 MB", "private 8-32 MB", "private 1-8 MB", "private under 1 MB"};
        uint64_t cr[7] = {0}, cc[7] = {0};
        for (int i = 0; i < nowners; i++)
            for (int k = 0; k < 7; k++)
                if (!strcmp(owners[i].name, classes[k])) cr[k] = owners[i].resident, cc[k] = owners[i].committed;
        hos_log("hostsplit: timeline %.0f s: RAM, MB: window %llu, tagged %llu, heaps %llu, images %llu, stacks %llu, mapped %llu, private by size 512+ %llu, 128-512 %llu, 64-128 %llu, 32-64 %llu, 8-32 %llu, under 8 %llu; "
                "counted %llu against the working set %llu; committed only: private 512+ %llu, 128-512 %llu, 32-128 %llu, BAR %llu, commit charge %llu",
                (double)(hos_now_ns() - start_ns) / 1e9, MB(window_res), MB(gr[G_TAG]), MB(gr[G_HEAP]), MB(gr[G_IMAGE] + gr[G_DRIVER_IMAGE]), MB(gr[G_STACK]), MB(gr[G_MAPPED]),
                MB(cr[0]), MB(cr[1]), MB(cr[2]), MB(cr[3]), MB(cr[4]), MB(cr[5] + cr[6]), MB(ram_counted), MB(mc.WorkingSetSize), MB(cc[0]), MB(cc[1]), MB(cc[2] + cc[3]), MB(gc[G_BAR]), MB(mc.PrivateUsage));
    }
    if (tags_dropped) hos_log("hostsplit: %d tags did not fit", tags_dropped);
    hos_log_flush();
    ReleaseSRWLockExclusive(&census_lock);
}

static DWORD WINAPI hostsplit_main(void *unused) {
    (void)unused;
    Sleep((DWORD)hs_first * 1000);
    census("census", NULL);
    while (hs_every > 0) {
        Sleep((DWORD)hs_every * 1000);
        census("census", NULL);
    }
    return 0;
}
static void hostsplit_at_exit(void) { census("at exit", NULL); }

/* (RX_VK_LEDGER, gpu/vk/vk_ledger.cpp, finds it by name) a census made now, quietly, its totals in out[0..n): the working set,
   the private commit, the RAM counted, the guest window's RAM and commit, the VRAM mapped through the BAR, then each group's commit
   and RAM (G_GUEST .. G_OTHER), the private allocations of 128 MB and up (the driver's arenas) in RAM and committed, the walk's
   milliseconds. Bytes. The number of values written (0: not on Windows, or no QueryWorkingSetEx). */
__declspec(dllexport) int hos_hostsplit_totals(uint64_t *out, int n) {
    uint64_t t[T_N];
    t[T_WALK_MS] = ~0ull;
    if (!start_ns) start_ns = hos_now_ns();
    census("ledger", t);
    if (t[T_WALK_MS] == ~0ull) return 0;
    const int m = n < T_N ? n : T_N;
    memcpy(out, t, sizeof(uint64_t) * (size_t)m);
    return m;
}

/* (RX_VK_LEDGER) the pages of guest [guest, guest + npages * 4 KB), one byte each in out: 1 committed, 2 in the working set.
   VirtualQuery for the commit, QueryWorkingSetEx for the residency; nothing is touched. 1 when done */
__declspec(dllexport) int hos_ledger_pages(uint64_t guest, uint64_t npages, uint8_t *out) {
    static QueryWsFn query;
    if (!query) query = (QueryWsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    if (!query) return 0;
    memset(out, 0, (size_t)npages);
    const uint64_t lo = (uint64_t)(uintptr_t)GP(guest), hi = lo + npages * 0x1000;
    enum { BATCH = 1024 };
    WsInfo info[BATCH];
    for (uint64_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        const uint64_t rend = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize, end = rend < hi ? rend : hi;
        if (mbi.State == MEM_COMMIT)
            for (uint64_t q = a; q < end;) {
                int m = 0;
                for (; m < BATCH && q < end; m++, q += 0x1000) info[m].va = (void *)(uintptr_t)q, info[m].attr = 0;
                const BOOL ok = query(GetCurrentProcess(), info, (DWORD)(m * sizeof(WsInfo)));
                for (int p = 0; p < m; p++) {
                    const uint64_t i = ((uint64_t)(uintptr_t)info[p].va - lo) >> 12;
                    out[i] = (uint8_t)(1 | (ok && (info[p].attr & 1) ? 2 : 0));
                }
            }
        if (rend <= a) break;
        a = end;
    }
    return 1;
}

void hos_hostsplit_init(void) {
    if (!hs_enabled() || hs_first <= 0) return;
    start_ns = hos_now_ns();
    hos_log("hostsplit: on, first census at %d s%s", hs_first, hs_every > 0 ? ", then every few seconds" : "");
    CloseHandle(CreateThread(NULL, 0, hostsplit_main, NULL, 0, NULL));
    atexit(hostsplit_at_exit);
}
#else
void hos_mem_tag(const void *ptr, uint64_t size, const char *name) { (void)ptr; (void)size; (void)name; }
void hos_mem_untag(const void *ptr) { (void)ptr; }
void hos_hostsplit_init(void) {}
#endif
