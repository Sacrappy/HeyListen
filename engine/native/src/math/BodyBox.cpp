// HeyListen native engine: the per-body box update of the last substep (hl/math/BodyBox.h). Each line is one guest
// operation of 0x817d9d80 (and of 0x80168130, 0x8173e2ac it calls), in the guest's order; lanes the function computes
// but never uses are left out (their values reach nothing the game reads, so they need not be exact).
#include "hl/math/BodyBox.h"

#include <cstring>

#include "hl/math/Exact.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace hl::math {

namespace {

constexpr float kByte = std::bit_cast<float>(0x3b808081u);    // (about 1/255)
constexpr float kSpeed = std::bit_cast<float>(0x4722f8e0u);
constexpr float kRound = std::bit_cast<float>(0x3f7fbe77u);   // (about 0.999: added before the conversions)
constexpr float kCast = std::bit_cast<float>(0x3c33abb1u);

float high_half(uint16_t h) { return f32((uint32_t)h << 16); }

uint16_t unsigned_min_ffff(int32_t v) { return (uint32_t)v < 0xffffu ? (uint16_t)v : 0xffff; }
uint16_t signed_min_ffff(int32_t v) { return (uint16_t)(v < 0xffff ? v : 0xffff); }
uint16_t sqxtun16(int32_t v) { return (uint16_t)(v < 0 ? 0 : v > 0xffff ? 0xffff : v); }

void mark(BodyUpdateOut& o, size_t off, size_t n) { std::memset(o.written + off, 1, n); }

// 0x80168130: rotation rows from a quaternion (x, y, z, w); only the lanes the caller stores are kept
void rows_from_quaternion(Guard& g, const float q[4], float r[3][3]) {
    const float t[4] = {g.add(q[0], q[0]), g.add(q[1], q[1]), g.add(q[2], q[2]), g.add(q[3], q[3])};
    const float xx = g.mul(t[0], q[0]), yx = g.mul(t[1], q[0]), zx = g.mul(t[2], q[0]);
    const float yy = g.mul(t[1], q[1]), zy = g.mul(t[2], q[1]);
    const float xw = g.mul(t[0], q[3]), yw = g.mul(t[1], q[3]), zw = g.mul(t[2], q[3]);
    const float zz = g.mul(q[2], t[2]);
    const float one_zz = g.sub(1.0f, zz);
    r[0][0] = g.sub(one_zz, yy);
    r[0][1] = g.add(yx, zw);
    r[0][2] = g.sub(zx, yw);
    r[1][0] = g.sub(yx, zw);
    r[1][1] = g.sub(one_zz, xx);
    r[1][2] = g.add(zy, xw);
    r[2][0] = g.add(zx, yw);
    r[2][1] = g.sub(zy, xw);
    r[2][2] = g.sub(g.sub(1.0f, xx), yy);
}

// all of lanes 0..2
template <class F> bool all3(F f) { return f(0) && f(1) && f(2); }

}   // namespace

