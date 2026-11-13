// HeyListen native engine: hl::math with no game (tests/test_native_engine.py): the exactness helpers' truth tables, each
// kernel against a reference written straight from its specification (docs/native-engine/physics-loops.md section 6,
// critical-jobs.md), the inputs the kernels must give up on, and the C surface reading the guest's layouts.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "hl/bridge/MathBridge.h"
#include "hl/c/hl_math.h"
#include "hl/math/Bounds.h"
#include "hl/math/Exact.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

using namespace hl;
using math::bits;
using math::f32;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

const uint32_t kQNaN = 0x7fc00001u, kQNaN2 = 0xffc00002u, kSNaN = 0x7f800003u, kSNaN2 = 0xff800004u;

void fmin_fmax_are_arms() {
    struct Row { uint32_t a, b, min, max; };
    const Row rows[] = {
        {0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u},   // +0, -0
        {0x80000000u, 0x00000000u, 0x80000000u, 0x00000000u},   // -0, +0
        {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u},
        {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x3f800000u, 0x40000000u, 0x3f800000u, 0x40000000u},   // 1, 2
        {0x40000000u, 0x3f800000u, 0x3f800000u, 0x40000000u},
        {0xbf800000u, 0x3f800000u, 0xbf800000u, 0x3f800000u},   // -1, 1
        {0xff800000u, 0x7f800000u, 0xff800000u, 0x7f800000u},   // -inf, +inf
        {0x00000001u, 0x80000001u, 0x80000001u, 0x00000001u},   // subnormals (FZ clear: ordered as values)
        {kQNaN, 0x3f800000u, kQNaN, kQNaN},                     // a quiet NaN is returned, the first operand's first
        {0x3f800000u, kQNaN2, kQNaN2, kQNaN2},
        {kQNaN, kQNaN2, kQNaN, kQNaN},
        {kSNaN, 0x3f800000u, kSNaN | 0x00400000u, kSNaN | 0x00400000u},   // a signalling NaN comes back quieted
        {0x3f800000u, kSNaN2, kSNaN2 | 0x00400000u, kSNaN2 | 0x00400000u},
        {kQNaN, kSNaN2, kSNaN2 | 0x00400000u, kSNaN2 | 0x00400000u},       // and wins over a quiet one
        {kSNaN, kSNaN2, kSNaN | 0x00400000u, kSNaN | 0x00400000u},
    };
    for (const Row& r : rows) {
        CHECK(bits(math::fmin(f32(r.a), f32(r.b))) == r.min);
        CHECK(bits(math::fmax(f32(r.a), f32(r.b))) == r.max);
    }
}

void outward_steps_are_the_guests() {
    const double one = 1.0, minus = -2.5, nan = std::nan("");
    CHECK(bits(math::outward_min_bits(one)) == bits(one) - 0x10000000u);
    CHECK(bits(math::outward_min_bits(minus)) == bits(minus) + 0x10000000u);
    CHECK(bits(math::outward_min_bits(0.0)) == 0x10000000u);
    CHECK(bits(math::outward_min_bits(-0.0)) == 0x8000000010000000ull);
    CHECK(bits(math::outward_min_bits(nan)) == bits(nan) - 0x10000000u);   // (unordered selects the step down)
    CHECK(bits(math::outward_max_bits(one)) == bits(one) + 0x10000000u);
    CHECK(bits(math::outward_max_bits(minus)) == bits(minus) - 0x10000000u);
    CHECK(bits(math::outward_max_bits(0.0)) == 0x10000000u);
    CHECK(bits(math::outward_max_bits(-0.0)) == 0x8000000010000000ull);
    CHECK(bits(math::outward_max_bits(nan)) == bits(nan) - 0x10000000u);
    // a corner on a float rounds outward or stays (a tie to even), never inward
    for (float f : {1.0f, 3.0f, -7.25f, 1e-3f, -1e6f}) {
        CHECK((float)math::outward_min_bits((double)f) <= f);
        CHECK((float)math::outward_max_bits((double)f) >= f);
    }
    // a corner between two floats goes to the outer one
    const double between = 1.0 + std::ldexp(1.0, -30);
    CHECK((float)math::outward_max_bits(between) == f32(0x3f800001u));
    CHECK((float)math::outward_min_bits(between) == 1.0f);
}

