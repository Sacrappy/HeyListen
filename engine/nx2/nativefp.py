# Copyright (C) 2026 Sacrappy
"""(ProjectReLink, pixl27; ported by hand into HeyListen) The hot functions' floating point on the host's own instructions, the
operations the fast paths give up on done without the software bodies, and the vector registers as C locals. Three lift-time
options, each off unless A64_<name>=1 (an A64_ variable, so in the lift key); all three off, the lift is the lift without
this module, to the byte. The kit's files are not edited (third_party/: CONTRIBUTING.md): install_regs() and install(), called
from nx2/lift.py, wrap a64_lift from outside.

A64_NATIVE_FP=1 (ReLink's A64_NATIVE_FP, its default there). The exact fast paths keep Arm's result AND its cumulative FPSR
flags, so every operation tests the gate (FPSR.IXC set, FPCR at its default but FZ) and the operands' and the result's ranges
on integer copies of the values, about fifteen instructions and four branches for one multiply, and the game reads FPSR
nowhere in its hot code. In the hot functions (A64_HOT_FUNCS) the helper calls take native ones instead:
  - the scalar FPX shapes (a64_fpx_hot_* -> a64_fpx_nat_*): one host instruction, the result kept unless it is a NaN, which
    goes to the exact wrapper (Arm's NaN rules and its default NaN stay);
  - the 2/4-lane FADD / FSUB / FMUL (a64_vfpx_add32 / sub32 / mul32 -> a64_vfpx_nat_*): a NaN lane is kept when exactly one of
    its operands was a NaN (that operand, quieted, on both architectures), else the exact helper runs;
  - FCVTZS / FCVTZU to 32 or 64 bits and SCVTF / UCVTF without fraction bits (recomp_fp_to_int / recomp_fixed_to_fp ->
    a64_nat_*): the host's conversion, its out-of-range answer to the exact body;
  - (HeyListen) the FP blocks of a64_block.py (TICK-063), which ReLink has not: the block's gate is FPCR alone and its
    arithmetic ops (a64_bk_vadd ... a64_bk_ssqrt -> a64_bk_n*) send only a NaN result to the block's per-op fallback, which then
    takes the native helpers above. The compares and conversions inside a block keep their tests.
The gate of all of them is FPCR alone (A64_NAT_OPEN: anything but FZ set, or the kit's inhibit bit, takes the exact path).
Flush-to-zero is the host's: MXCSR's FTZ and DAZ follow the guest's FPCR.FZ (host/hos_thread.c hos_fpcr_sync, called after
every MSR FPCR, which the lift makes call it in every function, and at a thread's and a fiber's start; the lift's own
constructor tells the host to follow, so a build without this option leaves MXCSR alone).
What differs from the exact paths: FPSR's cumulative flags are not raised by a native operation; under FZ a result that rounds
up to exactly the smallest normal is kept where Arm flushes it (x86 detects tininess after rounding); with MXCSR's FTZ set,
the exact fast paths of the other functions keep a sum that the host flushed to a zero, with the right value and without UFC.
Values are otherwise Arm's: IEEE round to nearest, the same NaN, and the same flushes. tests/test_native_fp.py.
Not taken from ReLink: its A64_LOCAL_REGS_HOT (here the hot functions keep their registers in the context: nx2/guest_locals.py
keep_hot, the FP blocks and the PGO profile), and its own file a64_hot_fp.inc (the helpers go into a64_fpx.h: they are
static inline, unused in the other pieces; a change of this option re-lifts every piece anyway).

A64_FPX_EASY=1 (ReLink's FPX_EASY_HEADER, on there). Inside the gate, past the fast path and the rescues (the kit's, TICK-044's
and nx2/fpxkeep.py's), the binary32 FADD / FSUB / FMUL wrappers compute what is left with the host's arithmetic where Arm's
result and flags are known (a NaN or infinite operand, an invalid operation, an overflow, a subnormal operand under FZ, a
tiny result) instead of the software body: in ReLink's Lost Woods the body ran for one lane of most cloth vectors (a NaN or an
infinity in the unused lane), 15.6 % of the game threads' time. Result and FPSR are the body's (tests/test_native_fp.py:
easy). ReLink's vector _fix route (redo only the lanes out of the fast test) is not ported: HeyListen's lane routes are
TICK-044's and fpxkeep's, which keep the NaN and infinite lanes on SSE and call these wrappers for the rest.

A64_LOCAL_VREGS=1 (ReLink's, off there too), with A64_LOCAL_REGS (nx2/guest_locals.py): the vector registers a function with
local registers names become locals too (uint64_t vN_[2], the context's own type, so every helper takes them as it took
c->vreg[N]). An instruction that names one counts as writing it. The guest's ABI: v16-v31 are dead at calls and returns
(never written back there, never read again after), v0-v15 are written back before a call or a return and read again after
a call; around tail calls, indirect jumps, SVCs and traps, everything, as for the general registers. nx2/fastvec.py's passes
run on such a function before its registers are renamed (its shapes spell c->vreg[N]).
"""
import os
import re

