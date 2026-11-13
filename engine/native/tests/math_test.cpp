// HeyListen native engine: hl::math with no game (tests/test_native_engine.py): the exactness helpers' truth tables, each
// kernel against a reference written straight from its specification (docs/native-engine/physics-loops.md section 6,
// critical-jobs.md), the inputs the kernels must give up on, and the C surface reading the guest's layouts.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>

#include "hl/bridge/MathBridge.h"
#include "hl/c/hl_math.h"
#include "hl/math/Bounds.h"
#include "hl/math/Exact.h"
#include "hl/math/MotionStep.h"

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
    // a midpoint reached exactly in binary64 is rounded as Arm's single rounding (a case seen in the game: w + a b
    // with a b = -1.5 units in w's last place)
    {
        const float w2 = f32(0x30c3f2efu), a2 = f32(0xb3c00000u), b2 = f32(0x31000000u);
        math::Guard k;
        CHECK(bits(k.fma(w2, a2, b2)) == bits(std::fmaf(a2, b2, w2)) && k.ok);
        const double s = (double)a2 * (double)b2 + (double)w2;
        CHECK((bits(s) & 0x1fffffffull) == 0x10000000ull);   // (it is a midpoint)
    }
    // exact midpoints in bulk: values with short mantissas
    uint64_t r = 0x123456789abcdefull;
    int exact = 0;
    for (int i = 0; i < 2000000; i++) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        const float w3 = f32(0x30000000u | ((uint32_t)r & 0x00ffffffu));
        const float a3 = f32(((uint32_t)(r >> 24) & 0x80000000u) | (0x33000000u + ((uint32_t)(r >> 32) & 0x01e00000u)));
        const float b3 = f32(0x31000000u | ((uint32_t)(r >> 40) & 0x00600000u));
        math::Guard k;
        const float t = k.fma(w3, a3, b3);
        if (!k.ok) continue;
        CHECK(bits(t) == bits(std::fmaf(a3, b3, w3)));
        const double s = (double)a3 * (double)b3 + (double)w3;
        if ((bits(s) & 0x1fffffffull) == 0x10000000ull) exact++;
    }
    CHECK(exact > 100);
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

math::SphereRecord ref_model_sphere(const float (*s)[4], const uint16_t* bone, int n, const float (*pal)[4][4],
                                    const uint8_t stale[12], math::SphereRecord rec) {
    for (int i = 0; i < n; i++) {
        const float (*m)[4] = pal[bone[i]];
        float c[4];
        for (int k = 0; k < 4; k++) {
            const float a = m[0][k] * s[i][0];
            const float b = std::fmaf(m[1][k], s[i][1], a);
            const float d = std::fmaf(m[2][k], s[i][2], b);
            c[k] = d + m[3][k];
        }
        float big = 0;
        for (int j = 0; j < 3; j++) {
            const float x = m[j][0] * m[j][0], y = m[j][1] * m[j][1], z = m[j][2] * m[j][2], w = m[j][3] * m[j][3];
            const float zx = z + x, wy = w + y;
            const float len = std::sqrt(zx + wy);
            if (j == 0 || big < len) big = len;
        }
        const float r = s[i][3] * big;
        if (i == 0) {
            std::memcpy(rec.centre, c, 16);
            rec.radius = r;
            continue;
        }
        float d[4], d2 = 0;
        for (int k = 0; k < 4; k++) d[k] = c[k] - rec.centre[k];
        const float z2 = d[2] * d[2], x2 = d[0] * d[0], w2 = d[3] * d[3], y2 = d[1] * d[1];
        const float a = z2 + x2, b = w2 + y2;
        d2 = a + b;
        const float gap0 = r - rec.radius;
        float gap = gap0 * gap0;
        if (gap < 1e-4f) gap = 1e-4f;
        if (d2 < gap) {
            if (rec.radius <= r) {
                std::memcpy(rec.centre, c, 16);
                rec.radius = r;
                std::memcpy(rec.tail, stale, 12);
            }
            continue;
        }
        const float inv = 1.0f / std::sqrt(d2);
        const float q = d2 * inv;
        const float both = r + rec.radius;
        const float sum = q + both;
        const float grown = sum * 0.5f;
        const float diff = grown - rec.radius;
        const float t = diff / q;
        rec.radius = grown;
        for (int k = 0; k < 4; k++) {
            const float step = d[k] * t;
            rec.centre[k] = rec.centre[k] + step;
        }
    }
    return rec;
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

void model_sphere_matches_the_specification() {
    Rng r;
    float pal[6][4][4];
    float s[12][4];
    uint16_t bone[12];
    uint8_t stale[12];
    for (int k = 0; k < 12; k++) stale[k] = (uint8_t)(0xa0 + k);
    int taken = 0, kept = 0, grown = 0;
    for (int it = 0; it < 30000; it++) {
        for (auto& m : pal) {
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++) m[c][k] = r.pick(-1.5f, 1.5f);
            for (int k = 0; k < 3; k++) m[3][k] = r.uniform(-30.0f, 30.0f);
            m[0][3] = m[1][3] = m[2][3] = 0.0f;
            m[3][3] = 1.0f;
        }
        const int n = 1 + (int)(r.next() % 12);
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < 3; k++) s[i][k] = r.uniform(-2.0f, 2.0f);
            s[i][3] = r.uniform(0.0f, r.next() % 4 ? 0.5f : 20.0f);
            bone[i] = (uint16_t)(r.next() % 6);
        }
        math::SphereRecord got, start;
        std::memset(&start, 0x5c, sizeof start);
        got = start;
        if (!math::model_sphere(s, bone, n, pal, stale, got)) continue;
        const math::SphereRecord want = ref_model_sphere(s, bone, n, pal, stale, start);
        CHECK(std::memcmp(&got, &want, sizeof got) == 0);
        taken++;
        if (got.tail[0] == stale[0]) kept++;
        if (n > 1) grown++;
    }
    CHECK(taken > 28000 && kept > 100 && grown > 20000);
    float none[1][4] = {{1, 2, 3, 1}};
    math::SphereRecord out = {};
    CHECK(!math::model_sphere(none, bone, 0, pal, stale, out));
}