void the_guard_gives_up_where_x86_and_arm_may_differ() {
    math::Guard g;
    g.in(1.0f);
    g.in(-0.0f);
    g.in(f32(0x00800000u));   // the smallest normal is an ordinary input
    CHECK(g.ok);
    for (uint32_t b : {0x00000001u, 0x807fffffu, 0x7f800000u, 0xff800000u, kQNaN, kSNaN}) {
        math::Guard h;
        h.in(f32(b));
        CHECK(!h.ok);
    }
    {   // a product at or below the smallest normal
        math::Guard h;
        h.mul(f32(0x00800000u), 1.0f);
        CHECK(!h.ok);
        math::Guard k;
        k.mul(1e-30f, 1e-30f);   // underflows to zero from two non-zero operands
        CHECK(!k.ok);
        math::Guard z;
        CHECK(bits(z.mul(-0.0f, 5.0f)) == 0x80000000u && z.ok);
    }
    {   // an overflow
        math::Guard h;
        h.mul(3e38f, 10.0f);
        CHECK(!h.ok);
    }
    {   // an exact cancellation is fine, a tiny sum is not
        math::Guard h;
        CHECK(bits(h.sub(1.5f, 1.5f)) == 0 && h.ok);
        math::Guard k;
        k.sub(f32(0x00800003u), f32(0x00800001u));
        CHECK(!k.ok);
    }
    {   // a narrowing to a subnormal float, and to zero
        math::Guard h;
        h.narrow(1e-40);
        CHECK(!h.ok);
        math::Guard k;
        CHECK(bits(k.narrow(-1e-300)) == 0x80000000u && k.ok);
    }
}

// fused multiply-add through binary64 against the C library's correctly rounded one
void fma_is_rounded_once() {
    uint64_t s = 0x9e3779b97f4a7c15ull;
    auto next = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    auto value = [&] {   // a float of moderate exponent, either sign
        const uint32_t m = (uint32_t)next() & 0x007fffffu, e = 100 + (uint32_t)(next() % 54), sg = (uint32_t)next() & 0x80000000u;
        return f32(sg | e << 23 | m);
    };
    int taken = 0, midpoints = 0;
    for (int i = 0; i < 2000000; i++) {
        const float w = value(), a = value(), b = value();
        math::Guard g;
        const float t = g.fma(w, a, b);
        if (!g.ok) { midpoints++; continue; }
        CHECK(bits(t) == bits(std::fmaf(a, b, w)));
        taken++;
    }
    CHECK(taken > 1900000);
    CHECK(midpoints < 100000);
    // where rounding twice is wrong: w + b c = (1 + 2^-23) + (2^-24 - 2^-70), just under the midpoint between
    // 1 + 2^-23 and 1 + 2^-22; binary64 rounds it onto the midpoint, and the tie then goes to the even neighbour above
    const float w = f32(0x3f800001u), b = f32(0x3f800001u), c = f32(0x337ffffeu);
    CHECK(bits(std::fmaf(b, c, w)) == 0x3f800001u);
    CHECK(bits((float)((double)b * (double)c + (double)w)) == 0x3f800002u);
    math::Guard h;
    h.fma(w, b, c);
    CHECK(!h.ok);
}

// ---- references written from the specifications, without the guard ----
float ref_narrow_min(double a) {
    uint64_t u;
    std::memcpy(&u, &a, 8);
    u += (a > 0.0 || a != a) ? (uint64_t)-0x10000000ll : 0x10000000ull;
    double d;
    std::memcpy(&d, &u, 8);
    return (float)d;
}
float ref_narrow_max(double a) {
    uint64_t u;
    std::memcpy(&u, &a, 8);
    u += a >= 0.0 ? 0x10000000ull : (uint64_t)-0x10000000ll;
    double d;
    std::memcpy(&d, &u, 8);
    return (float)d;
}
float ref_min(float a, float b) { return (a == 0 && b == 0) ? (std::signbit(a) ? a : b) : (a < b ? a : b); }
float ref_max(float a, float b) { return (a == 0 && b == 0) ? (std::signbit(a) ? b : a) : (a > b ? a : b); }

math::Box ref_vertex_set_box(const float* v, int n, const math::PreciseTransform& t, const float big[4]) {
    float mn[4], mx[4];
    for (int k = 0; k < 4; k++) { mn[k] = big[k]; mx[k] = -big[k]; }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) {
            const float tt = t.row[0][k] * v[3 * i];
            const float u = t.row[1][k] * v[3 * i + 1];
            const float w = t.row[2][k] * v[3 * i + 2];
            const float tu = tt + u;
            const float s = tu + w;
            mn[k] = ref_min(mn[k], s);
            mx[k] = ref_max(mx[k], s);
        }
    const double p[4] = {(double)t.hi[0] + t.lo[0], (double)t.hi[1] + t.lo[1], (double)t.hi[2] + t.lo[2], (double)t.hi[3] + t.lo[2]};
    math::Box b;
    for (int k = 0; k < 4; k++) {
        b.min[k] = ref_narrow_min(p[k] + (double)mn[k]);
        b.max[k] = ref_narrow_max(p[k] + (double)mx[k]);
    }
    return b;
}

