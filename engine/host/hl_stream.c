/* (HeyListen native engine) The OS layer's side of the stream module (engine/native, hl/c/hl_stream.h;
   docs/native-engine/stream-boundary.md): the game's decode of a resource file, answered natively.

   The resource system decodes every file it loads through one function (guest 0x80108da0, reached only through slot 2 of
   its decompressor's vtable), answered here instead of by its translation (hl_host.h HL_ANSWER: the lifted code is not
   touched). It opens the request's file object, reads the compressed file into the decompressor's read buffer, reads the
   zstd frame header, gets the destination (given, or allocated from the request's heap) and decodes the frame with the
   dictionary the frame names; a file larger than the read buffer is read chunk by chunk while a second thread decodes.
     HL_STREAM=0  off: the translated function, as before
     HL_STREAM=1  native (the default): the function's main path here, its decode by zstd's own decoder (third_party/zstd) with the
                  game's dictionaries (their bytes read from the game's dictionary objects); a file larger than one
                  read is decoded here too, chunk by chunk as it is read, by zstd's streaming decoder, with the
                  decompressor's descriptor and events kept as the game's decoding thread keeps them (no chunk is given
                  to that thread); the rare paths go to the translated function (hl::stream::choose;
                  stream-boundary.md sections 5 and 7)
   HL_STREAM_CHECK=1: the translated function answers, and after each decode the same compressed bytes are decoded here
   and compared with the destination byte for byte (the chunked case's chunks are collected as its decoding thread is
   given them: guest 0x80115bf0, also reached only through a vtable, is wrapped; they are decoded by the native path's
   streaming decode, fed in the same chunks). Counts and the first mismatches are logged. With the check on the
   translated function answers whatever HL_STREAM says.
   HL_STREAM_CHECK=2: the native function answers (HL_STREAM=1), and each chunked file it decodes is decoded again in one
   call from the bytes it read and compared with what it wrote.
   HL_STREAM_LOG=1: the counters every 10 s (always with the check). Every decode is recorded in the provenance table
   (hl::stream::Provenance: destination range, file, sizes, dictionary), not used yet. */
#include "hl_host.h"
#include "hl/c/hl_stream.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define HL_STREAM_DEFAULT 1   /* (validated in the three scenes, check at 0 mismatches and shots right: stream-boundary.md section 8) */
#define HL_DECODE_FILE 0x80108da0ULL    /* the resource decompressor's decode of a file (vtable 0x84c377e8, slot 2) */
#define HL_DECODE_CHUNK 0x80115bf0ULL   /* its parallel descriptor's step: one chunk decoded (vtable 0x84c377c0, slot 2) */

/* ---- the supported build's addresses and layouts (stream-boundary.md sections 2 and 3) ---- */
#define G_HOLDER_CLASS 0x84ca2d18ULL    /* (pointer) the file holder's class: its object's first word is this + 16 */
#define G_HEAPS 0x84c9dff0ULL           /* (pointer to a pointer) the heap system: 0 before it exists */
#define G_SKIP_PREPARE 0x84cb4768ULL    /* (function pointer) a file object's slot 4 that does nothing */
#define G_FORWARD_PREPARE 0x84cb47b0ULL /* (function pointer) a file object's slot 4 that forwards to the object at +96 */
#define G_ALWAYS_READY 0x84cb4788ULL    /* (function pointer) a file handle's slot 9 that says yes */
#define G_GUARD 0x84ca2ad0ULL           /* (pointer) the guard of a function-local static (the allocator kind's id) */
#define G_STATIC 0x84ca2ac8ULL          /* (pointer) that static */
#define G_STATIC_VALUE 0x84ca2ad8ULL    /* (pointer) what it is set to, + 16 */
#define G_ERROR_TABLE 0x83f44d60ULL     /* three 32-bit result codes for the decoder's errors -1, -2, -3 */
#define F_GUARD_ACQUIRE 0x857ec268ULL
#define F_GUARD_RELEASE 0x857ec2f0ULL
#define F_ALIGNED_ALLOC 0x8005eca0ULL
#define F_FREE 0x8021f7c0ULL
#define F_CLOSE 0x80108c90ULL
/* the decompressor (the first argument) */
#define D_DICTS 112                     /* its dictionaries by id: a tree (node +0 left, +8 right, +24 id, +32 the DDict) */
#define D_CHUNK 1424                    /* (u32) its read buffer's chunk */
#define D_BUFFER 1432                   /* its read buffer: the decoder's workspace, then two chunks */
#define D_DESC 1440                     /* the chunked case's descriptor (what the decoding thread is given) */
/* that descriptor's fields, by their offset in the decompressor (stream-boundary.md section 5) */
#define K_STATE 1456                    /* (u8) 1 a frame to start, 2 under way, 3 failed, 4 decoded */
#define K_DEST 1464                     /* where the next decoded byte goes */
#define K_LEFT 1472                     /* (s32) room left in the destination; +4: the frame's bytes still to come */
#define K_WORK 1480                     /* the decoder's workspace (the read buffer's start) */
#define K_STAGE 1488                    /* a staging area in that workspace, and +8 (u32) its size, +12 (u32) its fill */
#define K_STAGE_SIZE 1496
#define K_DCTX 1504                     /* the decoder context, set by the first chunk */
#define K_FRAME_DONE 1544               /* light event: the frame ended (or failed); set when idle */
#define K_CHUNK_DONE 1592               /* light event: the chunk handed over is decoded */
#define K_SOURCE 1608                   /* the first source buffer (the second is a chunk after it) */
#define K_CHUNK 1616                    /* (u32) the chunk */
#define K_INDEX 1620                    /* (u32) chunks decoded; +4 (u32): the byte count of the chunk handed over */
#define K_COUNT 1624
#define K_STATUS 1628                   /* (s32) negative: the decoder's error */
#define K_DDICT 1632                    /* the guest DDict, 0 without a dictionary */
#define STAGE_OFFSET 0x271f8            /* the staging area's place in the workspace */
#define F_SIGNAL_LIGHT 0x85101620ULL
#define F_CLEAR_LIGHT 0x85101710ULL
#define F_WAIT_LIGHT 0x85101740ULL
#define WORKSPACE 290816                /* the decoder's workspace at the read buffer's start */
#define BUFFER_FIXED 0x471f9u           /* the buffer's size is this + 2 chunks + 30 */
/* the request (the second argument) */
#define R_HEAP 16
#define R_DIRECTION 24                  /* (s32) its sign chooses the allocation's direction */
#define R_ALIGN 28                      /* (s32) */
#define R_DEST 32                       /* a destination given (0: allocate) */
#define R_LIMIT 40                      /* (u32) the largest decompressed size accepted (0: any) */
#define R_STEP 44                       /* (u32) the size granularity (0: 32) */
#define R_FILE 56                       /* the file object */
#define R_READ_MAX 64                   /* (u32) the largest single read (0: one read of a chunk) */
#define R_QUIET 69                      /* (bit 0) no result code on some errors */
#define R_RESULT 80                     /* where a result code goes (0: none) */
/* a file object, a file handle (their virtual slots by byte offset) */
#define F_KIND 88                       /* (u8) 1: opened through its own slot 13 */
#define V_PREPARE 32
#define V_OPEN 104
#define V_READY 72
#define V_READ 144
/* the frame the translated function builds below the caller's stack pointer (mirrored: callees see the same memory) */
#define FRAME 256
#define S_OUT_SIZE 24
#define S_OUT_ALLOC 32
#define S_OUT_FLAG 40
#define S_READ 52
#define S_HOLDER 56                     /* the file holder: class word, handle (+8), owner (+16), then zeros to +111 */
#define S_READ_MAX 112
#define S_COUNT 120                     /* a read's count in the loop of reads (the frame header's place) */

