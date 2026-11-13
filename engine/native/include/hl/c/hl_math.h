/* HeyListen native engine: the math kernels' C surface for the OS layer (engine/host/hl_math.c; hl/math/Bounds.h,
   hl/bridge/MathBridge.h). Every pointer is a host pointer to the guest's memory (or to scratch memory laid out the same).
   Each function writes its 32-byte box to out and returns 1, or returns 0 having written nothing when the inputs are
   not ones it is exact on (the OS layer then runs the translated code). */
#ifndef HL_C_MATH_H
#define HL_C_MATH_H

#include "hl/c/hl_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0x81746440: array = the vertex array's header (relative offset, count), xf = the precise transform, start = the
   starting bound (4 floats), out = the box */
int hl_math_vertex_set_box(const void *array, const void *xf, const void *start, void *out);
/* 0x81a040bc: body, xf = the precise transform, bias = the object holding the extents' bias, out = the box */
int hl_math_oriented_box_box(const void *body, const void *xf, const void *bias, void *out);
/* 0x80ba6d80: box = centre and half extents, m = the matrix's four columns, out = the box */
int hl_math_affine_box(const void *box, const void *m, void *out);
/* 0x800a8400, with its arguments as guest addresses (bounds = x0, holder = x1, sp = the stack pointer at the call) read
   through host->ptr: out = the 32-byte sphere record, holding the guest's record on entry, written whole on 1. Returns 0
   also when the bounds object has no record to write (the translated function then does nothing). */
int hl_math_model_sphere(const hl_guest_host *host, uint64_t bounds, uint64_t holder, uint64_t sp, void *out);

#ifdef __cplusplus
}
#endif
#endif