// a guest memory of 4 KB at guest address 0x10000 for the C surface's reads through the host's callback
struct Arena {
    alignas(16) uint8_t mem[4096] = {};
    static void* ptr(void* user, uint64_t guest) { return static_cast<Arena*>(user)->mem + (guest - 0x10000); }
    hl_guest_host host() {
        hl_guest_host h = {};   // (only the memory callback is used by the surface under test)
        h.user = this;
        h.ptr = ptr;
        return h;
    }
    template <class T> void put(uint64_t at, const T& v) { std::memcpy(mem + (at - 0x10000), &v, sizeof v); }
};

void the_c_surface_reads_the_model_bounds() {
    Arena a;
    const hl_guest_host host = a.host();
    // bounds object at 0x10000 -> descriptor 0x10100, record 0x10200; holder 0x10300 -> palette 0x10400 (2 matrices);
    // spheres 0x10600, bones 0x10700; the caller's stack pointer 0x10f00
    bridge::ModelBounds b = {};
    b.desc = 0x10100;
    b.sphere_out = 0x10200;
    a.put(0x10000, b);
    bridge::ModelBoundsDesc d = {};
    d.bones = 0x10700;
    d.spheres = 0x10600;
    d.bone = 1;
    d.count = 2;
    d.multi = 1;
    a.put(0x10100, d);
    bridge::BonePalette h = {};
    h.matrices = 0x10400;
    a.put(0x10300, h);
    float pal[2][4][4] = {};
    for (int m = 0; m < 2; m++)
        for (int k = 0; k < 4; k++) pal[m][k][k] = 1.0f;
    pal[1][3][0] = 100.0f;   // matrix 1 moves by 100 in x
    a.put(0x10400, pal);
    const float spheres[2][4] = {{0, 0, 0, 1}, {10, 0, 0, 1}};
    a.put(0x10600, spheres);
    const uint16_t bones[2] = {0, 1};
    a.put(0x10700, bones);
    uint8_t out[32];
    std::memset(out, 0x11, sizeof out);
    CHECK(hl_math_model_sphere(&host, 0x10000, 0x10300, 0x10f00, out) == 1);
    math::SphereRecord rec;
    std::memcpy(&rec, out, sizeof rec);
    CHECK(rec.radius > 55.0f && rec.radius < 56.5f && rec.centre[0] > 54.0f && rec.centre[0] < 56.0f && rec.tail[0] == 0x11);
    // single: one sphere under the descriptor's own index
    d.multi = 0;
    a.put(0x10100, d);
    std::memset(out, 0x11, sizeof out);
    CHECK(hl_math_model_sphere(&host, 0x10000, 0x10300, 0x10f00, out) == 1);
    std::memcpy(&rec, out, sizeof rec);
    CHECK(rec.centre[0] == 100.0f && rec.radius == 1.0f && rec.centre[3] == 1.0f);
    // no record: nothing to answer
    b.sphere_out = 0;
    a.put(0x10000, b);
    CHECK(hl_math_model_sphere(&host, 0x10000, 0x10300, 0x10f00, out) == 0);
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

void segment_box_matches_the_specification() {
    Rng r;
    int taken = 0;
    for (int i = 0; i < 100000; i++) {
        const math::PreciseTransform t = random_transform(r);
        float v[6];
        for (float& f : v) f = r.pick(-3.0f, 3.0f);
        const float radius = r.uniform(0.0f, 0.5f);
        math::Box got;
        if (!math::segment_box(v, t, radius, got)) continue;
        const float L[4] = {t.lo[0], t.lo[1], t.lo[2], t.lo[2]};
        for (int k = 0; k < 4; k++) {
            const float a = t.row[0][k] * v[0], b = t.row[1][k] * v[1], e = t.row[2][k] * v[2];
            const float c0 = t.row[0][k] * v[3], c1 = t.row[1][k] * v[4], f = t.row[2][k] * v[5];
            const float ab = a + b, c01 = c0 + c1;
            const float p0 = e + ab, q0 = c01 + f;
            const float pl = p0 + L[k], ql = L[k] + q0;
            const float p = t.hi[k] + pl, q = t.hi[k] + ql;
            const float mn = ref_min(p, q), mx = ref_max(p, q);
            CHECK(bits(got.min[k]) == bits(mn - radius) && bits(got.max[k]) == bits(mx + radius));
        }
        taken++;
    }
    CHECK(taken > 95000);
}

// Output over the inputs: each C surface reads all it needs before writing its box, so an out that overlaps the
// inputs gives what a separate one gives (the translated functions read their inputs before their stores too: the
// check in the game compares every such call).
void outputs_over_inputs() {
    alignas(16) uint8_t mem[256] = {};
    {   // affine: the box written over its centre / extents and the matrix
        bridge::CentreExtents ce = {{1.5f, -2.0f, 3.25f}, {0.5f, 0.75f, 2.0f}};
        bridge::AffineColumns m = {};
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) m.col[c][k] = 0.1f * (float)(c * 4 + k + 1);
        math::Box want;
        CHECK(hl_math_affine_box(&ce, &m, &want) == 1);
        std::memcpy(mem, &ce, sizeof ce);
        CHECK(hl_math_affine_box(mem, &m, mem) == 1 && std::memcmp(mem, &want, 32) == 0);
        std::memcpy(mem, &m, sizeof m);
        CHECK(hl_math_affine_box(&ce, mem, mem) == 1 && std::memcmp(mem, &want, 32) == 0);
    }
    {   // the oriented box and the vertex set: the box written over the transform
        math::PreciseTransform t = {};
        t.row[0][0] = t.row[1][1] = t.row[2][2] = 1.0f;
        t.row[0][1] = 0.25f;
        t.hi[0] = 3.0f, t.hi[1] = -1.0f, t.lo[0] = 1e-5f;
        alignas(16) bridge::HavokBodyBox body = {};
        body.convex_radius = 0.05f;
        for (int k = 0; k < 3; k++) body.frame[k][k] = 1.0f, body.frame[k][3] = 0.5f + (float)k;
        bridge::HavokBoxBias bias = {};
        math::Box want;
        CHECK(hl_math_oriented_box_box(&body, &t, &bias, &want) == 1);
        std::memcpy(mem, &t, sizeof t);
        CHECK(hl_math_oriented_box_box(&body, mem, &bias, mem) == 1 && std::memcmp(mem, &want, 32) == 0);
        const float verts[6] = {1, 2, 3, -1, 0.5f, 2};
        const float big[4] = {1e30f, 1e30f, 1e30f, 1e30f};
        alignas(16) uint8_t arr[64] = {};
        const bridge::HavokRelArray head = {16, 2};
        std::memcpy(arr, &head, 8);
        std::memcpy(arr + 16, verts, sizeof verts);
        CHECK(hl_math_vertex_set_box(arr, &t, big, &want) == 1);
        std::memcpy(mem, &t, sizeof t);
        CHECK(hl_math_vertex_set_box(arr, mem, big, mem) == 1 && std::memcmp(mem, &want, 32) == 0);
    }
}