static int stream_mode, check_on, log_on;
static int self_check;   /* HL_STREAM_CHECK=2: the native path answers, and each chunked file it decoded is decoded again
                            in one call from the bytes it read and compared */
static A64Fn translated_file, translated_chunk;

/* ---- guest memory ---- */
static inline uint8_t l8(uint64_t a) { return *(const uint8_t *)GP(a); }
static inline uint32_t l32(uint64_t a) { uint32_t v; memcpy(&v, GP(a), 4); return v; }
static inline uint64_t l64(uint64_t a) { uint64_t v; memcpy(&v, GP(a), 8); return v; }
static inline void s8(Ctx *c, uint64_t a, uint64_t v) { recomp_store8(c, a, v); }
static inline void s32(Ctx *c, uint64_t a, uint64_t v) { recomp_store32(c, a, v); }
static inline void s64(Ctx *c, uint64_t a, uint64_t v) { recomp_store64(c, a, v); }
static inline void s32_sp(Ctx *c, uint64_t a, uint64_t v) { recomp_store32_sp(c, a, v); }
static inline void s64_sp(Ctx *c, uint64_t a, uint64_t v) { recomp_store64_sp(c, a, v); }

/* a guest call on the caller's own context: fn(a[0..n-1]); x0 after it */
static uint64_t gcall(Ctx *c, uint64_t fn, int n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    const uint64_t a[5] = {a0, a1, a2, a3, a4};
    for (int i = 0; i < n; i++) c->x[i] = a[i];
    a64_icall(c, fn);
    return c->x[0];
}

/* the request's file, as its heap names it (one heap per loaded file, named by its path: memory-boundary.md section 10) */
static void request_name(uint64_t req, char *out, size_t cap) {
    out[0] = 0;
    const uint64_t heap = l64(req + R_HEAP);
    if (!heap || !hos_guest_range(heap, 40)) return;
    const uint64_t name = l64(heap + 32);
    const char *p = name ? (const char *)hos_guest_range(name, cap) : NULL;
    if (!p) return;
    size_t i = 0;
    for (; i + 1 < cap && p[i] >= 32 && p[i] < 127; i++) out[i] = p[i];
    out[i] = 0;
}

/* ---- the dictionaries ---- */
/* The decompressor keeps its dictionaries as zstd DDicts in guest memory, by id; a DDict's +8 is its dictionary's bytes
   and +16 their size (zstd's own layout). Those bytes, checked (zstd's dictionary magic, the same id), make a DDict of
   ours, once a dictionary: the exact bytes the game decodes with, whichever archive they came from. */
typedef struct { uint64_t guest; uint32_t id; ZSTD_DDict *dd; } Dict;
enum { DICTS = 64 };
static Dict dicts[DICTS];
static int ndicts;
static SRWLOCK dict_lock = SRWLOCK_INIT;

/* the guest DDict of a dictionary id in the decompressor's tree, 0 when it has none */
static uint64_t guest_ddict(uint64_t obj, uint32_t id) {
    uint64_t node = l64(obj + D_DICTS);
    for (int depth = 0; node && depth < 64; depth++) {
        if (!hos_guest_range(node, 40)) return 0;
        const uint32_t key = l32(node + 24);
        if (id == key) return l64(node + 32);
        node = id > key ? l64(node + 8) : l64(node + 0);
    }
    return 0;
}

static ZSTD_DDict *host_ddict(uint64_t guest, uint32_t id) {
    AcquireSRWLockShared(&dict_lock);
    for (int i = 0; i < ndicts; i++)
        if (dicts[i].guest == guest && dicts[i].id == id) {
            ZSTD_DDict *dd = dicts[i].dd;
            ReleaseSRWLockShared(&dict_lock);
            return dd;
        }
    ReleaseSRWLockShared(&dict_lock);
    if (!guest || !hos_guest_range(guest, 24)) return NULL;
    const uint64_t content = l64(guest + 8), size = l64(guest + 16);
    const uint8_t *p = size >= 8 && size < (64u << 20) ? (const uint8_t *)hos_guest_range(content, size) : NULL;
    uint32_t magic = 0, did = 0;
    if (p) memcpy(&magic, p, 4), memcpy(&did, p + 4, 4);
    if (!p || magic != 0xEC30A437u || did != id) {
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 4)
            hos_log("hl stream: dictionary %u: the game's DDict %#llx does not hold a zstd dictionary of that id (bytes %#llx, "
                    "%llu, magic %#x, id %u)", id, (unsigned long long)guest, (unsigned long long)content,
                    (unsigned long long)size, magic, did);
        return NULL;
    }
    ZSTD_DDict *dd = ZSTD_createDDict(p, (size_t)size);
    if (!dd) return NULL;
    AcquireSRWLockExclusive(&dict_lock);
    for (int i = 0; i < ndicts; i++)
        if (dicts[i].guest == guest && dicts[i].id == id) {   /* (another thread made it meanwhile) */
            ZSTD_DDict *was = dicts[i].dd;
            ReleaseSRWLockExclusive(&dict_lock);
            ZSTD_freeDDict(dd);
            return was;
        }
    if (ndicts < DICTS) {
        dicts[ndicts++] = (Dict){guest, id, dd};
        hos_log("hl stream: dictionary %u: %llu bytes, from the game's DDict %#llx", id, (unsigned long long)size,
                (unsigned long long)guest);
    }
    ReleaseSRWLockExclusive(&dict_lock);
    return dd;
}

static ZSTD_DCtx *thread_dctx(void) {
    static _Thread_local ZSTD_DCtx *dctx;
    if (!dctx) dctx = ZSTD_createDCtx();
    return dctx;
}

/* ---- the chunked case's decode: zstd's streaming decoder writing straight into the destination ---- */
/* A context of its own (a DDict referenced for streaming would stay referenced for the one-call decodes otherwise). The
   destination is the output buffer for the whole frame (stableOutBuffer: no window buffer of zstd's, whatever the frame's
   window), and any window the frame names is accepted, as the game's decoder accepts it. */
typedef struct { ZSTD_DCtx *z; ZSTD_outBuffer out; int done; } Stream;

static int stream_begin(Stream *s, void *dst, size_t cap, ZSTD_DDict *dd) {
    static _Thread_local ZSTD_DCtx *own;
    if (!own) own = ZSTD_createDCtx();
    s->z = own, s->out = (ZSTD_outBuffer){dst, cap, 0}, s->done = 0;
    return own && !ZSTD_isError(ZSTD_DCtx_reset(own, ZSTD_reset_session_and_parameters)) &&
           !ZSTD_isError(ZSTD_DCtx_setParameter(own, ZSTD_d_stableOutBuffer, 1)) &&
           !ZSTD_isError(ZSTD_DCtx_setParameter(own, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX)) &&
           (!dd || !ZSTD_isError(ZSTD_DCtx_refDDict(own, dd)));
}