import a64_lift

VREGS_INSTALLED = [False]
_installed = False


def on(value):
    """a switch's value: on for 1 (on, true, yes); off unset, 0 or anything else"""
    return (value or "0").strip().lower() in ("1", "on", "true", "yes")


def on_by_default(value):
    """a switch that is on unless set to 0 (off, false, no)"""
    return value is None or value.strip() == "" or on(value)


# (on by default since 2026-11-23, the owner's choice: RTX 5070 Market +10-12% (integ4b 84.9 -> 95.4 fps median), the game's own
# loop being the Market's limit; a 41-minute hand session with no physics or timing fault seen. =0 gives the exact path back)
def native_fp():
    # A64_NATIVE_FP (lift time, on unless =0): the hot functions' floating point on the host's own instructions, FPSR's
    # flags not kept there (ProjectReLink's, ported: nx2/nativefp.py)
    return on_by_default(os.environ.get("A64_NATIVE_FP"))


def fpx_easy():
    # A64_FPX_EASY (lift time, on unless =0): FADD / FSUB / FMUL S past the fast path done without the software body,
    # result and flags exact (ProjectReLink's, ported: nx2/nativefp.py)
    return on_by_default(os.environ.get("A64_FPX_EASY"))


def local_vregs():
    # A64_LOCAL_VREGS (lift time, on unless =0; with A64_LOCAL_REGS): the vector registers a function names as C locals
    # (ProjectReLink's, ported: nx2/nativefp.py)
    return on_by_default(os.environ.get("A64_LOCAL_VREGS"))


# ---- A64_NATIVE_FP --------------------------------------------------------------------------------------------------------
HOT_SCALAR = re.compile(r"\ba64_fpx_hot_(\w+)\(c,")
HOT_VFPX = re.compile(r"\ba64_vfpx_(add32|sub32|mul32)\(c,")
HOT_CONV = re.compile(r"\brecomp_(fp_to_int|fixed_to_fp)\(")
BLOCK_GATE = "if (RECOMP_LIKELY(RECOMP_FPX_OPEN(c))) {\nconst __m128i _fzm = a64_bk_fzm(c);"
# (the gate keeps its && and its RECOMP_LIKELY: clang's PGO matches a function's profile by the shape of its control flow,
# a && is a counted branch, and the hot functions' profile was recorded with RECOMP_FPX_OPEN's)
BLOCK_GATE_NAT = "if (RECOMP_LIKELY(A64_NAT_OPEN_BK(c))) {\nconst __m128i _fzm = a64_bk_fzm(c);"
BLOCK_ARITH = re.compile(r"\ba64_bk_(vadd|vsub|vmul|vfma|sadd|ssub|smul|sdiv|sfma|ssqrt)\(&_bad, _fzm,")
# (MSR FPCR, as the kit writes it, with the source register in the context or a local of nx2/guest_locals.py)
FPCR_SET = re.compile(r"(c->fpcr = \([^;]*? & 0xffffffffULL\) \| \(c->fpcr & RECOMP_FPX_INHIBIT\);)")
FPCR_SYNC = r"\1 { extern void hos_fpcr_sync(Ctx *A64_NOESC); hos_fpcr_sync(c); }"
HEAD = re.compile(r"^(?:/\*[^\n]*\*/\n)?void f_([0-9a-f]+)\(")
STATS = {"hot": 0, "scalar": 0, "vector": 0, "conv": 0, "blocks": 0, "block_ops": 0, "fpcr": 0,
         "cold": 0, "cold_scalar": 0, "cold_vector": 0, "cold_conv": 0}