bool body_box_update(const BodyUpdateIn& in, BodyUpdateOut& out, BodyDecline& why) {
    Guard g;
    out.body = in.body;
    std::memset(out.written, 0, sizeof out.written);
    out.append1 = out.append2 = false;
    Body& b = out.body;
    auto decline = [&](BodyDecline w) { why = w; return false; };
    auto values = [&] { return decline(BodyDecline::values); };

    if (!(b.flags & 0x400)) {   // not moving: the quantised speeds cleared
        b.q94 = 0;
        b.q76 = b.q78 = 0;
        mark(out, 94, 2);
        mark(out, 76, 4);
    } else {
        // the quantised speeds: from the motion's +96 pair and the state's +40 .. +60
        const float a = g.in(in.m96[0]);
        const float s40 = g.in(in.s40[0]), s44 = g.in(in.s40[1]), s48 = g.in(in.s40[2]);
        const float s52 = g.in(in.s40[3]), s56 = g.in(in.s40[4]), s60 = g.in(in.s40[5]);
        const float q = g.add(g.mul(s56, s56), g.mul(s60, s60));
        const float a0 = g.mul(s40, a), a1 = g.mul(s44, in.m96[1]), a2 = g.mul(s48, a);
        g.in(in.m96[1]);
        const float l = g.add(g.add(g.mul(a0, a0), g.mul(a1, a1)), g.mul(a2, a2));
        const float w = g.add(g.add(g.mul(s52, s52), q), g.in(in.g640[0]));
        const float len = g.sqrt(l);
        const float wa = g.sqrt(w);
        const float mn = fmin(len, g.in(in.g96[0]));
        const float aw = g.mul(a, wa);
        float sc = g.mul((float)b.b156, kByte);
        sc = g.add(sc, 0.0f);
        sc = g.mul(sc, g.in(high_half(b.scale154)));
        const float v4 = g.mul(mn, sc);
        float s1 = g.mul(kSpeed, mn);
        float v0 = g.add(v4, aw);
        const float m64 = g.in(in.m64[0]);
        float s6 = g.mul(m64, v4);
        s1 = g.add(s1, kRound);
        v0 = g.mul(m64, v0);
        s6 = g.add(s6, kRound);
        v0 = g.add(v0, kRound);
        if (!g.ok) return values();
        b.q78 = signed_min_ffff(cvtzs(s6));
        b.q94 = unsigned_min_ffff(cvtzs(s1));
        b.q76 = unsigned_min_ffff(cvtzs(v0));
        mark(out, 76, 4);
        mark(out, 94, 2);

        // the orientation: the motion's quaternion times the conjugate of the body's own, then rows
        float m[4], c[4];
        for (int k = 0; k < 4; k++) {
            m[k] = g.in(in.orient[k]);
            c[k] = g.in(b.orient[k]);
        }
        for (int k = 0; k < 3; k++) c[k] = f32(bits(c[k]) ^ 0x80000000u);
        const float m1[4] = {m[0], m[2], m[3], m[1]}, m2[4] = {m[3], m[1], m[0], m[2]};
        const float c1[4] = {c[0], c[3], c[1], c[2]}, c2[4] = {c[3], c[0], c[2], c[1]};
        float d[4], e[4];
        for (int k = 0; k < 4; k++) {
            const float p5 = g.mul(m1[k], c1[k]);
            const float p4 = g.mul(m2[k], c2[k]);
            const float p0 = g.mul(m1[k], c2[k]);
            const float p1 = g.mul(m2[k], c1[k]);
            d[k] = g.sub(p4, p5);
            e[k] = g.add(p1, p0);
        }
        const float dp[4] = {d[3], d[1], d[0], d[2]};
        float qn[4];
        for (int k = 0; k < 4; k++) qn[k] = g.sub(dp[k], e[k]);
        for (int k = 0; k < 3; k++) qn[k] = f32(bits(qn[k]) ^ 0x80000000u);
        float r[3][3];
        rows_from_quaternion(g, qn, r);
        // the centre of mass rotated (the rows' w lanes hold it locally), the precise position from the state's
        const float cx = g.in(b.xf.row[0][3]), cy = g.in(b.xf.row[1][3]), cz = g.in(b.xf.row[2][3]);
        float cm[3];
        for (int k = 0; k < 3; k++) cm[k] = g.add(g.mul(r[2][k], cz), g.add(g.mul(r[0][k], cx), g.mul(r[1][k], cy)));
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) b.xf.row[j][k] = r[j][k];
        for (int k = 0; k < 3; k++) {
            const double p = g.sub(g.in(in.com[k]), (double)cm[k]);
            const float hi = g.narrow(p);
            b.xf.hi[k] = hi;
            b.xf.lo[k] = g.narrow(g.sub(p, (double)hi));
        }
        if (!g.ok) return values();
        mark(out, 0, 12);
        mark(out, 16, 12);
        mark(out, 32, 12);
        mark(out, 48, 12);
        mark(out, 64, 12);
    }

    // the margin pair: max(max(p - m48 g336, m48 g336), r76) with p the position's w lane
    float f[2];
    const float hw = g.in(b.xf.hi[3]);
    for (int k = 0; k < 2; k++) {
        const float t = g.mul(g.in(in.m48[k]), g.in(in.g336[k]));
        f[k] = fmax(fmax(g.sub(hw, t), t), g.in(in.r76));
    }

    // the shape's box under the body's transform: min, max
    Box box;
    if (!(b.flags140 & 8)) {
        if (in.shape_kind == 1) {   // 0x8173e2ac: the vertex set's box, then the convex radius
            if (!vertex_set_box(in.vertices, in.vertex_count, b.xf, in.vertex_start, box)) return values();
            const float rr = g.mul(g.in(in.convex_radius), g.in(high_half(in.radius_scale)));
            for (int k = 0; k < 4; k++) {
                box.max[k] = g.add(box.max[k], rr);
                box.min[k] = g.sub(box.min[k], rr);
            }
        } else if (in.shape_kind == 2) {   // 0x81a040bc
            float frame[4][4];
            std::memcpy(frame, in.box_frame, sizeof frame);
            const float bias[2] = {in.g80[0], in.g80[1]};
            if (!oriented_box_box(b.xf, frame, in.box_radius, bias, box)) return values();
        } else {
            return decline(BodyDecline::shape);
        }
    } else {   // inline: the quantised local box, rotated, around the precise position plus the rotated centre
        const float s154 = g.in(high_half(b.scale154));
        float e[3];
        for (int k = 0; k < 3; k++) e[k] = g.mul(g.mul(g.in(in.g512[k & 1]), s154), (float)b.ext157[k]);
        float row[3][4];
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 4; k++) row[j][k] = g.in(b.xf.row[j][k]);
        const double lo[4] = {(double)g.in(b.xf.lo[0]), (double)g.in(b.xf.lo[1]), (double)g.in(b.xf.lo[2]),
                              (double)b.xf.lo[2]};
        for (int k = 0; k < 4; k++) {
            const float a1 = fabs_bits(g.mul(row[1][k], e[1]));
            const float a0 = fabs_bits(g.mul(row[0][k], e[0]));
            const float a2 = fabs_bits(g.mul(row[2][k], e[2]));
            float ext = g.add(a2, g.add(a0, a1));
            if ((int32_t)bits(ext) > (int32_t)bits(s154)) ext = s154;
            const float cl = g.add(g.mul(row[2][k], row[2][3]), g.add(g.mul(row[0][k], row[0][3]), g.mul(row[1][k], row[1][3])));
            const double p = g.add(g.add((double)g.in(b.xf.hi[k]), lo[k]), (double)cl);
            box.min[k] = g.narrow(outward_min_bits(g.add(p, (double)f32(bits(ext) ^ 0x80000000u))));
            box.max[k] = g.narrow(outward_max_bits(g.add(p, (double)ext)));
        }
    }
    if (!g.ok) return values();

    // grown by the margin, then by the motion
    const float f4[4] = {f[0], f[1], f[0], f[1]};
    float up[4], dn[4];
    for (int k = 0; k < 4; k++) {
        up[k] = g.add(f4[k], box.max[k]);
        dn[k] = g.sub(box.min[k], f4[k]);
    }
    float sv[12];
    for (int k = 0; k < 12; k++) sv[k] = g.in(in.s40[k]);
    const float lin[3] = {sv[6], sv[7], sv[8]};          // state +64, +68, +72
    const float ang[3] = {sv[10], sv[11], sv[9]};        // state +80, +84, +76 (the guest's lane order)
    const bool still = all3([&](int k) { return lin[k] == 0.0f && ang[k] == 0.0f; });
    float v8 = 0.0f;
    const float m64 = g.in(in.m64[0]);
    if (!still) {
        const float lin2 = g.add(g.mul(lin[2], lin[2]), g.add(g.mul(lin[0], lin[0]), g.mul(lin[1], lin[1])));
        const float ang2 = g.add(g.mul(ang[2], ang[2]), g.add(g.mul(ang[0], ang[0]), g.mul(ang[1], ang[1])));
        const float la = g.sqrt(ang2);
        const float m120[2] = {g.in(in.m120[0]), g.in(in.m120[1])};
        float add2[2];
        if (lin2 > 0.0f) {   // the motion's box, bounded by the box around the centre of mass it can reach
            const float ll = g.sqrt(lin2);
            float sc = g.mul((float)b.b156, kByte);
            sc = g.add(sc, 0.0f);
            sc = g.mul(sc, g.in(high_half(b.scale154)));
            const double reach = (double)g.add(f[0], high_half(b.scale154));
            float pad[2];
            for (int k = 0; k < 2; k++) pad[k] = g.mul(fmin(g.mul(m120[k], ll), g.in(in.g96[k])), sc);
            const double com[4] = {g.in(in.com[0]), g.in(in.com[1]), g.in(in.com[2]), 0.0};
            for (int k = 0; k < 4; k++) {
                const float lo = g.narrow(outward_min_bits(g.sub(com[k], reach)));
                const float hi = g.narrow(outward_max_bits(k < 3 ? g.add(com[k], reach) : g.add(reach, 0.0)));
                up[k] = fmin(g.add(up[k], pad[k & 1]), hi);
                dn[k] = fmax(g.sub(dn[k], pad[k & 1]), lo);
            }
            add2[0] = pad[0];
            add2[1] = pad[1];
        } else {
            add2[0] = g.in(in.g80[0]);
            add2[1] = g.in(in.g80[1]);
        }
        v8 = g.mul(m120[0], la);
        for (int k = 0; k < 2; k++) f[k] = g.add(f[k], add2[k]);
        const float v6[4] = {g.mul(m120[0], ang[0]), g.mul(m120[1], ang[1]), g.mul(m120[0], ang[2]), g.mul(m120[1], ang[1])};
        if ((in.r32 & 0x380) == 0x80) {
            const float t = g.add(g.mul(v6[2], v6[2]), g.add(g.mul(v6[0], v6[0]), g.mul(v6[1], v6[1])));
            const float s = g.sqrt(t);
            for (int k = 0; k < 4; k++) {
                const float gk = g.in(in.g336[k & 1]);
                const float v1 = g.mul(gk, v6[k]);
                const float a0 = g.mul(gk, s);
                const float lo = g.sub(v1, a0), hi = g.add(v1, a0);
                up[k] = g.add(up[k], hi);
                dn[k] = g.add(dn[k], lo);
            }
        } else if ((in.r32 & 0x20000) && lin2 > g.mul(kCast, g.mul(g.in(in.m104[0]), in.m104[0]))) {
            return decline(BodyDecline::cast);
        } else {
            for (int k = 0; k < 4; k++) {
                const float hi = g.add(v6[k], up[k]);
                const float lo = g.add(dn[k], v6[k]);
                up[k] = fmax(up[k], hi);
                dn[k] = fmin(dn[k], lo);
            }
        }
    }
    const float z0 = g.mul(g.add(v8, f[0]), m64);
    const float z1 = g.mul(f[0], m64);
    if (!g.ok) return values();
    b.q148 = sqxtun16(cvtzs(z0));
    b.q150 = sqxtun16(cvtzs(z1));
    mark(out, 148, 4);

    float om[3], oM[3];
    for (int k = 0; k < 3; k++) oM[k] = g.in(b.box_hi[k]);
    om[0] = g.in(b.box_lo[0]);
    om[1] = g.in(b.box_lo[1]);
    om[2] = g.in(b.box_hi[3]);
    float pm[3], pM[3];
    for (int k = 0; k < 3; k++) {
        pm[k] = g.in(in.prev[k]);
        pM[k] = g.in(in.prev[4 + k]);
    }
    if (!g.ok) return values();

    const float m80 = g.in(in.m80);
    if (!still && m80 > 0.0f) {   // slow enough: kept inside the stored box, or grown by +80
        const float t[3] = {g.mul(g.in(in.m104[0]), m80), g.mul(g.in(in.m104[1]), m80), g.mul(in.m104[0], m80)};
        const float sp[3] = {fabs_bits(sv[4]), fabs_bits(sv[5]), fabs_bits(sv[3])};   // state +56, +60, +52
        if (!g.ok) return values();
        if (all3([&](int k) { return t[k] > sp[k] && t[k] > fabs_bits(ang[k]); })) {
            const bool inside = all3([&](int k) { return oM[k] >= up[k] && dn[k] >= om[k]; });
            if (inside && all3([&](int k) { return pM[k] >= up[k] && dn[k] >= pm[k]; })) {
                why = BodyDecline::none;
                return true;
            }
            for (int k = 0; k < 4; k++) {
                up[k] = g.add(up[k], m80);
                dn[k] = g.sub(dn[k], m80);
            }
            if (!g.ok) return values();
        }
    }

    if (!in.has_ctx) return decline(BodyDecline::no_ctx);
    uint32_t h1 = in.header1;
    const bool held = all3([&](int k) { return oM[k] >= box.max[k] && box.min[k] >= om[k]; });
    if (!held) {
        if ((h1 & 0xffff) >= 4048) return decline(BodyDecline::block_full);
        out.append1 = true;
        out.add1.at = 32 + (h1 & 0xffff);
        h1 = h1 + 0x10000 + 16;
        out.add1.header = h1;
        const uint64_t tag = 0x0000000403000010ull;
        std::memcpy(out.add1.rec, &tag, 8);
        std::memcpy(out.add1.rec + 8, &b.id, 4);
        out.add1.len = 12;
    }
    const bool overlaps = all3([&](int k) { return pM[k] >= dn[k] && up[k] >= pm[k]; });
    if (!overlaps && in.has_stream2) {
        const uint32_t h2 = in.same_block && out.append1 ? h1 : in.header2;
        if ((h2 & 0xffff) >= 4048) return decline(BodyDecline::block_full);
        out.append2 = true;
        out.add2.at = 32 + (h2 & 0xffff);
        out.add2.header = h2 + 0x10000 + 16;
        const uint64_t tag = 0x0000000d04000010ull;
        std::memcpy(out.add2.rec, &tag, 8);
        std::memcpy(out.add2.rec + 8, &b.at176, 8);
        out.add2.len = 16;
    }
    up[3] = dn[2];
    if (still && all3([&](int k) { return oM[k] == up[k] && (k < 2 ? b.box_lo[k] : b.box_hi[3]) == dn[k]; })) {
        why = BodyDecline::none;
        return true;
    }
    b.flags |= 0x10;
    mark(out, 92, 2);
    for (int k = 0; k < 4; k++) b.box_hi[k] = up[k];
    b.box_lo[0] = dn[0];
    b.box_lo[1] = dn[1];
    mark(out, 96, 24);
    why = BodyDecline::none;
    return true;
}

}   // namespace hl::math