/* One chunk. 1: decoded (the frame may have ended in it; what follows the end in that chunk is left, as the game's
   decoder leaves it), 0: an error, no progress, or a chunk after the frame's end (an error in the game's decoder too). */
static int stream_feed(Stream *s, const void *src, size_t n) {
    if (s->done) return 0;
    ZSTD_inBuffer in = {src, n, 0};
    while (in.pos < in.size) {
        const size_t was_in = in.pos, was_out = s->out.pos;
        const size_t r = ZSTD_decompressStream(s->z, &s->out, &in);
        if (ZSTD_isError(r)) return 0;
        if (!r) { s->done = 1; return 1; }
        if (in.pos == was_in && s->out.pos == was_out) return 0;
    }
    return 1;
}

/* a whole chunked file in memory, fed in the decompressor's chunks (the check's side): 1 when the frame decoded to `fcs` */
static int stream_whole(void *dst, size_t fcs, const uint8_t *src, size_t n, size_t chunk, ZSTD_DDict *dd) {
    Stream s;
    if (!chunk || !stream_begin(&s, dst, fcs, dd)) return 0;
    for (size_t at = 0; at < n; at += chunk)
        if (!stream_feed(&s, src + at, n - at < chunk ? n - at : chunk)) return 0;
    return s.done && s.out.pos == fcs;
}

/* ---- the counters ---- */
static void report(int force) {
    static volatile LONG64 last;
    const int64_t now = hos_now_ns();
    const int64_t was = last;
    if (!force && (now - was < 10000000000LL || InterlockedCompareExchange64(&last, now, was) != was)) return;
    char line[1024];
    if (hl_stream_report(line, sizeof line)) hos_log("hl stream: %s", line);
}

static int pages_on;   /* HL_STREAM_PAGES=1 (section 9 of stream-boundary.md) */

static void record(uint64_t req, uint64_t dest, uint64_t size, uint64_t compressed, uint32_t dict_id, int chunked) {
    char name[160];
    request_name(req, name, sizeof name);
    if (pages_on) hl_stream_pages_track(dest, GP(dest), size, name, hos_now_ns());
    const uint64_t head = size < 4096 ? size : 4096;
    const uint8_t *p = (const uint8_t *)GP(dest);
    uint64_t fp = hl_stream_fingerprint(p, (size_t)head, 0xcbf29ce484222325ULL);
    if (size > head) fp = hl_stream_fingerprint(p + size - head, (size_t)head, fp);
    const hl_stream_decode d = {dest, size, compressed, l64(req + R_FILE), fp, dict_id, chunked, name};
    hl_stream_record(&d);
}

/* a decode that ended on an error path: counted, the first ones logged */
static void error_path(uint64_t req, const char *who, uint32_t code, uint64_t size) {
    hl_stream_error();
    static volatile LONG told;
    if (InterlockedIncrement(&told) > 12) return;
    char name[160];
    request_name(req, name, sizeof name);
    hos_log("hl stream: %s decode of %s ended on an error path: code %u, decompressed size %llu, given %#llx, limit %u, "
            "quiet %u, result at %#llx", who, name[0] ? name : "(no name)", code, (unsigned long long)size,
            (unsigned long long)l64(req + R_DEST), l32(req + R_LIMIT), l8(req + R_QUIET) & 1,
            (unsigned long long)l64(req + R_RESULT));
}

/* what a request is, read before anything is done for it */
static int choose(uint64_t obj, uint64_t req) {
    const uint64_t file = l64(req + R_FILE), buf = l64(obj + D_BUFFER);
    const uint32_t chunk = l32(obj + D_CHUNK), need = BUFFER_FIXED + 2 * chunk + 30;
    const uint64_t src = (buf + WORKSPACE + 535) & ~31ULL, end = (buf + need) & ~31ULL;
    /* (a read buffer that the translated function would refuse is a rare path: given to it whole) */
    const int buffer_ok = buf && need >= BUFFER_FIXED && !(buf & 7) && end > src && end - src >= 32;
    const hl_stream_request q = {l32(req + R_STEP), file != 0, file && l8(file + F_KIND) == 1, buffer_ok, l64(req + R_HEAP) != 0};
    return hl_stream_choose(&q);
}

/* ---- the check: the translated decode, then ours of the same bytes ---- */
/* The chunked case's chunks, collected as the decoding thread is given them, by decompressor */
typedef struct { volatile uint64_t obj; uint8_t *buf; size_t len, cap; } Collect;
enum { COLLECTS = 32 };
static Collect collects[COLLECTS];
static SRWLOCK collect_lock = SRWLOCK_INIT;

static Collect *collect_of(uint64_t obj) {
    for (int i = 0; i < COLLECTS; i++)
        if (collects[i].obj == obj) return &collects[i];
    return NULL;
}

static void hl_decode_chunk(Ctx *A64_NOESC c) {
    const uint64_t desc = c->x[0];
    if ((int32_t)l32(desc + 188) >= 0) {
        const uint64_t src0 = l64(desc + 168);
        const uint32_t step = l32(desc + 176), index = l32(desc + 180), n = l32(desc + 184);
        const uint64_t src = index & 1 ? src0 + step : src0;
        AcquireSRWLockExclusive(&collect_lock);
        Collect *k = collect_of(desc - D_DESC);
        if (k && n && hos_guest_range(src, n)) {
            if (k->len + n > k->cap) {
                size_t cap = k->cap ? k->cap : (8u << 20);
                while (cap < k->len + n) cap *= 2;
                uint8_t *b = (uint8_t *)realloc(k->buf, cap);
                if (b) k->buf = b, k->cap = cap;
            }
            if (k->len + n <= k->cap) memcpy(k->buf + k->len, GP(src), n), k->len += n;
        }
        ReleaseSRWLockExclusive(&collect_lock);
    }
    translated_chunk(c);
}