NATIVE_HEAD = r"""
/* ---- (ProjectReLink, pixl27; nx2/nativefp.py) A64_NATIVE_FP: the hot functions' floating point on the host's
   instructions. See nx2/nativefp.py for what it keeps and what it does not ---- */
#ifndef A64_HOT_FP_INC
#define A64_HOT_FP_INC
#define A64_NAT_INL static inline __attribute__((always_inline))
/* the gate: the guest's FPCR at its default apart from FZ (round to nearest, no default NaN); FPSR is not looked at */
#define A64_NAT_OPEN(c) RECOMP_LIKELY(!((c)->fpcr & RECOMP_FPX_FPCR_MASK))
/* (the FP blocks' gate: the same test, with RECOMP_FPX_OPEN's && kept for the PGO profile's shape) */
#define A64_NAT_OPEN_BK(c) (1 && !((c)->fpcr & RECOMP_FPX_FPCR_MASK))
/* the host's MXCSR follows FPCR.FZ from the start of the process (host/hos_thread.c): this lift calls it native */
#ifndef A64_NAT_NO_FOLLOW
static void __attribute__((constructor, used)) a64_nat_follow(void) { extern void hos_fpcr_follow(void); hos_fpcr_follow(); }
#endif
"""
# scalar helpers: (fast, exact) -> (the C type's letter, the native expression over x0.., needs a hardware FMA)
NATIVE_SCALAR = {
    ("add32", "add32"): ("f", "x0 + x1", False), ("sub32", "sub32"): ("f", "x0 - x1", False),
    ("mul32", "fma32"): ("f", "x0 * x1", False), ("div32", "div32"): ("f", "x0 / x1", False),
    ("sqrt32", "sqrt32"): ("f", "_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x0)))", False),
    ("fma32", "fma32"): ("f", "__builtin_fmaf(x0, x1, x2)", True),
    ("add64", "add64"): ("d", "x0 + x1", False), ("sub64", "sub64"): ("d", "x0 - x1", False),
    ("mul64", "fma64"): ("d", "x0 * x1", False), ("div64", "div64"): ("d", "x0 / x1", False),
    ("sqrt64", "sqrt64"): ("d", "_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x0)))", False),
    ("fma64", "fma64"): ("d", "__builtin_fma(x0, x1, x2)", True),
}
NATIVE_VEC = r"""
/* 2/4-lane single-precision add, subtract, multiply. A NaN lane is Arm's when exactly one of its operands was a NaN (that
   operand, quieted, on both architectures); a lane with two NaN operands, or a NaN made here (Arm's default NaN is
   positive, the host's negative), takes the exact helper */
#define A64_NAT_VEC(name, op, lanes) \
static __attribute__((noinline, cold)) __m128i a64_vfpx_slow_##name(Ctx *c, __m128i a, __m128i b, int k) { \
    uint64_t d[2] = {0, 0}, n[2], m[2]; \
    _mm_storeu_si128((__m128i *)n, a); _mm_storeu_si128((__m128i *)m, b); \
    a64_vfpx_##name(c, d, n, m, k); \
    return _mm_loadu_si128((const __m128i *)d); } \
A64_NAT_INL void a64_vfpx_nat_##name(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { \
    const __m128i ai = _mm_loadu_si128((const __m128i *)n), bi = _mm_loadu_si128((const __m128i *)m); \
    __m128i ri; \
    if (A64_NAT_OPEN(c)) { \
        const __m128 a = _mm_castsi128_ps(ai), b = _mm_castsi128_ps(bi); \
        const __m128 r = op(a, b); \
        const __m128 odd = _mm_andnot_ps(_mm_xor_ps(_mm_cmpunord_ps(a, a), _mm_cmpunord_ps(b, b)), _mm_cmpunord_ps(r, r)); \
        if (RECOMP_LIKELY(!(_mm_movemask_ps(odd) & (lanes)))) { \
            ri = (lanes) == 15 ? _mm_castps_si128(r) : _mm_move_epi64(_mm_castps_si128(r)); \
            goto done; } } \
    ri = a64_vfpx_slow_##name(c, ai, bi, k); \
    done: _mm_storeu_si128((__m128i *)d, ri); }
"""
NATIVE_CONV = r"""
/* FCVTZS / FCVTZU: the host's truncating conversion; its "indefinite" answer (a NaN, or out of range) takes the exact body,
   which saturates. A subnormal truncates to 0 with or without FZ */
A64_NAT_INL uint64_t a64_nat_fp_to_int(uint64_t bits, unsigned fp_bits, unsigned int_bits, unsigned is_signed, unsigned rmode,
                                       uint64_t fpcr, uint64_t *fpsr) {
#if RECOMP_FPX_HOST == 2
    if (rmode == 3 && is_signed) {
        if (fp_bits == 32 && int_bits == 32) {
            const int32_t r = _mm_cvttss_si32(_mm_set_ss(recomp_fpx_f(bits)));
            if (RECOMP_LIKELY(r != INT32_MIN)) return (uint32_t)r;
        } else if (fp_bits == 32 && int_bits == 64) {
            const int64_t r = _mm_cvttss_si64(_mm_set_ss(recomp_fpx_f(bits)));
            if (RECOMP_LIKELY(r != INT64_MIN)) return (uint64_t)r;
        } else if (fp_bits == 64 && int_bits == 64) {
            const int64_t r = _mm_cvttsd_si64(_mm_set_sd(recomp_fpx_d(bits)));
            if (RECOMP_LIKELY(r != INT64_MIN)) return (uint64_t)r;
        } else if (fp_bits == 64 && int_bits == 32) {
            const int32_t r = _mm_cvttsd_si32(_mm_set_sd(recomp_fpx_d(bits)));
            if (RECOMP_LIKELY(r != INT32_MIN)) return (uint32_t)r;
        }
    } else if (rmode == 3 && !is_signed && int_bits == 32) {   /* (through 64 bits: negative and too large fall out together) */
        const int64_t r = fp_bits == 32 ? _mm_cvttss_si64(_mm_set_ss(recomp_fpx_f(bits))) : _mm_cvttsd_si64(_mm_set_sd(recomp_fpx_d(bits)));
        if (RECOMP_LIKELY((uint64_t)r <= 0xffffffffull)) return (uint64_t)r;
    }
#endif
    return recomp_fp_to_int(bits, fp_bits, int_bits, is_signed, rmode, fpcr, fpsr);
}
/* SCVTF / UCVTF without fraction bits, round to nearest: one host conversion, rounded once as Arm's */
A64_NAT_INL uint64_t a64_nat_fixed_to_fp(uint64_t bits, unsigned fp_bits, unsigned int_bits, unsigned fbits, unsigned is_signed,
                                         uint64_t fpcr, uint64_t *fpsr) {
#if RECOMP_FPX_HOST == 2
    if (fbits == 0 && RECOMP_LIKELY(!((fpcr >> 22) & 3))) {
        if (int_bits == 32) {
            const int64_t v = is_signed ? (int64_t)(int32_t)(uint32_t)bits : (int64_t)(uint32_t)bits;
            return fp_bits == 32 ? recomp_fpx_fb((float)v) : recomp_fpx_db((double)v);
        }
        if (is_signed) {
            const int64_t v = (int64_t)bits;
            return fp_bits == 32 ? recomp_fpx_fb((float)v) : recomp_fpx_db((double)v);
        }
    }
#endif
    return recomp_fixed_to_fp(bits, fp_bits, int_bits, fbits, is_signed, fpcr, fpsr);
}
"""
# (HeyListen) the FP blocks' arithmetic, native: the host's result in the block's lanes, a NaN in any of them sends the block
# to its per-op fallback. FZ is MXCSR's (FTZ and DAZ), so the operands' subnormal test (fzm) is not needed
NATIVE_BLOCK = r"""
#if A64_BK
static inline __attribute__((always_inline)) __m128i a64_bk_nres(__m128i *bad, __m128i t, __m128i lanes) {
    *bad = _mm_or_si128(*bad, _mm_and_si128(A64_BK_I(_mm_cmpunord_ps(A64_BK_F(t), A64_BK_F(t))), lanes));
    return _mm_and_si128(t, lanes);
}
static inline __attribute__((always_inline)) __m128i a64_bk_nvadd(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i lanes) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_add_ps(A64_BK_F(a), A64_BK_F(b))), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_nvsub(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i lanes) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_sub_ps(A64_BK_F(a), A64_BK_F(b))), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_nvmul(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i lanes) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_mul_ps(A64_BK_F(a), A64_BK_F(b))), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_nvfma(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i w, __m128i lanes) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_fmadd_ps(A64_BK_F(a), A64_BK_F(b), A64_BK_F(w))), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_nsadd(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_add_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_nssub(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_sub_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_nsmul(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_mul_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_nsdiv(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_div_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_nsfma(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i w) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_fmadd_ss(A64_BK_F(a), A64_BK_F(b), A64_BK_F(w))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_nssqrt(__m128i *bad, __m128i fzm, __m128i a) {
    (void)fzm; return a64_bk_nres(bad, A64_BK_I(_mm_sqrt_ss(A64_BK_F(a))), A64_BK_L1); }
#endif
"""


