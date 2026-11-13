/* (HeyListen native engine) The crash recorder HL_TRACE (hl_trace_core.c: the rings and the tables, no OS layer; hl_trace.c:
   the game's side: the switches, the heaps' entry gates, the crash reporter's call).

   Each thread writes its events (32 bytes: a time stamp, a kind, three operands) into a ring of its own, with no lock and
   no allocation; the heaps' allocations and frees also go into two tables: the live blocks (address -> size, heap, when,
   which thread, the guest caller chain) and the last frees (a ring of the freed blocks with their free's and their
   allocation's records). At a crash the recorder prints, for the faulting address and every register, the block it is in
   (live or freed, and by whom), then the last events of every thread merged by time; and it writes everything to
   hl_trace.bin (tools/bench/hltrace.py reads it). Off unless HL_TRACE is set: an event site costs one predictable branch. */
#ifndef HL_TRACE_H
#define HL_TRACE_H
#include <stddef.h>
#include <stdint.h>

/* What HL_TRACE=<mask> records (1: everything). */
enum { HL_TR_MEM = 2, HL_TR_JOBS = 4, HL_TR_BRICKS = 8, HL_TR_SCENE = 16, HL_TR_ALL = 0xff };
#define HL_TR_KIND(bit, n) ((uint32_t)(bit) << 8 | (uint32_t)(n))

/* Event kinds: (operands a, b, c) */
enum {
    HL_EV_ALLOC = HL_TR_KIND(HL_TR_MEM, 1),      /* block, size, caller (guest return address) */
    HL_EV_FREE = HL_TR_KIND(HL_TR_MEM, 2),       /* block, size (0: not known), caller */
    HL_EV_HEAP = HL_TR_KIND(HL_TR_MEM, 3),       /* heap, 1 made / 0 destroyed, caller */
    HL_EV_UNMAP = HL_TR_KIND(HL_TR_MEM, 4),      /* address, size, what (1 unmap, 2 decommit, 3 discard, 4 free pages) */
    HL_EV_NVN = HL_TR_KIND(HL_TR_MEM, 5),        /* storage, size, what (1 pool init, 2 pool final, 3 buffer init, ...) */
    HL_EV_GPU = HL_TR_KIND(HL_TR_MEM, 6),        /* guest, size, what (MKWVK_TRACE_*: the back end's writes, mirrors, imports) */
    HL_EV_COLD = HL_TR_KIND(HL_TR_BRICKS, 1),    /* address, size, 1 out / 0 back */
    HL_EV_DROP = HL_TR_KIND(HL_TR_BRICKS, 2),    /* address, size, 1 dropped / 0 restored */
    HL_EV_ZERO = HL_TR_KIND(HL_TR_BRICKS, 3),    /* address, size, what */
    HL_EV_STAGE = HL_TR_KIND(HL_TR_JOBS, 1),     /* worker set, scheduler, what (HL_STAGE_*) */
    HL_EV_GROUP = HL_TR_KIND(HL_TR_JOBS, 2),     /* group, context, what (HL_GROUP_*) */
    HL_EV_JOB = HL_TR_KIND(HL_TR_JOBS, 3),       /* job's object, job record, what (HL_JOB_*) */
    HL_EV_FILE = HL_TR_KIND(HL_TR_SCENE, 1),     /* name hash, handle, what */
};
enum { HL_STAGE_REGISTER = 1, HL_STAGE_START = 2, HL_STAGE_WAIT = 3, HL_STAGE_WAITED = 4, HL_STAGE_NONE = 5, HL_STAGE_LAST = 6 };
enum { HL_GROUP_ENTER = 1, HL_GROUP_LEAVE = 2, HL_GROUP_OVER = 3, HL_GROUP_SEED = 4 };
enum { HL_JOB_RUN = 1, HL_JOB_DONE = 2 };

extern volatile uint32_t hl_trace_mask;   /* 0: off */
void hl_trace_event(uint32_t kind, uint64_t a, uint64_t b, uint32_t c);
#define HL_EV(kind, a, b, c) \
    do { if (__builtin_expect((hl_trace_mask & ((uint32_t)(kind) >> 8)) != 0, 0)) hl_trace_event((kind), (a), (b), (c)); } while (0)

/* ---- the core (hl_trace_core.c) ---- */
enum { HL_TRACE_CHAIN = 6 };
typedef struct {
    uint64_t addr, size, heap, stamp;
    uint32_t tid, chain[HL_TRACE_CHAIN];
} HlTraceRecord;
typedef struct {
    HlTraceRecord freed, alloc;   /* the free and, when it was seen, the allocation */
    int has_alloc;
} HlTraceFree;

typedef struct {
    const char *(*thread_name)(void);   /* the calling thread's name (copied at its first event) */
    void (*out)(const char *line);      /* one line of a report */
    uint32_t ring_events;               /* a thread's ring (a power of two; 0: 65536) */
    uint32_t live_slots;                /* the live blocks' table (a power of two of 256 or more; 0: 2 M) */
    uint32_t freed_slots;               /* the last frees kept (a power of two; 0: 1 M) */
} HlTraceConfig;

int hl_trace_start(uint32_t mask, const HlTraceConfig *config);
uint64_t hl_trace_now(void);   /* the events' clock (ticks) */
double hl_trace_ticks_per_ms(void);
void hl_trace_alloc(uint64_t block, uint64_t size, uint64_t heap, const uint32_t *chain);
void hl_trace_free(uint64_t block, uint64_t heap, const uint32_t *chain);
/* The block `addr` is in: 1 live (*live set), 2 freed and not allocated again since (*freed set), 3 both (freed, then
   another block allocated over it: *live is the new one), 0 none known. */
int hl_trace_lookup(uint64_t addr, HlTraceRecord *live, HlTraceFree *freed);
/* The report: the blocks of `fault` and of the n values in regs (with their names), then the last `own` events of the
   calling thread and the last `others` of every other thread, merged by time. */
void hl_trace_report(uint64_t fault, const uint64_t *regs, const char *const *names, int n, int own, int others);
/* Every ring, the freed table and the live table to a file. 0 when written. */
int hl_trace_write(const char *path);
/* The back end's writes into guest memory and its mirrors' and imports' lives (gpu/vk/vk_api.h MKWVK_TRACE_*): kept in
   a ring of their own, told beside a block at a crash. done: the GPU's completed timeline value when it was recorded. */
void hl_trace_gpu_record(uint32_t what, uint64_t guest, uint64_t size, uint64_t pool, uint64_t value, uint64_t done);
/* (counters for the log) */
void hl_trace_counts(uint64_t *allocs, uint64_t *frees, uint64_t *unknown_frees, uint64_t *live_full, uint64_t *threads);

#endif
