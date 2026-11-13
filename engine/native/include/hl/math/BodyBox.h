// HeyListen native engine: the per-body bounding-box update of Havok's last solver substep (guest 0x817d9d80,
// docs/native-engine/physics-loops.md section 6.3), operation for operation (hl/math/Exact.h). It works on copies: the
// caller gives the values the function reads and gets back the body as the function leaves it and the records it
// appends to the thread's streams; nothing is written to the game here. It declines (returns false, the reason in
// `why`) where the exactness rules do not cover a value, and on the paths it does not answer: a shape whose box is
// not one of the two native kernels', the swept cast (0x817d8c90), no thread context, a stream block that is full.
#ifndef HL_MATH_BODY_BOX_H
#define HL_MATH_BODY_BOX_H

#include <cstdint>

#include "hl/math/Bounds.h"

namespace hl::math {

// The body (192 bytes): its precise transform (rows with the local centre of mass in their w lanes), quantised speeds,
// flags, its stored box (max x, y, z and min z, then min x, y), its shape, the quantised local box of the inline path
// and its orientation. Names say what the function does with a field; where the meaning is a guess, the name is the
// offset.
struct Body {
    PreciseTransform xf;      // 0
    uint16_t q76, q78;        // 76, 78: quantised speeds (written)
    uint8_t at80[8];
    uint32_t id;              // 88: appended to the moved-bodies stream
    uint16_t flags;           // 92: bit 10 moving; bit 4 set when the box is stored
    uint16_t q94;             // 94 (written)
    float box_hi[4];          // 96: max x, y, z, min z
    float box_lo[2];          // 112: min x, y
    uint64_t shape;           // 120
    uint8_t at128[12];
    uint8_t flags140;         // 140: bit 3 the inline (quantised local box) path
    uint8_t at141[7];
    uint16_t q148, q150;      // 148, 150 (written)
    uint8_t at152[2];
    uint16_t scale154;        // 154: a float's high half
    uint8_t b156;             // 156
    uint8_t ext157[3];        // 157..159: the quantised local half extents
    float orient[4];          // 160: the body's orientation in its motion's frame
    uint64_t at176;           // 176: appended to the second stream
    uint8_t at184[8];
};
static_assert(sizeof(Body) == 192);

// What the function reads beside the body: its motion (x1), the solver record (x2), the box x3, the motion state (x5),
// the physics module's global object (G) and, for the table path, the shape.
struct BodyUpdateIn {
    Body body;
    float m48[2], m64[2], m80, m96[2], m104[2], m120[2];   // motion
    float r76;                                              // record +76
    uint32_t r32;                                           // record +32 (flags)
    float prev[8];                                          // the box x3 points at: min 4, max 4
    float orient[4];                                        // state +0
    double com[3];                                          // state +16, +24, +32
    float s40[12];                                          // state +40 .. +84
    float g80[4], g96[2], g336[2], g512[2], g640[2];        // G
    // shape box of the table path: kind 1 convex vertex set (0x8173e2ac), 2 oriented box (0x81a040bc), 0 other
    int shape_kind;
    const float* vertices;     // convex: packed xyz
    int32_t vertex_count;
    float convex_radius;       // convex: shape +104
    uint16_t radius_scale;     // convex: shape +108
    const float* box_frame;    // box: shape +128, four float4
    float box_radius;          // box: shape +104
    const float* vertex_start; // the starting bound of 0x81746440 (4 floats)
    // streams (when there is a thread context): the moved-bodies block's header, and whether the second stream is
    // there, with its block's header (blocks are the same when same_block)
    bool has_ctx;
    uint32_t header1;
    bool has_stream2;
    uint32_t header2;
    bool same_block;
};

enum class BodyDecline : uint8_t { none, values, shape, cast, no_ctx, block_full, count };

struct BodyAppend {
    uint32_t at;        // byte offset of the record in its block (32 + the header's low half)
    uint32_t header;    // the block's header word afterwards
    uint8_t rec[16];
    uint32_t len;       // bytes of rec written
};

struct BodyUpdateOut {
    Body body;                  // as the function leaves it
    uint8_t written[192];       // 1 where the function stores (a stored byte may keep its value)
    bool append1, append2;
    BodyAppend add1, add2;
};

bool body_box_update(const BodyUpdateIn& in, BodyUpdateOut& out, BodyDecline& why);

}   // namespace hl::math

#endif
