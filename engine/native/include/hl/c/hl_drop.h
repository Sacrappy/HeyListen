/* HeyListen native engine: the drop module's C surface for the OS layer (engine/host/hl_drop.c). The policy that gives
   a decoded resource's granules back to the system for good and restores them on touch (hl/mem/Drop.h); the OS layer
   gives it the substrate (hos_guestpages.h), the source's bytes (zstd lives on its side), memory and the working set. */
#ifndef HL_C_DROP_H
#define HL_C_DROP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* who touched a dropped granule (hl::mem::TouchPath, in its order) */
enum { HL_DROP_GUEST_READ = 0, HL_DROP_GUEST_WRITE, HL_DROP_HOST_READ, HL_DROP_HOST_WRITE, HL_DROP_KERNEL, HL_DROP_IMPORT };

typedef int (*hl_drop_fill)(void *fill_ctx, uint8_t *buf, uint64_t addr, uint64_t size);

typedef struct hl_drop_host {
    void *ctx;
    int (*is_dropped)(void *ctx, uint64_t addr);
    int (*drop)(void *ctx, uint64_t addr, uint64_t size);
    int (*restore)(void *ctx, uint64_t addr, uint64_t size, hl_drop_fill fill, void *fill_ctx);
    /* the file decoded again: `size` bytes, or NULL; given back with release() */
    const uint8_t *(*source)(void *ctx, const char *name, uint32_t dict_id, uint64_t size, uint64_t compressed);
    void (*release)(void *ctx, const uint8_t *bytes);
    const uint8_t *(*view)(void *ctx, uint64_t addr);
    uint32_t (*resident)(void *ctx, uint64_t granule);
    int (*protect)(void *ctx, uint64_t addr, uint64_t size, int read_only);
    int (*plain)(void *ctx, uint64_t addr);
} hl_drop_host;

typedef struct hl_drop_config {
    uint64_t granule;
    int64_t age_ns, retry_ns, hold_ns, kernel_ns;
    uint32_t give_up;
    uint64_t pass_bytes;
    int require_cold, check;
} hl_drop_config;

/* Makes the module (once); the host is copied. Returns 1 when ready. */
int hl_drop_start(const hl_drop_config *config, const hl_drop_host *host);
int hl_drop_ready(void);
/* A decode wrote [dest, dest + size) (name copied). */
void hl_drop_decoded(uint64_t dest, uint64_t size, uint64_t compressed, uint32_t dict_id, const char *name, int64_t now_ns);
/* The drop thread's pass; returns the granules dropped. */
uint64_t hl_drop_pass(int64_t now_ns);
/* The substrate's hook: 1 when `addr` was ours and is restored. */
int hl_drop_touch(uint64_t addr, int path, int64_t now_ns);
/* A writer's fault on a granule being decided: 1 when it waited. */
int hl_drop_wait_pending(uint64_t addr);
void hl_drop_kernel_use(uint64_t lo, uint64_t hi, int64_t now_ns);
void hl_drop_imported(uint64_t lo, uint64_t hi);
void hl_drop_unimported(uint64_t lo, uint64_t hi);
void hl_drop_forget(uint64_t lo, uint64_t hi);
uint64_t hl_drop_reconcile(void);
/* Granules dropped now; one line of counters. */
uint64_t hl_drop_dropped_now(void);
int hl_drop_report(char *out, size_t cap, int64_t now_ns);

#ifdef __cplusplus
}
#endif
#endif