// ---- u3, 0x817d9d80: the body box update through its C surface, on a guest memory laid out as the game's ----
struct BodyWorld {
    Arena a;
    hl_guest_host host;
    hl_math_body_args args;
    // guest addresses (Arena: 4 KB from 0x10000)
    static constexpr uint64_t kBody = 0x10000, kShape = 0x10100, kMotion = 0x10200, kRecord = 0x10280,
                              kPrev = 0x102f0, kState = 0x10310, kGlobal = 0x10400, kStart = 0x106a0,
                              kBlock = 0x10700, kHolder = 0x10800, kEntries = 0x10900, kWriter = 0x10960,
                              kBlock2 = 0x10b00;
    uint8_t ctx_mem[2048] = {};
    BodyWorld() {
        host = a.host();
        args = {};
        args.ctx = 0x20000;   // (the context lives in ctx_mem: see at())
        args.motion = kMotion;
        args.record = kRecord;
        args.prev = kPrev;
        args.body = kBody;
        args.state = kState;
        args.global = kGlobal;
        args.start = kStart;
        args.shape_kind = 2;
        host.user = this;
        host.ptr = at;
        math::Body b = {};
        b.xf.row[0][0] = b.xf.row[1][1] = b.xf.row[2][2] = 1.0f;
        b.xf.hi[0] = 100.0f;
        b.xf.hi[1] = -20.0f;
        b.xf.hi[2] = 7.0f;
        b.xf.hi[3] = 0.25f;   // (the w lane: the margin's base)
        b.id = 0x1234;
        b.shape = kShape;
        b.scale154 = 0x4000;  // 2.0
        b.b156 = 51;
        b.at176 = 0xfeedfacecafebeefull;
        a.put(kBody, b);
        // an oriented box shape: radius, frame with half extents 1, 2, 3, centre 0
        bridge::HavokBodyBox sh = {};
        sh.convex_radius = 0.05f;
        for (int k = 0; k < 3; k++) sh.frame[k][k] = 1.0f, sh.frame[k][3] = 1.0f + k;
        a.put(kShape, sh);
        bridge::HavokMotion m = {};
        m.f48[0] = 0.5f, m.f48[1] = 0.25f;
        m.f64[0] = 30.0f, m.f64[1] = 30.0f;
        m.f96[0] = 1.0f, m.f96[1] = 1.0f;
        m.f104[0] = 0.5f, m.f104[1] = 0.5f;
        m.f120[0] = 0.016f, m.f120[1] = 0.016f;
        a.put(kMotion, m);
        bridge::HavokSolverRecord r = {};
        r.f76 = 0.01f;
        a.put(kRecord, r);
        const float prev[8] = {500, 500, 500, 0, 600, 600, 600, 0};   // far away: no overlap
        a.put(kPrev, prev);
        bridge::HavokPhysicsGlobals g = {};
        g.f80[0] = 0.02f, g.f80[1] = 0.03f;
        g.f96[0] = g.f96[1] = 10.0f;
        g.f336[0] = 0.1f, g.f336[1] = 0.2f;
        g.f512[0] = g.f512[1] = 0.0625f;
        g.f640[0] = g.f640[1] = 0.5f;
        a.put(kGlobal, g);
        const float big[4] = {f32(0x7f7fffeeu), f32(0x7f7fffeeu), f32(0x7f7fffeeu), f32(0x7f7fffeeu)};
        a.put(kStart, big);
        bridge::HavokMotionState s = {};
        s.orient[3] = 1.0f;
        s.com[0] = 100.0, s.com[1] = -20.0, s.com[2] = 7.0;
        a.put(kState, s);
        const uint32_t header = 0x00020020u;   // two records, 32 bytes used
        a.put(kBlock, header);
        const uint32_t header2 = 0x00000000u;
        a.put(kBlock2, header2);
        put_ctx(1568, kBlock);
        put_ctx(1888, kHolder);
        put_ctx(1896, kEntries);
        const int8_t index = 1;
        a.put(kHolder + 677 - 0x200, index);   // (kHolder + 677 lies past the arena's 4 KB: moved below, see at())
        a.put(kEntries + 80 + 32, kWriter);
        a.put(kWriter, kBlock2);
    }
    void put_ctx(size_t off, uint64_t v) { std::memcpy(ctx_mem + off, &v, 8); }
    static void* at(void* user, uint64_t guest) {
        BodyWorld* w = static_cast<BodyWorld*>(user);
        if (guest >= 0x20000) return w->ctx_mem + (guest - 0x20000);
        if (guest == kHolder + 677) return w->a.mem + (kHolder + 677 - 0x200 - 0x10000);
        return w->a.mem + (guest - 0x10000);
    }
    math::Body body() const { math::Body b; std::memcpy(&b, a.mem + (kBody - 0x10000), sizeof b); return b; }
    template <class T> T get(uint64_t at) const { T v; std::memcpy(&v, a.mem + (at - 0x10000), sizeof v); return v; }
};

