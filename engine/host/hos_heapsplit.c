/* (HeyListen, 2026-11-13) HOS_HEAP_SPLIT=<seconds>: once, that long after the start, the game's heaps as a table. Every heap
   reachable from the heap manager's two root arrays, as a tree: name, class (its vtable), size, used and free by the heap's
   own bookkeeping, the resident bytes (pages in the working set) that are its own (in its range and in no child's), outside
   and inside GPU pools, and for the free-list heaps the block counts, the largest free block and how much of the inside of
   the free blocks is in RAM (what decommitting free blocks would give back). Then the totals: the game's memory in RAM in
   heaps, in GPU pools, in neither (by part of the window).
   HOS_HEAP_SPLIT_COLD=<seconds> adds coldness: right after the table every resident page owned by a heap and outside the GPU
   pools is taken out of the working set once (VirtualUnlock of a range that is not locked: the contents stay, a touch brings
   the page back as a soft fault), and that long after, the pages back in the working set are counted by heap. Those not back
   were not touched in the window.
   A measurement for docs/native-engine/memory-boundary.md section 8, off unless asked: with HOS_HEAP_SPLIT unset the
   constructor reads one variable and returns, and nothing else here runs.

   What it reads of the game (guest addresses of the supported build; offsets in the heap object; memory-boundary.md 2, 8):
   - the root arrays at 0x84e9f400 and 0x84e9f430: a 32-bit count, at +8 a pointer to the heap pointers; and the address
     region heap whose pointer is at 0x84e9f460;
   - a heap: +0 vtable, +32 its name (a C string), +40 arena start, +48 arena size; its child heaps: a list whose sentinel
     is at +64 (a node: +0 previous, +8 next; +72 the first), +84 the offset of the node in the child (88). (The list at
     +104 is the disposers' one: child heaps and other objects that end with the heap.)
   - the free-list heaps (vtables 0x84c172c8 and 0x84c177a8): the free blocks in a list whose sentinel is at +176 (+184 the
     first node), the node's offset in a block at +196, the free and used block counts at +192 and +216; a block has a
     32-byte header with its size after the header at +24;
   - the others' free size, as their own slot 16 computes it: +184 minus +176 (0x84c17400, 0x84c178e0), +192 (0x84c17c08),
     the arena size minus the sizes 8 bytes before each node of a list at +176 (0x84c1acb8), +7912 of the object at +176
     (0x84c179e8). Other classes: not known, left blank.

   Safety: it runs on its own thread while the game allocates. The heaps' mutexes are not taken: they are nn::os mutexes,
   and the host's mutex records its owner as the calling guest thread (nn_os_sync.c), which a host thread does not have, so
   taking one here would not exclude the game. A walk can therefore race a change. Every read is checked against committed,
   readable memory (VirtualQuery) before it is made, every pointer against the heap's own range before it is followed, every
   list walk is capped, and a walk that leaves the range, meets a bad size or reaches its cap stops and is marked "torn".
   What a walk can miscount: blocks taken or given back while it ran (missed or counted twice; the free total and the free
   block count then differ from the heap's own count, which is logged beside them), never memory outside the heap. Heaps
   made or ended during the enumeration may be missed; a heap object is taken only with a heap vtable (in the image, with the
   heap base's function 0x809dfe30 in slot 29) and an arena in the window. Residency is counted by page: a page goes to the smallest heap whose range holds its start. GPU pools are the
   ranges the host's GPU mapping list keeps (nvdrv.c hos_gpu_pools: the NVN pools with storage), merged. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int hos_gpu_pools(uint64_t (*out)[2], int max);   /* nvdrv.c */
extern int hos_large_pages;                        /* hos_main.c */

typedef struct { void *va; ULONG_PTR attr; } WsInfo;   /* PSAPI_WORKING_SET_EX_INFORMATION (bit 0: valid) */
typedef BOOL(WINAPI *QueryWsFn)(HANDLE, PVOID, DWORD);
static QueryWsFn query_ws;

