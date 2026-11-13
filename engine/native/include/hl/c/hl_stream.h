/* HeyListen native engine: the stream module's C surface for the OS layer (engine/host/hl_stream.c). The path a resource
   decode takes, the module's counters and the provenance table of decoded ranges (hl/stream/Decode.h, Provenance.h); the
   OS layer reads the game's request, decodes (zstd lives on its side) and acts. */
#ifndef HL_C_STREAM_H
#define HL_C_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the reasons of hl::stream::Reason, in its order */
enum { HL_STREAM_NATIVE = 0, HL_STREAM_SIZE_STEP, HL_STREAM_PATH_OPEN, HL_STREAM_FILE_KIND, HL_STREAM_NO_READ_BUFFER,
       HL_STREAM_NO_HEAP, HL_STREAM_CHUNKED, HL_STREAM_CHUNKED_FALLBACK };

typedef struct hl_stream_request {
    uint32_t size_step;
    int has_file, file_kind_ok, has_read_buffer, has_heap;
} hl_stream_request;

typedef struct hl_stream_decode {
    uint64_t dest, size, compressed, file, fingerprint;
    uint32_t dict_id;
    int chunked;
    const char *name;   /* (copied) */
} hl_stream_decode;

/* Makes the module (once): the provenance table keeps at most `capacity` ranges. Returns 1 when ready. */
int hl_stream_start(uint32_t capacity);
/* The path of a request (HL_STREAM_*), decided before anything is done for it. */
int hl_stream_choose(const hl_stream_request *r);
/* A request's outcome: its path (HL_STREAM_*) and its decompressed bytes when known. */
void hl_stream_count(int reason, uint64_t bytes);
void hl_stream_error(void);
/* The check's outcome for one file: 1 checked (with `mismatch` 1 when the bytes differ), 0 not checkable. */
void hl_stream_checked(int checked, int chunked, uint64_t bytes, int mismatch);
/* Provenance: a decode wrote a range; the decode holding an address (name copied into name/cap); a range freed. */
void hl_stream_record(const hl_stream_decode *d);
int hl_stream_find(uint64_t addr, hl_stream_decode *out, char *name, size_t cap);
void hl_stream_forget(uint64_t lo, uint64_t hi);
uint64_t hl_stream_fingerprint(const void *data, size_t n, uint64_t seed);
/* One line of counters (and the provenance table's) for the log. Returns the length written. */
int hl_stream_report(char *out, size_t cap);

/* The decoded pages that still hold the decoder's bytes, by file kind (hl/stream/Pages.h): made once (the two checks at
   these ages); a decode's bytes; the checks due now (each range checked is given to `seen` when not null: its whole
   pages from first_page, their decode-time hashes, and per page 1 equal, 0 changed, 2 not readable); the table, one
   line per kind separated by newlines. */
typedef const uint8_t *(*hl_stream_view)(uint64_t addr, size_t n, void *ctx);
typedef void (*hl_stream_seen)(void *ctx, uint64_t dest, uint64_t size, uint64_t first_page, size_t pages,
                               const uint64_t *hashes, const uint8_t *equal, int stage, const char *name);
int hl_stream_pages_start(int64_t first_ns, int64_t second_ns);
void hl_stream_pages_track(uint64_t dest, const void *bytes, uint64_t size, const char *name, int64_t now_ns);
size_t hl_stream_pages_check(int64_t now_ns, hl_stream_view view, void *ctx, size_t budget_pages, hl_stream_seen seen,
                             void *seen_ctx);
int hl_stream_pages_report(char *out, size_t cap);
uint64_t hl_stream_page_hash(const void *data, size_t n);

#ifdef __cplusplus
}
#endif
#endif