void body_box_still_box_shape() {
    BodyWorld w;
    hl_math_body_plan plan;
    CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 1);
    math::Body after;
    std::memcpy(&after, plan.body, sizeof after);
    const math::Body before = w.body();
    // not moving: the quantised speeds cleared, the transform kept
    CHECK(after.q76 == 0 && after.q78 == 0 && after.q94 == 0 && plan.written[76] && plan.written[94]);
    CHECK(std::memcmp(&after.xf, &before.xf, sizeof after.xf) == 0 && !plan.written[0]);
    // the box: the oriented box's (u2) grown by the margin max(max(0.25 - m48 g336, m48 g336), r76)
    bridge::HavokBodyBox sh = w.get<bridge::HavokBodyBox>(BodyWorld::kShape);
    const float bias[2] = {0.02f, 0.03f};
    const math::Box box = ref_oriented_box_box(before.xf, sh.frame, sh.convex_radius, bias);
    float f[2];
    const float m48[2] = {0.5f, 0.25f}, g336[2] = {0.1f, 0.2f};
    for (int k = 0; k < 2; k++) {
        const float t = m48[k] * g336[k];
        const float d = 0.25f - t;
        f[k] = std::fmax(std::fmax(d, t), 0.01f);
    }
    for (int k = 0; k < 3; k++) CHECK(after.box_hi[k] == f[k & 1] + box.max[k]);
    CHECK(after.box_hi[3] == box.min[2] - f[0] && after.box_lo[0] == box.min[0] - f[0] && after.box_lo[1] == box.min[1] - f[1]);
    CHECK((after.flags & 0x10) && plan.written[92] && plan.written[96] && plan.written[119] && !plan.written[120]);
    // B148 / B150: the margin times m64, toward zero, saturated to 16 bits
    CHECK(after.q148 == (uint16_t)(int)((0.0f + f[0]) * 30.0f) && after.q150 == (uint16_t)(int)(f[0] * 30.0f));
    // both appends: the old box (zero) does not hold the new one; the prev box is far away
    CHECK(plan.appends == 2);
    CHECK(plan.add[0].block == BodyWorld::kBlock && plan.add[0].at == 32 + 0x20 && plan.add[0].header == 0x00030030u);
    uint32_t id;
    std::memcpy(&id, plan.add[0].rec + 8, 4);
    CHECK(id == 0x1234 && plan.add[0].len == 12 && plan.add[0].rec[0] == 0x10 && plan.add[0].rec[3] == 0x03);
    uint64_t tail;
    std::memcpy(&tail, plan.add[1].rec + 8, 8);
    CHECK(plan.add[1].block == BodyWorld::kBlock2 && plan.add[1].at == 32 && plan.add[1].header == 0x00010010u &&
          tail == 0xfeedfacecafebeefull && plan.add[1].len == 16);
    // the guest's memory is not written by the plan
    CHECK(std::memcmp(&before, w.a.mem, sizeof before) == 0);

    // the same box stored already, still: nothing stored, no flag
    math::Body same = after;
    same.flags = before.flags;
    w.a.put(BodyWorld::kBody, same);
    CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 1);
    CHECK(!plan.written[92] && !plan.written[96]);
    // and inside the prev box: no second append
    const float around[8] = {-1000, -1000, -1000, 0, 1000, 1000, 1000, 0};
    w.a.put(BodyWorld::kPrev, around);
    CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 1 && plan.appends == 0);
}