def native_header(used):
    """a native helper for every FPX shape the hot text may call (a64_fpx_nat_<fast>_<exact>), the 2/4-lane vector helpers
    the lift made (a64_vfpx_nat_*), the integer conversions (a64_nat_*) and the FP blocks' native ops (a64_bk_n*). A shape
    with no native form, or a host that is not x86-64, gets the exact hot helper under the native name"""
    out = [NATIVE_HEAD]
    alias = []
    out.append("#if RECOMP_FPX_HOST == 2")
    for fast, exact, nf, ne in sorted(used):
        name = "%s_%s" % (fast, exact)
        params = ", ".join("uint64_t a%d" % i for i in range(ne))
        args = ", ".join("a%d" % i for i in range(ne))
        form = NATIVE_SCALAR.get((fast, exact))
        alias.append("#define a64_fpx_nat_%s a64_fpx_hot_%s" % (name, name))
        if not form:
            out.append(alias[-1])
            continue
        kind, expr, fma = form
        ctype, get, put = ("float", "recomp_fpx_f", "recomp_fpx_fb") if kind == "f" else ("double", "recomp_fpx_d", "recomp_fpx_db")
        loads = " ".join("const %s x%d = %s(a%d);" % (ctype, i, get, i) for i in range(nf))
        # (the result leaves as the C type on both paths, the exact helper's bits turned back into it: the value then stays in a
        # vector register from one operation to the next)
        body = ("A64_NAT_INL uint64_t a64_fpx_nat_%s(Ctx *c, %s) { %s r; if (A64_NAT_OPEN(c)) { %s r = %s; "
                "if (RECOMP_LIKELY(r == r)) goto done; } r = %s(a64_fpx_%s(c, %s)); done: return %s(r); }"
                % (name, params, ctype, loads, expr, get, name, args, put))
        if fma:
            out += ["#if RECOMP_FPX_FMA64", body, "#else", alias[-1], "#endif"]
        else:
            out.append(body)
    out.append(NATIVE_VEC)
    vec = []
    if ("add32", "add32", 2, 2) in used:
        vec.append(("add32", "_mm_add_ps", "k == 4 ? 15 : 3"))
    if ("sub32", "sub32", 2, 2) in used:
        vec.append(("sub32", "_mm_sub_ps", "k == 4 ? 15 : 3"))
    if ("mul32", "fma32", 2, 3) in used:
        vec.append(("mul32", "_mm_mul_ps", "15"))   # (a64_vfpx_mul32 has all four lanes whatever k)
    for name, op, lanes in vec:
        out.append("A64_NAT_VEC(%s, %s, %s)" % (name, op, lanes))
    out.append(NATIVE_BLOCK)
    out.append("#else")
    out += alias
    out += ["#define a64_vfpx_nat_%s a64_vfpx_%s" % (v[0], v[0]) for v in vec]
    out.append("#endif")
    out.append(NATIVE_CONV)
    out.append("#endif")
    return "\n".join(out) + "\n"