static void check_file(Ctx *A64_NOESC c) {
    const uint64_t obj = c->x[0], req = c->x[1];
    const int reason = choose(obj, req);
    AcquireSRWLockExclusive(&collect_lock);
    Collect *k = collect_of(obj);
    if (!k) k = collect_of(0);
    if (k) k->obj = obj, k->len = 0;
    ReleaseSRWLockExclusive(&collect_lock);

    translated_file(c);

    const uint64_t dest = c->x[0];
    AcquireSRWLockExclusive(&collect_lock);
    const int chunked = k && k->len > 0;
    uint8_t *own = NULL;
    size_t own_len = 0;
    if (chunked) own = k->buf, own_len = k->len, k->buf = NULL, k->cap = 0;   /* (taken: the next file starts empty) */
    if (k) k->obj = 0, k->len = 0;
    ReleaseSRWLockExclusive(&collect_lock);
    if (!dest) {   /* (an error path: nothing decoded) */
        hl_stream_count(reason, 0);
        const uint64_t at = l64(req + R_RESULT);
        error_path(req, "the translated", at ? l32(at) : 0, 0);
        free(own);
        return;
    }
    const uint8_t *src;
    size_t avail;
    if (chunked) {
        src = own, avail = own_len;
    } else {
        const uint64_t buf = l64(obj + D_BUFFER);
        const uint64_t s = (buf + WORKSPACE + 535) & ~31ULL;
        src = buf ? (const uint8_t *)GP(s) : NULL;
        avail = l32(obj + D_CHUNK);
    }
    ZSTD_frameHeader h;
    size_t csize = 0;
    uint64_t fcs = 0;
    int ok = src && ZSTD_getFrameHeader(&h, src, avail) == 0 && h.frameType == ZSTD_frame &&
             h.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN && !ZSTD_isError(csize = ZSTD_findFrameCompressedSize(src, avail));
    if (ok) fcs = h.frameContentSize;
    hl_stream_count(reason == HL_STREAM_NATIVE && chunked ? HL_STREAM_CHUNKED : reason, fcs);
    char name[160];
    if (!ok) {
        hl_stream_checked(0, chunked, 0, 0);
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 8) {
            request_name(req, name, sizeof name);
            hos_log("hl stream check: %s: not checked (%s, %zu bytes seen)", name[0] ? name : "(no name)",
                    !src ? "no read buffer" : "no complete frame with its size in what was read", avail);
        }
        free(own);
        return;
    }
    ZSTD_DDict *dd = NULL;
    if (h.dictID) dd = host_ddict(guest_ddict(obj, h.dictID), h.dictID);
    static _Thread_local uint8_t *scratch;
    static _Thread_local size_t scratch_cap;
    if (fcs > scratch_cap) {
        free(scratch);
        scratch_cap = (size_t)fcs + (1u << 20);
        scratch = (uint8_t *)malloc(scratch_cap);
        if (!scratch) scratch_cap = 0;
    }
    size_t r = (size_t)-1;
    if (scratch && (dd || !h.dictID)) {
        if (chunked)   /* (the native path's decode: zstd's streaming decoder, fed the chunks the decoding thread was given) */
            r = stream_whole(scratch, (size_t)fcs, own, own_len, l32(obj + D_CHUNK), dd) ? (size_t)fcs : (size_t)-1;
        else
            r = dd ? ZSTD_decompress_usingDDict(thread_dctx(), scratch, (size_t)fcs, src, csize, dd)
                   : ZSTD_decompressDCtx(thread_dctx(), scratch, (size_t)fcs, src, csize);
    }
    const uint8_t *got = (const uint8_t *)GP(dest);
    const int mismatch = ZSTD_isError(r) || r != fcs || memcmp(scratch, got, (size_t)fcs) != 0;
    hl_stream_checked(1, chunked, fcs, mismatch);
    record(req, dest, fcs, csize, h.dictID, chunked);
    static volatile LONG seen, told_mismatch, told_chunked;
    const LONG nth = InterlockedIncrement(&seen);
    if (mismatch && InterlockedIncrement(&told_mismatch) <= 10) {
        size_t at = 0;
        if (!ZSTD_isError(r)) while (at < r && at < fcs && scratch[at] == got[at]) at++;
        request_name(req, name, sizeof name);
        hos_log("hl stream check: MISMATCH %s: %llu bytes from %zu (%s, dictionary %u%s): ours %s%s, first difference at %zu",
                name[0] ? name : "(no name)", (unsigned long long)fcs, csize, chunked ? "chunked" : "one read", h.dictID,
                h.dictID && !dd ? " not found" : "", ZSTD_isError(r) ? "failed: " : "ok",
                ZSTD_isError(r) ? ZSTD_getErrorName(r) : "", at);
    } else if (!mismatch && (nth <= 6 || (chunked && InterlockedIncrement(&told_chunked) <= 4))) {
        request_name(req, name, sizeof name);
        const uint64_t file = l64(req + R_FILE);
        hos_log("hl stream check: %s: %llu bytes from %zu (%s), dictionary %u, same; request: heap %#llx, given %#llx, limit %u, "
                "step %u, file %#llx (class %#llx, kind %u), read max %u, quiet %u; read buffer %#llx, chunk %u; thread %s",
                name[0] ? name : "(no name)", (unsigned long long)fcs, csize, chunked ? "chunked" : "one read", h.dictID,
                (unsigned long long)l64(req + R_HEAP), (unsigned long long)l64(req + R_DEST), l32(req + R_LIMIT),
                l32(req + R_STEP), (unsigned long long)file, (unsigned long long)(file ? l64(file) : 0),
                file ? l8(file + F_KIND) : 0, l32(req + R_READ_MAX), l8(req + R_QUIET) & 1,
                (unsigned long long)l64(obj + D_BUFFER), l32(obj + D_CHUNK), hos_thread_name(hos_self()));
    }
    free(own);
}

/* ---- the native function ---- */
/* the result code of a decoder error (the translated function's mapping: -1, -2, -3 through the game's table, other
   negative codes 11, and 0 for the rest) */
static uint32_t result_code(int32_t e) {
    if (e >= 0) return 0;
    if ((uint32_t)e + 3u < 3u) return l32(G_ERROR_TABLE + 4 * (uint64_t)(uint32_t)~(uint32_t)e);
    return 11;
}

/* The next chunk read into `to` through the open handle, as the translated function's loop reads it (the handle's kind
   and readiness checked each time; one read of a chunk, or reads of at most read_max). The count lands at the frame's
   S_READ. 0: the code-9 path. */
static int read_next(Ctx *c, uint64_t sp, uint64_t holder, uint64_t to, uint32_t chunk, uint32_t read_max) {
    const uint64_t handle = l64(sp + 64);
    if (!handle || l8(handle + F_KIND) != 1) return 0;
    const uint64_t ready = l64(l64(handle) + V_READY);
    if (ready != l64(G_ALWAYS_READY) && !(gcall(c, ready, 2, handle, holder, 0, 0, 0) & 1)) return 0;
    const uint64_t read = l64(l64(handle) + V_READ);
    if (!read_max) return (int)(gcall(c, read, 5, handle, sp + S_READ, holder, to, chunk) & 1);
    uint32_t left = chunk, total = 0;
    for (;;) {
        s32_sp(c, sp + S_COUNT, 0);
        const uint32_t n = (int32_t)left < (int32_t)read_max ? left : read_max;
        if (!(gcall(c, read, 5, handle, sp + S_COUNT, holder, to, n) & 1)) {
            s32_sp(c, sp + S_READ, total);
            return 0;
        }
        const uint32_t got = l32(sp + S_COUNT);
        total += got;
        if (got < n) break;
        to += got;
        left -= n;
        if (!left) break;
    }
    s32_sp(c, sp + S_READ, total);
    return 1;
}

/* The chunked case on the loading thread (stream-boundary.md section 5): the descriptor filled and the events waited on,
   cleared and signalled as the translated function and its decoding thread do it, the file read chunk after chunk into
   the two source buffers in turn by the same calls, each chunk decoded here (zstd's streaming decoder, into the
   destination) where the decoding thread's step would have decoded it, and the descriptor left as that step leaves it
   after a frame. The decoding thread is not given anything (it only runs the step for a descriptor it is given).
   1: decoded, `*total` the compressed bytes; 0: a read failed (the code-9 path); -1: the decoder failed or the frame did
   not end where it should: nothing is left waiting, and the caller gives the request to the translated function, which
   reproduces whatever the game's decoder makes of the file. */
