// HeyListen native engine: one motion's step of the last substep (hl/math/MotionStep.h). Each line is one guest operation
// of 0x8182dc20 in the guest's order (physics-loops.md 9.3); lanes the function computes but never uses (the fourth lane
// of the V vectors, the second of pairs whose first alone is tested) are left out: they reach nothing the game reads.
#include "hl/math/MotionStep.h"

#include <cstring>

#include "hl/math/Exact.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace hl::math {

namespace {

// the motion's fields (byte offsets)
constexpr int kQuat = 0, kCom = 16, kVel = 40, kInt = 64, kOne = 94, kMod = 100, kDamp = 112, kHalf120 = 120;

float packed15(uint16_t h) { return f32((uint32_t)h << 15); }
float high_half(uint16_t h) { return f32((uint32_t)h << 16); }
// the guest's integer reciprocal-square-root step: 0x5f375a86 - (bits >> 1), 0 when the exponent field is 0
float rs(float x) { return (bits(x) & 0x7f800000u) ? f32(0x5f375a86u - (uint32_t)((int32_t)bits(x) >> 1)) : 0.0f; }

template <class T> T get(const uint8_t* m, int off) {
    T v;
    std::memcpy(&v, m + off, sizeof v);
    return v;
}
template <class T> void put(uint8_t* m, int off, T v) { std::memcpy(m + off, &v, sizeof v); }

// z² + (x² + y²), the guest's order for a 3-vector's square length (an add is commutative: (x² + y²) + z² is the same)
float len2(Guard& g, const float v[3]) { return g.add(g.mul(v[2], v[2]), g.add(g.mul(v[0], v[0]), g.mul(v[1], v[1]))); }

// a clamp (steps 2 and 4): v scaled by (C / sqrt(L)) * max, the pair's first lane for x, z and its second for y
void clamp(Guard& g, float v[3], float l, float max, const float c[4]) {
    float k0 = 0.0f, k1 = 0.0f;
    if (bits(l) & 0x7f800000u) {
        const float r = g.sqrt(l);
        k0 = g.div(c[0], r);
        k1 = g.div(c[1], r);
    }
    const float p0 = g.mul(k0, max), p1 = g.mul(k1, max);
    v[0] = g.mul(v[0], p0);
    v[1] = g.mul(v[1], p1);
    v[2] = g.mul(v[2], p0);
}

}   // namespace

