// HeyListen native engine: the rules under which native x64 floating point gives the same bits as the translated Arm
// code (docs/native-engine/physics-loops.md, section 4). Every kernel of hl::math is written with these and nothing else.
//
// What x86 SSE arithmetic and the translation agree on, bit for bit: a binary32 or binary64 add, subtract or multiply of
// ordinary operands (finite, not subnormal) whose result is ordinary too, each separately rounded to nearest; the
// conversions between the two widths; sign-bit operations. What they do not agree on, or only through care:
// - flush-to-zero (FPCR.FZ): Arm flushes subnormal operands and results, x86 does not unless MXCSR says so, and the two
//   tell a tiny result differently. The kernels run in the host's default MXCSR and give up (Guard) on any subnormal
//   operand, and on any result that is subnormal or close enough to the smallest normal to be told differently, as the
//   translated code's own fast path does (a64_fpx.h's "big" test);
// - NaN: Arm's FMIN / FMAX and its NaN propagation are not x86's. The kernels give up on any NaN or infinity;
// - fused multiply-add: the guest's FMLA rounds once. It is computed through binary64 (the product of two binary32 is
//   exact there), and given up when the binary64 sum lies on a binary32 midpoint, the only case where rounding twice
//   differs from rounding once;
// - FMIN / FMAX: Arm orders -0 below +0 and x86's MINSS / MAXSS do not: fmin / fmax below are Arm's.
// A kernel that gives up returns false having written nothing; its caller then runs the translated code.
//
// No contraction: the game is built with -ffp-contract=off; the pragma keeps it so in any other build of these headers.
#ifndef HL_MATH_EXACT_H
#define HL_MATH_EXACT_H

#include <bit>
#include <cmath>
#include <cstdint>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace hl::math {

inline uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }
inline uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }
inline float f32(uint32_t b) { return std::bit_cast<float>(b); }
inline double f64(uint64_t b) { return std::bit_cast<double>(b); }

inline bool is_zero(float f) { return (bits(f) & 0x7fffffffu) == 0; }
inline bool is_zero(double d) { return (bits(d) & 0x7fffffffffffffffull) == 0; }
// finite and not subnormal (a zero is ordinary)
inline bool ordinary(float f) {
    const uint32_t e = bits(f) & 0x7f800000u;
    return e != 0x7f800000u && (e != 0 || is_zero(f));
}
inline bool ordinary(double d) {
    const uint64_t e = bits(d) & 0x7ff0000000000000ull;
    return e != 0x7ff0000000000000ull && (e != 0 || is_zero(d));
}
// finite and above the smallest normal (the translation's "big" test: a result there is the same under FZ or not)
inline bool big(float f) { return (bits(f) & 0x7fffffffu) - 0x00800001u < 0x7effffffu; }
inline bool big(double d) { return (bits(d) & 0x7fffffffffffffffull) - 0x0010000000000001ull < 0x7fdfffffffffffffull; }

// Accumulates whether every value a kernel used and made is one on which x86 and the translated Arm code agree. Each
// operation is written as one call, so the kernel reads as the guest's sequence of instructions.
struct Guard {
    bool ok = true;

    float in(float a) { ok &= ordinary(a); return a; }
    double in(double a) { ok &= ordinary(a); return a; }
    // FADD / FSUB: an exact zero is fine (an IEEE sum never rounds to zero)
    float add(float a, float b) { const float t = a + b; ok &= big(t) || is_zero(t); return t; }
    float sub(float a, float b) { const float t = a - b; ok &= big(t) || is_zero(t); return t; }
    double add(double a, double b) { const double t = a + b; ok &= big(t) || is_zero(t); return t; }
    double sub(double a, double b) { const double t = a - b; ok &= big(t) || is_zero(t); return t; }
    // FMUL: a zero only as the product of a zero
    float mul(float a, float b) {
        const float t = a * b;
        ok &= big(t) || (is_zero(t) && (is_zero(a) || is_zero(b)));
        return t;
    }
    // FMLA / FMADD: w + a * b rounded once (through binary64; a binary32 midpoint there is given up)
    float fma(float w, float a, float b) {
        const double s = (double)a * (double)b + (double)w;
        ok &= (bits(s) & 0x1fffffffull) != 0x10000000ull;
        const float t = (float)s;
        ok &= big(t) || (is_zero(t) && (is_zero(a) || is_zero(b)) && is_zero(w));
        return t;
    }
    // FDIV: a zero only as the quotient of a zero by a non-zero
    float div(float a, float b) {
        const float t = a / b;
        ok &= big(t) || (is_zero(t) && is_zero(a) && !is_zero(b));
        return t;
    }
    // FSQRT (correctly rounded on both): of a zero or a positive normal
    float sqrt(float a) {
        ok &= is_zero(a) || (big(a) && !(bits(a) & 0x80000000u));
        return std::sqrt(a);
    }
    // FCVT double -> single, to nearest: a zero is fine (Arm with or without FZ gives the same signed zero)
    float narrow(double d) { const float t = (float)d; ok &= big(t) || is_zero(t); return t; }
};

// FABS: the sign bit cleared (exact, any value)
inline float fabs_bits(float a) { return f32(bits(a) & 0x7fffffffu); }

// Arm's FMIN / FMAX of binary32 with FPCR's FZ and DN clear: a signalling NaN operand gives that NaN quieted (the first
// operand's first), else a quiet NaN operand is returned (the first's first); min(+0, -0) = -0 and max(+0, -0) = +0 in
// either order; otherwise the smaller / larger.
inline float fmin(float a, float b) {
    const uint32_t x = bits(a), y = bits(b);
    const bool xn = (x & 0x7fffffffu) > 0x7f800000u, yn = (y & 0x7fffffffu) > 0x7f800000u;
    if (xn || yn) {
        const bool xs = xn && !(x & 0x00400000u), ys = yn && !(y & 0x00400000u);
        if (xs || ys) return f32((xs ? x : y) | 0x00400000u);
        return f32((xn ? x : y) | 0x00400000u);
    }
    if (is_zero(a) && is_zero(b)) return f32(x | y);
    return a < b ? a : b;
}
inline float fmax(float a, float b) {
    const uint32_t x = bits(a), y = bits(b);
    const bool xn = (x & 0x7fffffffu) > 0x7f800000u, yn = (y & 0x7fffffffu) > 0x7f800000u;
    if (xn || yn) {
        const bool xs = xn && !(x & 0x00400000u), ys = yn && !(y & 0x00400000u);
        if (xs || ys) return f32((xs ? x : y) | 0x00400000u);
        return f32((xn ? x : y) | 0x00400000u);
    }
    if (is_zero(a) && is_zero(b)) return f32(x & y);
    return a > b ? a : b;
}

// Havok's outward rounding of a box corner from binary64 to binary32: 2^28 units in the last place of the double is half
// a float's last place, so moving the double's bit pattern by it before rounding to nearest rounds away from the box's
// inside. As the guest selects the step: for a minimum, down when the value compares greater than zero (an unordered
// compare selects that too), else up (both zeros go up); for a maximum, up when it compares greater or equal to zero,
// else down (NaN goes down). Integer steps on the bit pattern, as the guest's, carries into the exponent included.
inline double outward_min_bits(double a) { return f64(bits(a) + (!(a <= 0.0) ? 0xfffffffff0000000ull : 0x10000000ull)); }
inline double outward_max_bits(double a) { return f64(bits(a) + (a >= 0.0 ? 0x10000000ull : 0xfffffffff0000000ull)); }

}   // namespace hl::math

#endif
