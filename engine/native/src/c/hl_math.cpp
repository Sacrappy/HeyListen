// HeyListen native engine: the C surface (hl/c/hl_math.h) over the math kernels, reading the guest's structures in place
// through hl/bridge/MathBridge.h.
#include "hl/c/hl_math.h"

#include <cstring>

#include "hl/bridge/MathBridge.h"
#include "hl/math/Bounds.h"

using namespace hl;

namespace {
int written(bool ok, const math::Box& box, void* out) {
    if (ok) std::memcpy(out, &box, sizeof box);
    return ok;
}
}   // namespace

extern "C" int hl_math_vertex_set_box(const void* array, const void* xf, const void* start, void* out) {
    const auto* a = static_cast<const bridge::HavokRelArray*>(array);
    const int32_t n = a->count;
    // (an offset of 0 means no array: the guest then walks from address 0, so only a count of 0 is answered)
    if (a->offset == 0 && n != 0) return 0;
    const auto* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(array) + a->offset);
    math::Box box;
    return written(math::vertex_set_box(v, n, *static_cast<const math::PreciseTransform*>(xf),
                                        static_cast<const float*>(start), box), box, out);
}

extern "C" int hl_math_oriented_box_box(const void* body, const void* xf, const void* bias, void* out) {
    const auto* b = static_cast<const bridge::HavokBodyBox*>(body);
    math::Box box;
    return written(math::oriented_box_box(*static_cast<const math::PreciseTransform*>(xf), b->frame, b->convex_radius,
                                          static_cast<const bridge::HavokBoxBias*>(bias)->bias, box), box, out);
}

extern "C" int hl_math_affine_box(const void* box_in, const void* m, void* out) {
    const auto* ce = static_cast<const bridge::CentreExtents*>(box_in);
    const float v[6] = {ce->centre[0], ce->centre[1], ce->centre[2], ce->extents[0], ce->extents[1], ce->extents[2]};
    math::Box box;
    return written(math::affine_box(v, static_cast<const bridge::AffineColumns*>(m)->col, box), box, out);
}