/* ---- reads of guest memory that cannot fault: only inside a committed, readable region (the last one seen is kept) */
static uint64_t ok_lo, ok_hi;
static int readable(uint64_t a, uint64_t n) {
    if (a < HOS_SYSTEM_BASE || a >= HOS_GUEST_SPACE || n > HOS_GUEST_SPACE - a) return 0;
    if (a >= ok_lo && a + n <= ok_hi) return 1;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(GP(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return 0;
    ok_lo = (uint64_t)((uint8_t *)mbi.BaseAddress - (uint8_t *)GP(0));
    ok_hi = ok_lo + mbi.RegionSize;
    return a + n <= ok_hi;
}
static int rd64(uint64_t a, uint64_t *v) {
    if ((a & 7) || !readable(a, 8)) return 0;
    *v = __atomic_load_n((const uint64_t *)GP(a), __ATOMIC_RELAXED);
    return 1;
}
static int rd32(uint64_t a, uint32_t *v) {
    if ((a & 3) || !readable(a, 4)) return 0;
    *v = __atomic_load_n((const uint32_t *)GP(a), __ATOMIC_RELAXED);
    return 1;
}
static int rd8(uint64_t a, uint8_t *v) {
    if (!readable(a, 1)) return 0;
    *v = __atomic_load_n((const uint8_t *)GP(a), __ATOMIC_RELAXED);
    return 1;
}

/* ---- the heap classes read so far (memory-boundary.md 2) */
enum { K_OTHER, K_FREELIST, K_FRAME, K_AT192, K_LIST, K_TERRAIN };
static const struct { uint64_t vt; const char *name; int kind; } classes[] = {
    {0x84c172c8, "free-list", K_FREELIST},   {0x84c177a8, "free-list2", K_FREELIST}, {0x84c17400, "frame", K_FRAME},
    {0x84c178e0, "over-heaps", K_FRAME},     {0x84c17628, "virt-addr", K_OTHER},     {0x84c179e8, "terrain", K_TERRAIN},
    {0x84c17c08, "actor-inst", K_AT192},     {0x84c1acb8, "gpu-res", K_LIST},        {0x84c41fe0, "lmem", K_OTHER},
    {0x84c77660, "small", K_OTHER},          {0x84c78328, "small2", K_OTHER},        {0x84c79200, "small3", K_OTHER},
};
enum { NCLASSES = sizeof classes / sizeof classes[0] };

typedef struct {
    uint64_t obj, vt, start, size;
    int parent, depth, cls;   /* (cls: index in classes, -1 not known) */
    int first_child, next_sibling;
    char name[56];
    int64_t free;   /* (by the heap's own bookkeeping; -1 not known) */
    uint32_t used_count, free_count;   /* (free-list: the heap's counts) */
    int64_t walked_free, largest_free;   /* (free-list: free blocks found by the walk, the largest one's size) */
    int walk;   /* (0 none, 1 complete, 2 torn) */
    uint64_t res_own, res_pool, res_incl, res_free;   /* (pages) */
    uint64_t cold_out, cold_back;   /* (pages taken out of the working set, back after the window) */
} Heap;
enum { MAX_HEAPS = 16384, MAX_DEPTH = 32 };
static Heap *heaps;
static int nheaps;
static int64_t torn_walks;

static uint64_t *seen;   /* (open addressing set of heap objects: no heap twice, no cycle) */
enum { SEEN_SLOTS = 65536 };
static int seen_add(uint64_t obj) {
    uint64_t h = (obj >> 3) * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < SEEN_SLOTS; i++) {
        uint64_t *s = &seen[(h + (uint64_t)i) & (SEEN_SLOTS - 1)];
        if (*s == obj) return 0;
        if (!*s) { *s = obj; return 1; }
    }
    return 0;
}

static void read_name(uint64_t p, char *out, size_t n) {
    size_t k = 0;
    for (; k + 1 < n; k++) {
        uint8_t ch;
        if (!rd8(p + k, &ch) || !ch) break;
        out[k] = ch >= 0x20 && ch < 0x7f ? (char)ch : '?';
    }
    out[k] = 0;
}

/* (free blocks' inside, page aligned: what decommitting them would give back; counted resident after the walk) */
typedef struct { uint64_t lo, hi; int heap; } Range;
static Range *inner;
static int64_t ninner, capinner;
static void inner_add(uint64_t lo, uint64_t hi, int heap) {
    lo = (lo + 0xFFF) & ~0xFFFull, hi &= ~0xFFFull;
    if (hi <= lo || ninner >= (1 << 22)) return;
    if (ninner == capinner) {
        const int64_t cap = capinner ? capinner * 2 : 65536;
        Range *r = (Range *)realloc(inner, (size_t)cap * sizeof *r);
        if (!r) return;
        inner = r, capinner = cap;
    }
    inner[ninner++] = (Range){lo, hi, heap};
}

/* the free list of a free-list heap: every node inside the heap's range, every size inside what is left of it */
static void walk_freelist(Heap *h, int idx) {
    const uint64_t lo = h->start, hi = h->start + h->size, sentinel = h->obj + 176;
    uint32_t off32 = 0;
    uint64_t node;
    if (!rd32(h->obj + 196, &off32) || !rd32(h->obj + 192, &h->free_count) || !rd32(h->obj + 216, &h->used_count) ||
        !rd64(sentinel + 8, &node)) {
        h->walk = 2;
        return;
    }
    const int64_t off = (int32_t)off32;
    /* (the heap's own count may be torn too: the cap is also absolute. A list that loops ends here) */
    const uint64_t cap = h->free_count < (1u << 20) ? (uint64_t)h->free_count * 2 + 4096 : (1u << 21);
    int64_t sum = 0, n = 0, largest = 0;
    int torn = 0;
    for (uint64_t k = 0; node != sentinel; k++) {
        const uint64_t blk = node - (uint64_t)off;
        uint64_t size, next;
        if (k > cap || (node & 7) || node < lo || node + 16 > hi || blk < lo || blk + 32 > hi || !rd64(blk + 24, &size) ||
            size > hi - (blk + 32) || !rd64(node + 8, &next)) {
            torn = 1;
            break;
        }
        sum += (int64_t)size, n++;
        if ((int64_t)size > largest) largest = (int64_t)size;
        inner_add(blk + 32, blk + 32 + size, idx);
        node = next;
    }
    h->free = sum, h->walked_free = n, h->largest_free = largest;
    h->walk = torn ? 2 : 1;
    if (torn) torn_walks++;
}

/* the other classes' free size, as their slot 16 computes it (memory-boundary.md 8) */
static void own_free(Heap *h, int idx) {
    const int kind = h->cls >= 0 ? classes[h->cls].kind : K_OTHER;
    uint64_t a, b;
    h->free = -1;
    switch (kind) {
    case K_FREELIST: walk_freelist(h, idx); return;
    case K_FRAME:
        if (rd64(h->obj + 176, &a) && rd64(h->obj + 184, &b) && b >= a && b - a <= h->size) h->free = (int64_t)(b - a);
        return;
    case K_AT192:
        if (rd64(h->obj + 192, &a) && a <= h->size) h->free = (int64_t)a;
        return;
    case K_TERRAIN:
        if (rd64(h->obj + 176, &a) && rd64(a + 7912, &b) && b <= h->size) h->free = (int64_t)b;
        return;
    case K_LIST: {
        const uint64_t sentinel = h->obj + 176;
        uint64_t node, used = 0;
        if (!rd64(sentinel + 8, &node)) return;
        for (int k = 0; node != sentinel; k++) {
            uint64_t size, next;
            if (k > 1000000 || !rd64(node - 8, &size) || !rd64(node + 8, &next) || size > h->size || used + size > h->size) {
                h->walk = 2;
                torn_walks++;
                return;
            }
            used += size;
            node = next;
        }
        h->walk = 1;
        h->free = (int64_t)(h->size - used);
        return;
    }
    default: return;
    }
}

static int add_heap(uint64_t obj, int parent, int depth) {
    uint64_t vt, start, size, namep, slot29;
    ok_hi = 0;   /* (the readable region is looked up again for each heap: memory may have been given back meanwhile) */
    /* (a heap's vtable has the heap base's own function in slot 29, 0x809dfe30, in all thirteen classes: a list node that
       moved under the walk leads to some other object, whose vtable does not) */
    if (nheaps >= MAX_HEAPS || depth >= MAX_DEPTH || (obj & 7) || !rd64(obj, &vt) || vt < hos_image_base ||
        vt >= hos_image_end || !rd64(vt + 232, &slot29) || slot29 != 0x809dfe30 || !rd64(obj + 40, &start) || !rd64(obj + 48, &size) || !size || start < HOS_SYSTEM_BASE ||
        start >= HOS_GUEST_SPACE || size > HOS_GUEST_SPACE - start || !seen_add(obj))
        return -1;
    const int idx = nheaps++;
    Heap *h = &heaps[idx];
    memset(h, 0, sizeof *h);
    h->obj = obj, h->vt = vt, h->start = start, h->size = size, h->parent = parent, h->depth = depth;
    h->first_child = h->next_sibling = -1;
    h->cls = -1;
    for (int i = 0; i < NCLASSES; i++)
        if (classes[i].vt == vt) h->cls = i;
    if (rd64(obj + 32, &namep)) read_name(namep, h->name, sizeof h->name);
    if (!h->name[0]) snprintf(h->name, sizeof h->name, "(heap %#llx)", (unsigned long long)obj);
    if (parent >= 0) {   /* (children kept in list order) */
        int *at = &heaps[parent].first_child;
        while (*at >= 0) at = &heaps[*at].next_sibling;
        *at = idx;
    }
    own_free(h, idx);
    /* its child heaps: the list at +64 (the walk of 0x80503f00, which finds the heap that holds an address) */
    const uint64_t sentinel = obj + 64;
    uint32_t off32 = 0;
    uint64_t node;
    if (!rd32(obj + 84, &off32) || off32 > 4096 || !rd64(sentinel + 8, &node)) return idx;
    for (uint64_t k = 0; node != sentinel && node && k < 65536; k++) {
        uint64_t next;
        if ((node & 7) || !rd64(node + 8, &next)) break;
        add_heap(node - off32, idx, depth + 1);
        node = next;
    }
    return idx;
}

/* ---- residency */
static WsInfo *batch;
enum { BATCH = 4096 };
/* the resident pages of [lo, hi) (page aligned), committed parts only; with runs, each resident page is also kept */
typedef struct { uint64_t page; uint32_t n; int heap; } Run;
static Run *runs;
static int64_t nruns, capruns;
static void run_add(uint64_t page, int heap) {
    if (nruns && runs[nruns - 1].heap == heap && runs[nruns - 1].page + runs[nruns - 1].n == page && runs[nruns - 1].n < 0x40000) {
        runs[nruns - 1].n++;
        return;
    }
    if (nruns == capruns) {
        const int64_t cap = capruns ? capruns * 2 : 65536;
        Run *r = (Run *)realloc(runs, (size_t)cap * sizeof *r);
        if (!r) return;
        runs = r, capruns = cap;
    }
    runs[nruns++] = (Run){page, 1, heap};
}
static uint64_t resident_in(uint64_t lo, uint64_t hi, int keep_heap) {
    uint64_t got = 0, a = lo;
    while (a < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(GP(a), &mbi, sizeof mbi)) break;
        uint64_t end = (uint64_t)((uint8_t *)mbi.BaseAddress - (uint8_t *)GP(0)) + mbi.RegionSize;
        if (end > hi) end = hi;
        if (end <= a) break;
        if (mbi.State == MEM_COMMIT)
            for (uint64_t q = a; q < end;) {
                int m = 0;
                for (; m < BATCH && q < end; m++, q += 0x1000) batch[m].va = GP(q), batch[m].attr = 0;
                if (!query_ws(GetCurrentProcess(), batch, (DWORD)(m * sizeof(WsInfo)))) continue;
                for (int k = 0; k < m; k++)
                    if (batch[k].attr & 1) {
                        got++;
                        if (keep_heap != -2) run_add((uint64_t)((uint8_t *)batch[k].va - (uint8_t *)GP(0)) >> 12, keep_heap);
                    }
            }
        a = end;
    }
    return got;
}

