// HeyListen native engine: the C surface (hl/c/hl_math.h) over the math kernels, reading the guest's structures in place
// through hl/bridge/MathBridge.h.
#include "hl/c/hl_math.h"

#include <cstring>

#include "hl/bridge/MathBridge.h"
#include "hl/math/Bounds.h"
#include "hl/math/Exact.h"

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

extern "C" int hl_math_model_sphere(const hl_guest_host* host, uint64_t bounds, uint64_t holder, uint64_t sp, void* out) {
    auto at = [host](uint64_t guest) { return host->ptr(host->user, guest); };
    const auto* b = static_cast<const bridge::ModelBounds*>(at(bounds));
    if (!b->sphere_out) return 0;
    const auto* d = static_cast<const bridge::ModelBoundsDesc*>(at(b->desc));
    const auto* palette = static_cast<const float(*)[4][4]>(at(static_cast<const bridge::BonePalette*>(at(holder))->matrices));
    const auto* spheres = static_cast<const float(*)[4]>(at(d->spheres));
    const uint16_t* bone = d->multi ? static_cast<const uint16_t*>(at(d->bones)) : &d->bone;
    const int n = d->multi && d->count >= 2 ? d->count : 1;
    math::SphereRecord rec;
    std::memcpy(&rec, out, sizeof rec);
    const auto* stale = static_cast<const uint8_t*>(at(sp - bridge::kSphereStaleBelowSp));
    if (!math::model_sphere(spheres, bone, n, palette, stale, rec)) return 0;
    std::memcpy(out, &rec, sizeof rec);
    return 1;
}

extern "C" int hl_math_body_box(const hl_guest_host* host, const hl_math_body_args* a, hl_math_body_plan* plan) {
    auto at = [host](uint64_t guest) { return host->ptr(host->user, guest); };
    auto u64 = [&](uint64_t guest) { uint64_t v; std::memcpy(&v, at(guest), 8); return v; };
    auto u32 = [&](uint64_t guest) { uint32_t v; std::memcpy(&v, at(guest), 4); return v; };
    math::BodyUpdateIn in;
    std::memcpy(&in.body, at(a->body), sizeof in.body);
    const auto* m = static_cast<const bridge::HavokMotion*>(at(a->motion));
    std::memcpy(in.m48, m->f48, 8);
    std::memcpy(in.m64, m->f64, 8);
    in.m80 = m->f80;
    std::memcpy(in.m96, m->f96, 8);
    std::memcpy(in.m104, m->f104, 8);
    std::memcpy(in.m120, m->f120, 8);
    const auto* r = static_cast<const bridge::HavokSolverRecord*>(at(a->record));
    in.r76 = r->f76;
    in.r32 = r->flags;
    std::memcpy(in.prev, at(a->prev), sizeof in.prev);
    const auto* s = static_cast<const bridge::HavokMotionState*>(at(a->state));
    std::memcpy(in.orient, s->orient, sizeof in.orient);
    std::memcpy(in.com, s->com, sizeof in.com);
    std::memcpy(in.s40, s->f40, sizeof in.s40);
    const auto* g = static_cast<const bridge::HavokPhysicsGlobals*>(at(a->global));
    std::memcpy(in.g80, g->f80, 16);
    std::memcpy(in.g96, g->f96, 8);
    std::memcpy(in.g336, g->f336, 8);
    std::memcpy(in.g512, g->f512, 8);
    std::memcpy(in.g640, g->f640, 8);
    in.shape_kind = a->shape_kind;
    in.vertices = nullptr;
    in.vertex_count = 0;
    in.box_frame = nullptr;
    in.vertex_start = static_cast<const float*>(at(a->start));
    const uint64_t shape = in.body.shape;
    if (!(in.body.flags140 & 8) && a->shape_kind) {
        const auto* sh = static_cast<const bridge::HavokShape*>(at(shape));
        if (a->shape_kind == 1) {
            // (an offset of 0 means no array: only a count of 0 is answered, as hl_math_vertex_set_box)
            if (sh->vertices.offset == 0 && sh->vertices.count != 0) {
                plan->decline = (int)math::BodyDecline::shape;
                return 0;
            }
            in.vertices = static_cast<const float*>(at(shape + 56 + (int64_t)sh->vertices.offset));
            in.vertex_count = sh->vertices.count;
            in.convex_radius = sh->radius;
            in.radius_scale = sh->radius_scale;
        } else {
            const auto* bb = static_cast<const bridge::HavokBodyBox*>(at(shape));
            in.box_frame = &bb->frame[0][0];
            in.box_radius = bb->convex_radius;
        }
    }
    // the streams
    uint64_t block1 = 0, block2 = 0;
    in.has_ctx = a->ctx != 0;
    in.has_stream2 = in.same_block = false;
    in.header1 = in.header2 = 0;
    if (in.has_ctx) {
        const auto* t = static_cast<const bridge::HavokThreadStreams*>(at(a->ctx));
        block1 = t->block1;
        in.header1 = u32(block1);
        const int64_t index = (int8_t)*static_cast<const uint8_t*>(at(t->index_holder + bridge::kStreamIndexAt));
        if (index != -1 && t->entries) {
            const uint64_t writer = u64(t->entries + (uint64_t)(int64_t)(int32_t)((int32_t)index * (int32_t)bridge::kStreamEntrySize)
                                        + bridge::kStreamEntryWriter);
            block2 = u64(writer);
            in.has_stream2 = true;
            in.header2 = u32(block2);
            in.same_block = block2 == block1;
        }
    }
    math::BodyUpdateOut out;
    math::BodyDecline why = math::BodyDecline::none;
    if (!math::body_box_update(in, out, why)) {
        plan->decline = (int)why;
        return 0;
    }
    std::memcpy(plan->body, &out.body, sizeof plan->body);
    std::memcpy(plan->written, out.written, sizeof plan->written);
    plan->appends = 0;
    plan->decline = 0;
    auto add = [&](uint64_t block, const math::BodyAppend& x) {
        hl_math_body_append& d = plan->add[plan->appends++];
        d.block = block;
        d.at = x.at;
        d.header = x.header;
        d.len = x.len;
        std::memcpy(d.rec, x.rec, sizeof d.rec);
    };
    if (out.append1) add(block1, out.add1);
    if (out.append2) add(block2, out.add2);
    return 1;
}

extern "C" int hl_math_last_why(uint64_t values[3]) {
    const int w = (int)math::last_why;
    math::last_why = math::Why::none;
    for (int k = 0; k < 3; k++) values[k] = math::last_why_values[k];
    return w;
}

extern "C" int hl_math_affine_box(const void* box_in, const void* m, void* out) {
    const auto* ce = static_cast<const bridge::CentreExtents*>(box_in);
    const float v[6] = {ce->centre[0], ce->centre[1], ce->centre[2], ce->extents[0], ce->extents[1], ce->extents[2]};
    math::Box box;
    return written(math::affine_box(v, static_cast<const bridge::AffineColumns*>(m)->col, box), box, out);
}