void body_box_declines() {
    {   // a shape whose box function is not native
        BodyWorld w;
        w.args.shape_kind = 0;
        hl_math_body_plan plan;
        CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 0 && plan.decline == (int)math::BodyDecline::shape);
    }
    {   // no thread context
        BodyWorld w;
        w.args.ctx = 0;
        hl_math_body_plan plan;
        CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 0 && plan.decline == (int)math::BodyDecline::no_ctx);
    }
    {   // the block full
        BodyWorld w;
        const uint32_t full = 0x00fd0fd0u;
        w.a.put(BodyWorld::kBlock, full);
        hl_math_body_plan plan;
        CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 0 && plan.decline == (int)math::BodyDecline::block_full);
    }
    {   // a NaN in the state's velocities
        BodyWorld w;
        bridge::HavokMotionState s = w.get<bridge::HavokMotionState>(BodyWorld::kState);
        s.f40[6] = f32(kQNaN);
        w.a.put(BodyWorld::kState, s);
        hl_math_body_plan plan;
        CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 0 && plan.decline == (int)math::BodyDecline::values);
        uint64_t v[3];
        CHECK(hl_math_last_why(v) == (int)math::Why::in_nan_inf && v[0] == kQNaN);
        CHECK(hl_math_last_why(v) == 0);
    }
    {   // fast enough for the swept cast (record flag bit 17, |v|^2 above the threshold)
        BodyWorld w;
        bridge::HavokMotionState s = w.get<bridge::HavokMotionState>(BodyWorld::kState);
        s.f40[6] = 50.0f;
        w.a.put(BodyWorld::kState, s);
        bridge::HavokSolverRecord r = w.get<bridge::HavokSolverRecord>(BodyWorld::kRecord);
        r.flags = 0x20000;
        w.a.put(BodyWorld::kRecord, r);
        hl_math_body_plan plan;
        CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 0 && plan.decline == (int)math::BodyDecline::cast);
        r.flags = 0;   // without the flag: answered, the box grown along the motion
        w.a.put(BodyWorld::kRecord, r);
        CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 1);
    }
}