static uint64_t (*pools)[2];
static int npools;
static int cmp_range(const void *a, const void *b) {
    const uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static int in_pool(uint64_t a) {   /* (pools sorted, merged) */
    int l = 0, r = npools - 1;
    while (l <= r) {
        const int m = (l + r) / 2;
        if (a < pools[m][0]) r = m - 1;
        else if (a >= pools[m][1]) l = m + 1;
        else return 1;
    }
    return 0;
}

#define MB(p) ((double)(p) / 256.0)   /* (pages to MB) */
#define BMB(b) ((double)(b) / 1048576.0)   /* (bytes to MB) */

static const char *class_name(const Heap *h, char *buf, size_t n) {
    if (h->cls >= 0) return classes[h->cls].name;
    snprintf(buf, n, "vt %#llx", (unsigned long long)h->vt);
    return buf;
}

static int log_lines;
static void line_done(void) {
    if (++log_lines % 200 == 0) hos_log_flush();
}

static void log_heap(const Heap *h, const char *tag, int indent) {
    char cb[32], fb[160] = "";
    const char *cn = class_name(h, cb, sizeof cb);
    if (h->cls >= 0 && classes[h->cls].kind == K_FREELIST)
        snprintf(fb, sizeof fb, "; blocks %u used, %lld free (heap's count %u)%s, largest free %.2f MB, free blocks' pages in RAM %.1f MB",
                 h->used_count, (long long)h->walked_free, h->free_count, h->walk == 2 ? " walk torn" : "", BMB(h->largest_free),
                 MB(h->res_free));
    char used[48] = "?", freeb[48] = "?";
    if (h->free >= 0) {
        snprintf(used, sizeof used, "%.1f", BMB(h->size - (uint64_t)h->free));
        snprintf(freeb, sizeof freeb, "%.1f", BMB(h->free));
    }
    hos_log("%s: %*s%s [%s] size %.1f MB, used %s, free %s; RAM own %.1f + pools %.1f MB, with children %.1f MB%s", tag,
            indent * 2, "", h->name, cn, BMB(h->size), used, freeb, MB(h->res_own), MB(h->res_pool), MB(h->res_incl), fb);
    line_done();
}

static void log_tree(int idx, int depth) {
    const Heap *h = &heaps[idx];
    log_heap(h, "heapsplit", depth);
    int small = 0;
    uint64_t small_size = 0, small_res = 0;
    for (int c = h->first_child; c >= 0; c = heaps[c].next_sibling) {
        const Heap *k = &heaps[c];
        if (k->res_incl >= 256 || k->size >= (64ull << 20)) log_tree(c, depth + 1);
        else small++, small_size += k->size, small_res += k->res_incl;
    }
    if (small) {
        hos_log("heapsplit: %*s(%d smaller heaps: %.1f MB of size, %.1f MB in RAM with their children)", (depth + 1) * 2, "", small,
                BMB(small_size), MB(small_res));
        line_done();
    }
}

static int by_res(const void *a, const void *b) {
    const Heap *x = &heaps[*(const int *)a], *y = &heaps[*(const int *)b];
    const uint64_t p = x->res_own + x->res_pool, q = y->res_own + y->res_pool;
    return p < q ? 1 : p > q ? -1 : 0;
}

/* (heaps grouped by name with digits taken out: actor instance heaps and the like) */
typedef struct { char name[56]; int count; uint64_t size, own, pool, free_ram, cold_out, cold_back; int64_t freeb; } Group;
static int by_group(const void *a, const void *b) {
    const Group *x = (const Group *)a, *y = (const Group *)b;
    const uint64_t p = x->own + x->pool, q = y->own + y->pool;
    return p < q ? 1 : p > q ? -1 : 0;
}

static DWORD WINAPI heapsplit_main(void *p) {
    const int at = (int)(uintptr_t)p;
    Sleep((DWORD)at * 1000);
    query_ws = (QueryWsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    heaps = (Heap *)calloc(MAX_HEAPS, sizeof *heaps);
    seen = (uint64_t *)calloc(SEEN_SLOTS, sizeof *seen);
    batch = (WsInfo *)calloc(BATCH, sizeof *batch);
    pools = (uint64_t (*)[2])calloc(32768, sizeof *pools);
    if (!query_ws || !heaps || !seen || !batch || !pools) { hos_log("heapsplit: not started (no memory or no K32QueryWorkingSetEx)"); return 0; }
    const char *ce = getenv("HOS_HEAP_SPLIT_COLD");
    const int cold = ce ? atoi(ce) : 0;
    const int64_t t0 = hos_now_ns();
    hos_log("heapsplit: reading the heaps");
    hos_log_flush();

    /* 1. the heaps: the root arrays (the second: heaps made with no parent, sead's independent heaps), then each heap's children */
    static const uint64_t root_arrays[2] = {0x84e9f400, 0x84e9f430};
    int nroots = 0;
    for (int r = 0; r < 2; r++) {
        uint32_t count;
        uint64_t arr;
        if (!rd32(root_arrays[r], &count) || count > 256 || !rd64(root_arrays[r] + 8, &arr)) continue;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t hp;
            if (rd64(arr + 8ull * i, &hp) && add_heap(hp, -1, 0) >= 0) nroots++;
        }
    }
    /* (and the address-region heap, "NinVirtualAddressHeap", made by 0x809e20c0 and kept at 0x84e9f460: in no array) */
    uint64_t vah;
    if (rd64(0x84e9f460, &vah) && vah && add_heap(vah, -1, 0) >= 0) nroots++;
    const int64_t t_walk = hos_now_ns();
    hos_log("heapsplit: %d heaps read (%lld free blocks' insides kept), %.0f ms; counting their pages", nheaps, (long long)ninner,
            (t_walk - t0) / 1e6);
    hos_log_flush();

    /* 2. the GPU pools, page aligned, sorted and merged */
    npools = hos_gpu_pools(pools, 32768);
    for (int i = 0; i < npools; i++) pools[i][1] = (pools[i][0] + pools[i][1] + 0xFFF) & ~0xFFFull, pools[i][0] &= ~0xFFFull;
    qsort(pools, (size_t)npools, sizeof *pools, cmp_range);
    int m = 0;
    for (int i = 0; i < npools; i++) {
        if (m && pools[i][0] <= pools[m - 1][1]) {
            if (pools[i][1] > pools[m - 1][1]) pools[m - 1][1] = pools[i][1];
        } else pools[m][0] = pools[i][0], pools[m][1] = pools[i][1], m++;
    }
    npools = m;

    /* 3. the pieces between consecutive heap and pool edges: each belongs to the smallest heap that holds it (or none), and is
       inside a pool or not */
    const int nb_max = 2 * nheaps + 2 * npools;
    uint64_t *edges = (uint64_t *)malloc((size_t)(nb_max ? nb_max : 1) * sizeof *edges);
    if (!edges) return 0;
    int nb = 0;
    for (int i = 0; i < nheaps; i++) edges[nb++] = heaps[i].start & ~0xFFFull, edges[nb++] = (heaps[i].start + heaps[i].size) & ~0xFFFull;
    for (int i = 0; i < npools; i++) edges[nb++] = pools[i][0], edges[nb++] = pools[i][1];
    qsort(edges, (size_t)nb, sizeof *edges, cmp_u64);
    uint64_t res_pools_outside = 0;
    for (int e = 0; e + 1 < nb; e++) {
        const uint64_t lo = edges[e], hi = edges[e + 1];
        if (hi <= lo) continue;
        int owner = -1;
        for (int i = 0; i < nheaps; i++) {
            const uint64_t hl = heaps[i].start & ~0xFFFull, hh = (heaps[i].start + heaps[i].size) & ~0xFFFull;
            if (lo >= hl && lo < hh && (owner < 0 || heaps[i].size < heaps[owner].size)) owner = i;
        }
        const int pool = in_pool(lo);
        if (owner < 0 && !pool) continue;
        const uint64_t got = resident_in(lo, hi, cold && owner >= 0 && !pool ? owner : -2);
        if (owner < 0) res_pools_outside += got;
        else if (pool) heaps[owner].res_pool += got;
        else heaps[owner].res_own += got;
    }
    free(edges);
    /* the free blocks' inside */
    for (int64_t i = 0; i < ninner; i++) heaps[inner[i].heap].res_free += resident_in(inner[i].lo, inner[i].hi, -2);
    /* with the children: deepest first (a child always comes after its parent) */
    for (int i = 0; i < nheaps; i++) heaps[i].res_incl = heaps[i].res_own + heaps[i].res_pool;
    for (int i = nheaps - 1; i >= 0; i--)
        if (heaps[i].parent >= 0) heaps[heaps[i].parent].res_incl += heaps[i].res_incl;

    /* 4. the whole window by part (as host/hos_memsplit.c counts it), for the rest */
    struct { const char *name; uint64_t lo, hi, res; } parts[] = {
        {"system region", HOS_SYSTEM_BASE, HOS_SYSTEM_END, 0},
        {"image", hos_image_base, hos_image_end, 0},
        {"heap", HOS_HEAP_BASE, HOS_HEAP_BASE + hos_heap_size, 0},
        {"address regions", HOS_HEAP_BASE + hos_heap_size, HOS_GUEST_SPACE, 0},
    };
    uint64_t total = 0;
    for (int i = 0; i < 4; i++) total += parts[i].res = resident_in(parts[i].lo, parts[i].hi, -2);
    uint64_t in_heaps = 0, pools_in_heaps = 0, free_ram = 0, heap_size_top = 0;
    for (int i = 0; i < nheaps; i++) {
        in_heaps += heaps[i].res_own, pools_in_heaps += heaps[i].res_pool, free_ram += heaps[i].res_free;
        if (heaps[i].parent < 0) heap_size_top += heaps[i].size;
    }
    const int64_t t_count = hos_now_ns();

    /* 5. the table */
    hos_log("heapsplit: %d heaps (%d roots), %.1f GB of root arenas; %lld free-list walks torn; %d GPU pool ranges; %.0f ms to read "
            "the heaps, %.0f ms to count", nheaps, nroots, heap_size_top / 1073741824.0, (long long)torn_walks, npools, (t_walk - t0) / 1e6,
            (t_count - t_walk) / 1e6);
    for (int i = 0; i < nheaps; i++)
        if (heaps[i].parent < 0) log_tree(i, 0);
    int *order = (int *)malloc((size_t)(nheaps ? nheaps : 1) * sizeof *order);
    if (order) {
        for (int i = 0; i < nheaps; i++) order[i] = i;
        qsort(order, (size_t)nheaps, sizeof *order, by_res);
        for (int k = 0; k < nheaps && k < 40; k++) {
            char tag[32];
            snprintf(tag, sizeof tag, "heaptop %2d", k + 1);
            log_heap(&heaps[order[k]], tag, 0);
        }
    }
    hos_log("heapsplit: the game's memory in RAM %.0f MB: in heaps outside GPU pools %.0f MB, in GPU pools %.0f MB (%.0f inside "
            "heaps, %.0f outside), in neither %.0f MB; free blocks' pages in RAM %.0f MB", MB(total), MB(in_heaps),
            MB(pools_in_heaps + res_pools_outside), MB(pools_in_heaps), MB(res_pools_outside),
            ((double)total - (double)in_heaps - (double)pools_in_heaps - (double)res_pools_outside) / 256.0, MB(free_ram));
    for (int i = 0; i < 4; i++) {
        uint64_t inh = 0, inp = 0;   /* (by part: what the heaps and pools hold of it) */
        for (int k = 0; k < nheaps; k++)
            if (heaps[k].start >= parts[i].lo && heaps[k].start < parts[i].hi) inh += heaps[k].res_own, inp += heaps[k].res_pool;
        hos_log("heapsplit: %-15s in RAM %6.0f MB: owned by heaps %6.0f MB outside pools, %6.0f MB in pools inside heaps", parts[i].name,
                MB(parts[i].res), MB(inh), MB(inp));
    }
    line_done();

    /* 6. coldness: the heaps' resident pages outside pools out of the working set once, counted back after the window */
    if (cold > 0 && !hos_large_pages) {
        uint64_t out = 0;
        const int64_t c0 = hos_now_ns();
        for (int64_t i = 0; i < nruns; i++) VirtualUnlock(GP(runs[i].page << 12), (SIZE_T)runs[i].n << 12);
        /* (taken out: not resident right after; pages kept in by someone else are left out of the count) */
        int64_t total_pages = 0;
        for (int64_t i = 0; i < nruns; i++) total_pages += runs[i].n;
        uint64_t *gone_bits = (uint64_t *)calloc((size_t)(total_pages / 64 + 1), sizeof *gone_bits);
        if (gone_bits) {
            int64_t bit = 0;
            for (int64_t i = 0; i < nruns; i++)
                for (uint64_t q = 0; q < runs[i].n;) {
                    int mm = 0;
                    const int64_t first = bit;
                    for (; mm < BATCH && q < runs[i].n; mm++, q++, bit++) batch[mm].va = GP((runs[i].page + q) << 12), batch[mm].attr = 0;
                    if (!query_ws(GetCurrentProcess(), batch, (DWORD)(mm * sizeof(WsInfo)))) continue;
                    for (int k = 0; k < mm; k++)
                        if (!(batch[k].attr & 1)) {
                            gone_bits[(first + k) >> 6] |= 1ull << ((first + k) & 63);
                            heaps[runs[i].heap].cold_out++;
                            out++;
                        }
                }
            hos_log("heapcold: %.0f MB of heap pages outside GPU pools taken out of the working set (%.0f MB stayed in), %.0f ms; "
                    "counted again in %d s", MB(out), MB((uint64_t)total_pages - out), (hos_now_ns() - c0) / 1e6, cold);
            hos_log_flush();
            Sleep((DWORD)cold * 1000);
            uint64_t back = 0;
            bit = 0;
            for (int64_t i = 0; i < nruns; i++)
                for (uint64_t q = 0; q < runs[i].n;) {
                    int mm = 0;
                    const int64_t first = bit;
                    for (; mm < BATCH && q < runs[i].n; mm++, q++, bit++) batch[mm].va = GP((runs[i].page + q) << 12), batch[mm].attr = 0;
                    if (!query_ws(GetCurrentProcess(), batch, (DWORD)(mm * sizeof(WsInfo)))) continue;
                    for (int k = 0; k < mm; k++)
                        if ((batch[k].attr & 1) && (gone_bits[(first + k) >> 6] >> ((first + k) & 63) & 1)) {
                            heaps[runs[i].heap].cold_back++;
                            back++;
                        }
                }
            hos_log("heapcold: after %d s, %.0f MB of the %.0f MB came back into the working set; %.0f MB did not (cold)", cold, MB(back),
                    MB(out), MB(out - back));
            if (order) {
                for (int i = 0; i < nheaps; i++) order[i] = i;
                qsort(order, (size_t)nheaps, sizeof *order, by_res);
                for (int k = 0, shown = 0; k < nheaps && shown < 40; k++) {
                    const Heap *h = &heaps[order[k]];
                    if (!h->cold_out) continue;
                    hos_log("heapcold %2d: %s: RAM own %.1f MB, taken out %.1f MB, back %.1f MB, cold %.1f MB", ++shown, h->name, MB(h->res_own),
                            MB(h->cold_out), MB(h->cold_back), MB(h->cold_out - h->cold_back));
                    line_done();
                }
            }
        }
        free(gone_bits);
    }

    /* 7. by name group (digits taken out), and every heap of some size as a table line for the document's scripts */
    Group *groups = (Group *)calloc(4096, sizeof *groups);
    int ngroups = 0;
    for (int i = 0; groups && i < nheaps; i++) {
        char key[56];
        size_t k = 0;
        for (const char *s = heaps[i].name; *s && k + 1 < sizeof key; s++)
            if (!(*s >= '0' && *s <= '9')) key[k++] = *s;
            else if (!k || key[k - 1] != '#') key[k++] = '#';
        key[k] = 0;
        int g = 0;
        while (g < ngroups && strcmp(groups[g].name, key)) g++;
        if (g == ngroups) {
            if (ngroups == 4096) continue;
            snprintf(groups[ngroups++].name, sizeof groups[0].name, "%s", key);
        }
        Group *G = &groups[g];
        G->count++, G->size += heaps[i].size, G->own += heaps[i].res_own, G->pool += heaps[i].res_pool, G->free_ram += heaps[i].res_free;
        G->cold_out += heaps[i].cold_out, G->cold_back += heaps[i].cold_back;
        G->freeb += heaps[i].free > 0 ? heaps[i].free : 0;
    }
    if (groups) {
        qsort(groups, (size_t)ngroups, sizeof *groups, by_group);
        for (int g = 0; g < ngroups && g < 30; g++) {
            hos_log("heapgroup %2d: %s x%d: size %.1f MB, free %.1f MB; RAM own %.1f + pools %.1f MB, free blocks' pages %.1f MB; cold %.1f of %.1f MB",
                    g + 1, groups[g].name, groups[g].count, BMB(groups[g].size), BMB(groups[g].freeb), MB(groups[g].own), MB(groups[g].pool),
                    MB(groups[g].free_ram), MB(groups[g].cold_out - groups[g].cold_back), MB(groups[g].cold_out));
            line_done();
        }
        free(groups);
    }
    for (int i = 0; i < nheaps; i++) {
        const Heap *h = &heaps[i];
        if (h->res_incl < 16 && h->size < (1ull << 20)) continue;
        char cb[32];
        hos_log("heaptsv\t%d\t%d\t%d\t%s\t%s\t%#llx\t%llu\t%lld\t%llu\t%llu\t%llu\t%llu\t%u\t%lld\t%lld\t%d\t%llu\t%llu", i, h->parent, h->depth,
                class_name(h, cb, sizeof cb), h->name, (unsigned long long)h->start, (unsigned long long)h->size, (long long)h->free,
                (unsigned long long)h->res_own, (unsigned long long)h->res_pool, (unsigned long long)h->res_incl,
                (unsigned long long)h->res_free, h->used_count, (long long)h->walked_free, (long long)h->largest_free, h->walk,
                (unsigned long long)h->cold_out, (unsigned long long)h->cold_back);
        line_done();
    }
    free(order);
    hos_log_flush();
    return 0;
}

__attribute__((constructor)) static void heapsplit_init(void) {
    const char *e = getenv("HOS_HEAP_SPLIT");
    if (e && atoi(e) > 0) CloseHandle(CreateThread(NULL, 0, heapsplit_main, (void *)(uintptr_t)atoi(e), 0, NULL));
}
#endif