def native_text(text):
    """a hot function's text (after the kit's hot_text, the FP blocks and the other passes) with the native helpers' names,
    and its FP blocks' gate and arithmetic native. Only names and the gate change: the function's control flow, which the
    PGO profile is matched by, stays"""
    STATS["hot"] += 1
    text, n = HOT_SCALAR.subn(r"a64_fpx_nat_\1(c,", text)
    STATS["scalar"] += n
    text, n = HOT_VFPX.subn(r"a64_vfpx_nat_\1(c,", text)
    STATS["vector"] += n
    text, n = HOT_CONV.subn(r"a64_nat_\1(", text)
    STATS["conv"] += n
    if BLOCK_GATE in text:
        STATS["blocks"] += text.count(BLOCK_GATE)
        text = text.replace(BLOCK_GATE, BLOCK_GATE_NAT)
        text, n = BLOCK_ARITH.subn(r"a64_bk_n\1(&_bad, _fzm,", text)
        STATS["block_ops"] += n
    return text


def native_fp_cold():
    # (LIFT3) A64_NATIVE_FP_COLD (lift time, off unless =1; with A64_NATIVE_FP): the same native helpers in every other function
    # (the cold ones, not in A64_HOT_FUNCS), except those the text edits and the gates read (fastvec.PLAIN0)
    return native_fp() and on(os.environ.get("A64_NATIVE_FP_COLD"))


# (a cold function's FPX helper calls as outline_fpx writes them: a64_fpx_<fast>_<exact>(c, ...), noinline, gate and range tests)
COLD_SCALAR = re.compile(r"\ba64_fpx_([a-z]+(?:32|64)_[a-z]+(?:32|64))\(c,")


def cold_text(text):
    """(LIFT3, A64_NATIVE_FP_COLD) a cold function's text with the native helpers' names: the scalar FPX shapes (a64_fpx_<shape> ->
    a64_fpx_nat_<shape>, inline: one host instruction, the exact helper for a NaN), the 2/4-lane add / sub / mul and the
    integer conversions, as native_text does for the hot ones. The values are the hot functions' (tests/test_native_fp.py)"""
    STATS["cold"] += 1
    text, n = COLD_SCALAR.subn(r"a64_fpx_nat_\1(c,", text)
    STATS["cold_scalar"] += n
    text, n = HOT_VFPX.subn(r"a64_vfpx_nat_\1(c,", text)
    STATS["cold_vector"] += n
    text, n = HOT_CONV.subn(r"a64_nat_\1(", text)
    STATS["cold_conv"] += n
    return text


def fpcr_text(text):
    """every MSR FPCR tells the host (its MXCSR follows FZ)"""
    if "c->fpcr = (" not in text:
        return text
    text, n = FPCR_SET.subn(FPCR_SYNC, text)
    STATS["fpcr"] += n
    return text


def report():
    if not native_fp():
        return ""
    return "A64_NATIVE_FP: %s" % ", ".join("%s %d" % kv for kv in sorted(STATS.items()))


