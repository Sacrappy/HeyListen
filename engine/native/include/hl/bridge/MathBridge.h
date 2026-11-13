// HeyListen native engine: the guest structures the hl::math kernels read in place (docs/native-engine/physics-loops.md
// sections 3 and 6, critical-jobs.md). Only the fields the kernels use are named; every offset is checked here. The
// guest addresses of the functions answered and of the globals read are the OS layer's (engine/host/hl_math.c).
#ifndef HL_BRIDGE_MATH_BRIDGE_H
#define HL_BRIDGE_MATH_BRIDGE_H

#include <cstddef>
#include <cstdint>

#include "hl/math/Bounds.h"

namespace hl::bridge {

// Havok's relative array: a signed 32-bit offset from the header's own address to the first element (0: no array),
// then the element count.
struct HavokRelArray {
    int32_t offset;
    int32_t count;
};
static_assert(offsetof(HavokRelArray, count) == 4);

// A body as 0x81a040bc reads it: its convex radius at +104 and its local frame, four float4 at +128 (rows with the half
// extents in their w lanes, then the centre).
struct HavokBodyBox {
    uint8_t before_radius[104];
    float convex_radius;
    uint8_t before_frame[20];
    float frame[4][4];
};
static_assert(offsetof(HavokBodyBox, convex_radius) == 104);
static_assert(offsetof(HavokBodyBox, frame) == 128);

// The object a global pointer of the physics module points at: two floats at +80 added to every body box's extents.
struct HavokBoxBias {
    uint8_t before[80];
    float bias[2];
};
static_assert(offsetof(HavokBoxBias, bias) == 80);

// 0x80ba6d80's box (centre then half extents, 6 floats) and matrix (four float4 columns).
struct CentreExtents {
    float centre[3];
    float extents[3];
};
static_assert(offsetof(CentreExtents, extents) == 12);
struct AffineColumns {
    float col[4][4];
};
static_assert(sizeof(AffineColumns) == 64);

static_assert(offsetof(math::PreciseTransform, hi) == 48);
static_assert(offsetof(math::PreciseTransform, lo) == 64);

}   // namespace hl::bridge

#endif