// a moving body: the transform rebuilt from the motion's orientation and the body's own, the position from the
// centre of mass in binary64 (high + low parts), the quantised speeds
void body_box_moving() {
    BodyWorld w;
    math::Body b = w.body();
    b.flags = 0x400;
    b.xf.row[0][3] = 0.5f;   // the local centre of mass (the rows' w lanes)
    b.xf.row[1][3] = -0.25f;
    b.xf.row[2][3] = 0.125f;
    b.orient[3] = 1.0f;      // (the body's own orientation: identity)
    w.a.put(BodyWorld::kBody, b);
    bridge::HavokMotionState s = w.get<bridge::HavokMotionState>(BodyWorld::kState);
    const float h = std::sqrt(0.5f);
    s.orient[0] = 0, s.orient[1] = 0, s.orient[2] = h, s.orient[3] = h;   // 90 degrees about z
    s.com[0] = 1000.123456789, s.com[1] = -2.5, s.com[2] = 3.0;
    for (int k = 0; k < 6; k++) s.f40[k] = 0.1f * (float)(k + 1);
    w.a.put(BodyWorld::kState, s);
    hl_math_body_plan plan;
    CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 1);
    math::Body after;
    std::memcpy(&after, plan.body, sizeof after);
    // rows of the quarter turn about z (to rounding), w lanes kept
    CHECK(std::fabs(after.xf.row[0][1] - 1.0f) < 1e-6f && std::fabs(after.xf.row[1][0] + 1.0f) < 1e-6f &&
          std::fabs(after.xf.row[2][2] - 1.0f) < 1e-6f && std::fabs(after.xf.row[0][0]) < 1e-6f);
    CHECK(after.xf.row[0][3] == 0.5f && after.xf.row[1][3] == -0.25f && after.xf.row[2][3] == 0.125f);
    // position = com - R c, as high + low
    for (int k = 0; k < 3; k++) {
        const double c = (double)after.xf.row[0][k] * 0.5 + (double)after.xf.row[1][k] * -0.25 + (double)after.xf.row[2][k] * 0.125;
        const double p = (double)after.xf.hi[k] + (double)after.xf.lo[k];
        CHECK(std::fabs(p - (s.com[k] - c)) < 1e-6);
        CHECK((double)after.xf.hi[k] == (double)(float)(s.com[k] - c) || std::fabs(after.xf.lo[k]) < 1e-3f);
    }
    CHECK(plan.written[0] && plan.written[11] && !plan.written[12] && plan.written[48] && !plan.written[60] &&
          plan.written[72] && !plan.written[76 + 12]);
    // speeds: lane 0 of the guest's sums, plus about 0.999, toward zero
    const float a0 = 0.1f * 1.0f, a1 = 0.2f * 1.0f, a2 = 0.3f * 1.0f;
    const float l = std::sqrt((a0 * a0 + a1 * a1) + a2 * a2);
    const float mn = l < 10.0f ? l : 10.0f;
    float sc = (float)51 * f32(0x3b808081u);
    sc = (sc + 0.0f) * 2.0f;
    const float v4 = mn * sc;
    CHECK(after.q78 == (uint16_t)(int)(30.0f * v4 + f32(0x3f7fbe77u)));
    CHECK(after.q94 == (uint16_t)((int)(f32(0x4722f8e0u) * mn + f32(0x3f7fbe77u)) < 0xffff ? (int)(f32(0x4722f8e0u) * mn + f32(0x3f7fbe77u)) : 0xffff));
}