static int decode_chunked(Ctx *c, uint64_t obj, uint64_t sp, uint64_t holder, uint64_t src, uint32_t chunk,
                          uint32_t read_max, uint64_t dest, uint32_t fcs, uint64_t gdd, ZSTD_DDict *dd, uint32_t count,
                          uint32_t *total) {
    const uint64_t buf = l64(obj + D_BUFFER);
    gcall(c, F_WAIT_LIGHT, 1, obj + K_FRAME_DONE, 0, 0, 0, 0);
    gcall(c, F_CLEAR_LIGHT, 1, obj + K_FRAME_DONE, 0, 0, 0, 0);
    s32(c, obj + K_CHUNK, chunk);
    s64(c, obj + K_DEST, dest);
    s32(c, obj + K_LEFT, fcs), s32(c, obj + K_LEFT + 4, 0);
    s64(c, obj + K_WORK, buf);
    s64(c, obj + K_DCTX, 0);
    s64(c, obj + K_SOURCE, src);
    s32(c, obj + K_STATUS, 0);
    s64(c, obj + K_DDICT, gdd);
    s8(c, obj + K_STATE, 1);
    s64(c, obj + K_STAGE, buf + STAGE_OFFSET);
    s64(c, obj + K_STAGE_SIZE, 0x20000);
    s64(c, obj + K_INDEX, 0);
    Stream s;
    static _Thread_local uint8_t *seen;   /* (HL_STREAM_CHECK=2: the compressed bytes, for the check after) */
    static _Thread_local size_t seen_cap;
    size_t seen_len = 0;
    if (!stream_begin(&s, GP(dest), fcs, dd)) {
        gcall(c, F_SIGNAL_LIGHT, 1, obj + K_FRAME_DONE, 0, 0, 0, 0);
        return -1;
    }
    uint32_t index = 0, sum = 0;
    for (;;) {
        /* hand-off: the chunk in the buffer its index chooses, decoded as the step decodes it */
        gcall(c, F_CLEAR_LIGHT, 1, obj + K_CHUNK_DONE, 0, 0, 0, 0);
        s32(c, obj + K_COUNT, count);
        const uint64_t from = index & 1 ? src + chunk : src;
        if (self_check) {
            if (seen_len + count > seen_cap) {
                size_t cap = seen_cap ? seen_cap : (32u << 20);
                while (cap < seen_len + count) cap *= 2;
                uint8_t *b = (uint8_t *)realloc(seen, cap);
                if (b) seen = b, seen_cap = cap;
            }
            if (seen_len + count <= seen_cap) memcpy(seen + seen_len, GP(from), count), seen_len += count;
        }
        const int ok = stream_feed(&s, GP(from), count);
        s32(c, obj + K_INDEX, ++index);
        sum += count;
        if (!ok) {
            gcall(c, F_SIGNAL_LIGHT, 1, obj + K_FRAME_DONE, 0, 0, 0, 0);
            gcall(c, F_SIGNAL_LIGHT, 1, obj + K_CHUNK_DONE, 0, 0, 0, 0);
            return -1;
        }
        /* (the step's result: the frame's bytes still to come, 0 once it ended; 0 also signals the frame's event) */
        if (s.done || s.out.pos == fcs) gcall(c, F_SIGNAL_LIGHT, 1, obj + K_FRAME_DONE, 0, 0, 0, 0);
        gcall(c, F_SIGNAL_LIGHT, 1, obj + K_CHUNK_DONE, 0, 0, 0, 0);
        if (count < chunk) break;   /* (the last, partial chunk: handed over after the loop of full ones) */
        if (!read_next(c, sp, holder, index & 1 ? src + chunk : src, chunk, read_max)) return 0;
        gcall(c, F_WAIT_LIGHT, 1, obj + K_CHUNK_DONE, 0, 0, 0, 0);
        count = l32(sp + S_READ);
        if (!count) break;
    }
    if (count) gcall(c, F_WAIT_LIGHT, 1, obj + K_CHUNK_DONE, 0, 0, 0, 0);
    if (!s.done || s.out.pos != fcs) {
        gcall(c, F_SIGNAL_LIGHT, 1, obj + K_FRAME_DONE, 0, 0, 0, 0);
        return -1;
    }
    HOS_WRITTEN(GP(dest), fcs);
    /* the descriptor as the step leaves it after a frame */
    s64(c, obj + K_DEST, dest + fcs);
    s32(c, obj + K_LEFT, 0), s32(c, obj + K_LEFT + 4, 0);
    s64(c, obj + K_DCTX, buf);
    s8(c, obj + K_STATE, 4);
    *total = sum;
    if (self_check) {
        size_t n = seen_len, r = (size_t)-1;
        const size_t fsz = ZSTD_findFrameCompressedSize(seen, seen_len);
        if (!ZSTD_isError(fsz)) n = fsz;
        uint8_t *mine = (uint8_t *)malloc(fcs ? fcs : 1);
        if (mine && seen_len == sum)
            r = dd ? ZSTD_decompress_usingDDict(thread_dctx(), mine, fcs, seen, n, dd)
                   : ZSTD_decompressDCtx(thread_dctx(), mine, fcs, seen, n);
        const int mismatch = ZSTD_isError(r) || r != fcs || memcmp(mine, GP(dest), fcs) != 0;
        hl_stream_checked(1, 1, fcs, mismatch);
        static volatile LONG told;
        if (mismatch && InterlockedIncrement(&told) <= 10)
            hos_log("hl stream check: MISMATCH of a native chunked decode: %u bytes from %u in %u chunks (%s)", fcs, sum,
                    index, ZSTD_isError(r) ? ZSTD_getErrorName(r) : "bytes differ");
        free(mine);
    }
    return 1;
}

