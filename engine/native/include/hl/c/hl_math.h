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

/* 0x817d9d80, the per-body box update (hl/math/BodyBox.h), with its arguments as guest addresses read through host->ptr:
   ctx = x0 (0: none), motion = x1, record = x2, prev = x3, body = x4, state = x5; global = the physics module's global
   object, start = the starting bound of 0x81746440; shape_kind = what the shape-type table gives for the body's shape:
   1 the convex vertex set's function, 2 the oriented box's, 3 the segment's (0x8174bb00), 0 another. Nothing is written to the game: the plan says
   what the function would leave (the body's 192 bytes and which it stores, and the records it appends to the thread's
   stream blocks); the OS layer writes it, or compares it after running the translation. Returns 1, or 0 with the
   reason in plan->decline (hl::math::BodyDecline). */
typedef struct {
    uint64_t ctx, motion, record, prev, body, state, global, start;
    int shape_kind;
} hl_math_body_args;
typedef struct {
    uint64_t block;            /* guest address of the block */
    uint32_t at;               /* the record's byte offset in it */
    uint32_t header;           /* the block's first word afterwards */
    uint32_t len;
    uint8_t rec[16];
} hl_math_body_append;
typedef struct {
    uint8_t body[192];
    uint8_t written[192];
    int appends;
    hl_math_body_append add[2];
    int decline;
} hl_math_body_plan;
int hl_math_body_box(const hl_guest_host *host, const hl_math_body_args *a, hl_math_body_plan *plan);

/* The calling thread's reason the last declining kernel gave up (hl::math::Why: 0 none; see hl/math/Exact.h), cleared
   by the call; values gets the operation's operands and result (bits). */
int hl_math_last_why(uint64_t values[3]);

#ifdef __cplusplus
}
#endif
#endif