// the inline path: the quantised local box (bytes times the global scale times the body's), rotated, around the
// precise position plus the rotated centre of mass, rounded outward
void body_box_inline() {
    BodyWorld w;
    math::Body b = w.body();
    b.flags140 = 8;
    b.ext157[0] = 10, b.ext157[1] = 20, b.ext157[2] = 30;
    b.xf.row[0][3] = 0.5f;
    b.xf.lo[0] = 1e-5f;
    w.a.put(BodyWorld::kBody, b);
    w.args.shape_kind = 0;   // (not asked on this path)
    hl_math_body_plan plan;
    CHECK(hl_math_body_box(&w.host, &w.args, &plan) == 1);
    math::Body after;
    std::memcpy(&after, plan.body, sizeof after);
    const float s154 = 2.0f, g = 0.0625f;
    const float e[3] = {g * s154 * 10, g * s154 * 20, g * s154 * 30};
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++) {
        float ext = std::fabs(b.xf.row[2][k] * e[2]) + (std::fabs(b.xf.row[0][k] * e[0]) + std::fabs(b.xf.row[1][k] * e[1]));
        if (ext > s154) ext = s154;
        const float c = b.xf.row[2][k] * b.xf.row[2][3] + (b.xf.row[0][k] * b.xf.row[0][3] + b.xf.row[1][k] * b.xf.row[1][3]);
        const double p = ((double)b.xf.hi[k] + (double)b.xf.lo[k]) + (double)c;
        mn[k] = ref_narrow_min(p + (double)-ext);
        mx[k] = ref_narrow_max(p + (double)ext);
    }
    float f0;
    {
        const float t = 0.5f * 0.1f;
        f0 = std::fmax(std::fmax(0.25f - t, t), 0.01f);
    }
    const float f1 = std::fmax(std::fmax(0.25f - 0.25f * 0.2f, 0.25f * 0.2f), 0.01f);
    const float f[3] = {f0, f1, f0};
    for (int k = 0; k < 3; k++) CHECK(after.box_hi[k] == f[k] + mx[k]);
    CHECK(after.box_lo[0] == mn[0] - f0 && after.box_lo[1] == mn[1] - f1 && after.box_hi[3] == mn[2] - f0);
    CHECK(mx[0] - mn[0] == 2 * s154 || mx[0] - mn[0] > 2 * 1.25f);   // (x clamped to the body's scale: 1.25 < 2)
}

// ---- 0x8182dc20's step of one motion (physics-loops.md 9.3) ----
template <class T> T at_off(const uint8_t* m, int off) {
    T v;
    std::memcpy(&v, m + off, sizeof v);
    return v;
}
// a motion at rest at (10, 20, 30) with the identity orientation; unit factors; maxima of 65536 (B +24 0x8f00)
math::MotionStepIn motion_in() {
    math::MotionStepIn in = {};
    const float q[4] = {0, 0, 0, 1};
    const double com[3] = {10, 20, 30};
    std::memcpy(in.motion, q, 16);
    std::memcpy(in.motion + 16, com, 24);
    in.motion[92] = 0xff;
    in.max_w = in.max_v = 0x8f00;
    in.d32 = 1;
    in.d96[0] = in.d96[1] = 0.5f;
    in.d136 = 1;
    in.d144[0] = in.d144[1] = 1;
    for (int k = 0; k < 4; k++) in.q0[k] = in.q1[k] = in.c[k] = 1;
    in.h[0] = in.h[1] = 0.5f;
    in.c1[0] = in.c1[1] = -0.5f;
    return in;
}