# ---- A64_FPX_EASY ----------------------------------------------------------------------------------------------------------
EASY_HEADER = r"""
/* ---- (ProjectReLink, pixl27; nx2/nativefp.py) A64_FPX_EASY: what the fast paths give up on, done with the host's own
   arithmetic where Arm's result is known without the software body (recomp_fpe_*): a NaN or an infinite operand, an invalid
   operation, an overflow, a subnormal operand, a tiny result. Called inside the RECOMP_FPX_OPEN gate only (round to nearest,
   no trap enabled, no default NaN, IXC raised already), after the fast tests and the rescues have refused the operation.
   1: *v is Arm's result and FPSR has what Arm's operation raises (never 0 for these two: kept as a way out).
   tests/test_native_fp.py (easy): against the bodies, result and FPSR. ---- */
#if RECOMP_FPX_HOST
#define A64_EASY_NAN32(x) (((x) & 0x7fffffffu) > 0x7f800000u)
#define A64_EASY_INF32(x) (((x) & 0x7fffffffu) == 0x7f800000u)
/* (a NaN operand: the first signalling one, quieted, IOC; else the first quiet one, as it is) */
static inline __attribute__((always_inline)) uint32_t a64_fpx_easy_nan32(uint32_t a, uint32_t b, uint32_t *fl) {
    if (A64_EASY_NAN32(a) && !(a & 0x00400000u)) { *fl |= 1u; return a | 0x00400000u; }
    if (A64_EASY_NAN32(b) && !(b & 0x00400000u)) { *fl |= 1u; return b | 0x00400000u; }
    return A64_EASY_NAN32(a) ? a : b;
}
/* a + b (sub: a - b) */
static inline __attribute__((always_inline)) int a64_fpx_easy_sum32(Ctx *c, uint64_t a0, uint64_t b0, int sub, uint64_t *v) {
    uint32_t a = (uint32_t)a0, b = (uint32_t)b0, fl = 0, t;
    const uint32_t fz = RECOMP_FPX_FZ(c);
    if (fz) {   /* (a subnormal operand is a zero of its sign, IDC: before anything else is looked at) */
        if (RECOMP_FPX_SUBN32(a)) a &= 0x80000000u, fl |= 0x80u;
        if (RECOMP_FPX_SUBN32(b)) b &= 0x80000000u, fl |= 0x80u;
    }
    if (A64_EASY_NAN32(a) || A64_EASY_NAN32(b)) {
        t = a64_fpx_easy_nan32(a, b, &fl);
    } else {
        if (sub) b ^= 0x80000000u;
        if (A64_EASY_INF32(a) || A64_EASY_INF32(b)) {
            if (A64_EASY_INF32(a) && A64_EASY_INF32(b) && ((a ^ b) & 0x80000000u)) t = 0x7fc00000u, fl |= 1u;   /* inf - inf */
            else t = A64_EASY_INF32(a) ? a : b;
        } else {
            t = (uint32_t)recomp_fpx_fb(recomp_fpx_f(a) + recomp_fpx_f(b));
            const uint32_t m = t & 0x7fffffffu;
            if (m == 0x7f800000u) fl |= 4u | 16u;   /* (overflow: OFC and IXC) */
            else if (fz && m != 0 && m < 0x00800000u) t &= 0x80000000u, fl |= 8u;   /* (tiny, exactly: flushed, UFC) */
        }
    }
    c->fpsr |= fl;
    *v = t;
    return 1;
}
/* a * b */
static inline __attribute__((always_inline)) int a64_fpx_easy_mul32(Ctx *c, uint64_t a0, uint64_t b0, uint64_t *v) {
    uint32_t a = (uint32_t)a0, b = (uint32_t)b0, fl = 0, t;
    const uint32_t fz = RECOMP_FPX_FZ(c);
    if (fz) {
        if (RECOMP_FPX_SUBN32(a)) a &= 0x80000000u, fl |= 0x80u;
        if (RECOMP_FPX_SUBN32(b)) b &= 0x80000000u, fl |= 0x80u;
    }
    const uint32_t sign = (a ^ b) & 0x80000000u;
    if (A64_EASY_NAN32(a) || A64_EASY_NAN32(b)) {
        t = a64_fpx_easy_nan32(a, b, &fl);
    } else if (A64_EASY_INF32(a) || A64_EASY_INF32(b)) {
        if (RECOMP_FPX_ZERO32(a) || RECOMP_FPX_ZERO32(b)) t = 0x7fc00000u, fl |= 1u;   /* inf * 0 */
        else t = sign | 0x7f800000u;
    } else if (RECOMP_FPX_ZERO32(a) || RECOMP_FPX_ZERO32(b)) {
        t = sign;
    } else {
        const float fa = recomp_fpx_f(a), fb = recomp_fpx_f(b), ft = fa * fb;
        t = (uint32_t)recomp_fpx_fb(ft);
        const uint32_t m = t & 0x7fffffffu;
        if (m == 0x7f800000u) {
            fl |= 4u | 16u;   /* (overflow) */
        } else if (m <= 0x00800000u) {   /* (at most the smallest normal: tiny before rounding? the product is exact in binary64) */
            const double p = (double)fa * (double)fb, ap = p < 0 ? -p : p;
            if (ap < 0x1p-126) {
                if (fz) t = sign, fl |= 8u;                 /* (flushed, UFC) */
                else if ((double)ft != p) fl |= 8u | 16u;   /* (gradual, inexact: UFC) */
            }
        }
    }
    c->fpsr |= fl;
    *v = t;
    return 1;
}
#else
static inline int a64_fpx_easy_sum32(Ctx *c, uint64_t a0, uint64_t b0, int sub, uint64_t *v) { (void)c; (void)a0; (void)b0; (void)sub; (void)v; return 0; }
static inline int a64_fpx_easy_mul32(Ctx *c, uint64_t a0, uint64_t b0, uint64_t *v) { (void)c; (void)a0; (void)b0; (void)v; return 0; }
#endif
"""
EASY = {("add32", "add32"): ("add32", "a0, a1", "if (a64_fpx_easy_sum32(c, a0, a1, 0, &v)) return v; "),
        ("sub32", "sub32"): ("sub32", "a0, a1", "if (a64_fpx_easy_sum32(c, a0, a1, 1, &v)) return v; "),
        ("mul32", "fma32"): ("fma32", "a0, a1, a2", "if ((uint32_t)a2 == (((uint32_t)a0 ^ (uint32_t)a1) & 0x80000000u) && "
                                                    "a64_fpx_easy_mul32(c, a0, a1, &v)) return v; ")}


