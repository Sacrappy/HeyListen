/* HeyListen native engine: the math kernels' C surface for the OS layer (engine/host/hl_math.c; hl/math/Bounds.h,
   hl/bridge/MathBridge.h). Every pointer is a host pointer to the guest's memory (or to scratch memory laid out the same).
   Each function writes its 32-byte box to out and returns 1, or returns 0 having written nothing when the inputs are
   not ones it is exact on (the OS layer then runs the translated code). */
#ifndef HL_C_MATH_H
#define HL_C_MATH_H

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

#ifdef __cplusplus
}
#endif
#endif
