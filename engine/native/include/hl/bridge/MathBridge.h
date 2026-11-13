// HeyListen native engine: the guest structures the hl::math kernels read in place (docs/native-engine/physics-loops.md
// sections 3 and 6, critical-jobs.md). Only the fields the kernels use are named; every offset is checked here. The
// guest addresses of the functions answered and of the globals read are the OS layer's (engine/host/hl_math.c).
#ifndef HL_BRIDGE_MATH_BRIDGE_H
#define HL_BRIDGE_MATH_BRIDGE_H

#include <cstddef>
#include <cstdint>

#include "hl/math/BodyBox.h"
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

// 0x800a8400's model bounds: x0 is the bounds object (its descriptor, its output sphere record), x1 the object holding
// the bone matrix palette (64-byte matrices). Guest pointers are 64-bit guest addresses.
struct ModelBounds {
    uint64_t desc;
    uint8_t before_out[48];
    uint64_t sphere_out;   // 0: nothing to do
};
static_assert(offsetof(ModelBounds, sphere_out) == 56);
struct ModelBoundsDesc {
    uint8_t before_bones[32];
    uint64_t bones;        // 16-bit palette index a sphere (multi)
    uint8_t before_spheres[24];
    uint64_t spheres;      // 16-byte local spheres
    uint8_t before_bone[12];
    uint16_t bone;         // the one palette index (single)
    uint8_t before_count[2];
    uint16_t count;        // spheres (multi; the first is used whatever the count)
    uint8_t multi;
};
static_assert(offsetof(ModelBoundsDesc, bones) == 32);
static_assert(offsetof(ModelBoundsDesc, spheres) == 64);
static_assert(offsetof(ModelBoundsDesc, bone) == 84);
static_assert(offsetof(ModelBoundsDesc, count) == 88);
static_assert(offsetof(ModelBoundsDesc, multi) == 90);
struct BonePalette {
    uint8_t before[32];
    uint64_t matrices;
};
static_assert(offsetof(BonePalette, matrices) == 32);
// (0x800a8400's frame: 144 bytes; when a sphere replaces the record whole it copies 32 bytes from its frame, the last 12
// of which it never wrote: they are what lay at the caller's stack pointer - 28 at the call)
constexpr uint64_t kSphereStaleBelowSp = 28;

// ---- 0x817d9d80 (the per-body box update; hl/math/BodyBox.h has the body) ----
static_assert(offsetof(math::Body, q76) == 76);
static_assert(offsetof(math::Body, id) == 88);
static_assert(offsetof(math::Body, flags) == 92);
static_assert(offsetof(math::Body, q94) == 94);
static_assert(offsetof(math::Body, box_hi) == 96);
static_assert(offsetof(math::Body, box_lo) == 112);
static_assert(offsetof(math::Body, shape) == 120);
static_assert(offsetof(math::Body, flags140) == 140);
static_assert(offsetof(math::Body, q148) == 148);
static_assert(offsetof(math::Body, scale154) == 154);
static_assert(offsetof(math::Body, b156) == 156);
static_assert(offsetof(math::Body, ext157) == 157);
static_assert(offsetof(math::Body, orient) == 160);
static_assert(offsetof(math::Body, at176) == 176);

// The motion (x1): the pairs and scalars the update reads.
struct HavokMotion {
    uint8_t before48[48];
    float f48[2];
    uint8_t before64[8];
    float f64[2];
    uint8_t before80[8];
    float f80;
    uint8_t before96[12];
    float f96[2];
    float f104[2];
    uint8_t before120[8];
    float f120[2];
};
static_assert(offsetof(HavokMotion, f48) == 48);
static_assert(offsetof(HavokMotion, f64) == 64);
static_assert(offsetof(HavokMotion, f80) == 80);
static_assert(offsetof(HavokMotion, f96) == 96);
static_assert(offsetof(HavokMotion, f104) == 104);
static_assert(offsetof(HavokMotion, f120) == 120);

// The solver's per-body record (x2, 104 bytes each): flags at +32, a float at +76.
struct HavokSolverRecord {
    uint8_t before32[32];
    uint32_t flags;
    uint8_t before76[40];
    float f76;
};
static_assert(offsetof(HavokSolverRecord, flags) == 32);
static_assert(offsetof(HavokSolverRecord, f76) == 76);

// The motion state (x5): orientation, the centre of mass in binary64, then twelve floats (+64 linear and +76 angular
// velocity among them).
struct HavokMotionState {
    float orient[4];
    double com[3];
    float f40[12];
};
static_assert(offsetof(HavokMotionState, com) == 16);
static_assert(offsetof(HavokMotionState, f40) == 40);

// The physics module's global object (the pointer at a fixed guest address, the OS layer's): the update's constants.
struct HavokPhysicsGlobals {
    uint8_t before80[80];
    float f80[4];
    float f96[2];
    uint8_t before336[232];
    float f336[2];
    uint8_t before512[168];
    float f512[2];
    uint8_t before640[120];
    float f640[2];
};
static_assert(offsetof(HavokPhysicsGlobals, f96) == 96);
static_assert(offsetof(HavokPhysicsGlobals, f336) == 336);
static_assert(offsetof(HavokPhysicsGlobals, f512) == 512);
static_assert(offsetof(HavokPhysicsGlobals, f640) == 640);

// A shape as the two box functions read it: its type (the shape-type table's index) at +24; a convex vertex set's
// relative vertex array at +56, its radius at +104 times the float whose high half is at +108; an oriented box's
// radius at +104 and frame at +128 (HavokBodyBox above).
struct HavokShape {
    uint8_t before24[24];
    uint8_t type;
    uint8_t before56[31];
    HavokRelArray vertices;
    uint8_t before104[40];
    float radius;
    uint16_t radius_scale;
};
static_assert(offsetof(HavokShape, type) == 24);
static_assert(offsetof(HavokShape, vertices) == 56);
static_assert(offsetof(HavokShape, radius) == 104);
static_assert(offsetof(HavokShape, radius_scale) == 108);

// The simulation thread context (x0): the moved-bodies stream's current block at +1568, the second stream's index
// holder at +1888 (a signed byte at its +677) and its array of 80-byte entries at +1896 (the entry's writer at +32,
// whose first word is the block). A block's header word: bytes used in the low half (records from +32), records in
// the high half.
struct HavokThreadStreams {
    uint8_t before1568[1568];
    uint64_t block1;
    uint8_t before1888[312];
    uint64_t index_holder;
    uint64_t entries;
};
static_assert(offsetof(HavokThreadStreams, block1) == 1568);
static_assert(offsetof(HavokThreadStreams, index_holder) == 1888);
static_assert(offsetof(HavokThreadStreams, entries) == 1896);
constexpr uint64_t kStreamIndexAt = 677, kStreamEntrySize = 80, kStreamEntryWriter = 32;

static_assert(offsetof(math::PreciseTransform, hi) == 48);
static_assert(offsetof(math::PreciseTransform, lo) == 64);

}   // namespace hl::bridge

#endif
