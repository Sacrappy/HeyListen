/* (HeyListen native engine) The crash recorder's core (hl_trace.h): the threads' rings, the live and freed blocks' tables,
   the report and the file. Plain C11 with no OS layer, so that tests/hl_trace_test.c runs it with no game. */
#include "hl_trace.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

volatile uint32_t hl_trace_mask;

typedef struct {
    uint64_t stamp;
    uint32_t kind, c;
    uint64_t a, b;
} Event;
_Static_assert(sizeof(Event) == 32, "an event is 32 bytes");

typedef struct {
    _Atomic uint64_t head;   /* events written so far (the slot of event i is i & mask) */
    uint32_t tid;
    char name[32];
    Event *ev;
} Ring;

enum { MAX_RINGS = 512, STRIPES = 256 };
static HlTraceConfig cfg;
static Ring rings[MAX_RINGS];
static _Atomic uint32_t nrings;
static _Thread_local Ring *my_ring;
static uint32_t ring_mask;
static double tick_ms = 1e6;

/* the live blocks: STRIPES open-addressed tables (linear probing, backward-shift removal), each under a spin lock */
typedef struct {
    atomic_flag lock;
    uint32_t used;
    HlTraceRecord *slot;   /* addr 0: empty */
} Stripe;
static Stripe stripes[STRIPES];
static uint32_t stripe_slots;
/* the last frees: a ring */
static HlTraceFree *freed;
static uint32_t freed_mask;
static _Atomic uint64_t freed_head;
static _Atomic uint64_t n_allocs, n_frees, n_unknown, n_full;