static void hl_decode_file(Ctx *A64_NOESC c) {
    if (check_on) {
        check_file(c);
        if (log_on) report(0);
        return;
    }
    const uint64_t obj = c->x[0], req = c->x[1], actx = c->x[2];
    const int reason = choose(obj, req);
    if (reason != HL_STREAM_NATIVE) {
        const uint64_t out_size = c->x[3];
        translated_file(c);
        hl_stream_count(reason, c->x[0] && out_size ? l32(out_size) : 0);
        if (log_on) report(0);
        return;
    }
    const uint64_t a3 = c->x[3], a4 = c->x[4], a5 = c->x[5];
    const uint64_t sp0 = c->x[31], sp = sp0 - FRAME;
    c->x[31] = sp;
    const uint64_t heap = l64(req + R_HEAP), file = l64(req + R_FILE), holder = sp + S_HOLDER;
    const uint64_t holder_class = l64(G_HOLDER_CLASS);
    uint64_t x28 = actx;   /* (the translated function's register for "allocated": it holds other values before) */
    uint64_t dest = 0;
    uint32_t fcs = 0, size = 0, csize = 0;
    int code = 0;          /* a result code to store at the request's result, 0: none */
    int quiet_ok = 1;      /* the code is stored whatever the request's quiet bit says */
    s64_sp(c, sp + S_OUT_SIZE, a3);
    s64_sp(c, sp + 96, 0), s64_sp(c, sp + 104, 0), s64_sp(c, sp + 80, 0), s64_sp(c, sp + 88, 0);
    s64_sp(c, sp + 72, 0);
    s32_sp(c, sp + S_READ_MAX, 0);
    s64_sp(c, holder, holder_class + 16), s64_sp(c, holder + 8, 0);

    /* the file object: prepared, opened */
    const uint32_t read_max = l32(req + R_READ_MAX);
    const uint64_t prep = l64(l64(file) + V_PREPARE);
    if (prep != l64(G_SKIP_PREPARE)) {
        if (prep != l64(G_FORWARD_PREPARE)) gcall(c, prep, 1, file, 0, 0, 0, 0);
        else {
            const uint64_t inner = l64(file + 96);
            gcall(c, l64(l64(inner) + V_PREPARE), 1, inner, 0, 0, 0, 0);
        }
    }
    s32_sp(c, sp + S_READ_MAX, read_max);
    const uint64_t handle = gcall(c, l64(l64(file) + V_OPEN), 4, file, holder, req, 0, 0);
    s64_sp(c, sp + 64, handle);
    if (!handle) {   /* (not opened: code 8, nothing to close) */
        const uint64_t at = l64(req + R_RESULT);
        if (at) s32(c, at, 8);
        error_path(req, "the native", 8, 0);
        goto out_unflagged;
    }
    s64_sp(c, sp + 72, file);

    /* one read of a chunk (or reads of at most read_max) into the read buffer, after the workspace */
    const uint32_t chunk = l32(obj + D_CHUNK);
    const uint64_t buf = l64(obj + D_BUFFER);
    const uint64_t src = (buf + WORKSPACE + 535) & ~31ULL;
    s64_sp(c, sp + S_OUT_ALLOC, a4);
    s32_sp(c, sp + S_READ, 0);
    {
        int ready = 0;
        if (l8(handle + F_KIND) == 1) {
            const uint64_t fn = l64(l64(handle) + V_READY);
            ready = fn == l64(G_ALWAYS_READY) ? 1 : (int)(gcall(c, fn, 2, handle, holder, 0, 0, 0) & 1);
        }
        if (!ready || !src) { code = 9; goto fail; }
    }
    s64_sp(c, sp + S_OUT_FLAG, a5);
    if (!read_max) {
        if (!(gcall(c, l64(l64(handle) + V_READ), 5, handle, sp + S_READ, holder, src, chunk) & 1)) { code = 9; goto fail; }
        csize = l32(sp + S_READ);
    } else {
        uint32_t left = chunk, total = 0;
        uint64_t to = src;
        for (;;) {
            s32_sp(c, sp + S_COUNT, 0);
            const uint32_t n = (int32_t)left < (int32_t)read_max ? left : read_max;
            if (!(gcall(c, l64(l64(handle) + V_READ), 5, handle, sp + S_COUNT, holder, to, n) & 1)) {
                s32_sp(c, sp + S_READ, total);
                code = 9;
                goto fail;
            }
            const uint32_t got = l32(sp + S_COUNT);
            total += got;
            if (got < n) break;
            to += got;
            left -= n;
            if (!left) break;
        }
        s32_sp(c, sp + S_READ, total);
        csize = total;
    }
    /* the frame header: the decompressed size, the dictionary */
    const uint8_t *s = (const uint8_t *)GP(src);
    ZSTD_frameHeader h;
    {
        const size_t hr = ZSTD_getFrameHeader(&h, s, csize);
        const uint64_t v = hr ? (uint64_t)-2 : h.frameType == ZSTD_skippableFrame ? 0 : h.frameContentSize;
        if (v == (uint64_t)-2) { code = 4; quiet_ok = 0; goto fail; }
        if (v == (uint64_t)-1) { code = 5; quiet_ok = 0; goto fail; }
        fcs = (uint32_t)v;
    }
    const uint64_t gdd = h.dictID ? guest_ddict(obj, h.dictID) : 0;
    {
        const uint32_t limit = l32(req + R_LIMIT);
        if (limit < fcs && limit) { code = 2; quiet_ok = 0; goto fail; }
        const uint32_t step = l32(req + R_STEP);
        if (!step) size = (fcs + 31) & ~31u;
        else {
            uint32_t q = fcs / step;
            size = fcs;
            if (fcs - q * step) {
                q++;
                if (((uint64_t)step * q) >> 32) q = 0xffffffffu / step;
                size = q * step;
            }
        }
    }

    /* the destination: given, or allocated (the request's heap, with an alignment its allocator kind may raise) */
    dest = l64(req + R_DEST);
    if (dest) x28 = 0;
    else {
        const uint64_t guard = l64(G_GUARD);
        if (!(__atomic_load_n((const uint8_t *)GP(guard), __ATOMIC_ACQUIRE) & 1) &&
            (uint32_t)gcall(c, F_GUARD_ACQUIRE, 1, guard, 0, 0, 0, 0)) {
            s64(c, l64(G_STATIC), l64(G_STATIC_VALUE) + 16);
            gcall(c, F_GUARD_RELEASE, 1, l64(G_GUARD), 0, 0, 0, 0);
        }
        uint32_t align;
        const uint64_t kind = x28 ? gcall(c, l64(l64(x28)), 2, x28, l64(G_STATIC), 0, 0, 0) : 0;
        if (!kind) {
            align = (int32_t)l32(req + R_DIRECTION) >= 0 ? 0xffffffe0u : 32u;
        } else {
            const int32_t al = (int32_t)l32(req + R_ALIGN);
            if (!al) {
                const uint32_t r = (uint32_t)gcall(c, l64(l64(kind) + 32), 1, kind, 0, 0, 0, 0);
                const uint32_t a = (int32_t)r > 32 ? r : 32u;
                align = (int32_t)l32(req + R_DIRECTION) >= 0 ? a : (uint32_t)-a;
            } else {
                uint32_t a = al < 0 ? (uint32_t)-al : (uint32_t)al;
                a = a > 32 ? a : 32u;
                const uint32_t r = (uint32_t)gcall(c, l64(l64(kind) + 32), 1, kind, 0, 0, 0, 0);
                if ((int32_t)(a - r) < 0) a = r;
                align = (int32_t)l32(req + R_ALIGN) >= 0 ? a : (uint32_t)-a;
            }
        }
        x28 = l64(l64(G_HEAPS));
        if (!x28) dest = gcall(c, F_ALIGNED_ALLOC, 2, (uint64_t)(int64_t)(int32_t)align, size, 0, 0, 0);
        else dest = gcall(c, l64(l64(heap) + 48), 3, heap, size, align, 0, 0);
        if (!dest) { code = 10; goto fail; }
        x28 = 1;
    }

    /* the chunked case: the file is larger than one read (stream-boundary.md section 5) */
    if (csize >= chunk) {
        ZSTD_DDict *dd = gdd ? host_ddict(gdd, h.dictID) : NULL;
        uint32_t total = 0;
        const int k = h.frameType != ZSTD_frame || (h.dictID && !dd)
                          ? -1
                          : decode_chunked(c, obj, sp, holder, src, chunk, read_max, dest, fcs, gdd, dd, csize, &total);
        if (k < 0) {
            /* (never seen: the decoder failed, or a frame the game's decoder takes another way) the destination given
               back if it was allocated here, the file closed, and the request given whole to the translated function,
               which opens and reads the file again */
            static volatile LONG told;
            if (InterlockedIncrement(&told) <= 8) {
                char name[160];
                request_name(req, name, sizeof name);
                hos_log("hl stream: %s: the native chunked decode gave up (%u bytes, dictionary %u%s): the translated "
                        "function decodes it", name[0] ? name : "(no name)", fcs, h.dictID,
                        h.dictID && !dd ? " not found" : "");
            }
            hl_stream_count(HL_STREAM_CHUNKED_FALLBACK, 0);
            if (x28 & 1) gcall(c, F_FREE, 1, dest, 0, 0, 0, 0);
            s64_sp(c, holder, holder_class + 16);
            gcall(c, F_CLOSE, 2, file, holder, 0, 0, 0);
            c->x[31] = sp0;
            c->x[0] = obj, c->x[1] = req, c->x[2] = actx, c->x[3] = a3, c->x[4] = a4, c->x[5] = a5;
            translated_file(c);
            if (log_on) report(0);
            return;
        }
        if (!k) {   /* (a read failed: code 9 whatever the quiet bit, the destination freed, the flag left as it was) */
            error_path(req, "the native", 9, fcs);
            const uint64_t at = l64(req + R_RESULT);
            if (at) s32(c, at, 9);
            if (x28 & 1) gcall(c, F_FREE, 1, dest, 0, 0, 0, 0);
            dest = 0;
            goto flag;
        }
        const uint64_t os = l64(sp + S_OUT_SIZE), oa = l64(sp + S_OUT_ALLOC);
        if (os) s32(c, os, fcs);
        if (oa) s32(c, oa, size);
        hl_stream_count(HL_STREAM_CHUNKED, fcs);
        record(req, dest, fcs, total, h.dictID, 1);
        goto flag;
    }

    /* the decode: the whole file at once */
    {
        ZSTD_DDict *dd = gdd ? host_ddict(gdd, h.dictID) : NULL;
        size_t r;
        if (h.dictID && !dd) r = (size_t)-ZSTD_error_dictionary_wrong;
        else {
            r = dd ? ZSTD_decompress_usingDDict(thread_dctx(), GP(dest), size, s, csize, dd)
                   : ZSTD_decompressDCtx(thread_dctx(), GP(dest), size, s, csize);
            HOS_WRITTEN(GP(dest), size);
        }
        int32_t e = 0;
        if (ZSTD_isError(r)) e = ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? -2 : -1;
        else if ((int32_t)r < 0) e = (int32_t)r;
        if (e) {
            error_path(req, "the native", result_code(e), fcs);
            if (!(l8(req + R_QUIET) & 1) && l64(req + R_RESULT)) s32(c, l64(req + R_RESULT), result_code(e));
            if (x28 & 1) gcall(c, F_FREE, 1, dest, 0, 0, 0, 0);
            dest = 0;
            goto flag;
        }
    }
    {
        const uint64_t os = l64(sp + S_OUT_SIZE), oa = l64(sp + S_OUT_ALLOC);
        if (os) s32(c, os, fcs);
        if (oa) s32(c, oa, size);
    }
    hl_stream_count(HL_STREAM_NATIVE, fcs);
    record(req, dest, fcs, csize, h.dictID, 0);
    goto flag;

fail:
    error_path(req, "the native", (uint32_t)code, fcs);
    {
        const uint64_t at = l64(req + R_RESULT);
        if (at && (quiet_ok || !(l8(req + R_QUIET) & 1))) s32(c, at, (uint32_t)code);
    }
    dest = 0;
flag:
    {
        const uint64_t at = l64(sp + S_OUT_FLAG);
        if (at) s8(c, at, x28);
    }
    s64_sp(c, holder, holder_class + 16);
    if (l64(sp + 72)) gcall(c, F_CLOSE, 2, l64(sp + 72), holder, 0, 0, 0);
out_unflagged:
    c->x[31] = sp0;
    c->x[0] = dest;
    if (log_on) report(0);
}

