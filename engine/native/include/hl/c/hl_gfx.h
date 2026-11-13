/* HeyListen native engine: the graphics front end's C surface for the layer that runs guest code (engine/host/hl_gfx.c).
   docs/native-engine/gfx-record-job.md has the plan and the check. */
#ifndef HL_C_GFX_H
#define HL_C_GFX_H

#include <stddef.h>
#include <stdint.h>

#include "hl/c/hl_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { HL_GFX_OFF = 0, HL_GFX_SHELL = 1 };

/* Starts the front end (once; the host must outlive the process). mode: HL_GFX_*. check: 1 to keep the pass check (the
   OS layer then feeds it every NVN call a pass makes, and runs each pass on the path hl_gfx_check_next_native says).
   Returns 1 when the native pass is in use. */
int hl_gfx_start(const hl_guest_host *host, int mode, int check);

/* The guest address of the pass's listener (answered), and the native pass: call it with the listener's two arguments on
   the thread that runs the listener. Returns the listener's result. */
uint64_t hl_gfx_pass_address(void);
uint32_t hl_gfx_record_pass(uint64_t listener, uint64_t args);

/* The check. Per pass: next_native says which path it takes (1 native, 0 the translated listener); begin starts the
   calling thread's recording (args: the pass's argument block, whose mode it reads before the pass); call adds one NVN
   call to it (op: the OS layer's number of the procedure; x: x0..x5; f: the low 32 bits of s0..s3); end files it. */
int hl_gfx_check_next_native(void);
void hl_gfx_check_begin(uint64_t args);
void hl_gfx_check_call(uint32_t op, const uint64_t *x, const uint32_t *f);
void hl_gfx_check_end(int native);

/* One line of counters for the log (op_names: the procedures' names by op, n_names of them). Returns the length written. */
int hl_gfx_report(char *out, size_t cap, const char *const *op_names, size_t n_names);

/* ---- the render-state function (guest 0x80077d60: called directly, answered through an entry gate) ---- */

/* The NVN procedures native code calls, in the native side's numbering (hl/gfx/Nvn.h): how many, each one's NVN name,
   and the game's global that holds it (the OS layer checks that the procedure it calls is the one the game calls). */
uint32_t hl_gfx_nvn_count(void);
const char *hl_gfx_nvn_name(uint32_t proc);
uint64_t hl_gfx_nvn_global(uint32_t proc);

typedef struct hl_gfx_host {
    void *user;
    /* calls NVN procedure `proc` on the calling thread's guest context: x[0..nx) in x0.., f[0..nf) in the low 32 bits
       of s0.. */
    void (*nvn)(void *user, uint32_t proc, const uint64_t *x, uint32_t nx, const uint32_t *f, uint32_t nf);
    /* n bytes of guest memory at `guest` were written by native code */
    void (*written)(void *user, uint64_t guest, uint64_t n);
    /* one line for the log (the check's mismatches) */
    void (*log)(void *user, const char *line);
} hl_gfx_host;

/* Starts the native render-state function (once; the hosts must outlive the process). check: 1 to compare each call
   (below) instead of answering it. Returns 1. */
int hl_gfx_state_start(const hl_guest_host *host, const hl_gfx_host *gfx, int check);
uint64_t hl_gfx_state_address(void);

/* The native answer, on the thread that runs the call (block x0, key w1, sp the caller's stack pointer): writes the
   block and calls the NVN procedures. Returns 1 when it answered, 0 when the call reaches guest code the native version
   does not run (the material's state callback): the translated body must run. */
int hl_gfx_state_apply(uint64_t block, uint64_t key, uint64_t sp);

/* The check, per call: begin runs the native version into a recording (no procedure called, nothing written) and
   returns 1 when it would answer (0: not compared; run the translated body as always); then the caller runs the
   translated body with the tap feeding call (proc: the native numbering, hl_gfx_nvn_count or more for a procedure the
   native side does not know; x: x0..x5; f: s0..s3); end compares the calls and the block's bytes. */
int hl_gfx_state_check_begin(uint64_t block, uint64_t key, uint64_t sp);
void hl_gfx_state_check_call(uint32_t proc, const uint64_t *x, const uint32_t *f);
void hl_gfx_state_check_end(void);

/* One line of the render-state function's counters. Returns the length written. */
int hl_gfx_state_report(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