def easy_header(text, used):
    """a64_fpx.h with the easy helpers before the first wrapper and their calls last in the binary32 FADD / FSUB / FMUL
    wrappers, just before the body. Each wrapper of `used` must be found once"""
    first = text.find("\nA64_FPX_FN uint64_t a64_fpx_")
    if first < 0:
        return text   # (no wrapper: nothing calls the helpers)
    text = text[:first + 1] + EASY_HEADER.lstrip("\n") + text[first + 1:]
    for (fast, exact), (body, args, call) in EASY.items():
        if not any(u[:2] == (fast, exact) for u in used):
            continue
        head = "A64_FPX_FN uint64_t a64_fpx_%s_%s(" % (fast, exact)
        tail = "} return recomp_fpe_%s(c, %s); }" % (body, args)
        lines = text.split("\n")
        at = [i for i, x in enumerate(lines) if x.startswith(head)]
        if len(at) != 1 or lines[at[0]].count(tail) != 1:
            raise SystemExit("nativefp: the wrapper a64_fpx_%s_%s is not in a64_fpx.h once as A64_FPX_EASY expects it" % (fast, exact))
        lines[at[0]] = lines[at[0]].replace(tail, call + tail)
        text = "\n".join(lines)
    return text


# ---- A64_LOCAL_VREGS (the kit's localize_regs, reg_writes and Lifter.dirty_regs as ReLink has them) ----------------------
VREG_REF = re.compile(r"c->vreg\[(\d+)\]")
VSHIFT = 32
VLOW = 0xFFFF << VSHIFT          # v0-v15
VSCRATCH = 0xFFFF0000 << VSHIFT  # v16-v31
VALL = VLOW | VSCRATCH
_reg_writes = {}


def reg_writes(text):
    """the guest registers an instruction's C text may write (a mask; the vector registers it names from bit 32 up)"""
    m = _reg_writes.get(text)
    if m is None:
        m = 0
        for a, b in a64_lift.REG_WRITE.findall(text):
            m |= 1 << int(a or b)
        for n in VREG_REF.findall(text):
            m |= 1 << (VSHIFT + int(n))
        _reg_writes[text] = m
    return m


def localize_regs(text, local, dirty):
    """the kit's localize_regs, with the vector registers a function names as locals too (ReLink's)"""
    L = a64_lift
    flags = L.local_flags() and L.FLAG_REF.search(text) is not None
    if flags:
        text = L.FLAG_REF.sub(r"f\1_", text)
    fstore, fload = (L.FLAG_STORE, L.FLAG_LOAD) if flags else ("", "")
    fdecl = "\n    uint8_t fn_ = c->n, fz_ = c->z, fc_ = c->c, fv_ = c->v;" if flags else ""
    if not local:
        text = L.SYNC_MARK.sub(lambda m: fstore if m.group(1) in "AT" else fload if m.group(1) == "L" else "", text)
        head, _, rest = text.partition("\n")
        return head + fdecl + "\n" + rest
    import fastvec   # (its shapes spell c->vreg[N]: its passes first, as its emit_function would have made them after)
    if fastvec.pass_on("VBYTES") or fastvec.pass_on("VMOVES"):
        text = fastvec.rewrite(text)
    uses = {}
    for n in L.REG_REF.findall(text):
        uses[int(n)] = uses.get(int(n), 0) + 1
    top = sorted((n for n in uses if n != 31), key=lambda n: (-uses[n], n))[:L.local_regs()]
    regs = sorted(top + ([31] if 31 in uses else []))
    chosen = set(regs)
    text = L.REG_REF.sub(lambda m: "x%s_" % m.group(1) if int(m.group(1)) in chosen else m.group(0), text)
    vregs = sorted({int(n) for n in VREG_REF.findall(text)})
    if vregs:
        text = VREG_REF.sub(r"v\1_", text)

    def store(mask):
        return ("".join("c->x[%d] = x%d_; " % (n, n) for n in regs if mask >> n & 1) +
                "".join("c->vreg[%d][0] = v%d_[0]; c->vreg[%d][1] = v%d_[1]; " % (n, n, n, n)
                        for n in vregs if mask >> (VSHIFT + n) & 1))

    def load(mask):
        return ("".join("x%d_ = c->x[%d]; " % (n, n) for n in regs if mask >> n & 1) +
                "".join("v%d_[0] = c->vreg[%d][0]; v%d_[1] = c->vreg[%d][1]; " % (n, n, n, n)
                        for n in vregs if mask >> (VSHIFT + n) & 1))

    def expand(m):
        kind, i = m.group(1), int(m.group(2))
        before, after = dirty[0][i], dirty[1][i]
        if kind == "C":   # a call: what it may read (x30 is set just before it)
            return store((before | 1 << 30) & ~L.SCRATCH & ~VSCRATCH)
        if kind == "R":
            return store(before & ~L.SCRATCH & ~VSCRATCH)
        if kind == "A":
            return store(before) + fstore
        if kind == "T":
            return store(after) + fstore
        if kind == "B":   # after a call: x0-x18 and x30 (x19-x29 and SP come back as they were); v0-v15
            return load(0x4007FFFF | VLOW)
        return load(0xFFFFFFFF | VALL) + fload
    text = L.SYNC_MARK.sub(expand, text)
    head, _, rest = text.partition("\n")
    decl = ("\n    uint64_t " + ", ".join("x%d_ = c->x[%d]" % (n, n) for n in regs) + ";") if regs else ""
    if vregs:
        decl += "\n    uint64_t " + ", ".join("v%d_[2] = { c->vreg[%d][0], c->vreg[%d][1] }" % (n, n, n) for n in vregs) + ";"
    return head + decl + fdecl + "\n" + rest


