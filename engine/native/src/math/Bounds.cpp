// HeyListen native engine: bounding-box kernels (hl/math/Bounds.h). Each line is one guest operation, in the guest's
// order; the comments name the guest function the sequence is read from (by address, nothing of its code).
#include "hl/math/Bounds.h"

#include "hl/math/Exact.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace hl::math {

namespace {

bool inputs_ordinary(Guard& g, const float* v, int n) {
    for (int i = 0; i < n; i++) g.in(v[i]);
    return g.ok;
}

// The binary64 corners of a box rounded outward to binary32 (0x81746440, 0x81a040bc share it).
void outward(Guard& g, const double lo[4], const double hi[4], Box& out) {
    for (int k = 0; k < 4; k++) {
        out.min[k] = g.narrow(outward_min_bits(lo[k]));
        out.max[k] = g.narrow(outward_max_bits(hi[k]));
    }
}

// The binary64 position of a precise transform: high + low per lane, the w lane adding the low z.
void position(Guard& g, const PreciseTransform& t, double p[4]) {
    const float lo[4] = {t.lo[0], t.lo[1], t.lo[2], t.lo[2]};
    for (int k = 0; k < 4; k++) p[k] = g.add((double)t.hi[k], (double)lo[k]);
}

}   // namespace

// 0x81746440
bool vertex_set_box(const float* vertices, int32_t n, const PreciseTransform& t, const float start[4], Box& out) {
    Guard g;
    if (n < 0 || !inputs_ordinary(g, &t.row[0][0], 19) || !inputs_ordinary(g, start, 4)) return false;
    float mn[4], mx[4];
    for (int k = 0; k < 4; k++) {
        mn[k] = start[k];
        mx[k] = f32(bits(start[k]) ^ 0x80000000u);
    }
    for (int32_t i = 0; i < n; i++) {
        const float* p = vertices + 3 * i;
        const float x = g.in(p[0]), y = g.in(p[1]), z = g.in(p[2]);
        for (int k = 0; k < 4; k++) {
            const float a = g.mul(t.row[0][k], x);
            const float b = g.mul(t.row[1][k], y);
            const float c = g.mul(t.row[2][k], z);
            const float v = g.add(c, g.add(a, b));
            mn[k] = fmin(mn[k], v);
            mx[k] = fmax(mx[k], v);
        }
        if (!g.ok) return false;
    }
    double p[4], lo[4], hi[4];
    position(g, t, p);
    for (int k = 0; k < 4; k++) {
        lo[k] = g.add(p[k], (double)mn[k]);
        hi[k] = g.add(p[k], (double)mx[k]);
    }
    Box r;
    outward(g, lo, hi, r);
    if (!g.ok) return false;
    out = r;
    return true;
}

// 0x8174bb00
bool segment_box(const float v[6], const PreciseTransform& t, float radius, Box& out) {
    Guard g;
    g.in(radius);
    if (!inputs_ordinary(g, &t.row[0][0], 19) || !inputs_ordinary(g, v, 6)) return false;
    const float lo[4] = {t.lo[0], t.lo[1], t.lo[2], t.lo[2]};
    Box r;
    for (int k = 0; k < 4; k++) {
        const float a = g.mul(t.row[0][k], v[0]);
        const float b = g.mul(t.row[1][k], v[1]);
        const float c0 = g.mul(t.row[0][k], v[3]);
        const float c1 = g.mul(t.row[1][k], v[4]);
        const float e = g.mul(t.row[2][k], v[2]);
        const float f = g.mul(t.row[2][k], v[5]);
        const float p0 = g.add(e, g.add(a, b));
        const float q0 = g.add(g.add(c0, c1), f);
        const float p = g.add(t.hi[k], g.add(p0, lo[k]));
        const float q = g.add(t.hi[k], g.add(lo[k], q0));
        r.min[k] = g.sub(fmin(p, q), radius);
        r.max[k] = g.add(fmax(p, q), radius);
    }
    if (!g.ok) return false;
    out = r;
    return true;
}