bool motion_step(const MotionStepIn& in, MotionStepOut& out, MotionDecline& why) {
    Guard g;
    std::memcpy(out.motion, in.motion, sizeof out.motion);
    out.clamp_v = out.clamp_w = out.moving = out.still_record = false;
    out.n_path = 0;
    out.body_flag = 0;
    uint8_t* m = out.motion;
    auto values = [&] { why = MotionDecline::values; return false; };

    float c[4];
    for (int k = 0; k < 4; k++) c[k] = g.in(in.c[k]);
    // 1. the differences, the maxima squared
    float a[6], b[6];
    for (int k = 0; k < 6; k++) {
        a[k] = g.in(in.a[k]);
        b[k] = g.in(in.b[k]);
    }
    float dv[3] = {g.sub(a[0], b[0]), g.sub(a[1], b[1]), g.sub(a[2], b[2])};
    float dw[3] = {g.sub(a[4], b[4]), g.sub(a[5], b[5]), g.sub(a[3], b[3])};
    const float max_w = g.in(packed15(in.max_w)), max_v = g.in(packed15(in.max_v));
    const float mw = g.mul(max_w, max_w), mv = g.mul(max_v, max_v);
    const float lv = len2(g, dv);
    if (!g.ok) return values();

    // 2. V clamped
    if (!(mv >= lv)) {
        clamp(g, dv, lv, max_v, c);
        out.clamp_v = true;
    }
    // 3. the n path
    const float lw = len2(g, dw);
    if (const uint32_t n = in.b28 & 31) {
        out.n_path = 1;
        const float nf = (float)n;
        const float e = g.mul(g.mul(nf, f32(0x3a800000u)), nf);
        const float s3 = g.mul(e, g.in(in.d32));
        float t0 = s3, t1 = s3, test = g.mul(s3, s3);
        if (in.b28 & 0x40) {
            const float hs = high_half(get<uint16_t>(in.motion, kHalf120));
            if (hs == 1.0f) {
            } else if (hs == 0.0f) {
                t0 = g.in(in.g50[0]);
                t1 = g.in(in.g50[1]);
                test = g.mul(t0, t0);
            } else {
                const float mm = fabs_bits(g.in(hs));
                const float ms = g.mul(mm, s3);
                const float r = rs(mm);
                t0 = t1 = g.mul(ms, r);
                test = g.mul(ms, ms);
            }
        }
        if (!g.ok) return values();
        if (!(lw >= test)) {
            const float u0 = g.mul(g.in(in.d144[0]), t0), u1 = g.mul(g.in(in.d144[1]), t1);
            const float e2 = g.add(g.mul(e, g.mul(e, 0.75f)), 0.25f);
            const float uu = g.mul(u0, u0);
            if (!g.ok) return values();
            if (g.mul(lw, e2) > uu) {
                const float r = rs(lw);
                const float f0 = fmax(g.in(in.g50[0]), g.sub(c[0], g.mul(r, u0)));
                const float f1 = fmax(g.in(in.g50[1]), g.sub(c[1], g.mul(r, u1)));
                dw[0] = g.mul(dw[0], f0);
                dw[1] = g.mul(dw[1], f1);
                dw[2] = g.mul(dw[2], f0);
                out.n_path = 2;
            } else {
                out.n_path = 3;
                if (uu > g.mul(len2(g, dv), e2)) {
                    dv[0] = dv[1] = dv[2] = 0.0f;
                    out.n_path = 4;
                }
                dw[0] = dw[1] = dw[2] = 0.0f;
            }
        }
    }
    // 4. W clamped (against the square length from before step 3)
    float cw[3] = {dw[0], dw[1], dw[2]};
    if (!(mw >= lw)) {
        clamp(g, cw, lw, max_w, c);
        out.clamp_w = true;
    }
    if (!g.ok) return values();
    if (in.a24 && (get<uint16_t>(in.motion, kMod) & 0x7fff)) {
        why = MotionDecline::modifier;
        return false;
    }

    // 5. the new velocities: lanes x, y, z of W are B's floats 4, 5, 3
    const float bv[3] = {b[0], b[1], b[2]}, bw[3] = {b[4], b[5], b[3]};
    const float s = g.in(in.d136);
    float nv[3], nw[3];
    for (int k = 0; k < 3; k++) {
        const float q0 = g.in(in.q0[k]), m1 = g.mul(g.in(in.q1[k]), s);
        nw[k] = g.mul(g.add(bw[k], g.mul(cw[k], q0)), m1);
        nv[k] = g.mul(g.add(g.mul(dv[k], q0), bv[k]), m1);
    }
    // 6. damping: lanes 2 and 3 of C - FMIN(t * d, C)
    const float t[2] = {g.in(in.d96[0]), g.in(in.d96[1])};
    const float d2 = g.in(high_half(get<uint16_t>(in.motion, kDamp + 4)));
    const float d3 = g.in(high_half(get<uint16_t>(in.motion, kDamp + 6)));
    const float f2 = g.sub(c[2], fmin(g.mul(t[0], d2), c[2]));
    const float f3 = g.sub(c[3], fmin(g.mul(t[1], d3), c[3]));
    float iw[3], iv[3];
    for (int k = 0; k < 3; k++) {
        iw[k] = g.mul(cw[k], f2);
        iv[k] = g.mul(dv[k], f3);
    }
    if (in.b28 & 0x20) {
        out.body_flag = 0x1000;
        const float liw = len2(g, iw), liv = len2(g, iv), lnw = len2(g, nw), lnv = len2(g, nv);
        if (!g.ok) return values();
        if (!(lnw > liw))
            for (int k = 0; k < 3; k++) iw[k] = nw[k];
        if (!(lnv > liv))
            for (int k = 0; k < 3; k++) iv[k] = nv[k];
    }
    if (!g.ok) return values();
    // (the previous W, read before the stores: the deactivation's zero case)
    const float pw[3] = {get<float>(in.motion, kVel + 16), get<float>(in.motion, kVel + 20), get<float>(in.motion, kVel + 12)};
    out.still_record = pw[0] == 0.0f && pw[1] == 0.0f && pw[2] == 0.0f && dw[0] == 0.0f && dw[1] == 0.0f && dw[2] == 0.0f;

    // 8. stores
    for (int k = 0; k < 3; k++) {
        put(m, kInt + 4 * k, iv[k]);
        put(m, kVel + 4 * k, nv[k]);
    }
    put(m, kInt + 12, iw[2]);
    put(m, kInt + 16, iw[0]);
    put(m, kInt + 20, iw[1]);
    put(m, kVel + 12, nw[2]);
    put(m, kVel + 16, nw[0]);
    put(m, kVel + 20, nw[1]);
    put<uint16_t>(m, kOne, 0x3f80);
    for (int k = 0; k < 3; k++)
        if (iw[k] != 0.0f || iv[k] != 0.0f || nw[k] != 0.0f || nv[k] != 0.0f) out.moving = true;

    // 9. integration
    double com[3];
    for (int k = 0; k < 3; k++) com[k] = g.in(get<double>(in.motion, kCom + 8 * k));
    if (out.moving) {
        const float h[2] = {g.in(in.h[0]), g.in(in.h[1])};
        float av[3], x[3];
        for (int k = 0; k < 3; k++) {
            av[k] = g.mul(g.mul(nv[k], t[k & 1]), h[k & 1]);
            x[k] = g.mul(nw[k], t[k & 1]);
        }
        const float s2 = g.add(g.add(g.mul(av[0], av[0]), g.mul(av[1], av[1])), g.mul(av[2], av[2]));
        float kq[2];
        for (int l = 0; l < 2; l++)
            kq[l] = g.add(c[l], g.mul(s2, g.add(g.in(in.c1[l]),
                                                g.mul(s2, g.add(g.in(in.c2[l]), g.mul(g.in(in.c3[l]), s2))))));
        float q[4];
        for (int k = 0; k < 4; k++) q[k] = g.in(get<float>(in.motion, kQuat + 4 * k));
        const float cr[3] = {g.sub(g.mul(av[2], q[1]), g.mul(av[1], q[2])), g.sub(g.mul(av[0], q[2]), g.mul(av[2], q[0])),
                             g.sub(g.mul(av[1], q[0]), g.mul(av[0], q[1]))};
        float qn[4];
        for (int k = 0; k < 3; k++) qn[k] = g.add(g.add(g.mul(av[k], q[3]), cr[k]), g.mul(q[k], kq[k & 1]));
        const float dot = g.add(g.add(g.mul(av[0], q[0]), g.mul(av[1], q[1])), g.mul(av[2], q[2]));
        qn[3] = g.sub(g.mul(kq[0], q[3]), dot);
        const float nn = g.add(g.add(g.mul(qn[0], qn[0]), g.mul(qn[1], qn[1])), g.add(g.mul(qn[2], qn[2]), g.mul(qn[3], qn[3])));
        const float r = g.sqrt(nn);
        for (int k = 0; k < 3; k++) com[k] = g.add(com[k], (double)x[k]);
        for (int k = 0; k < 4; k++) qn[k] = g.div(qn[k], r);
        if (!g.ok) return values();
        for (int k = 0; k < 3; k++) put(m, kCom + 8 * k, com[k]);
        for (int k = 0; k < 4; k++) put(m, kQuat + 4 * k, qn[k]);
    }
    // 11. the cell position and the accumulators' lanes
    for (int k = 0; k < 3; k++) {
        out.cell_pos[k] = g.narrow(com[k]);
        out.acc[k] = cvtzs(g.mul(out.cell_pos[k], 128.0f));
    }
    out.cell_pos[3] = 0.0f;
    out.acc[3] = cvtzs(c[0]);
    if (!g.ok) return values();
    why = MotionDecline::none;
    return true;
}

}   // namespace hl::math