math::Box ref_oriented_box_box(const math::PreciseTransform& A, const float B[4][4], float r, const float G[2]) {
    float T[4][4];
    for (int q = 0; q < 4; q++)
        for (int k = 0; k < 4; k++) {
            const float x = A.row[0][k] * B[q][0];
            const float y = A.row[1][k] * B[q][1];
            const float z = A.row[2][k] * B[q][2];
            const float xy = x + y;
            T[q][k] = xy + z;
        }
    const float L[4] = {A.lo[0], A.lo[1], A.lo[2], A.lo[2]};
    float hi[4], lo[3];
    for (int k = 0; k < 4; k++) {
        const float s = T[3][k] + L[k];
        hi[k] = A.hi[k] + s;
        if (k < 3) {
            const float back = hi[k] - A.hi[k];
            lo[k] = s - back;
        }
    }
    const float e[3] = {B[0][3] + r, B[1][3] + r, B[2][3] + r};
    const float C[4] = {G[0], G[1], G[0], G[1]};
    math::Box b;
    for (int k = 0; k < 4; k++) {
        const float a0 = std::fabs(T[0][k] * e[0]), a1 = std::fabs(T[1][k] * e[1]), a2 = std::fabs(T[2][k] * e[2]);
        const float l = C[k] + a0, rr = a1 + a2;
        const float E = l + rr;
        const double P = (double)hi[k] + (double)lo[k < 3 ? k : 2];
        b.min[k] = ref_narrow_min(P - (double)E);
        b.max[k] = ref_narrow_max(P + (double)E);
    }
    return b;
}

math::Box ref_affine_box(const float ce[6], const float m[4][4]) {
    math::Box b;
    for (int k = 0; k < 4; k++) {
        const float c0 = m[0][k] * ce[0];
        const float c1 = std::fmaf(m[1][k], ce[1], c0);
        const float c2 = std::fmaf(m[2][k], ce[2], c1);
        const float c = c2 + m[3][k];
        const float e0 = std::fabs(m[0][k]) * ce[3], e1 = std::fabs(m[1][k]) * ce[4], e2 = std::fabs(m[2][k]) * ce[5];
        const float e01 = e0 + e1;
        const float e = e01 + e2;
        b.min[k] = c - e;
        b.max[k] = c + e;
    }
    return b;
}

bool same(const math::Box& a, const math::Box& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

struct Rng {
    uint64_t s = 0x243f6a8885a308d3ull;
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    float uniform(float lo, float hi) { return lo + (hi - lo) * (float)((next() >> 40) * (1.0 / 16777216.0)); }
    float pick(float lo, float hi) {   // mostly ordinary values, with exact zeros and signed zeros among them
        const uint64_t r = next() % 32;
        return r == 0 ? 0.0f : r == 1 ? -0.0f : uniform(lo, hi);
    }
};

math::PreciseTransform random_transform(Rng& r) {
    math::PreciseTransform t;
    for (auto& row : t.row)
        for (float& f : row) f = r.pick(-1.0f, 1.0f);
    for (float& f : t.hi) f = r.uniform(-4000.0f, 4000.0f);
    for (float& f : t.lo) f = r.uniform(-1e-4f, 1e-4f);
    return t;
}

void vertex_set_box_matches_the_specification() {
    Rng r;
    const float big[4] = {f32(0x7f7fffeeu), f32(0x7f7fffeeu), f32(0x7f7fffeeu), f32(0x7f7fffeeu)};
    float v[3 * 40];
    int taken = 0;
    for (int i = 0; i < 20000; i++) {
        const math::PreciseTransform t = random_transform(r);
        const int n = (int)(r.next() % 40);
        for (int j = 0; j < 3 * n; j++) v[j] = r.pick(-50.0f, 50.0f);
        math::Box got;
        if (!math::vertex_set_box(v, n, t, big, got)) continue;
        CHECK(same(got, ref_vertex_set_box(v, n, t, big)));
        taken++;
    }
    CHECK(taken > 19000);
    // no vertex: the starting bound around the position
    {
        math::PreciseTransform t = random_transform(r);
        math::Box got;
        CHECK(math::vertex_set_box(v, 0, t, big, got) && same(got, ref_vertex_set_box(v, 0, t, big)));
    }
    // given up: a NaN, an infinity, a subnormal vertex; a negative count
    for (uint32_t bad : {kQNaN, 0x7f800000u, 0x00000010u}) {
        math::PreciseTransform t = random_transform(r);
        for (int j = 0; j < 9; j++) v[j] = 1.0f;
        v[4] = f32(bad);
        math::Box got;
        std::memset(&got, 0xab, sizeof got);
        CHECK(!math::vertex_set_box(v, 3, t, big, got));
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&got);
        CHECK(p[0] == 0xab && p[31] == 0xab);   // (nothing written)
    }
    {
        math::PreciseTransform t = random_transform(r);
        math::Box got;
        CHECK(!math::vertex_set_box(v, -1, t, big, got));
    }
}