/* ---- HL_STREAM_PAGES=1: which decoded pages still hold the decoder's bytes, and the restore path's proof ----
   (stream-boundary.md section 9) Every decode's whole pages are hashed when it is recorded; a thread of its own hashes
   them again when the range is 30 s and 60 s old (hl::stream::Pages) and logs the table by file kind every 10 s. At the
   second check, ranges are decoded again from the RomFS by their name (the restore a decommitted page would need) and
   compared: with the hashes taken at decode time for every page, and byte for byte with memory for the pages that still
   match. Measurement only: it decommits nothing. Reading pages brings back those the cold-pages thread took out. */
extern const char *hos_romfs_dir;
int hos_mods_romfs(const char *rel, char *out, size_t cap);

/* the host bytes of a guest page when it is committed and readable now (a freed region's page is not: reading it would
   commit a zero megabyte through the commit-on-touch handler) */
static const uint8_t *page_view(uint64_t addr, size_t n, void *ctx) {
    (void)ctx;
    MEMORY_BASIC_INFORMATION m;
    if (!hos_guest_range(addr, n) || !VirtualQuery(GP(addr), &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ |
                                                                      PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) ||
        (uint64_t)((const uint8_t *)m.BaseAddress + m.RegionSize - (const uint8_t *)GP(addr)) < n)
        return NULL;
    return (const uint8_t *)GP(addr);
}

static ZSTD_DDict *ddict_by_id(uint32_t id) {
    ZSTD_DDict *dd = NULL;
    AcquireSRWLockShared(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)
        if (dicts[i].id == id) dd = dicts[i].dd;
    ReleaseSRWLockShared(&dict_lock);
    return dd;
}

/* the restore test's counters */
static struct {
    uint64_t tests, bytes, pages_equal_now, mismatches, hash_mismatches, changed_meanwhile, no_entry, no_file,
        size_differs, failed, ns, max_ns;
} rt;
static int64_t rt_budget_at;
static uint64_t rt_budget_bytes;

/* a whole RomFS file into a malloc'd buffer (the mods' file when a mod has it, as the game would read it) */
static uint8_t *read_romfs(const char *name, size_t *len) {
    char rel[200], path[1024];
    snprintf(rel, sizeof rel, "%s.zs", name);
    if (!hos_mods_romfs(rel, path, sizeof path)) {
        if (!hos_romfs_dir) return NULL;
        snprintf(path, sizeof path, "%s/%s", hos_romfs_dir, rel);
    }
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *b = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1LL << 31) && (b = (uint8_t *)malloc((size_t)sz.QuadPart)) &&
        (!ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart))
        free(b), b = NULL;
    CloseHandle(h);
    if (b) *len = (size_t)sz.QuadPart;
    return b;
}