uint64_t hl_trace_now(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

double hl_trace_ticks_per_ms(void) { return tick_ms; }

static uint64_t wall_ns(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

int hl_trace_start(uint32_t mask, const HlTraceConfig *config) {
    if (hl_trace_mask) return 1;
    cfg = *config;
    const uint32_t events = cfg.ring_events ? cfg.ring_events : 65536;
    ring_mask = events - 1;
    stripe_slots = (cfg.live_slots ? cfg.live_slots : 2u << 20) / STRIPES;
    const uint32_t nfreed = cfg.freed_slots ? cfg.freed_slots : 1u << 20;
    freed_mask = nfreed - 1;
    freed = calloc(nfreed, sizeof *freed);
    if (!freed) return 0;
    for (int s = 0; s < STRIPES; s++) {
        atomic_flag_clear(&stripes[s].lock);
        stripes[s].slot = calloc(stripe_slots, sizeof(HlTraceRecord));
        if (!stripes[s].slot) return 0;
    }
    /* (the clock: ticks against the wall clock over about 20 ms) */
    const uint64_t w0 = wall_ns(), t0 = hl_trace_now();
    while (wall_ns() - w0 < 20000000u) {}
    const uint64_t w1 = wall_ns(), t1 = hl_trace_now();
    tick_ms = (double)(t1 - t0) / ((double)(w1 - w0) / 1e6);
    if (tick_ms <= 0) tick_ms = 1e6;
    hl_trace_mask = mask;
    return 1;
}

/* The calling thread's ring, made and listed at its first event. */
static Ring *ring(void) {
    Ring *r = my_ring;
    if (r) return r;
    const uint32_t i = atomic_fetch_add(&nrings, 1);
    if (i >= MAX_RINGS) return NULL;
    r = &rings[i];
    r->ev = calloc((size_t)ring_mask + 1, sizeof(Event));
    r->tid = i;
    const char *name = cfg.thread_name ? cfg.thread_name() : NULL;
    snprintf(r->name, sizeof r->name, "%s", name ? name : "?");
    my_ring = r;
    return r;
}

void hl_trace_event(uint32_t kind, uint64_t a, uint64_t b, uint32_t c) {
    Ring *r = ring();
    if (!r || !r->ev) return;
    const uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    Event *e = &r->ev[h & ring_mask];
    e->stamp = hl_trace_now();
    e->kind = kind;
    e->c = c;
    e->a = a;
    e->b = b;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);   /* (a reader takes events below head) */
}

/* ---- the live blocks ---- */

static uint32_t hash(uint64_t a) { return (uint32_t)((a >> 4) * 0x9E3779B97F4A7C15ull >> 32); }

static void lock(Stripe *s) {
    while (atomic_flag_test_and_set_explicit(&s->lock, memory_order_acquire)) {}
}
static void unlock(Stripe *s) { atomic_flag_clear_explicit(&s->lock, memory_order_release); }

static void record(HlTraceRecord *r, uint64_t block, uint64_t size, uint64_t heap, const uint32_t *chain) {
    r->addr = block;
    r->size = size;
    r->heap = heap;
    r->stamp = hl_trace_now();
    Ring *g = ring();
    r->tid = g ? g->tid : 0xffffffffu;
    for (int k = 0; k < HL_TRACE_CHAIN; k++) r->chain[k] = chain ? chain[k] : 0;
}

void hl_trace_alloc(uint64_t block, uint64_t size, uint64_t heap, const uint32_t *chain) {
    if (!block || !hl_trace_mask) return;
    atomic_fetch_add_explicit(&n_allocs, 1, memory_order_relaxed);
    hl_trace_event(HL_EV_ALLOC, block, size, chain ? chain[0] : 0);
    const uint32_t h = hash(block);
    Stripe *s = &stripes[h & (STRIPES - 1)];
    const uint32_t m = stripe_slots - 1;
    lock(s);
    uint32_t i = (h >> 8) & m;
    while (s->slot[i].addr && s->slot[i].addr != block) i = (i + 1) & m;
    if (!s->slot[i].addr) {
        if (s->used + 1 >= stripe_slots - stripe_slots / 8) {   /* (full enough: not listed) */
            unlock(s);
            atomic_fetch_add_explicit(&n_full, 1, memory_order_relaxed);
            return;
        }
        s->used++;
    }
    record(&s->slot[i], block, size, heap, chain);
    unlock(s);
}

void hl_trace_free(uint64_t block, uint64_t heap, const uint32_t *chain) {
    if (!block || !hl_trace_mask) return;
    atomic_fetch_add_explicit(&n_frees, 1, memory_order_relaxed);
    HlTraceFree f;
    memset(&f, 0, sizeof f);
    const uint32_t h = hash(block);
    Stripe *s = &stripes[h & (STRIPES - 1)];
    const uint32_t m = stripe_slots - 1;
    lock(s);
    uint32_t i = (h >> 8) & m;
    while (s->slot[i].addr && s->slot[i].addr != block) i = (i + 1) & m;
    if (s->slot[i].addr) {
        f.alloc = s->slot[i];
        f.has_alloc = 1;
        /* (backward-shift removal: the entries after it move up while their home is not between) */
        uint32_t j = i;
        for (;;) {
            s->slot[i].addr = 0;
            for (;;) {
                j = (j + 1) & m;
                if (!s->slot[j].addr) goto removed;
                const uint32_t home = (hash(s->slot[j].addr) >> 8) & m;
                if (i <= j ? (i < home && home <= j) : (i < home || home <= j)) continue;
                break;
            }
            s->slot[i] = s->slot[j];
            i = j;
        }
    removed:
        s->used--;
    }
    unlock(s);
    if (!f.has_alloc) atomic_fetch_add_explicit(&n_unknown, 1, memory_order_relaxed);
    record(&f.freed, block, f.has_alloc ? f.alloc.size : 0, heap, chain);
    hl_trace_event(HL_EV_FREE, block, f.freed.size, chain ? chain[0] : 0);
    const uint64_t at = atomic_fetch_add_explicit(&freed_head, 1, memory_order_relaxed);
    freed[at & freed_mask] = f;
}

int hl_trace_lookup(uint64_t addr, HlTraceRecord *live, HlTraceFree *fr) {
    int got = 0;
    HlTraceRecord lv;
    memset(&lv, 0, sizeof lv);
    if (!hl_trace_mask) return 0;
    for (int k = 0; k < STRIPES && !(got & 1); k++) {
        const Stripe *s = &stripes[k];
        for (uint32_t i = 0; i < stripe_slots; i++) {
            const HlTraceRecord *r = &s->slot[i];
            if (r->addr && addr >= r->addr && addr < r->addr + (r->size ? r->size : 1)) {
                lv = *r;
                got |= 1;
                break;
            }
        }
    }
    const uint64_t head = atomic_load(&freed_head);
    const uint64_t n = head < (uint64_t)freed_mask + 1 ? head : (uint64_t)freed_mask + 1;
    for (uint64_t k = 1; k <= n; k++) {   /* (the latest free first) */
        const HlTraceFree *f = &freed[(head - k) & freed_mask];
        const uint64_t size = f->freed.size ? f->freed.size : 1;
        if (f->freed.addr && addr >= f->freed.addr && addr < f->freed.addr + size) {
            /* (a live block over it allocated before this free is not "over it") */
            if (!(got & 1) || lv.stamp > f->freed.stamp) {
                if (fr) *fr = *f;
                got |= 2;
            }
            break;
        }
    }
    if ((got & 1) && live) *live = lv;
    return got;
}

/* ---- the report ---- */

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (cfg.out) cfg.out(line);
}