void oriented_box_box_matches_the_specification() {
    Rng r;
    int taken = 0;
    for (int i = 0; i < 100000; i++) {
        const math::PreciseTransform a = random_transform(r);
        float q[4][4];
        for (auto& row : q)
            for (float& f : row) f = r.pick(-2.0f, 2.0f);
        for (int k = 0; k < 3; k++) q[k][3] = r.uniform(0.0f, 3.0f);
        for (int k = 0; k < 3; k++) q[3][k] = r.uniform(-20.0f, 20.0f);
        const float radius = r.uniform(0.0f, 0.1f), g[2] = {r.uniform(0.0f, 0.05f), r.uniform(0.0f, 0.05f)};
        math::Box got;
        if (!math::oriented_box_box(a, q, radius, g, got)) continue;
        CHECK(same(got, ref_oriented_box_box(a, q, radius, g)));
        taken++;
    }
    CHECK(taken > 95000);
}

void affine_box_matches_the_specification() {
    Rng r;
    int taken = 0;
    for (int i = 0; i < 200000; i++) {
        float m[4][4], ce[6];
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) m[c][k] = r.pick(-3.0f, 3.0f);
        for (int k = 0; k < 3; k++) m[3][k] = r.uniform(-5000.0f, 5000.0f);
        m[0][3] = m[1][3] = m[2][3] = 0.0f;
        m[3][3] = 1.0f;
        for (int k = 0; k < 3; k++) ce[k] = r.uniform(-100.0f, 100.0f);
        for (int k = 3; k < 6; k++) ce[k] = r.uniform(0.0f, 50.0f);
        math::Box got;
        if (!math::affine_box(ce, m, got)) continue;
        CHECK(same(got, ref_affine_box(ce, m)));
        taken++;
    }
    CHECK(taken > 190000);
}

// The C surface reads the guest's layouts: a vertex array placed by its relative offset, a body at its offsets.
void the_c_surface_reads_the_layouts() {
    alignas(16) uint8_t mem[512] = {};
    const float verts[6] = {1.0f, 2.0f, 3.0f, -4.0f, 0.5f, 8.0f};
    std::memcpy(mem + 256, verts, sizeof verts);
    const bridge::HavokRelArray head = {256 - 64, 2};
    std::memcpy(mem + 64, &head, sizeof head);
    math::PreciseTransform t = {};
    t.row[0][0] = t.row[1][1] = t.row[2][2] = 1.0f;
    t.hi[0] = 10.0f;
    const float big[4] = {1e30f, 1e30f, 1e30f, 1e30f};
    math::Box out, want = ref_vertex_set_box(verts, 2, t, big);
    CHECK(hl_math_vertex_set_box(mem + 64, &t, big, &out) == 1 && same(out, want));
    CHECK(out.min[0] <= 6.0f && out.max[0] >= 11.0f && out.min[2] <= 3.0f && out.max[2] >= 8.0f);
    const bridge::HavokRelArray none = {0, 3};
    CHECK(hl_math_vertex_set_box(&none, &t, big, &out) == 0);

    alignas(16) bridge::HavokBodyBox body = {};
    body.convex_radius = 0.05f;
    for (int k = 0; k < 3; k++) body.frame[k][k] = 1.0f, body.frame[k][3] = 0.5f;
    body.frame[3][0] = 3.0f;
    bridge::HavokBoxBias bias = {};
    bias.bias[0] = 0.01f;
    bias.bias[1] = 0.02f;
    CHECK(hl_math_oriented_box_box(&body, &t, &bias, &out) == 1);
    CHECK(same(out, ref_oriented_box_box(t, body.frame, 0.05f, bias.bias)));
    CHECK(out.min[0] < 13.0f - 0.5f && out.max[0] > 13.0f + 0.5f);

    const bridge::CentreExtents ce = {{1.0f, 2.0f, 3.0f}, {0.5f, 0.5f, 0.5f}};
    bridge::AffineColumns m = {};
    m.col[0][0] = m.col[1][1] = m.col[2][2] = m.col[3][3] = 1.0f;
    m.col[3][1] = 100.0f;
    CHECK(hl_math_affine_box(&ce, &m, &out) == 1);
    CHECK(out.min[1] == 101.5f && out.max[1] == 102.5f && out.min[3] == 1.0f && out.max[3] == 1.0f);
}

}   // namespace

int main() {
    fmin_fmax_are_arms();
    outward_steps_are_the_guests();
    the_guard_gives_up_where_x86_and_arm_may_differ();
    fma_is_rounded_once();
    vertex_set_box_matches_the_specification();
    oriented_box_box_matches_the_specification();
    affine_box_matches_the_specification();
    the_c_surface_reads_the_layouts();
    std::puts("math: ok");
    return 0;
}
