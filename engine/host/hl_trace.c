/* (HeyListen native engine) The crash recorder's game side (hl_trace.h; tools/bench/hltrace.py reads its file).

   HL_TRACE=<mask> (0 or unset: off, the default; 1: everything; else the bits of hl_trace.h: 2 memory, 4 jobs, 8 the
   native bricks' memory decisions, 16 scene). With the memory bit, entry gates on the game's heaps' allocation and free
   functions (every heap class's virtual alloc and free of docs/native-engine/memory-boundary.md section 2, and the
   standard allocator's aligned_alloc, malloc, calloc, realloc and free) record each block with its size, heap and the
   guest caller chain (6 return addresses from the frame chain) into the live blocks' table, and each free into the last
   frees' ring with the block's allocation record; the gates are in every build and switched on only by the trace.
   At a crash (the unhandled exception report, and the OS layer's FATAL, among them a call to an address with no
   function) the report says which block the faulting address and each register are in, live or freed, allocated and
   freed by whom and when, then the last 200 events of the crashing thread and the last 50 of every other thread merged
   by time, the same lines also into hl_trace_crash.txt; and hl_trace.bin is written in the game's folder.
   The back end (gpu/vk, through the renderer interface's trace_gpu) records its writes into guest memory (write-backs and
   the lazy release's), its mirrors made and given back, imports made and given back and readback pages: a block's report
   says which of them covered it, when, in which submission, and how far the GPU was.
   HL_TRACE_DUMP=<s>: also write hl_trace.bin <s> seconds after the start (runs that end by being stopped), after a
   report as a crash prints it (with the last 3 events of each thread). */
#include "hl_host.h"
#include "hl_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The heaps' entry points, and how their arguments are: alloc functions return the block in x0. */
typedef struct {
    uint64_t guest;
    int size_arg;    /* the size's register (-1: calloc's two) */
    int ptr_arg;     /* free: the block's register; realloc: the old block's */
    int heap_arg;    /* the heap's register (-1: the standard allocator) */
} Entry;

static const uint64_t mem_gates[] = {0x809df950, 0x803b44e0, 0x834921a0, 0x801e8460, 0x80504840, 0x8080fd70, 0x834931b0,
                                     0x808101d0, 0x806a22f0, 0x834991f0, 0x83499320, 0x834bdd90, 0x834bdfc0, 0x8005eca0,
                                     0x814e6c90, 0x814e6ce0, 0x814e6d50, 0x80709580};
#define GATES_IN(what, list) hl_gates_in(what, list, (int)(sizeof list / sizeof list[0]))

/* The caller chain: the return address, then the frame chain from x29 (the gate runs before the function's own frame),
   followed while it stays on this thread's stack above sp. */
static void chain_of(Ctx *c, uint32_t *chain) {
    chain[0] = (uint32_t)c->x[30];
    uint64_t fp = c->x[29];
    const uint64_t sp = c->x[31];
    int k = 1;
    for (; k < HL_TRACE_CHAIN; k++) {
        if (!fp || (fp & 15) || fp < sp || fp - sp > (8u << 20)) break;
        const uint64_t lr = G64(fp + 8), next = G64(fp);
        chain[k] = (uint32_t)lr;
        if (next <= fp) {
            k++;
            break;
        }
        fp = next;
    }
    for (; k < HL_TRACE_CHAIN; k++) chain[k] = 0;
}

static _Thread_local int through;   /* the next gate entered on this thread lets the translated body run */

static int alloc_gate(Ctx *c, uint64_t guest, int size_arg, int heap_arg) {
    if (through) {
        through = 0;
        return 0;
    }
    static A64Fn fns[16];
    static uint64_t addrs[16];
    A64Fn fn = NULL;
    for (int k = 0; k < 16 && !fn; k++)
        if (addrs[k] == guest) fn = fns[k];
    if (!fn) {
        fn = hl_translated(guest);
        if (!fn) return 0;
        for (int k = 0; k < 16; k++)
            if (!addrs[k] || addrs[k] == guest) {
                fns[k] = fn;
                addrs[k] = guest;
                break;
            }
    }
    uint32_t chain[HL_TRACE_CHAIN];
    chain_of(c, chain);
    const uint64_t size = size_arg < 0 ? c->x[0] * c->x[1] : c->x[size_arg];
    const uint64_t heap = heap_arg < 0 ? 0 : c->x[heap_arg];
    const uint64_t old = guest == 0x814e6d50 ? c->x[0] : 0;   /* (realloc: the block it may free) */
    through = 1;
    fn(c);
    const uint64_t block = c->x[0];
    if (old && block) hl_trace_free(old, 0, chain);   /* (moved, or resized in place: listed again with its new size) */
    hl_trace_alloc(block, size, heap, chain);
    return 1;
}

