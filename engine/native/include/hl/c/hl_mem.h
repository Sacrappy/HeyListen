/* HeyListen native engine: the memory module's C surface for the OS layer (engine/host/hl_mem.c). The policy that decides
   which units of pages leave the process's working set (hl/mem/ColdPages.h); the OS layer measures and acts. */
#ifndef HL_C_MEM_H
#define HL_C_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hl_mem_cold_config {
    uint64_t units;            /* the unit index space */
    uint32_t unit_pages;       /* pages a unit */
    uint32_t page_bytes;
    uint32_t backoff_passes;   /* passes a unit that came back is left in, doubled at each further come-back */
    uint32_t give_up;          /* come-backs after which a unit stays in for good */
    uint32_t budget_pages;     /* pages taken out a pass at most */
} hl_mem_cold_config;

/* Makes the policy (once). Returns 1 when it is ready. */
int hl_mem_cold_start(const hl_mem_cold_config *config);
/* The pass thread: a new pass; a unit seen with `resident` pages in the working set (1: take them out now); what a
   take-out did. */
void hl_mem_cold_pass(void);
int hl_mem_cold_look(uint64_t unit, uint32_t resident);
void hl_mem_cold_taken(uint64_t unit, uint32_t left, uint32_t stayed);
/* Any thread: units [lo, hi) hold new data; units [lo, hi) are pinned by the driver (never taken out). */
void hl_mem_cold_forget(uint64_t lo, uint64_t hi);
void hl_mem_cold_imported(uint64_t lo, uint64_t hi);
/* One line of counters for the log. Returns the length written. */
int hl_mem_cold_report(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
