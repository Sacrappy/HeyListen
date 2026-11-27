// Copyright (C) 2026 Sacrappy

/* (HeyListen, BOOT1) The game's allocator returned null: told once, with what the game itself would need to know.

   "Title and intro" crashes since v0.4.1 (family B of v0.4.3's reports: f_8572f300+0x31 from f_80c4afa0, f_80a9f3c0,
   f_83b51a00, f_80a9e750; RTX 3050 / 4060 Ti, RX 580 / 9070 alike) are the game's operator new (f_8005ebe0) returning 0 while
   its modules are made, and the caller writing through the result. A log of such a crash held only the guest's frames: not which
   heap, nor what was asked. nx2/hl_gates.py's HL_NEW_HOOK lines call hl_oot_new_null from the three places where operator new
   (f_8005ebe0) and aligned_alloc (f_8005eca0) end in null:
     kind 0: the heap's own tryAlloc (vtable +0x30) returned 0 (heap = the one asked, or the thread's current one)
     kind 1: there was no heap to take from (the thread has no current heap and the root's slot is empty)
     kind 2: aligned_alloc failed (the size is not known there)
   A block that was made costs one compare of x0. The null is never changed: the game goes on as shipped.

   What is told (hos_log, the first 12 times, then every 256th): the kind, the size and alignment asked, the heap (address, name,
   size, free size by its own getFreeSize) and its parents up to the root with the same, the caller (guest return address, and
   the one above it when the frame chain shows it), and the thread. A heap of the sead
   class here has +0x20 its name, +0x28 its memory, +0x30 its size, +0x38 its parent, vtable +0x80 its free size (hl_heap_retry.c).

   HOS_NEW_NULL_LOG=0: nothing told. */
#include "hl_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the lift writes a function in one of two forms: its registers kept in C locals (x0_, x31_), or in the context (c->x[0]). The
   functions named here are kept in the context (nx2/guest_locals.py keep_mentioned), but a lift made before a line was added
   here still has the locals: the lift key covers nx2/, not host/hl_*.c. So each of the three has two hashes: the older lift's
   text, and a lift made with these lines in place (any fresh lift: a Linux builder's, the laptop's). Both are hooked; the
   anchors (hl_gates.py new_places) fit both forms. A form of neither list is warned as before) */
HL_NEW_HOOK(8005e9d0, "cc5c961686c8cd09,1f2826d67e2d3e25");   /* operator new variants (size, heap, alignment): the same function written three ways */
HL_NEW_HOOK(8005eb20, "a523c51881267cd9,0c50092f6cb3904f");
HL_NEW_HOOK(8005ebe0, "b26a4038456275d8,fb3beb00575415da");
HL_NEW_HOOK(8005eca0, "0ce5468d82ee701d");   /* aligned_alloc (alignment, size) */
HL_NEW_HOOK(83b7b340, "39c8d417e1c2204e");   /* the module system: each module heap is made here */

enum { HEAP_NAME = 0x20, HEAP_START = 0x28, HEAP_SIZE = 0x30, HEAP_PARENT = 0x38, VT_FREE_SIZE = 0x80 };

static int readable(uint64_t a) { return a >= 0x80000000ULL && a < (1ULL << 39) - 0x100; }
static void guest_string(Ctx *c, uint64_t at, char *out, int n) {
    int i = 0, ok = readable(at);
    for (; ok && i < n - 1; i++) {
        const char ch = (char)recomp_load8(c, at + i);
        if (!ch) break;
        if (ch < 32 || ch >= 127) ok = 0;
        out[i] = ch;
    }
    if (!ok || i < 2) i = 0, out[i++] = '?';
    out[i] = 0;
}
/* the heap's own getFreeSize, called as the game would (~0: not known) */
static uint64_t free_size(Ctx *c, uint64_t heap) {
    if (!readable(heap)) return ~0ULL;
    const uint64_t vt = recomp_load64(c, heap);
    const uint64_t fn = readable(vt) ? recomp_load64(c, vt + VT_FREE_SIZE) : 0;
    if (!fn || !a64_lookup(fn)) return ~0ULL;
    uint64_t x[31];
    const uint8_t n = c->n, z = c->z, cf = c->c, v = c->v;
    memcpy(x, c->x, sizeof x);
    c->x[0] = heap, c->x[1] = c->x[2] = 0;
    a64_icall(c, fn);
    const uint64_t r = c->x[0];
    memcpy(c->x, x, sizeof x);
    c->n = n, c->z = z, c->c = cf, c->v = v;
    return r;
}
static int describe(Ctx *c, uint64_t heap, char *out, int cap) {
    if (!readable(heap)) return snprintf(out, cap, "heap %#llx (not a guest address)", (unsigned long long)heap);
    char name[48], fr[32];
    guest_string(c, recomp_load64(c, heap + HEAP_NAME), name, sizeof name);
    const uint64_t f = free_size(c, heap);
    if (f == ~0ULL) snprintf(fr, sizeof fr, "?");
    else snprintf(fr, sizeof fr, "%llu", (unsigned long long)(f >> 10));
    return snprintf(out, cap, "heap %#llx \"%s\" (%llu KB, %s KB free, memory %#llx)", (unsigned long long)heap, name,
                    (unsigned long long)(recomp_load64(c, heap + HEAP_SIZE) >> 10), fr,
                    (unsigned long long)recomp_load64(c, heap + HEAP_START));
}

