// HeyListen native engine: bounding-box kernels of the physics and actor loops (docs/native-engine/physics-loops.md
// section 6, critical-jobs.md). Plain functions on plain values; each computes exactly what the guest function it
// replaces computes, operation for operation (hl/math/Exact.h), or returns false having written nothing when an input or
// a value on the way is one the exactness rules do not cover (the caller then runs the translated code).
//
// Boxes are 8 floats: min x, y, z, w then max x, y, z, w (the w lanes are computed like the others, not cleared).
#ifndef HL_MATH_BOUNDS_H
#define HL_MATH_BOUNDS_H

#include <cstdint>

namespace hl::math {

// Havok's precise transform: three rotation rows (each a float4: the rotated basis vectors, applied as
// row0 * x + row1 * y + row2 * z), a translation in two parts, high (4 lanes) and low (3 lanes; the w lane reuses z's),
// added in binary64 where the position is needed.
struct PreciseTransform {
    float row[3][4];
    float hi[4];
    float lo[3];
};
static_assert(sizeof(PreciseTransform) == 76);

struct Box {
    float min[4];
    float max[4];
};
static_assert(sizeof(Box) == 32);

// Box of n vertices (packed x, y, z, 12 bytes each) under a precise transform: each vertex rotated (three products, then
// (row0 x + row1 y) + row2 z, separately rounded), the lanes' Arm FMIN / FMAX from start (min = start, max = -start),
// then the translation added in binary64 and rounded outward to binary32.
bool vertex_set_box(const float* vertices, int32_t n, const PreciseTransform& t, const float start[4], Box& out);

// Box of a body's oriented box under a precise transform A. q: the body's four float4 at its +128 (its local frame: the
// rows' w lanes hold the half extents, q[3] the centre); radius: its convex radius; g: two floats added to the extents
// (lanes x, y, x, y). The body's frame is composed with A (rows, then the precise translation in two parts), the
// extents are the absolute rows times the half extents plus the radius, and the box is the binary64 centre minus / plus
// the extents, rounded outward.
bool oriented_box_box(const PreciseTransform& a, const float q[4][4], float radius, const float g[2], Box& out);

// A box given as centre (3 floats) and half extents (3 floats) under a 3x4 affine matrix m (four float4 columns: the
// rotation's three, then the translation): centre' = m0 cx + m1 cy + m2 cz + m3 with the guest's two fused
// multiply-adds, half extents' = |m0| ex + |m1| ey + |m2| ez; out = centre' -/+ extents'.
bool affine_box(const float centre_extents[6], const float m[4][4], Box& out);

// A model's bounding sphere record: centre (4 lanes), radius, and 12 bytes the guest copies along when a sphere replaces
// the record whole (from its own stack: the kernel is given them).
struct SphereRecord {
    float centre[4];
    float radius;
    uint8_t tail[12];
};
static_assert(sizeof(SphereRecord) == 32);

// The bounding sphere of n >= 1 local spheres (centre x, y, z, radius: 16 bytes each), sphere i under the matrix
// palette[bone[i]] (four float4 columns): its centre transformed as in affine_box, its radius times the largest column
// length (squares summed as (z + x) + (w + y), square roots, the largest by compares). The first sphere is the record;
// each next one grows it in order: when the distance squared between the centres is under max((r - R)^2, 1e-4), the
// larger of the two spheres is kept (the new one whole, with `stale` as the tail, unless the record's radius is the
// larger); otherwise the record moves towards the new centre and grows to cover both. `out` is read for its tail and
// written whole.
bool model_sphere(const float (*spheres)[4], const uint16_t* bone, int n, const float (*palette)[4][4],
                  const uint8_t stale[12], SphereRecord& out);

}   // namespace hl::math

#endif