static const char *ring_name(uint32_t tid) { return tid < MAX_RINGS && tid < atomic_load(&nrings) ? rings[tid].name : "?"; }

static double ms_ago(uint64_t stamp, uint64_t now) { return (double)(int64_t)(now - stamp) / tick_ms; }

static void chain_text(char *out, size_t cap, const uint32_t *chain) {
    int n = 0;
    out[0] = 0;
    for (int k = 0; k < HL_TRACE_CHAIN && chain[k] && n < (int)cap - 12; k++) n += snprintf(out + n, cap - n, " %#x", chain[k]);
}

static void print_record(const char *what, const HlTraceRecord *r, uint64_t now) {
    char ch[96];
    chain_text(ch, sizeof ch, r->chain);
    outf("TRACE:     %s %.3f ms ago on %s, heap %#llx, size %#llx, by%s", what, ms_ago(r->stamp, now), ring_name(r->tid),
         (unsigned long long)r->heap, (unsigned long long)r->size, ch);
}

static void print_block(const char *what, uint64_t addr, uint64_t now) {
    HlTraceRecord live;
    HlTraceFree fr;
    const int got = hl_trace_lookup(addr, &live, &fr);
    if (!got) return;
    if (got & 2) {
        outf("TRACE: %s %#llx is in a FREED block %#llx (+%#llx)%s", what, (unsigned long long)addr,
             (unsigned long long)fr.freed.addr, (unsigned long long)(addr - fr.freed.addr),
             (got & 1) ? ", since allocated again (below)" : "");
        print_record("freed", &fr.freed, now);
        if (fr.has_alloc) print_record("allocated", &fr.alloc, now);
    }
    if (got & 1) {
        outf("TRACE: %s %#llx is in a live block %#llx (+%#llx)", what, (unsigned long long)addr, (unsigned long long)live.addr,
             (unsigned long long)(addr - live.addr));
        print_record("allocated", &live, now);
    }
}

static const char *kind_name(uint32_t kind) {
    switch (kind) {
    case HL_EV_ALLOC: return "alloc";
    case HL_EV_FREE: return "free";
    case HL_EV_HEAP: return "heap";
    case HL_EV_UNMAP: return "unmap";
    case HL_EV_NVN: return "nvn";
    case HL_EV_COLD: return "cold";
    case HL_EV_DROP: return "drop";
    case HL_EV_ZERO: return "zero";
    case HL_EV_STAGE: return "stage";
    case HL_EV_GROUP: return "group";
    case HL_EV_JOB: return "job";
    case HL_EV_FILE: return "file";
    default: return "?";
    }
}

typedef struct {
    Event e;
    uint32_t tid;
} Tagged;

static int by_stamp(const void *x, const void *y) {
    const uint64_t a = ((const Tagged *)x)->e.stamp, b = ((const Tagged *)y)->e.stamp;
    return a < b ? -1 : a > b;
}