static void restore_test(void *ctx, uint64_t dest, uint64_t size, uint64_t first_page, size_t pages, const uint64_t *hashes,
                         const uint8_t *equal, int stage, const char *name) {
    (void)ctx;
    if (stage != 2 || !pages || !name[0]) return;
    /* (a budget: 64 MB of decoded bytes a second at most, so that the thread keeps up with the checks) */
    const int64_t now = hos_now_ns();
    if (now - rt_budget_at > 1000000000LL) rt_budget_at = now, rt_budget_bytes = 0;
    if (rt_budget_bytes > (64u << 20)) return;
    rt_budget_bytes += size;
    hl_stream_decode d;
    if (!hl_stream_find(dest, &d, NULL, 0) || d.dest != dest || d.size != size) { rt.no_entry++; return; }
    size_t clen = 0;
    uint8_t *cbytes = read_romfs(name, &clen);
    if (!cbytes) {
        rt.no_file++;
        static int told;
        if (told++ < 6) hos_log("hl stream restore: no RomFS file for %s", name);
        return;
    }
    if (d.compressed && d.compressed != clen) rt.size_differs++;
    ZSTD_DDict *dd = d.dict_id ? ddict_by_id(d.dict_id) : NULL;
    static uint8_t *scratch;
    static size_t scratch_cap;
    if (size > scratch_cap) {
        free(scratch);
        scratch_cap = (size_t)size + (4u << 20);
        scratch = (uint8_t *)malloc(scratch_cap);
        if (!scratch) scratch_cap = 0;
    }
    size_t r = (size_t)-1;
    if (scratch && (dd || !d.dict_id)) {
        const size_t fsz = ZSTD_findFrameCompressedSize(cbytes, clen);
        const size_t n = ZSTD_isError(fsz) ? clen : fsz;
        r = dd ? ZSTD_decompress_usingDDict(thread_dctx(), scratch, (size_t)size, cbytes, n, dd)
               : ZSTD_decompressDCtx(thread_dctx(), scratch, (size_t)size, cbytes, n);
    }
    free(cbytes);
    const int64_t took = hos_now_ns() - now;
    rt.tests++, rt.bytes += size, rt.ns += (uint64_t)took;
    if ((uint64_t)took > rt.max_ns) rt.max_ns = (uint64_t)took;
    if (ZSTD_isError(r) || r != size) {
        rt.failed++;
        static int told;
        if (told++ < 6) hos_log("hl stream restore: %s: the decode failed (%s)", name, ZSTD_isError(r) ? ZSTD_getErrorName(r) : "size");
        return;
    }
    int told_here = 0;
    for (size_t i = 0; i < pages; i++) {
        const uint8_t *mine = scratch + (first_page - dest) + i * 4096;
        if (hl_stream_page_hash(mine, 4096) != hashes[i]) rt.hash_mismatches++, told_here |= 1;
        if (equal[i] != 1) continue;
        const uint8_t *now_p = page_view(first_page + i * 4096, 4096, NULL);
        if (!now_p) continue;
        if (!memcmp(mine, now_p, 4096)) { rt.pages_equal_now++; continue; }
        if (hl_stream_page_hash(now_p, 4096) != hashes[i]) { rt.changed_meanwhile++; continue; }   /* (written since the check) */
        rt.mismatches++, told_here |= 2;
    }
    static int told;
    if (told_here && told++ < 10)
        hos_log("hl stream restore: MISMATCH %s (%llu bytes at %#llx):%s%s", name, (unsigned long long)size,
                (unsigned long long)dest, told_here & 1 ? " a page differs from its decode-time hash" : "",
                told_here & 2 ? " a page equal to its decode differs from the decode again" : "");
}

static DWORD WINAPI pages_thread(LPVOID unused) {
    (void)unused;
    int64_t last = hos_now_ns();
    for (;;) {
        Sleep(500);
        hl_stream_pages_check(hos_now_ns(), page_view, NULL, (size_t)1 << 18, restore_test, NULL);
        if (hos_now_ns() - last < 10000000000LL) continue;
        last = hos_now_ns();
        static char table[16384];
        if (hl_stream_pages_report(table, sizeof table) > 0) {
            hos_log("hl stream pages: whole 4 KB pages still equal to the decode (MB), by file kind:");
            for (char *line = table, *nl; line; line = nl) {
                nl = strchr(line, '\n');
                if (nl) *nl++ = 0;
                hos_log("hl stream pages: %s", line);
            }
        }
        hos_log("hl stream restore: tests %llu (%.1f MB), pages equal to memory %llu, MISMATCHES %llu, pages unlike their "
                "decode-time hash %llu, written meanwhile %llu; no entry %llu, no file %llu, size differs %llu, decode "
                "failed %llu; a restore took %.2f ms on average (%.2f ms a MB), at most %.2f ms",
                (unsigned long long)rt.tests, rt.bytes / 1048576.0, (unsigned long long)rt.pages_equal_now,
                (unsigned long long)rt.mismatches, (unsigned long long)rt.hash_mismatches,
                (unsigned long long)rt.changed_meanwhile, (unsigned long long)rt.no_entry, (unsigned long long)rt.no_file,
                (unsigned long long)rt.size_differs, (unsigned long long)rt.failed,
                rt.tests ? rt.ns / 1e6 / rt.tests : 0.0, rt.bytes ? rt.ns / 1e6 / (rt.bytes / 1048576.0) : 0.0,
                rt.max_ns / 1e6);
    }
    return 0;
}

/* 1 when the decode is answered here (asked once, at the function's first lookup) */
static int hl_stream_on(void) {
    const char *e = getenv("HL_STREAM"), *k = getenv("HL_STREAM_CHECK");
    stream_mode = e && e[0] ? atoi(e) : HL_STREAM_DEFAULT;
    check_on = k && k[0] ? atoi(k) == 1 : 0;
    self_check = k && k[0] && atoi(k) == 2 && stream_mode;
    if (!stream_mode && !check_on) {
        hos_log("hl stream: off (HL_STREAM=0)");
        return 0;
    }
    translated_file = hl_translated(HL_DECODE_FILE);
    translated_chunk = hl_translated(HL_DECODE_CHUNK);
    if (!translated_file || !translated_chunk) {
        hos_log("hl stream: off: no translated function at %#llx or %#llx", (unsigned long long)HL_DECODE_FILE,
                (unsigned long long)HL_DECODE_CHUNK);
        return 0;
    }
    hl_stream_start(1u << 16);
    log_on = check_on || self_check || getenv("HL_STREAM_LOG") != NULL;
    {
        const char *p = getenv("HL_STREAM_PAGES");
        if (p && atoi(p) == 1 && hl_stream_pages_start(30000000000LL, 60000000000LL)) {
            pages_on = 1;
            CloseHandle(CreateThread(NULL, 0, pages_thread, NULL, 0, NULL));
            hos_log("hl stream: pages: every decode's whole pages hashed, checked again at 30 s and 60 s, and the ranges "
                    "decoded again from the RomFS at 60 s (HL_STREAM_PAGES=1, a measurement)");
        }
    }
    hos_log("hl stream: %s (zstd %s)",
            check_on ? "the check: the translated decode of each resource file, then the same bytes decoded natively and "
                       "compared with it"
            : self_check ? "the native decode of resource files (rare paths translated), each chunked file checked against "
                           "a one-call decode of the bytes read"
                         : "the native decode of resource files, the chunked case included (rare paths translated)",
            ZSTD_versionString());
    return 1;
}
static int hl_stream_chunk_on(void) { return check_on; }

HL_ANSWER(HL_DECODE_FILE, hl_decode_file, hl_stream_on, "the resource decompressor's decode of a file")
HL_ANSWER(HL_DECODE_CHUNK, hl_decode_chunk, hl_stream_chunk_on, "the resource decompressor's decode of one chunk")