static int free_gate(Ctx *c, int ptr_arg, int heap_arg) {
    if (through) {
        through = 0;
        return 0;
    }
    uint32_t chain[HL_TRACE_CHAIN];
    chain_of(c, chain);
    hl_trace_free(c->x[ptr_arg], heap_arg < 0 ? 0 : c->x[heap_arg], chain);
    return 0;   /* (the translated free runs after the record: a block reused after it is seen allocated after it) */
}

/* the heap classes' virtual alloc (heap, size, alignment) and free (heap, block) */
HL_GATE(809df950, "1163e6600b20a70c") { return alloc_gate(c, 0x809df950, 1, 0); }
HL_GATE(803b44e0, "2cb7c92bec707d9a") { return free_gate(c, 1, 0); }
HL_GATE(834921a0, "9ddaa391731b56d9") { return alloc_gate(c, 0x834921a0, 1, 0); }
HL_GATE(801e8460, "8ddb0194e8ec8bf0") { return alloc_gate(c, 0x801e8460, 1, 0); }
HL_GATE(80504840, "dbbe8ced21c227e1") { return free_gate(c, 1, 0); }
HL_GATE(8080fd70, "260b46a53e4654b2") { return alloc_gate(c, 0x8080fd70, 1, 0); }
HL_GATE(834931b0, "7d29d01f835a53bf") { return free_gate(c, 1, 0); }
HL_GATE(808101d0, "7fcfd7ef9a74c5ad") { return alloc_gate(c, 0x808101d0, 1, 0); }
HL_GATE(806a22f0, "8eebf72d129c7e71") { return free_gate(c, 1, 0); }
HL_GATE(834991f0, "c3f2f9bdbeeb8f2b") { return alloc_gate(c, 0x834991f0, 1, 0); }
HL_GATE(83499320, "9b7b3f4409f116d0") { return free_gate(c, 1, 0); }
HL_GATE(834bdd90, "18cfbfaad7a9cd94") { return alloc_gate(c, 0x834bdd90, 1, 0); }
HL_GATE(834bdfc0, "c65f768e158ffd75") { return free_gate(c, 1, 0); }
/* the standard allocator: aligned_alloc (alignment, size), malloc (size), calloc (n, size), realloc (block, size), free */
HL_GATE(8005eca0, "0ce5468d82ee701d") { return alloc_gate(c, 0x8005eca0, 1, -1); }
HL_GATE(814e6c90, "770aecd6a5b7e353") { return alloc_gate(c, 0x814e6c90, 0, -1); }
HL_GATE(814e6ce0, "d3b49da72eac206e") { return alloc_gate(c, 0x814e6ce0, -1, -1); }
HL_GATE(814e6d50, "3b72a1fb983c411a") { return alloc_gate(c, 0x814e6d50, 1, -1); }
HL_GATE(80709580, "e67268a6064c41d8") { return free_gate(c, 0, -1); }

static const char *thread_name(void) {   /* (a thread of the OS layer's or the back end's has no guest thread) */
    HosThread *t = hos_self();
    return t ? hos_thread_name(t) : "host";
}
/* (at a crash the report also goes to hl_trace_crash.txt, written through at each line: a log queue the process's end
   does not drain loses nothing there) */
static FILE *crash_file;
static void out_line(const char *line) {
    hos_log("%s", line);
    if (crash_file) {
        fputs(line, crash_file);
        fputc('\n', crash_file);
        fflush(crash_file);
    }
}

/* (rx_backend.c gives it to the back end: vk_api.h trace_gpu) */
void hl_trace_gpu(uint32_t what, uint64_t guest, uint64_t size, uint64_t pool, uint64_t value, uint64_t done) {
    hl_trace_gpu_record(what, guest, size, pool, value, done);
}

/* (the crash reporter, mkw_glue.c, and hos_fatal, hos_thread.c) */
void hl_trace_on_crash(Ctx *c, uint64_t fault) {
    static volatile LONG once;
    if (!hl_trace_mask || InterlockedExchange(&once, 1)) return;
    static const char *const names[31] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                                          "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
                                          "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30"};
    hos_log_flush();
    crash_file = fopen("hl_trace_crash.txt", "w");
    hl_trace_report(fault, c ? c->x : NULL, names, c ? 31 : 0, 200, 50);
    hos_log_flush();
    const int failed = hl_trace_write("hl_trace.bin");
    out_line(failed ? "TRACE: could not write hl_trace.bin" : "TRACE: wrote hl_trace.bin");
    hos_log_flush();
    if (crash_file) fclose(crash_file), crash_file = NULL;
}