def dirty_regs(self, body, pcs, texts):
    """the kit's Lifter.dirty_regs, where a call or an SVC leaves v16-v31 as the locals have them (ReLink's)"""
    L = a64_lift
    e = self.cur

    def inner(t):
        return t if t in body and (t not in self.entries or t == e) else None
    n = len(pcs)
    at = {pc: i for i, pc in enumerate(pcs)}
    writes, sync, succ = [0] * n, [False] * n, [()] * n
    for i, pc in enumerate(pcs):
        k, w = body[pc]
        kind = k[0]
        targets, falls = [], True
        if kind == "op":
            text, fl = texts[(w, pc) if L.pc_relative(w) else w]
            if fl & L.a64emit.UNHANDLED:
                falls = False
            else:
                writes[i] = reg_writes(text)
        elif kind == "b":
            targets, falls = [k[1]], False
        elif kind in ("bcond", "cbz", "tbz"):
            targets = [k[1]]
        elif kind in ("bl", "blr", "svc"):
            sync[i] = True
        elif kind in ("ret", "trap"):
            falls = False
        elif kind == "br":
            targets, falls = list(self.jt.get(pc) or ()), False
        if falls:
            targets.append(pc + 4)
        succ[i] = tuple(at[t2] for t2 in (inner(t) for t in targets) if t2 is not None)
    dirty_in = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n):
            # (a call or an SVC writes back and reads again all but v16-v31, which stay as the locals have them)
            out = dirty_in[i] & VSCRATCH if sync[i] else dirty_in[i] | writes[i]
            for j in succ[i]:
                if out & ~dirty_in[j]:
                    dirty_in[j] |= out
                    changed = True
    return dirty_in, [dirty_in[i] & VSCRATCH if sync[i] else dirty_in[i] | writes[i] for i in range(n)]


# ---- installation ----------------------------------------------------------------------------------------------------------
def install_regs():
    """A64_LOCAL_VREGS: the kit's localize_regs, reg_writes and Lifter.dirty_regs replaced. Before nx2/guest_locals.py's
    install(), which wraps the localize_regs it finds. Once"""
    if VREGS_INSTALLED[0] or not local_vregs():
        return
    VREGS_INSTALLED[0] = True
    a64_lift.localize_regs = localize_regs
    a64_lift.reg_writes = reg_writes
    a64_lift.Lifter.dirty_regs = dirty_regs


def install():
    """A64_NATIVE_FP and A64_FPX_EASY: around the kit's fpx_header, hot_text and Lifter.emit_function. After nx2/fastvec.py's
    install() (the native names go on the hot text as it leaves every other pass). Once; nothing with both off"""
    global _installed
    if _installed:
        return
    _installed = True
    nat, easy = native_fp(), fpx_easy()
    if not (nat or easy):
        return
    import fastvec
    base_header = a64_lift.fpx_header

    def fpx_header(used):
        text = base_header(used)
        if easy:
            text = easy_header(text, used)
        if nat:
            text += native_header(used)
        return text
    a64_lift.fpx_header = fpx_header
    if not nat:
        return
    base_hot = a64_lift.hot_text

    def hot_text(text):
        text = base_hot(text)
        m = HEAD.match(text)
        if m and int(m.group(1), 16) in fastvec.PLAIN0:   # (a function the text edits and the gates read: as main writes it)
            return text
        return native_text(text)
    a64_lift.hot_text = hot_text
    base_emit = a64_lift.Lifter.emit_function
    cold = native_fp_cold()
    hot = []   # (the kit's hot list, read once)

    def emit_function(self, e, texts):
        text, refs = base_emit(self, e, texts)
        if cold:
            if not hot:
                hot.append(a64_lift.hot_functions())
            if self.fname(e) not in hot[0] and e not in fastvec.PLAIN0:
                text = cold_text(text)
        return fpcr_text(text), refs
    a64_lift.Lifter.emit_function = emit_function