void hl_trace_report(uint64_t fault, const uint64_t *regs, const char *const *names, int n, int own, int others) {
    if (!hl_trace_mask) return;
    const uint64_t now = hl_trace_now();
    uint64_t a, f, u, full, t;
    hl_trace_counts(&a, &f, &u, &full, &t);
    outf("TRACE: the recorder: %llu allocations, %llu frees (%llu of blocks it did not see allocated), %llu not listed "
         "(table full), %llu threads", (unsigned long long)a, (unsigned long long)f, (unsigned long long)u,
         (unsigned long long)full, (unsigned long long)t);
    if (fault) print_block("the faulting address", fault, now);
    for (int k = 0; k < n; k++)
        if (regs[k] >= 0x10000) print_block(names[k], regs[k], now);
    const uint32_t nr = atomic_load(&nrings) < MAX_RINGS ? atomic_load(&nrings) : MAX_RINGS;
    size_t cap = 0;
    for (uint32_t r = 0; r < nr; r++) cap += (size_t)(&rings[r] == my_ring ? own : others);
    Tagged *all = calloc(cap ? cap : 1, sizeof *all);
    if (!all) return;
    size_t m = 0;
    for (uint32_t r = 0; r < nr; r++) {
        Ring *g = &rings[r];
        if (!g->ev) continue;
        const uint64_t head = atomic_load_explicit(&g->head, memory_order_acquire);
        const uint64_t want = (uint64_t)(g == my_ring ? own : others);
        const uint64_t take = head < want ? head : want;
        for (uint64_t k = head - take; k < head; k++) all[m++] = (Tagged){g->ev[k & ring_mask], r};
    }
    qsort(all, m, sizeof *all, by_stamp);
    outf("TRACE: the last events (%d of this thread, %d of each other), oldest first:", own, others);
    for (size_t k = 0; k < m; k++) {
        const Event *e = &all[k].e;
        outf("TRACE: %10.3f ms %-24s %-6s %#llx %#llx %#x", -ms_ago(e->stamp, now), ring_name(all[k].tid), kind_name(e->kind),
             (unsigned long long)e->a, (unsigned long long)e->b, e->c);
    }
    free(all);
}

void hl_trace_counts(uint64_t *allocs, uint64_t *frees, uint64_t *unknown_frees, uint64_t *live_full, uint64_t *threads) {
    *allocs = atomic_load(&n_allocs);
    *frees = atomic_load(&n_frees);
    *unknown_frees = atomic_load(&n_unknown);
    *live_full = atomic_load(&n_full);
    *threads = atomic_load(&nrings);
}

/* ---- the file (tools/bench/hltrace.py) ----
   "HLTRACE1", ticks a millisecond (double), the clock now (u64), ring events (u32), rings (u32); each ring: name (32),
   id (u32), head (u64), its events (ring events x 32 bytes); frees kept (u32), freed head (u64), the freed ring
   (HlTraceFree each); live slots (u32), the live table (HlTraceRecord each, address 0 empty). */
int hl_trace_write(const char *path) {
    if (!hl_trace_mask) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    const uint64_t now = hl_trace_now();
    const uint32_t events = ring_mask + 1, nr = atomic_load(&nrings) < MAX_RINGS ? atomic_load(&nrings) : MAX_RINGS;
    fwrite("HLTRACE1", 1, 8, fp);
    fwrite(&tick_ms, sizeof tick_ms, 1, fp);
    fwrite(&now, sizeof now, 1, fp);
    fwrite(&events, 4, 1, fp);
    fwrite(&nr, 4, 1, fp);
    for (uint32_t r = 0; r < nr; r++) {
        const uint64_t head = atomic_load(&rings[r].head);
        fwrite(rings[r].name, 1, 32, fp);
        fwrite(&rings[r].tid, 4, 1, fp);
        fwrite(&head, 8, 1, fp);
        if (rings[r].ev) fwrite(rings[r].ev, sizeof(Event), events, fp);
        else for (uint32_t k = 0; k < events; k++) fwrite(&(Event){0}, sizeof(Event), 1, fp);
    }
    const uint32_t nf = freed_mask + 1;
    const uint64_t fh = atomic_load(&freed_head);
    fwrite(&nf, 4, 1, fp);
    fwrite(&fh, 8, 1, fp);
    fwrite(freed, sizeof *freed, nf, fp);
    const uint32_t nl = stripe_slots * STRIPES;
    fwrite(&nl, 4, 1, fp);
    for (int s = 0; s < STRIPES; s++) fwrite(stripes[s].slot, sizeof(HlTraceRecord), stripe_slots, fp);
    return fclose(fp) ? -1 : 0;
}