// 0x81a040bc, with 0x8170341c (the composition of the body's frame with A)
bool oriented_box_box(const PreciseTransform& a, const float q[4][4], float radius, const float gx[2], Box& out) {
    Guard g;
    g.in(radius);
    if (!inputs_ordinary(g, &a.row[0][0], 19) || !inputs_ordinary(g, &q[0][0], 16) || !inputs_ordinary(g, gx, 2))
        return false;
    // 0x8170341c: T = A composed with q (rows of q[3], q[2], q[1], q[0], each (A0 qx + A1 qy) + A2 qz), then the
    // precise translation: s = T3 + low, high' = high + s, low' = s - (high' - high).
    PreciseTransform tr;
    float t3[4];
    for (int r = 3; r >= 0; r--) {
        float* d = r == 3 ? t3 : tr.row[r];
        for (int k = 0; k < 4; k++) {
            const float x = g.mul(a.row[0][k], q[r][0]);
            const float y = g.mul(a.row[1][k], q[r][1]);
            const float z = g.mul(a.row[2][k], q[r][2]);
            d[k] = g.add(z, g.add(x, y));
        }
    }
    const float low[4] = {a.lo[0], a.lo[1], a.lo[2], a.lo[2]};
    float s[4];
    for (int k = 0; k < 4; k++) {
        s[k] = g.add(t3[k], low[k]);
        tr.hi[k] = g.add(a.hi[k], s[k]);
    }
    for (int k = 0; k < 3; k++) tr.lo[k] = g.sub(s[k], g.sub(tr.hi[k], a.hi[k]));
    // 0x81a040bc: half extents e = (q0.w, q1.w, q2.w) + radius; the extents (C + |T0 ex|) + (|T1 ey| + |T2 ez|) with
    // C = (g0, g1, g0, g1); the box around the binary64 centre.
    const float ex = g.add(q[0][3], radius), ey = g.add(q[1][3], radius), ez = g.add(q[2][3], radius);
    const float cl[4] = {gx[0], gx[1], gx[0], gx[1]};
    float e[4];
    for (int k = 0; k < 4; k++) {
        const float a1 = fabs_bits(g.mul(tr.row[1][k], ey));
        const float a0 = fabs_bits(g.mul(tr.row[0][k], ex));
        const float a2 = fabs_bits(g.mul(tr.row[2][k], ez));
        e[k] = g.add(g.add(cl[k], a0), g.add(a1, a2));
    }
    double p[4], lo[4], hi[4];
    position(g, tr, p);
    for (int k = 0; k < 4; k++) {
        lo[k] = g.sub(p[k], (double)e[k]);
        hi[k] = g.add(p[k], (double)e[k]);
    }
    Box r;
    outward(g, lo, hi, r);
    if (!g.ok) return false;
    out = r;
    return true;
}

// 0x80ba6d80
bool affine_box(const float ce[6], const float m[4][4], Box& out) {
    Guard g;
    if (!inputs_ordinary(g, ce, 6) || !inputs_ordinary(g, &m[0][0], 16)) return false;
    Box r;
    for (int k = 0; k < 4; k++) {
        float c = g.mul(m[0][k], ce[0]);
        c = g.fma(c, m[1][k], ce[1]);
        c = g.fma(c, m[2][k], ce[2]);
        c = g.add(c, m[3][k]);
        float e = g.mul(fabs_bits(m[0][k]), ce[3]);
        e = g.add(e, g.mul(fabs_bits(m[1][k]), ce[4]));
        e = g.add(e, g.mul(fabs_bits(m[2][k]), ce[5]));
        r.min[k] = g.sub(c, e);
        r.max[k] = g.add(c, e);
    }
    if (!g.ok) return false;
    out = r;
    return true;
}

// 0x800a8400
bool model_sphere(const float (*s)[4], const uint16_t* bone, int n, const float (*palette)[4][4], const uint8_t stale[12],
                  SphereRecord& out) {
    const float kMinGap = f32(0x38d1b717u);   // (1e-4 as the guest builds it)
    Guard g;
    if (n < 1) return false;
    SphereRecord rec = out;
    for (int i = 0; i < n; i++) {
        const float (*m)[4] = palette[bone[i]];
        if (!inputs_ordinary(g, s[i], 4) || !inputs_ordinary(g, &m[0][0], 16)) return false;
        float ci[4];
        for (int k = 0; k < 4; k++) {
            float c = g.mul(m[0][k], s[i][0]);
            c = g.fma(c, m[1][k], s[i][1]);
            c = g.fma(c, m[2][k], s[i][2]);
            ci[k] = g.add(c, m[3][k]);
        }
        float len[3];
        for (int j = 0; j < 3; j++) {
            float sq[4];
            for (int k = 0; k < 4; k++) sq[k] = g.mul(m[j][k], m[j][k]);
            len[j] = g.sqrt(g.add(g.add(sq[2], sq[0]), g.add(sq[3], sq[1])));
        }
        float scale = len[0];
        if (scale < len[1]) scale = len[1];
        if (scale < len[2]) scale = len[2];
        const float ri = g.mul(s[i][3], scale);
        if (!g.ok) return false;
        if (i == 0) {
            for (int k = 0; k < 4; k++) rec.centre[k] = ci[k];
            rec.radius = ri;
            continue;
        }
        const float R = rec.radius;
        float d[4], dd[4];
        for (int k = 0; k < 4; k++) {
            d[k] = g.sub(ci[k], rec.centre[k]);
            dd[k] = g.mul(d[k], d[k]);
        }
        const float d2 = g.add(g.add(dd[2], dd[0]), g.add(dd[3], dd[1]));
        float gap = g.sub(ri, R);
        gap = g.mul(gap, gap);
        if (gap < kMinGap) gap = kMinGap;
        if (!g.ok) return false;
        if (d2 < gap) {   // one inside the other: the larger stays
            if (!(R > ri)) {
                for (int k = 0; k < 4; k++) rec.centre[k] = ci[k];
                rec.radius = ri;
                for (int k = 0; k < 12; k++) rec.tail[k] = stale[k];
            }
            continue;
        }
        const float dist = g.sqrt(d2);
        const float q = g.mul(d2, g.div(1.0f, dist));
        const float grown = g.mul(g.add(q, g.add(ri, R)), 0.5f);
        const float t = g.div(g.sub(grown, R), q);
        rec.radius = grown;
        for (int k = 0; k < 4; k++) rec.centre[k] = g.add(rec.centre[k], g.mul(d[k], t));
        if (!g.ok) return false;
    }
    out = rec;
    return true;
}

}   // namespace hl::math
