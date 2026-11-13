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

// Why a Guard gave up first (the rule and the kind of value), for the counters of what the kernels decline.
enum class Why : uint8_t {
    none,
    in_nan_inf,       // an input NaN or infinite
    in_subnormal,     // an input subnormal
    sum_tiny,         // a sum or difference non-zero below the smallest normal (or at it)
    sum_overflow,     // a sum or difference infinite or NaN
    mul_underflow,    // a product non-zero below the smallest normal, or zero from non-zero operands
    mul_overflow,     // a product infinite or NaN
    fma_midpoint,     // a fused sum on a binary32 midpoint in binary64
    fma_range,        // a fused sum outside the normal range (or zero from non-zero products)
    div_range,        // a quotient outside the normal range
    sqrt_range,       // a square root of a negative, subnormal or non-finite value
    narrow_tiny,      // a binary64 narrowed to a non-zero binary32 at or below the smallest normal
    narrow_overflow,  // a binary64 narrowed to an infinite or NaN binary32
    count
};

// The calling thread's last reason a Guard gave up (set by the first failure of each Guard; read and cleared by
// the OS layer's counters).
inline thread_local Why last_why = Why::none;
// ... and the operands and result of that operation (bits; binary64 values in full)
inline thread_local uint64_t last_why_values[3] = {};

// Accumulates whether every value a kernel used and made is one on which x86 and the translated Arm code agree. Each
// operation is written as one call, so the kernel reads as the guest's sequence of instructions.
struct Guard {
    bool ok = true;

    void fail(Why w, uint64_t a, uint64_t b, uint64_t r) {
        if (!ok) return;
        ok = false;
        last_why = w;
        last_why_values[0] = a;
        last_why_values[1] = b;
        last_why_values[2] = r;
    }
    static Why bad_sum(float t) { return (bits(t) & 0x7f800000u) == 0x7f800000u ? Why::sum_overflow : Why::sum_tiny; }
    static Why bad_sum(double t) {
        return (bits(t) & 0x7ff0000000000000ull) == 0x7ff0000000000000ull ? Why::sum_overflow : Why::sum_tiny;
    }

    float in(float a) {
        if (!ordinary(a)) fail(is_zero(f32(bits(a) & 0x7f800000u)) ? Why::in_subnormal : Why::in_nan_inf, bits(a), 0, 0);
        return a;
    }
    double in(double a) {
        if (!ordinary(a)) fail((bits(a) & 0x7ff0000000000000ull) ? Why::in_nan_inf : Why::in_subnormal, bits(a), 0, 0);
        return a;
    }
    // FADD / FSUB: an exact zero is fine (an IEEE sum never rounds to zero)
    float add(float a, float b) {
        const float t = a + b;
        if (!(big(t) || is_zero(t))) fail(bad_sum(t), bits(a), bits(b), bits(t));
        return t;
    }
    float sub(float a, float b) {
        const float t = a - b;
        if (!(big(t) || is_zero(t))) fail(bad_sum(t), bits(a), bits(b), bits(t));
        return t;
    }
    double add(double a, double b) {
        const double t = a + b;
        if (!(big(t) || is_zero(t))) fail(bad_sum(t), bits(a), bits(b), bits(t));
        return t;
    }
    double sub(double a, double b) {
        const double t = a - b;
        if (!(big(t) || is_zero(t))) fail(bad_sum(t), bits(a), bits(b), bits(t));
        return t;
    }
    // FMUL: a zero only as the product of a zero
    float mul(float a, float b) {
        const float t = a * b;
        if (!(big(t) || (is_zero(t) && (is_zero(a) || is_zero(b)))))
            fail((bits(t) & 0x7f800000u) == 0x7f800000u ? Why::mul_overflow : Why::mul_underflow, bits(a), bits(b), bits(t));
        return t;
    }
    // FMLA / FMADD: w + a * b rounded once (through binary64; a binary32 midpoint there is given up)
    float fma(float w, float a, float b) {
        const double s = (double)a * (double)b + (double)w;
        if ((bits(s) & 0x1fffffffull) == 0x10000000ull) fail(Why::fma_midpoint, bits(a), bits(b), bits(w));
        const float t = (float)s;
        if (!(big(t) || (is_zero(t) && (is_zero(a) || is_zero(b)) && is_zero(w))))
            fail(Why::fma_range, bits(a), bits(b), bits(w));
        return t;
    }
    // FDIV: a zero only as the quotient of a zero by a non-zero
    float div(float a, float b) {
        const float t = a / b;
        if (!(big(t) || (is_zero(t) && is_zero(a) && !is_zero(b)))) fail(Why::div_range, bits(a), bits(b), bits(t));
        return t;
    }
    // FSQRT (correctly rounded on both): of a zero or a positive normal
    float sqrt(float a) {
        if (!(is_zero(a) || (big(a) && !(bits(a) & 0x80000000u)))) fail(Why::sqrt_range, bits(a), 0, 0);
        return std::sqrt(a);
    }
    // FCVT double -> single, to nearest: a zero is fine (Arm with or without FZ gives the same signed zero)
    float narrow(double d) {
        const float t = (float)d;
        if (!(big(t) || is_zero(t)))
            fail((bits(t) & 0x7f800000u) == 0x7f800000u ? Why::narrow_overflow : Why::narrow_tiny, bits(d), 0, bits(t));
        return t;
    }
};

// FCVTZS binary32 -> int32 of an ordinary value: toward zero, saturating as Arm does (x86's conversion gives
// 0x80000000 out of range instead)
inline int32_t cvtzs(float f) {
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f < -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}

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