/* (OLD1, the crash report) the last of these lines, kept for the crash handler (host/mkw_glue.c "CRASH: last failed allocation"):
   the log tells only the first twelve, and the report's log tail may have lost them */
static char alloc_null_last[400];
static void alloc_null_keep(const char *line) {
    snprintf(alloc_null_last, sizeof alloc_null_last, "%s", line);
}
const char *hl_alloc_null_last(void) { return alloc_null_last; }

void hl_oot_new_null(Ctx *c, uint64_t heap, uint64_t size, uint64_t align, uint64_t lr, int kind) {
    static volatile long told;
    static int off = -1;
    if (off < 0) off = getenv("HOS_NEW_NULL_LOG") && atoi(getenv("HOS_NEW_NULL_LOG")) == 0;
    if (off) return;
    const long k = __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED);
    if (k >= 12 && (k & 255)) return;
    static const char *const how[] = {"the heap's tryAlloc returned 0", "no heap to take from", "aligned_alloc failed"};
    char line[1400];
    int at = snprintf(line, sizeof line, "oot: the game's allocator returned null (#%ld, %s): size %#llx align %llu, caller %#llx, ",
                      k + 1, how[kind >= 0 && kind < 3 ? kind : 0], (unsigned long long)size, (unsigned long long)align,
                      (unsigned long long)lr);
    HosThread *t = hos_self();
    at += snprintf(line + at, sizeof line - at, "thread %s; ", t ? hos_thread_name(t) : "host");
    if (kind == 0) {
        at += describe(c, heap, line + at, (int)sizeof line - at);
        uint64_t p = readable(heap) ? recomp_load64(c, heap + HEAP_PARENT) : 0;
        for (int depth = 0; p && readable(p) && depth < 6 && at < (int)sizeof line - 200; depth++) {
            at += snprintf(line + at, sizeof line - at, "; parent ");
            at += describe(c, p, line + at, (int)sizeof line - at);
            p = recomp_load64(c, p + HEAP_PARENT);
        }
    } else {
        at += snprintf(line + at, sizeof line - at, "no heap");
    }
    (void)at;
    hos_log("%s", line);
    alloc_null_keep(line);
}

/* ---- the module system's heaps: f_83b7b340 makes each module's heap with f_809df710 (x0 the size, x1 a record whose first word is
   the name, x2 the parent heap). A heap that cannot be made (the parent has no room for it) leaves the module's heap null, and the
   first allocation made in that module is the null above. hl_oot_mheap_pre saves the arguments before the call, hl_oot_mheap_post
   tells a null result after it (the first 12) ---- */
static __thread struct { uint64_t size, name, parent; } mh;
static void mheap_tell(Ctx *c, const char *what) {
    static volatile long told;
    static int off = -1;
    if (off < 0) off = getenv("HOS_NEW_NULL_LOG") && atoi(getenv("HOS_NEW_NULL_LOG")) == 0;
    if (off || __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) >= 12) return;
    char nm[48], line[1400];
    guest_string(c, mh.name, nm, sizeof nm);
    int at = snprintf(line, sizeof line, "oot: a module heap was not made: \"%s\", %s (%llu KB), parent ", nm, what,
                      (unsigned long long)(mh.size >> 10));
    at += describe(c, mh.parent, line + at, (int)sizeof line - at);
    uint64_t p = readable(mh.parent) ? recomp_load64(c, mh.parent + HEAP_PARENT) : 0;
    for (int depth = 0; p && readable(p) && depth < 6 && at < (int)sizeof line - 200; depth++) {
        at += snprintf(line + at, sizeof line - at, "; its parent ");
        at += describe(c, p, line + at, (int)sizeof line - at);
        p = recomp_load64(c, p + HEAP_PARENT);
    }
    hos_log("%s", line);
}
/* before the module's size is tested: x0 the size the heap definition gives it (0: the module has no heap of its own, as most), the name
   at sp + 8, the parent in x23 */
void hl_oot_mheap_pre(Ctx *c) {
    mh.size = c->x[0];
    mh.name = readable(c->x[31] + 8) ? recomp_load64(c, c->x[31] + 8) : 0;
    mh.parent = c->x[23];
}
/* after the call that makes it: x0 the heap, 0 when the parent had no room */
void hl_oot_mheap_post(Ctx *c) {
    if (!c->x[0] && mh.size) mheap_tell(c, "the parent had no room for the size asked");
}
