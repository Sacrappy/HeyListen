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

#ifdef __cplusplus
}
#endif
#endif
