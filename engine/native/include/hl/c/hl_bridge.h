/* HeyListen native engine: what the layer that runs guest code (plain C) gives the engine and asks of it.
   The engine never includes that layer's headers; this file is the whole surface between the two. */
#ifndef HL_C_BRIDGE_H
#define HL_C_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hl_guest_host {
    void *user;
    /* the host address of a guest address */
    void *(*ptr)(void *user, uint64_t guest);
    /* the guest thread the calling host thread runs as (NULL: none) */
    void *(*current)(void *user);
    /* calls guest function fn(a0, a1) on the calling host thread as guest thread `as`, on that thread's own guest context */
    void (*call)(void *user, void *as, uint64_t fn, uint64_t a0, uint64_t a1);
    /* a host thread fit to run guest code (its host stack); returns a handle for thread_join */
    void *(*thread_start)(void *user, void (*entry)(void *), void *arg, unsigned index);
    void (*thread_join)(void *user, void *handle);
} hl_guest_host;

enum { HL_JOBS_OFF = 0, HL_JOBS_THROUGH = 1, HL_JOBS_POOL = 2 };

/* Starts the native job system and its bridge to the game's (once; the host must outlive the process). mode: HL_JOBS_*.
   workers: the pool's threads, 0 for the default. Returns 1 when the stages' work goes through the native job system. */
int hl_bridge_start(const hl_guest_host *host, int mode, unsigned workers);

/* The guest address of the worker message function the bridge answers, and its answer: call it with the function's two
   arguments on the worker's own thread. */
uint64_t hl_bridge_worker_message_address(void);
void hl_bridge_worker_message(uint64_t worker, uint64_t message);

/* One line of counters for the log. Returns the length written. */
int hl_bridge_report(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