static DWORD WINAPI dump_after(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    /* (the report as a crash prints it, on this thread's state, with every thread's last 3 events: the format seen) */
    hl_trace_report(0, NULL, NULL, 0, 0, 3);
    uint64_t a, f, u, full, t;
    hl_trace_counts(&a, &f, &u, &full, &t);
    hos_log("hl trace: %llu allocations, %llu frees (%llu of blocks not seen allocated), %llu not listed, %llu threads; %s "
            "hl_trace.bin", (unsigned long long)a, (unsigned long long)f, (unsigned long long)u, (unsigned long long)full,
            (unsigned long long)t, hl_trace_write("hl_trace.bin") ? "could not write" : "wrote");
    return 0;
}

/* ---- HL_STEP_SCAN=<s> (with HL_TRACE=1): which of the game's objects count frames and which count time ----
   At <s> seconds every live block of 16 KB or less (512 MB at most) is copied; one second later each 32-bit word is
   compared with its copy. Over that second P frames were presented: a word that went up by exactly P is a frame counter,
   one that went up by 60 (+-1) counts the game's time in 1/60 s; a float that went up by P, by 60 or by 1.0 (+-2%) the
   same in floats. Words are summed by the allocation site of their block (its caller chain's first two return
   addresses): at a frame rate other than 60 (HL_FPS_LIMIT=120 in a run) the frame-counted objects and their makers stand
   apart from the time-counted ones. The table goes to the log and to hl_step_scan.txt. */
typedef struct {
    uint64_t addr, size, off;
    uint32_t site0, site1;
} ScanBlock;
typedef struct {
    ScanBlock *blocks;
    size_t n, cap;
    uint8_t *bytes;
    uint64_t used, budget;
} Scan;
static void scan_take(const HlTraceRecord *r, void *u) {
    Scan *s = u;
    if (r->size == 0 || r->size > 16384 || s->used + r->size > s->budget || s->n == s->cap) return;
    s->blocks[s->n++] = (ScanBlock){r->addr, r->size, s->used, r->chain[0], r->chain[1]};
    s->used += r->size;
}
typedef struct {
    uint32_t site0, site1;
    uint32_t frames, time, fframes, ftime, fsec, words;
} Site;
static int by_frames(const void *a, const void *b) {
    const Site *x = a, *y = b;
    const uint32_t fx = x->frames + x->fframes, fy = y->frames + y->fframes;
    return fx < fy ? 1 : fx > fy ? -1 : 0;
}
static int by_time(const void *a, const void *b) {
    const Site *x = a, *y = b;
    const uint32_t fx = x->time + x->ftime + x->fsec, fy = y->time + y->ftime + y->fsec;
    return fx < fy ? 1 : fx > fy ? -1 : 0;
}
static DWORD WINAPI step_scan(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    int64_t hos_vi_presents(void);
    Scan s = {0};
    s.cap = 4u << 20;
    s.budget = 512ull << 20;
    s.blocks = malloc(s.cap * sizeof *s.blocks);
    s.bytes = malloc(s.budget);
    if (!s.blocks || !s.bytes) return 0;
    hl_trace_live_each(scan_take, &s);
    const int64_t p0 = hos_vi_presents();
    for (size_t k = 0; k < s.n; k++) memcpy(s.bytes + s.blocks[k].off, GP(s.blocks[k].addr), s.blocks[k].size);
    Sleep(1000);
    const int64_t P = hos_vi_presents() - p0;
    enum { SITES = 1 << 16 };
    Site *sites = calloc(SITES, sizeof *sites);
    if (!sites) return 0;
    uint64_t frames = 0, time = 0;
    for (size_t k = 0; k < s.n; k++) {
        const ScanBlock *b = &s.blocks[k];
        Site *st = NULL;
        for (uint32_t h = (b->site0 * 2654435761u ^ b->site1) & (SITES - 1);; h = (h + 1) & (SITES - 1)) {
            if (!sites[h].site0 || (sites[h].site0 == b->site0 && sites[h].site1 == b->site1)) {
                st = &sites[h];
                break;
            }
        }
        st->site0 = b->site0, st->site1 = b->site1;
        const uint8_t *was = s.bytes + b->off, *now = GP(b->addr);
        for (uint64_t o = 0; o + 4 <= b->size; o += 4) {
            uint32_t a, c;
            memcpy(&a, was + o, 4);
            memcpy(&c, now + o, 4);
            if (a == c) continue;
            st->words++;
            const int64_t d = (int64_t)(int32_t)(c - a);
            if (d == P && P != 60) st->frames++, frames++;
            else if (d >= 59 && d <= 61 && (P < 57 || P > 63)) st->time++, time++;
            float fa, fc;
            memcpy(&fa, &a, 4);
            memcpy(&fc, &c, 4);
            const float df = fc - fa;
            if (fa == fa && fc == fc && fa > -1e7f && fa < 1e7f && fc > -1e7f && fc < 1e7f) {
                if (P != 60 && df > 0.98f * P && df < 1.02f * P) st->fframes++, frames++;
                else if ((P < 57 || P > 63) && df > 58.8f && df < 61.2f) st->ftime++, time++;
                else if (df > 0.98f && df < 1.02f) st->fsec++, time++;
            }
        }
    }
    FILE *f = fopen("hl_step_scan.txt", "w");
    char line[256];
#define SCAN_OUT(...) do { snprintf(line, sizeof line, __VA_ARGS__); hos_log("%s", line); if (f) fprintf(f, "%s\n", line); } while (0)
    SCAN_OUT("hl step scan: %llu blocks (%llu MB) compared over 1 s with %lld frames presented: %llu words counting frames, "
             "%llu counting time", (unsigned long long)s.n, (unsigned long long)(s.used >> 20), (long long)P,
             (unsigned long long)frames, (unsigned long long)time);
    qsort(sites, SITES, sizeof *sites, by_frames);
    for (int k = 0; k < 40 && sites[k].frames + sites[k].fframes; k++)
        SCAN_OUT("hl step scan: frames: site %#x %#x: %u ints, %u floats counting frames (time: %u ints, %u floats, %u seconds; "
                 "%u words changed)", sites[k].site0, sites[k].site1, sites[k].frames, sites[k].fframes, sites[k].time,
                 sites[k].ftime, sites[k].fsec, sites[k].words);
    qsort(sites, SITES, sizeof *sites, by_time);
    for (int k = 0; k < 20 && sites[k].time + sites[k].ftime + sites[k].fsec; k++)
        SCAN_OUT("hl step scan: time: site %#x %#x: %u ints, %u floats in 1/60 s, %u in seconds (frames: %u ints, %u floats)",
                 sites[k].site0, sites[k].site1, sites[k].time, sites[k].ftime, sites[k].fsec, sites[k].frames, sites[k].fframes);
#undef SCAN_OUT
    if (f) fclose(f);
    free(sites);
    free(s.blocks);
    free(s.bytes);
    return 0;
}

