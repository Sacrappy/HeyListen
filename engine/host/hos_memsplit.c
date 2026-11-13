/* (HeyListen) HOS_MEM_SPLIT=<seconds>: once, that long after the start, where the game's memory in RAM is: the pages of
   the guest window that are resident (in the working set), by part of the window (system region, image, heap, address
   regions), and of them, those inside an NVN memory pool with storage (what the GPU reads: textures, vertex and index
   data, uniform blocks), by the pool's flags. A measurement for docs/native-engine/memory-boundary.md: RAM is the
   resident pages, not the committed ones. Windows only. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct { uint64_t addr, size; uint32_t flags; } Pool;
static Pool pools[16384];
static int npools;
static SRWLOCK pools_lock = SRWLOCK_INIT;

/* host/nvn_native.c: a pool with storage in guest memory made (add 1) or ended (add 0) */
void hos_memsplit_pool(uint64_t addr, uint64_t size, uint32_t flags, int add) {
    AcquireSRWLockExclusive(&pools_lock);
    if (add) {
        if (npools < (int)(sizeof pools / sizeof pools[0])) pools[npools++] = (Pool){addr, size, flags};
    } else {
        for (int i = 0; i < npools; i++)
            if (pools[i].addr == addr) { pools[i] = pools[--npools]; break; }
    }
    ReleaseSRWLockExclusive(&pools_lock);
}

static int by_addr(const void *a, const void *b) {
    const uint64_t x = ((const Pool *)a)->addr, y = ((const Pool *)b)->addr;
    return x < y ? -1 : x > y;
}

typedef struct { void *va; ULONG_PTR attr; } WsInfo;   /* PSAPI_WORKING_SET_EX_INFORMATION (bit 0: valid) */
typedef BOOL(WINAPI *QueryWsFn)(HANDLE, PVOID, DWORD);

typedef struct { const char *name; uint64_t lo, hi, committed, resident, in_pools; } Part;

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
    struct { uint32_t flags; uint64_t size, resident; int count; } byflag[16];
    int nflags = 0;
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < nflags && byflag[k].flags != snap[i].flags) k++;
        if (k == nflags && nflags < 16) { byflag[nflags].flags = snap[i].flags; byflag[nflags].size = byflag[nflags].resident = 0; byflag[nflags].count = 0; nflags++; }
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
                        if (!(info[k].attr & 1)) continue;
                        part->resident += 0x1000;
                        const uint64_t g = (uint64_t)((uint8_t *)info[k].va - (uint8_t *)GP(0));
                        while (pool_at < n && snap[pool_at].addr + snap[pool_at].size <= g) pool_at++;
                        if (pool_at < n && snap[pool_at].addr <= g) {
                            part->in_pools += 0x1000;
                            for (int f = 0; f < nflags; f++)
                                if (byflag[f].flags == snap[pool_at].flags) { byflag[f].resident += 0x1000; break; }
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
        hos_log("memsplit: pools with flags %#06x: %4d, %6llu MB of storage, %6llu MB in RAM", byflag[f].flags, byflag[f].count,
                (unsigned long long)(byflag[f].size >> 20), (unsigned long long)(byflag[f].resident >> 20));
    hos_log_flush();
    return 0;
}

void hos_memsplit_init(void) {
    const char *e = getenv("HOS_MEM_SPLIT");
    if (e && atoi(e) > 0) CloseHandle(CreateThread(NULL, 0, split_main, (void *)(uintptr_t)atoi(e), 0, NULL));
}
#else
void hos_memsplit_pool(uint64_t addr, uint64_t size, uint32_t flags, int add) { (void)addr; (void)size; (void)flags; (void)add; }
void hos_memsplit_init(void) {}
#endif
