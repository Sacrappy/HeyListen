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
    uint64_t (*call)(void *user, void *as, uint64_t fn, uint64_t a0, uint64_t a1);
    /* the same with up to four arguments; with frame_size > 0, a copy of frame is put on the guest stack (16-byte aligned)
       first and its guest address passed as the argument after args. Returns x0. */
    uint64_t (*call_with)(void *user, void *as, uint64_t fn, const uint64_t *args, unsigned nargs, const void *frame,
                          unsigned frame_size);
    /* one line for the game's log */
    void (*log)(void *user, const char *line);
    /* a host thread fit to run guest code (its host stack); returns a handle for thread_join */
    void *(*thread_start)(void *user, void (*entry)(void *), void *arg, unsigned index);
    void (*thread_join)(void *user, void *handle);
} hl_guest_host;

enum { HL_JOBS_OFF = 0, HL_JOBS_THROUGH = 1, HL_JOBS_POOL = 2 };

enum { HL_DISPATCH_NATIVE = 1, HL_DISPATCH_CHECK = 2 };

/* Starts the native job system and its bridge to the game's (once; the host must outlive the process). mode: HL_JOBS_*.
   workers: the pool's threads, 0 for the default. dispatch: HL_DISPATCH_* bits (the native dispatcher, its invariants).
   Returns 1 when the stages' work goes through the native job system. */
int hl_bridge_start(const hl_guest_host *host, int mode, unsigned workers, int dispatch);

/* The guest address of the worker message function the bridge answers, and its answer: call it with the function's two
   arguments on the worker's own thread. */
uint64_t hl_bridge_worker_message_address(void);
void hl_bridge_worker_message(uint64_t worker, uint64_t message);

/* The entry gate of the game's dispatcher (hl::bridge::kDispatcher), with its two arguments, on the calling thread's own
   guest context: 1 when the native dispatcher ran the stage, 0 to let the translated body run (no native dispatcher, or
   the dispatcher itself is calling the translated one). */
int hl_bridge_dispatch_gate(uint64_t scheduler, uint64_t context);

/* One line of counters for the log. Returns the length written. */
int hl_bridge_report(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