/* (a constructor: the heaps' first allocations come before the game's code runs anything else) */__attribute__((constructor)) static void hl_trace_switch(void) {
    const char *e = getenv("HL_TRACE");
    uint32_t mask = e && e[0] ? (uint32_t)strtoul(e, NULL, 0) : 0;
    if (!mask) return;
    if (mask == 1) mask = HL_TR_ALL;
    const HlTraceConfig config = {.thread_name = thread_name, .out = out_line};
    if (!hl_trace_start(mask, &config)) return;
    const char *d = getenv("HL_TRACE_DUMP");
    if (d && atoi(d) > 0) CreateThread(NULL, 0, dump_after, (void *)(uintptr_t)atoi(d), 0, NULL);
    const char *sc = getenv("HL_STEP_SCAN");
    if (sc && atoi(sc) > 0) CreateThread(NULL, 0, step_scan, (void *)(uintptr_t)atoi(sc), 0, NULL);
}

/* The gates go on once the lift's gates are known (hl_gates_in reads the presence lines' constructors: asked at the
   first use, from the OS layer's start). */
void hl_trace_gates(void) {
    if (!(hl_trace_mask & HL_TR_MEM)) return;
    const int in = GATES_IN("HL_TRACE memory", mem_gates);
    hos_log("hl trace: on (mask %#x): the heaps' allocations and frees %s", hl_trace_mask,
            in ? "recorded (18 entry gates)" : "NOT recorded (gates missing from this lift)");
    if (!in) return;
    hl_gate_on_809df950 = hl_gate_on_803b44e0 = hl_gate_on_834921a0 = hl_gate_on_801e8460 = hl_gate_on_80504840 = 1;
    hl_gate_on_8080fd70 = hl_gate_on_834931b0 = hl_gate_on_808101d0 = hl_gate_on_806a22f0 = hl_gate_on_834991f0 = 1;
    hl_gate_on_83499320 = hl_gate_on_834bdd90 = hl_gate_on_834bdfc0 = hl_gate_on_8005eca0 = hl_gate_on_814e6c90 = 1;
    hl_gate_on_814e6ce0 = hl_gate_on_814e6d50 = hl_gate_on_80709580 = 1;
}