void motion_step_still() {
    const math::MotionStepIn in = motion_in();
    math::MotionStepOut out;
    math::MotionDecline why;
    CHECK(math::motion_step(in, out, why) && why == math::MotionDecline::none);
    CHECK(!out.moving && !out.clamp_v && !out.clamp_w && out.n_path == 0 && out.still_record && out.body_flag == 0);
    CHECK(std::memcmp(out.motion, in.motion, 40) == 0);   // (the orientation and the centre of mass untouched)
    CHECK(at_off<uint16_t>(out.motion, 94) == 0x3f80);
    CHECK(out.cell_pos[0] == 10 && out.cell_pos[1] == 20 && out.cell_pos[2] == 30 && out.cell_pos[3] == 0);
    CHECK(out.acc[0] == 1280 && out.acc[1] == 2560 && out.acc[2] == 3840 && out.acc[3] == 1);
}

void motion_step_moving() {
    math::MotionStepIn in = motion_in();
    in.a[4] = 1, in.a[5] = 2, in.a[3] = 3;   // W = (1, 2, 3): it moves the centre of mass by W t
    math::MotionStepOut out;
    math::MotionDecline why;
    CHECK(math::motion_step(in, out, why));
    CHECK(out.moving && !out.clamp_w && !out.still_record);
    CHECK(at_off<double>(out.motion, 16) == 10.5 && at_off<double>(out.motion, 24) == 21 && at_off<double>(out.motion, 32) == 31.5);
    CHECK(at_off<float>(out.motion, 12) == 1 && at_off<float>(out.motion, 0) == 0);   // (no turn)
    CHECK(at_off<float>(out.motion, 56) == 1 && at_off<float>(out.motion, 60) == 2 && at_off<float>(out.motion, 52) == 3);
    CHECK(at_off<float>(out.motion, 80) == 1 && at_off<float>(out.motion, 84) == 2 && at_off<float>(out.motion, 76) == 3);
    // a turn about z: V = (0, 0, 1) and t h = 1/4 make a = (0, 0, 0.25), s = 1/16, kq = 1 - 1/32
    in = motion_in();
    in.a[2] = 1;
    CHECK(math::motion_step(in, out, why) && out.moving);
    const float kq = 1.0f + (1.0f / 16) * -0.5f, n = std::sqrt(0.25f * 0.25f + kq * kq);
    CHECK(at_off<float>(out.motion, 8) == 0.25f / n && at_off<float>(out.motion, 12) == kq / n);
    // W clamped to 2.5 (B +24 0x8040): (3, 4, 0) is 5 long, so it is halved
    in = motion_in();
    in.a[4] = 3, in.a[5] = 4;
    in.max_w = 0x8040;
    in.a24 = 1;   // (the clamps and the n path only with A's word at +24 set)
    CHECK(math::motion_step(in, out, why) && out.clamp_w && !out.clamp_v);
    CHECK(at_off<float>(out.motion, 56) == 1.5f && at_off<float>(out.motion, 60) == 2);
    in.a24 = 0;
    CHECK(math::motion_step(in, out, why) && !out.clamp_w && at_off<float>(out.motion, 56) == 3);
    // the n path: a slow W under the threshold is zeroed, and V with it
    in = motion_in();
    in.a[4] = 0.001f;
    in.b28 = 4;
    in.a24 = 1;
    CHECK(math::motion_step(in, out, why) && out.n_path == 4 && !out.moving);
}

void motion_step_declines() {
    math::MotionStepIn in = motion_in();
    math::MotionStepOut out;
    math::MotionDecline why;
    in.a[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!math::motion_step(in, out, why) && why == math::MotionDecline::values);
    in = motion_in();
    in.a24 = 1;
    in.motion[100] = 1;
    CHECK(!math::motion_step(in, out, why) && why == math::MotionDecline::modifier);
    in.motion[100] = 0;   // (a modifier word with no modifier bit: answered)
    CHECK(math::motion_step(in, out, why));
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
    model_sphere_matches_the_specification();
    the_c_surface_reads_the_model_bounds();
    segment_box_matches_the_specification();
    outputs_over_inputs();
    body_box_still_box_shape();
    body_box_declines();
    body_box_moving();
    body_box_inline();
    motion_step_still();
    motion_step_moving();
    motion_step_declines();
    std::puts("math: ok");
    return 0;
}
