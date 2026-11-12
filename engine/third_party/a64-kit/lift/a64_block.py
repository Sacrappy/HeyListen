"""(HeyListen TICK-063) A64_FPX_BLOCK=1 at lift time (off by default): straight-line runs of FP / vector statements in the hot
functions computed as one speculative block in XMM registers, with every op's keep tests accumulated into one lane mask that
is tested once at the end; the block's registers are written to the context only then. If any lane of any op fails its
test, nothing was written and the run's original per-op code runs from the same state (the exact fallback).

Why: each FP op of the default lowering re-reads the FPX gate (fpsr / fpcr), loads its operands from c->vreg, runs the op,
tests the result's bits (~15 x86 instructions), branches, and stores the whole register back (the next op's operand comes
from that store). The NavMesh / model kernels are 150-270 bytes of x86 per guest instruction mostly for that. Here a run
of n ops reads the gate once, keeps the values in registers between the ops, and tests once.

Exactness: inside an open gate (FPSR.IXC set, no FPCR control bit but FZ), an op whose lanes pass the keep tests of the
default fast path (a64_fpx.h: a big result, or the zero rules; under FZ no subnormal operand) returns the IEEE result the
Arm op returns and raises no FPSR bit, so the context after the op is the same whether the op ran alone or in the block,
and the block writes only what the run would have written. The tests here are those of the default helpers, lane for
lane (a64_bk_* in BLOCK_HEADER mirror A64_VFPX_SUM, a64_vfpx_mul32, a64_vfpx_fma32_fast, recomp_fpx_div32 / sqrt32); the
FMA is the hardware fused multiply-add (correctly rounded, as recomp_fpx_fma32's). Data movement (DUP, INS, EXT, ZIP,
the loads, the bit ops, FNEG / FABS / FMOV) is bit-exact by construction. tests/test_fpx_block.py compares the whole
context and the guest memory with the default lowering on random registers, memory and FP states.

A run ends at anything else: a store, a compare, a flag or general-register write, a call, a label, a branch. Blocks
need at least BLOCK_MIN_ARITH arithmetic ops.

A64_FPX_BLOCK_ST=1 (off by default; TICK-063's second lever): the vector stores (STR S / D / Q, STP S / D, through SP or
not) join the runs instead of ending them. A store's address and value are taken where it stands in the run and the store
itself is made at the commit, after the keep test, in program order, through the same recomp_store* helper: the fallback
still starts from a run that stored nothing. A load after a store (a vector load or a general-register statement that
reads guest memory) ends the run before it, so no load in a block can need a value the block has not stored yet; the
stores move later only past register arithmetic, which no other thread can see.

A64_FPX_BLOCK_CVT=1 (off by default): the binary32 <-> 32-bit integer conversions join the runs: SCVTF / UCVTF (vector,
lane and from a W register) and FCVTZS / FCVTZU (vector, lane and to a W register). In the gate the rounding is to nearest
and IXC is already set, so the host conversion gives the Arm result and no new flag; the lanes the host cannot do the same
way (UCVTF of a value >= 2^31; FCVTZ* of |x| >= 2^31, a NaN or an infinity, FCVTZU of a negative value <= -1; under FZ a
subnormal operand: IDC) are "bad" and the run takes the fallback.

    text = block_text(text)         # a hot function's text (after hot_text's other passes)
    header = block_header()         # appended to a64_fpx.h
"""
import os
import re
from collections import Counter

STATS = Counter()
BARRIERS = Counter()
BLOCK_MIN_ARITH = int(os.environ.get("A64_FPX_BLOCK_MIN", "2"))
GPR_IN_BLOCKS = os.environ.get("A64_FPX_BLOCK_GPR", "1") != "0"


def cvt_on():
    """A64_FPX_BLOCK_CVT=1: the binary32 / int32 conversions inside blocks"""
    return os.environ.get("A64_FPX_BLOCK_CVT", "0") == "1"


def stores_on():
    """A64_FPX_BLOCK_ST=1: vector stores inside blocks (read at each call: the test turns it on and off)"""
    return os.environ.get("A64_FPX_BLOCK_ST", "0") == "1"

V = r"c->vreg\[(\d+)\]"
# an address made of general registers and constants (no vector register, no assignment, one statement)
ADDR = r"((?:(?!vreg)[^;=])+?)"
L4, L2, L1 = "A64_BK_L4", "A64_BK_L2", "A64_BK_L1"


class Op:
    __slots__ = ("kind", "dst", "srcs", "code", "arith", "text", "ends_run", "gpr_reads", "gpr_writes", "store")

    def __init__(self, kind, dst, srcs, code, arith=False, text="", ends_run=False):
        self.kind, self.dst, self.srcs, self.code, self.arith, self.text = kind, dst, srcs, code, arith, text
        self.ends_run = ends_run
        self.gpr_reads, self.gpr_writes = [], []
        self.store = None   # (a store: (helper, address expression, [(register, part)]); made at the commit)


def bv(r):
    return "_bv%d" % int(r)


# ---- shapes: (regex, builder(match) -> Op or None) --------------------------------------------------------------------
def sh_vsum(m):
    op, d, n, mm, k = m.groups()
    lanes = L4 if k == "4" else L2
    return Op("v" + op[:3], d, [n, mm], "%s = a64_bk_v%s(&_bad, _fzm, %s, %s, %s);" % (bv(d), op[:3], bv(n), bv(mm), lanes), True)


def sh_vmul(m):
    d, n, mm = m.groups()
    return Op("vmul", d, [n, mm], "%s = a64_bk_vmul(&_bad, _fzm, %s, %s, %s);" % (bv(d), bv(n), bv(mm), L4), True)


def sh_vmule(m):
    mm, off, d, n = m.groups()
    k = int(off) // 4
    return Op("vmule", d, [n, mm], "%s = a64_bk_vmul(&_bad, _fzm, %s, a64_bk_bcast(%s, %d), %s);" % (bv(d), bv(n), bv(mm), k, L4), True)


def sh_vfmae(m):
    mm, off, d, n, neg = m.groups()
    k = int(off) // 4
    a = "a64_bk_negv(%s)" % bv(n) if neg != "0" else bv(n)
    return Op("vfmae", d, [d, n, mm], "%s = a64_bk_vfma(&_bad, _fzm, %s, a64_bk_bcast(%s, %d), %s, %s);" % (bv(d), a, bv(mm), k, bv(d), L4), True)


def sh_vfma(m):
    d, n, mm, neg = m.groups()
    a = "a64_bk_negv(%s)" % bv(n) if neg != "0" else bv(n)
    return Op("vfma", d, [d, n, mm], "%s = a64_bk_vfma(&_bad, _fzm, %s, %s, %s, %s);" % (bv(d), a, bv(mm), bv(d), L4), True)


def sh_smul(m):
    a, b, _, op, _, neg, d = m.groups()
    f = "a64_bk_s%s(&_bad, _fzm, %s, %s)" % (op[:3], bv(a), bv(b))
    if neg:
        f = "a64_bk_negs(%s)" % f
    return Op("s" + op[:3], d, [a, b], "%s = %s;" % (bv(d), f), True)


def sh_sfma(m):
    a, b, z, na, nz, d = m.groups()
    fa = "a64_bk_negs(%s)" % bv(a) if na else bv(a)
    fz = "a64_bk_negs(%s)" % bv(z) if nz else bv(z)
    return Op("sfma", d, [a, b, z], "%s = a64_bk_sfma(&_bad, _fzm, %s, %s, %s);" % (bv(d), fa, bv(b), fz), True)


LANE = re.compile(r"\{uint64_t _a=_([nm])\[(\d)\],_b=_([nm])\[(\d)\],_v=0;_v=a64_fpx_hot_(add32_add32|sub32_sub32)\(c,_a,_b\);"
                  r"_r\[(\d)\]=\(uint32_t\)_v;\}")


def sh_pairs(m):
    """lane-wise FADD / FSUB of chosen lanes (FADDP, scalar FADD / FSUB, ...): r[k] = X[i] op Y[j], the other lanes zero"""
    n, mm, lanes_text, d = m.groups()
    lanes = LANE.findall(lanes_text)
    if not lanes or len(LANE.sub("", lanes_text).strip()) or len({l[4] for l in lanes}) != 1:
        return None
    op = lanes[0][4][:3]
    ks = [int(l[5]) for l in lanes]
    if len(set(ks)) != len(ks) or any(k > 3 for k in ks):
        return None
    src = {"n": n, "m": mm}
    # p / q: for each result lane k the source lane; unused lanes take lane ks[0]'s choice (a defined value, no new assist)
    pa = [None] * 4
    qa = [None] * 4
    for x, i, y, j, _, k in lanes:
        pa[int(k)] = (src[x], int(i))
        qa[int(k)] = (src[y], int(j))
    fill_p, fill_q = pa[ks[0]], qa[ks[0]]
    pa = [p or fill_p for p in pa]
    qa = [q or fill_q for q in qa]
    mask = sum(1 << k for k in ks)

    def gather(sel):
        regs = sorted({r for r, _ in sel}, key=int)
        if len(regs) == 1:
            return "a64_bk_shuf4(%s, %s, %s)" % (bv(regs[0]), bv(regs[0]), ", ".join(str(i) for _, i in sel))
        return "a64_bk_shuf4(%s, %s, %s)" % (bv(regs[0]), bv(regs[1]), ", ".join(str(i + (4 if r == regs[1] else 0)) for r, i in sel))
    srcs = sorted({n, mm}, key=int)
    code = "%s = a64_bk_v%s(&_bad, _fzm, %s, %s, a64_bk_lanemask(%d));" % (bv(d), op, gather(pa), gather(qa), mask)
    return Op("pair" if len(ks) > 1 or ks != [0] else "s" + op, d, srcs, code, True)


def sh_mul2(m):
    n, mm, d, d2 = m.groups()
    if d != d2:
        return None
    return Op("vmul2", d, [n, mm], "%s = a64_bk_vmul(&_bad, _fzm, %s, %s, %s);" % (bv(d), bv(n), bv(mm), L2), True)


def sh_sqrt(m):
    n, mm, d = m.groups()
    return Op("ssqrt", d, [n], "%s = a64_bk_ssqrt(&_bad, _fzm, %s);" % (bv(d), bv(n)), True)


def sh_ldq(m):
    addr, d, d2 = m.groups()
    if d != d2:
        return None
    return Op("ldq", d, [], "%s = a64_bk_ldq(%s);" % (bv(d), addr))


def sh_lds(m):
    size, addr, d = m.groups()
    return Op("ld" + size, d, [], "%s = a64_bk_ld%s(%s);" % (bv(d), size, addr))


def sh_ldpq(m):
    xb, off, d1, d1b, d2, d2b = m.groups()
    if d1 != d1b or d2 != d2b or d1 == d2:
        return None
    return Op("ldpq", (d1, d2), [], "%s = a64_bk_ldq(c->x[%s]+(%s)); %s = a64_bk_ldq(c->x[%s]+(%s)+16);" % (
        bv(d1), xb, off, bv(d2), xb, off))


def sh_ldps(m):
    xb, off, size, d1, d2 = m.groups()
    if d1 == d2:
        return None
    n = 4 if size == "32" else 8
    return Op("ldp" + size, (d1, d2), [], "%s = a64_bk_ld%s(c->x[%s]+(%s)); %s = a64_bk_ld%s(c->x[%s]+(%s)+%d);" % (
        bv(d1), size, xb, off, bv(d2), size, xb, off, n))


def perm_code(d, a, b, bytes_):
    """bytes_[i]: ('a', j) | ('b', j) | None (zero); a, b: register numbers (b may be None)"""
    if b is None or b == a:
        idx = [(16 if x is None else x[1]) for x in bytes_]
        return "%s = a64_bk_perm1(%s, %s);" % (bv(d), bv(a), ", ".join(str(i) for i in idx))
    zero = [x is None for x in bytes_]
    idx = [(0 if x is None else x[1] + (16 if x[0] == "b" else 0)) for x in bytes_]
    code = "a64_bk_perm2(%s, %s, %s)" % (bv(a), bv(b), ", ".join(str(i) for i in idx))
    if any(zero):
        code = "a64_bk_andb(%s, %s)" % (code, ", ".join("0" if z else "255" for z in zero))
    return "%s = %s;" % (bv(d), code)


def sh_dupv(m):
    s, off, size, k, size2, size3, d = m.groups()
    off, size, k = int(off), int(size), int(k)
    if size != int(size2) or size != int(size3) or off + size > 16 or k * size > 16:
        return None
    bytes_ = [None] * 16
    for i in range(k):
        for j in range(size):
            bytes_[i * size + j] = ("a", off + j)
    return Op("dup", d, [s], perm_code(d, s, None, bytes_))


def sh_dups(m):
    s, off, size, d = m.groups()
    off, size = int(off), int(size)
    if off + size > 16:
        return None
    bytes_ = [("a", off + j) if j < size else None for j in range(16)]
    return Op("dups", d, [s], perm_code(d, s, None, bytes_))


def sh_dupg(m):
    x, k, size, size2, d = m.groups()
    k, size = int(k), int(size)
    if size != int(size2) or k * size > 16 or size not in (1, 2, 4, 8):
        return None
    return Op("dupg", d, [], "%s = a64_bk_andb(a64_bk_set1_%d(c->x[%s]), %s);" % (
        bv(d), size, x, ", ".join("255" if i < k * size else "0" for i in range(16))))


def sh_ins(m):
    s, d, do, so, size, d2 = m.groups()
    if d != d2:
        return None
    do, so, size = int(do), int(so), int(size)
    if do + size > 16 or so + size > 16:
        return None
    bytes_ = [("b", so + (i - do)) if do <= i < do + size else ("a", i) for i in range(16)]
    return Op("ins", d, [d, s], perm_code(d, d, s, bytes_))


def sh_ins0(m):
    d, do, size, d2 = m.groups()
    if d != d2:
        return None
    do, size = int(do), int(size)
    if do + size > 16:
        return None
    bytes_ = [None if do <= i < do + size else ("a", i) for i in range(16)]
    return Op("ins0", d, [d], perm_code(d, d, None, bytes_))


def sh_ext(m):
    a, b, sh, d, d2 = m.groups()
    if d != d2:
        return None
    sh = int(sh)
    if not 0 <= sh < 16:
        return None
    bytes_ = [("a", i + sh) if i + sh < 16 else ("b", i + sh - 16) for i in range(16)]
    return Op("ext", d, [a, b], perm_code(d, a, b, bytes_))


IDX = re.compile(r"^[\d_i+*\- ()]+$")
ASSIGN = re.compile(r"_r\[([^\]]+)\]=_([ab])\[([^\]]+)\]; ")


def sh_zip(m):
    bits, count, a, size, b, n, body, d, d2, lo = m.groups()
    if d != d2:
        return None
    w = int(bits) // 8
    count, n = int(count), int(n)
    if w * count != int(size) or len(ASSIGN.sub("", body).strip()):
        return None
    elems = [None] * count
    for ri, src, si in ASSIGN.findall(body):
        if not IDX.match(ri) or not IDX.match(si):
            return None
        for i in range(n):
            r, s = eval(ri, {"__builtins__": {}}, {"_i": i}), eval(si, {"__builtins__": {}}, {"_i": i})   # (whitelisted text)
            if not (0 <= r < count and 0 <= s < count):
                return None
            elems[r] = (src, s)
    keep = 16 if lo is None else int(lo)
    if keep % w or any(e is None for e in elems[:keep // w]):
        return None
    bytes_ = [None] * 16
    for i in range(keep // w):
        src, s = elems[i]
        for j in range(w):
            bytes_[i * w + j] = (src, s * w + j)
    return Op("zip", d, [a, b], perm_code(d, a, b, bytes_))


def sh_bitop(m):
    d, a, op, b, a2, op2, b2 = m.groups()
    if a != a2 or b != b2 or op != op2:
        return None
    f = {"|": "or", "&": "and", "^": "xor"}[op]
    return Op("bit" + f, d, [a, b], "%s = _mm_%s_si128(%s, %s);" % (bv(d), f, bv(a), bv(b)))


def sh_fmovs(m):
    ty, s, expr, d, d2, size = m.groups()
    if d != d2 or (ty == "float") != (size == "4"):
        return None
    if expr == "_a":
        f = "a64_bk_lo%s(%s)" % (size, bv(s))
    elif expr == "-_a":
        f = "a64_bk_lo%s(a64_bk_neg%s(%s))" % (size, size, bv(s))
    else:
        f = "a64_bk_lo%s(a64_bk_abs%s(%s))" % (size, size, bv(s))
    return Op("fmov", d, [s], "%s = %s;" % (bv(d), f))


def sh_fmovg(m):
    d, d2, x, size = m.groups()
    if d != d2:
        return None
    return Op("fmovg", d, [], "%s = a64_bk_fromx%s(c->x[%s]);" % (bv(d), size, x))


def sh_ext8(m):
    a, b, sh, d = m.groups()
    sh = int(sh)
    if not 0 <= sh < 8:
        return None
    bytes_ = [(("a", i + sh) if i + sh < 8 else ("b", i + sh - 8)) if i < 8 else None for i in range(16)]
    return Op("ext", d, [a, b], perm_code(d, a, b, bytes_))


def sh_fnegv(m):
    a, d, d2 = m.groups()
    if d != d2:
        return None
    return Op("fnegv", d, [a], "%s = a64_bk_negv(%s);" % (bv(d), bv(a)))


def sh_fmovi(m):
    d, lo, hi = m.group(1), m.group(2), m.group(3) if m.lastindex >= 3 else None
    if hi is None:
        return Op("fmovi", d, [], "%s = _mm_cvtsi64_si128((long long)%s);" % (bv(d), lo))
    return Op("fmovi", d, [], "%s = _mm_set_epi64x((long long)%s, (long long)%s);" % (bv(d), hi, lo))


def sh_smule(m):
    """scalar FMUL / FMLA / FMLS by element: d = [d +] [-]a * m[k], the lane 0 result"""
    a, mm, off, d, pre, op, d2 = m.groups()
    if d != d2 or (op == "mul32_fma32") != pre.startswith("_z="):
        return None
    k = int(off) // 4
    if op == "mul32_fma32":
        return Op("smule", d, [a, mm], "%s = a64_bk_smul(&_bad, _fzm, %s, a64_bk_bcast(%s, %d));" % (bv(d), bv(a), bv(mm), k), True)
    fa = "a64_bk_negs(%s)" % bv(a) if pre else bv(a)
    return Op("sfmae", d, [a, mm, d], "%s = a64_bk_sfma(&_bad, _fzm, %s, a64_bk_bcast(%s, %d), %s);" % (bv(d), fa, bv(mm), k, bv(d)), True)


def sh_fcmp(m):
    """scalar FCMP S (against a register or zero): the flags of a64_fcmp32's native compare, written at the commit; it ends
    the run (nothing after it in a block reads the flags, and the next statement is usually the branch)"""
    a, b = m.group(1), m.group(2)
    if b != "a64_vzero":
        b = re.match(r"c->vreg\[(\d+)\]", b).group(1)
    srcs = [a] if b == "a64_vzero" else [a, b]
    bb = "_mm_setzero_si128()" if b == "a64_vzero" else bv(b)
    return Op("fcmp", None, srcs, "_bfl = a64_bk_fcmp(&_bad, _fzm, %s, %s);" % (bv(a), bb), True, ends_run=True)


SHAPES = [
    (re.compile(r"a64_vfpx_(add32|sub32)\(c," + V + "," + V + "," + V + r",(2|4)\);"), sh_vsum),
    (re.compile(r"a64_vfpx_mul32\(c," + V + "," + V + "," + V + r",4\);"), sh_vmul),
    (re.compile(r"\{ uint32_t _e; memcpy\(&_e,\(const uint8_t\*\)" + V + r"\+(0|4|8|12),4\); a64_vfpx_mul32e\(c," + V + "," + V + r",_e\); \}"), sh_vmule),
    (re.compile(r"\{ uint32_t _e; memcpy\(&_e,\(const uint8_t\*\)" + V + r"\+(0|4|8|12),4\); a64_vfpx_fma32e\(c," + V + "," + V + r",_e,(0|0x80000000u)\); \}"), sh_vfmae),
    (re.compile(r"a64_vfpx_fma32\(c," + V + "," + V + "," + V + r",(0|0x80000000u)\);"), sh_vfma),
    (re.compile(r"\{uint64_t _a=0,_b=0,_z,_v;memcpy\(&_a," + V + r",4\);memcpy\(&_b," + V + r",4\);"
                r"_z=\(_a\^_b\)&0x80000000ULL;\s*(\(void\)_z;\s*)?_v=a64_fpx_hot_(mul32_fma32|div32_div32)\(c,_a,_b(,_z)?\);"
                r"\s*(_v\^=0x80000000ULL;\s*)?a64_vset64\(" + V + r",_v\);\}"), sh_smul),
    (re.compile(r"\{ uint64_t _a=0,_b=0,_z=0,_v; memcpy\(&_a," + V + r",4\);memcpy\(&_b," + V + r",4\);"
                r"memcpy\(&_z," + V + r",4\);\s*(_a\^=0x80000000ULL;\s*)?(_z\^=0x80000000ULL;\s*)?"
                r"_v=a64_fpx_hot_fma32_fma32\(c,_a,_b,_z\);\s*a64_vset64\(" + V + r",_v\);\}"), sh_sfma),
    (re.compile(r"\{ uint32_t _n\[4\],_m\[4\],_r\[4\]=\{0\};memcpy\(_n," + V + r",16\);memcpy\(_m," + V + r",16\);\s*"
                r"((?:\{uint64_t _a=_[nm]\[\d\],_b=_[nm]\[\d\],_v=0;_v=a64_fpx_hot_(?:add32_add32|sub32_sub32)\(c,_a,_b\);"
                r"_r\[\d\]=\(uint32_t\)_v;\}\s*)+)a64_vst16\(" + V + r",_r\);\}"), sh_pairs),
    (re.compile(r"\{uint32_t _n\[2\],_m\[2\],_d\[2\];memcpy\(_n," + V + r",8\);memcpy\(_m," + V + r",8\);"
                r"for\(unsigned _j=0;_j<2;\+\+_j\)\{uint64_t _a=_n\[_j\],_b=_m\[_j\],_z=\(_a\^_b\)&0x80000000ULL,_v;\s*"
                r"_v=a64_fpx_hot_mul32_fma32\(c,_a,_b,_z\);\s*_d\[_j\]=\(uint32_t\)_v;\}memcpy\(" + V + r",_d,8\);\s*"
                r"c->vreg\[(\d+)\]\[1\]=0;\s*\}"), sh_mul2),
    (re.compile(r"\{ uint32_t _n\[4\],_m\[4\],_r\[4\]=\{0\}; memcpy\(_n," + V + r",16\); memcpy\(_m," + V + r",16\);\s*"
                r"for\(unsigned _lane=0;_lane<1;\+\+_lane\) \{ uint64_t _a=_n\[_lane\],_v=0;_v=a64_fpx_hot_sqrt32_sqrt32\(c,_a\);"
                r"_r\[_lane\]=\(uint32_t\)\(_v\); \} a64_vst16\(" + V + r",_r\); \}"), sh_sqrt),
    (re.compile(r"\{ recomp_ldp64\(c,\(" + ADDR + r"\),&" + V + r"\[0\],&" + V + r"\[1\]\); \}"), sh_ldq),
    (re.compile(r"\{ \{ uint64_t _v=recomp_load(32|64)\(c,\(" + ADDR + r"\)\); a64_vset64\(" + V + r",_v\); \} \}"), sh_lds),
    (re.compile(r"\{ uint64_t _b=c->x\[(\d+)\]; int64_t _o=(-?\d+); recomp_ldp64\(c,\(_b\+_o\),&" + V + r"\[0\],&" + V + r"\[1\]\); "
                r"recomp_ldp64\(c,\(_b\+_o\)\+16,&" + V + r"\[0\],&" + V + r"\[1\]\); \}"), sh_ldpq),
    (re.compile(r"\{ uint64_t _b=c->x\[(\d+)\]; int64_t _o=(-?\d+); \{ uint64_t _p0,_p1; recomp_ldp(32|64)\(c,\(_b\+_o\),&_p0,&_p1\); "
                r"a64_vset64\(" + V + r",_p0\); a64_vset64\(" + V + r",_p1\); \}\}"), sh_ldps),
    (re.compile(r"\{ uint8_t _s\[16\]; memcpy\(_s," + V + r",16\); uint64_t _e=0; memcpy\(&_e,_s\+(\d+),(1|2|4|8)\); uint8_t _d\[16\]; "
                r"memset\(_d,0,16\); for\(int _i=0;_i<(\d+);_i\+\+\) memcpy\(_d\+_i\*(\d),&_e,(\d)\); a64_vst16\(" + V + r",_d\); \}"), sh_dupv),
    (re.compile(r"\{ uint8_t _s\[16\]; memcpy\(_s," + V + r",16\); uint64_t _e=0; memcpy\(&_e,_s\+(\d+),(1|2|4|8)\); a64_vset64\(" + V + r",_e\); \}"), sh_dups),
    (re.compile(r"\{ uint64_t _e=c->x\[(\d+)\]; uint8_t _d\[16\]; memset\(_d,0,16\); for\(int _i=0;_i<(\d+);_i\+\+\) "
                r"memcpy\(_d\+_i\*(\d),&_e,(\d)\); a64_vst16\(" + V + r",_d\); \}"), sh_dupg),
    (re.compile(r"\{ uint8_t _s\[16\],_d\[16\]; memcpy\(_s," + V + r",16\); memcpy\(_d," + V + r",16\); memcpy\(_d\+(\d+),_s\+(\d+),(1|2|4|8)\); "
                r"a64_vst16\(" + V + r",_d\); \}"), sh_ins),
    (re.compile(r"\{ uint8_t _d\[16\]; memcpy\(_d," + V + r",16\); uint64_t _e=\(uint64_t\)0; memcpy\(_d\+(\d+),&_e,(1|2|4|8)\); "
                r"a64_vst16\(" + V + r",_d\); \}"), sh_ins0),
    (re.compile(r"\{ uint8_t _a\[16\],_b\[16\],_r\[16\]; memcpy\(_a," + V + r",16\); memcpy\(_b," + V + r",16\); "
                r"for\(int _i=0;_i<16;_i\+\+\)\{ int _k=_i\+(\d+); _r\[_i\] = \(_k<16\) \? _a\[_k\] : _b\[_k-16\]; \} "
                r"a64_vset64\(" + V + r",0\); a64_vst16\(" + V + r",_r\); \}"), sh_ext),
    (re.compile(r"\{ uint(8|16|32|64)_t _a\[(\d+)\],_b\[\2\],_r\[\2\]; memcpy\(_a," + V + r",(8|16)\); memcpy\(_b," + V + r",\4\); "
                r"for\(int _i=0;_i<(\d+);_i\+\+\)\{ ((?:_r\[[^\]]+\]=_[ab]\[[^\]]+\]; )+)\} "
                r"(?:a64_vset64\(" + V + r",0\); a64_vst16\(" + V + r",_r\)|a64_vset_lo\(" + V + r",_r,(\d+)\)); \}"), None),
    (re.compile(r"\{ a64_vset128\(" + V + "," + V + r"\[0\]([|&^])" + V + r"\[0\]," + V + r"\[1\]([|&^])" + V + r"\[1\]\); \}"), sh_bitop),
    (re.compile(r"\{ (float|double) _a,_b,_r; memcpy\(&_a,&" + V + r"\[0\],(?:4|8)\); \(void\)_b; _r = (_a|-_a|fabsf\(_a\)|fabs\(_a\)); "
                r"a64_vset64\(" + V + r",0\); memcpy\(&" + V + r"\[0\],&_r,(4|8)\); \}"), sh_fmovs),
    (re.compile(r"\{ a64_vset64\(" + V + r",0\); memcpy\(&" + V + r"\[0\],&c->x\[(\d+)\],(4|8)\); \}"), sh_fmovg),
    (re.compile(r"\{ uint8_t _a\[8\],_b\[8\],_r\[8\]; memcpy\(_a," + V + r",8\); memcpy\(_b," + V + r",8\); "
                r"for\(int _i=0;_i<8;_i\+\+\)\{ int _k=_i\+(\d+); _r\[_i\] = \(_k<8\) \? _a\[_k\] : _b\[_k-8\]; \} a64_vset_lo\(" + V + r",_r,8\); \}"), sh_ext8),
    (re.compile(r"\{ uint32_t _a\[4\],_r\[4\]; memcpy\(_a," + V + r",16\); for\(int _i=0;_i<4;_i\+\+\) _r\[_i\]=_a\[_i\]\^0x80000000U; "
                r"a64_vset64\(" + V + r",0\); a64_vst16\(" + V + r",_r\); \}"), sh_fnegv),
    (re.compile(r"a64_vset64\(" + V + r",(0x[0-9a-fA-F]+ULL|\d+ULL)\);"), sh_fmovi),
    (re.compile(r"a64_vset128\(" + V + r",(0x[0-9a-fA-F]+ULL|\d+ULL),(0x[0-9a-fA-F]+ULL|\d+ULL)\);"), sh_fmovi),
    (re.compile(r"\{ uint32_t _a\[1\],_r\[1\],_m; memcpy\(_a," + V + r",4\); memcpy\(&_m,\(const uint8_t\*\)" + V + r"\+(0|4|8|12),4\); "
                r"memcpy\(_r," + V + r",4\);for\(int _i=0;_i<1;\+\+_i\)\{uint64_t _n=_a\[_i\],_z0=_r\[_i\];\{uint64_t _a=_n,_b=_m,_z=_z0,_v;"
                r"(_z=\(_a\^_b\)&0x80000000ULL;|_a\^=0x80000000ULL;|)_v=a64_fpx_hot_(mul32_fma32|fma32_fma32)\(c,_a,_b,_z\);"
                r"_r\[_i\]=\(uint32_t\)_v;\}\}a64_vset_lo\(" + V + r",_r,4\); \}"), sh_smule),
    (re.compile(r"\{ if \(!a64_fcmp32\(c," + V + r",(c->vreg\[\d+\]|a64_vzero)\)\) \{ uint64_t _a=0,_b=0; [^\n]*?c->(?:n|z|c|v)=[^\n]*?\} \} \}"), sh_fcmp),
]


def zip_builder(m):
    # (the ZIP / UZP / TRN shape has two alternative stores: regroup its groups for sh_zip)
    g = m.groups()
    bits, count, a, size, b, n, body = g[:7]
    d_a, d_b, d_lo, lo = g[7:11]
    if d_lo is not None:
        return sh_zip(_G((bits, count, a, size, b, n, body, d_lo, d_lo, lo)))
    return sh_zip(_G((bits, count, a, size, b, n, body, d_a, d_b, None)))


class _G:
    def __init__(self, g):
        self.g = g

    def groups(self):
        return self.g


SHAPES = [(rx, (zip_builder if fn is None else fn)) for rx, fn in SHAPES]


# ---- stores (A64_FPX_BLOCK_ST=1): the value's part of a register: lo32 / lo64 / hi64 ------------------------------------
def st_op(fn, addr, parts, more=()):
    """a store statement: one helper call (fn, addr, parts), or several in order (more: further (fn, addr, parts))"""
    calls = [(fn, addr, parts)] + list(more)
    op = Op("st", None, sorted({r for _, _, ps in calls for r, _ in ps}, key=int), "")
    op.store = calls
    return op


def sh_st1(m):
    r, n, bits, sp, addr = m.groups()
    if (n == "4") != (bits == "32"):
        return None
    return st_op("recomp_store%s%s" % (bits, sp or ""), addr, [(r, "lo" + bits)])


def sh_stq(m):
    sp, addr, r, r2 = m.groups()
    if r != r2:
        return None
    return st_op("recomp_stp64%s" % (sp or ""), addr, [(r, "lo64"), (r, "hi64")])


def sh_stp(m):
    xb, off, r1, n, r2, bits, sp = m.groups()
    if (n == "4") != (bits == "32"):
        return None
    return st_op("recomp_stp%s%s" % (bits, sp or ""), "c->x[%s]+(%s)" % (xb, off), [(r1, "lo" + bits), (r2, "lo" + bits)])


def sh_stpq(m):
    xb, off, sp, r, r_, sp2, r2, r2_ = m.groups()
    if r != r_ or (r2 is not None and r2 != r2_):
        return None
    base = "c->x[%s]+(%s)" % (xb, off)
    more = [("recomp_stp64%s" % (sp2 or ""), "(%s)+16" % base, [(r2, "lo64"), (r2, "hi64")])] if r2 is not None else []
    return st_op("recomp_stp64%s" % (sp or ""), base, [(r, "lo64"), (r, "hi64")], more)


STORE_SHAPES = [
    (re.compile(r"\{ \{ uint64_t _v=0; memcpy\(&_v,&" + V + r"\[0\],(4|8)\); recomp_store(32|64)(_sp)?\(c,\(" + ADDR + r"\),_v\); \} \}"), sh_st1),
    (re.compile(r"\{ recomp_stp64(_sp)?\(c,\(" + ADDR + r"\)," + V + r"\[0\]," + V + r"\[1\]\); \}"), sh_stq),
    (re.compile(r"\{ uint64_t _b=c->x\[(\d+)\]; int64_t _o=(-?\d+); \{ uint64_t _p0=0,_p1=0; memcpy\(&_p0,&" + V + r"\[0\],(4|8)\); "
                r"memcpy\(&_p1,&" + V + r"\[0\],\4\); recomp_stp(32|64)(_sp)?\(c,\(_b\+_o\),_p0,_p1\); \}\}"), sh_stp),
    # (STP Q / STR Q with an offset: one or two 16-byte stores from the base and offset)
    (re.compile(r"\{ uint64_t _b=c->x\[(\d+)\]; int64_t _o=(-?\d+); recomp_stp64(_sp)?\(c,\(_b\+_o\)," + V + r"\[0\]," + V + r"\[1\]\);"
                r"(?: recomp_stp64(_sp)?\(c,\(_b\+_o\)\+16," + V + r"\[0\]," + V + r"\[1\]\);)? \}"), sh_stpq),
]
# ---- conversions (A64_FPX_BLOCK_CVT=1) ----------------------------------------------------------------------------------
LANES_N = {"1": L1, "2": L2, "4": L4}


def whole_line(m):
    """the match is a statement of its own line (not the body of an if or part of a bigger statement)"""
    t = m.string
    return not t[t.rfind("\n", 0, m.start()) + 1:m.start()].strip() and not t[m.end():(t.find("\n", m.end()) + 1 or len(t))].strip()


def sh_scvtv(m):
    s_, n, sg, d = m.groups()
    return Op("scvtf", d, [s_], "%s = a64_bk_scvtf(&_bad, %s, %s, %s);" % (bv(d), bv(s_), sg, LANES_N[n]), True)


def sh_fcvtzv(m):
    s_, n, sg, d = m.groups()
    return Op("fcvtz", d, [s_], "%s = a64_bk_fcvtz(&_bad, _fzm, %s, %s, %s);" % (bv(d), bv(s_), sg, LANES_N[n]), True)


def sh_scvtg(m):
    x, sg, d, d2 = m.groups()
    if d != d2 or not whole_line(m):
        return None
    return Op("scvtfg", d, [], "%s = a64_bk_scvtf(&_bad, _mm_cvtsi32_si128((int)(uint32_t)c->x[%s]), %s, %s);" % (bv(d), x, sg, L1), True)


def sh_fcvtzg(m):
    x, s_, sg = m.groups()
    if not whole_line(m):
        return None
    op = Op("fcvtzg", None, [s_], "c->x[%s] = (uint64_t)(uint32_t)_mm_cvtsi128_si32(a64_bk_fcvtz(&_bad, _fzm, %s, %s, %s));" % (
        x, bv(s_), sg, L1), True)
    op.gpr_writes = [int(x)]
    return op


CVT_SHAPES = [
    (re.compile(r"\{ uint32_t _a\[4\],_r\[4\]=\{0\}; memcpy\(_a," + V + r",16\); for\(int _i=0;_i<([124]);_i\+\+\) "
                r"_r\[_i\]=\(uint32_t\)recomp_fixed_to_fp\(_a\[_i\],32,32,0,([01]),c->fpcr,&c->fpsr\); a64_vst16\(" + V + r",_r\); \}"), sh_scvtv),
    (re.compile(r"\{ uint32_t _a\[4\],_r\[4\]=\{0\}; memcpy\(_a," + V + r",16\); for\(int _i=0;_i<([124]);_i\+\+\) "
                r"_r\[_i\]=\(uint32_t\)recomp_fp_to_int\(_a\[_i\],32,32,([01]),3,c->fpcr,&c->fpsr\); a64_vst16\(" + V + r",_r\); \}"), sh_fcvtzv),
    (re.compile(r"\{ uint64_t _r=recomp_fixed_to_fp\(c->x\[(\d+)\],32,32,0,([01]),c->fpcr,&c->fpsr\); a64_vset64\(" + V + r",0\); "
                + V + r"\[0\]=_r; \}"), sh_scvtg),
    (re.compile(r"c->x\[(\d+)\]=recomp_fp_to_int\(" + V + r"\[0\]&UINT64_C\(0xffffffff\),32,32,([01]),3,c->fpcr,&c->fpsr\);"), sh_fcvtzg),
]
LOAD_KINDS = {"ldq", "ld32", "ld64", "ldp32", "ldp64", "ldpq"}


def reads_memory(op):
    return op.kind in LOAD_KINDS or (op.kind == "gpr" and ("recomp_load" in op.code or "recomp_ldp" in op.code))

# (general-register statements between FP ops: address arithmetic, pointer loads, immediates. Kept inside a block when they are
# pure: only c->x[N] written, read from c->x / the flags / guest memory (recomp_load*), no call, no brace nesting)
GPR_STMT = re.compile(r"(?:\{[^{}\n]*\}|[^{};\n]*;)")
GPR_WORD = re.compile(r"[A-Za-z_]\w*")
GPR_OK_WORDS = {"uint64_t", "int64_t", "uint32_t", "int32_t", "uint16_t", "int16_t", "uint8_t", "int8_t", "c", "x", "n", "z", "v",
                "_a", "_b", "_r", "_s", "_o", "_t", "_v", "_p0", "_p1", "_lo", "_hi", "ULL", "LL", "U", "L",
                "recomp_load8", "recomp_load16", "recomp_load32", "recomp_load64", "recomp_ldp32", "recomp_ldp64"}
GPR_X = re.compile(r"c->x\[(\d+)\]")
GPR_WRITE = re.compile(r"c->x\[(\d+)\]\s*(?:=(?!=)|&=|\|=|\^=|\+=|-=|<<=|>>=)")
GPR_NUM = re.compile(r"(?<![A-Za-z_0-9])(?:0[xX][0-9a-fA-F]+|\d+)")   # (numbers, not the digits of uint64_t / recomp_load64)


def gpr_op(stmt):
    """an Op for a pure general-register statement, else None"""
    t = stmt.strip()
    if not t or "c->vreg" in t or "recomp_store" in t or "goto" in t or "A64_" in t or "TAILCALL" in t:
        return None
    body = GPR_NUM.sub("", t)
    words = set(GPR_WORD.findall(body.replace("c->", " c->")))
    if not words <= GPR_OK_WORDS:
        return None
    # (the flags and the context only through c->x / c->n.. reads: any other "c->" field is refused)
    if re.search(r"c->(?!x\[|n\b|z\b|c\b|v\b)", t):
        return None
    writes = [int(x) for x in GPR_WRITE.findall(t)]
    if not writes:
        return None
    reads = [int(x) for x in GPR_X.findall(t)]
    code = GPR_X.sub(lambda m: "_bx%s" % m.group(1), t)
    op = Op("gpr", None, [], code, False)
    op.gpr_reads = sorted(set(reads))
    op.gpr_writes = sorted(set(writes))
    return op


def find_gpr(text, ops):
    """the pure general-register statements that lie between two shaped ops, as ops of their own"""
    extra = []
    for i in range(len(ops) - 1):
        gap = text[ops[i][1]:ops[i + 1][0]]
        if not gap.strip():
            continue
        pos = 0
        found = []
        ok = True
        for m in GPR_STMT.finditer(gap):
            if gap[pos:m.start()].strip():
                ok = False
                break
            g = gpr_op(m.group(0))
            if g is None:
                ok = False
                break
            g.text = m.group(0)
            found.append((ops[i][1] + m.start(), ops[i][1] + m.end(), g))
            pos = m.end()
        if ok and not gap[pos:].strip():
            extra += found
    return extra


def find_ops(text):
    """[(start, end, Op)] of every statement one of the shapes matches, in text order (non-overlapping)"""
    found = []
    for rx, build in SHAPES + (STORE_SHAPES if stores_on() else []) + (CVT_SHAPES if cvt_on() else []):
        for m in rx.finditer(text):
            op = build(m)
            if op is not None:
                op.text = m.group(0)
                found.append((m.start(), m.end(), op))
    found.sort(key=lambda t: (t[0], -t[1]))
    out, last = [], -1
    for s, e, op in found:
        if s >= last:
            out.append((s, e, op))
            last = e
    if GPR_IN_BLOCKS:
        out = sorted(out + find_gpr(text, out), key=lambda t: t[0])
    return out


def emit_block(ops):
    # (stores, A64_FPX_BLOCK_ST: the address and the value taken where the store stands, the store made at the commit)
    stores = []
    for op in ops:
        if op.store is not None:
            code = []
            for fn, addr, parts in op.store:
                k = len(stores)
                regs = sorted({r for r, _ in parts}, key=int)
                code.append("const uint64_t _sa%d = (uint64_t)(%s); %s" % (k, addr, " ".join(
                    "const __m128i _sv%d_%s = %s;" % (k, r, bv(r)) for r in regs)))
                stores.append("%s(c, _sa%d, %s);" % (fn, k, ", ".join("a64_bk_%s(_sv%d_%s)" % (part, k, r) for r, part in parts)))
            op.code = " ".join(code)
    gpr_used, gpr_written = [], []
    for op in ops:
        for r in op.gpr_reads + op.gpr_writes + [int(x) for x in GPR_X.findall(op.code)]:
            if r not in gpr_used:
                gpr_used.append(r)
        for r in op.gpr_writes:
            if r not in gpr_written:
                gpr_written.append(r)
    read_first, written = [], []
    seen_w = set()
    for op in ops:
        for r in op.srcs:
            r = int(r)
            if r not in seen_w and r not in read_first:
                read_first.append(r)
        ds = op.dst if isinstance(op.dst, tuple) else (op.dst,) if op.dst is not None else ()
        for d in ds:
            d = int(d)
            seen_w.add(d)
            if d not in written:
                written.append(d)
    decl = ["__m128i " + ", ".join("%s = a64_bk_ld(c->vreg[%d])" % (bv(r), r) for r in read_first) + ";"] if read_first else []
    only_w = [d for d in written if d not in read_first]
    if only_w:
        decl.append("__m128i " + ", ".join(bv(d) for d in only_w) + ";")
    if gpr_used:
        decl.append("uint64_t " + ", ".join("_bx%d = c->x[%d]" % (r, r) for r in gpr_used) + ";")
    n_arith = sum(1 for op in ops if op.arith)
    if any(op.kind == "fcmp" for op in ops):
        decl.append("uint32_t _bfl = 0;")
    out = ["{ /* (TICK-063) FP block: %d ops, %d arithmetic */" % (len(ops), n_arith), "#if A64_BK", "int _bk = 0;",
           "if (RECOMP_LIKELY(RECOMP_FPX_OPEN(c))) {",
           "const __m128i _fzm = a64_bk_fzm(c); __m128i _bad = _mm_setzero_si128();"] + decl
    out += [GPR_X.sub(lambda m: "_bx%s" % m.group(1), op.code) for op in ops]
    commit = " ".join("a64_bk_st(c->vreg[%d], %s);" % (d, bv(d)) for d in written)
    commit += "".join(" c->x[%d] = _bx%d;" % (r, r) for r in gpr_written)
    if any(op.kind == "fcmp" for op in ops):
        commit += " a64_bk_fcmp_commit(c, _bfl);"
    if stores:   # (A64_FPX_BLOCK_ST: in program order, after the keep test)
        commit += " " + " ".join(stores)
    out.append("if (RECOMP_LIKELY(!_mm_movemask_epi8(_bad))) { %s _bk = 1; A64_BK_PROBE(1); } else A64_BK_PROBE(0);" % commit)
    out.append("}")
    out.append("if (__builtin_expect(!_bk, 0)) {")   # (the fallback out of line, cold without a profile too)
    out.append("#endif")
    out += [op.text for op in ops]
    out += ["#if A64_BK", "}", "#endif", "}"]
    return "\n".join(out)


def block_text(text):
    """a hot function's text with its FP runs as speculative blocks"""
    if "c->vreg[" not in text:
        return text
    ops = find_ops(text)
    if not ops:
        return text
    out, pos, run = [], 0, []

    def flush():
        if not run:
            return
        # (general-register statements at a run's ends stay outside the block: nothing to carry them for)
        lo, hi = 0, len(run)
        while lo < hi and run[lo][2].kind in ("gpr", "st"):   # (a leading store: nothing to carry it for either)
            lo += 1
        while hi > lo and run[hi - 1][2].kind == "gpr":
            hi -= 1
        kept = run[lo:hi]
        n_arith = sum(1 for _, _, op in kept if op.arith)
        if n_arith >= BLOCK_MIN_ARITH:
            STATS["blocks"] += 1
            STATS["ops_in_blocks"] += len(kept)
            STATS["arith_in_blocks"] += n_arith
            STATS["block_len_max"] = max(STATS["block_len_max"], len(kept))
            for _, _, op in kept:
                STATS["op_" + op.kind] += 1
            if lo:
                out.append(text[run[0][0]:kept[0][0]])
            out.append(emit_block([op for _, _, op in kept]))
            if hi < len(run):
                out.append(text[kept[-1][1]:run[-1][1]])
        else:
            STATS["runs_short"] += 1
            out.append(text[run[0][0]:run[-1][1]])
        run.clear()

    for s, e, op in ops:
        between = text[pos:s]
        if run and not between.strip() and reads_memory(op) and any(o.kind == "st" for _, _, o in run):
            flush()   # (A64_FPX_BLOCK_ST: a load after a store starts a new run: no load in a block reads past a delayed store)
        if run and between.strip():
            # a statement no shape knows ends the run: count its kind (what to add next)
            BARRIERS[re.sub(r"\d+", "N", between.strip().splitlines()[0])[:90]] += 1
            flush()
        if not run:
            out.append(between)
        run.append((s, e, op))
        pos = e
        if op.ends_run:
            flush()
    flush()
    out.append(text[pos:])
    return "".join(out)


def report():
    if not STATS:
        return ""
    lines = ["A64_FPX_BLOCK: %s" % ", ".join("%s %d" % kv for kv in sorted(STATS.items()))]
    if BARRIERS:
        lines.append("A64_FPX_BLOCK: run-ending statements (top 12): " + "; ".join("%d x %s" % (n, s) for s, n in BARRIERS.most_common(12)))
    return "\n".join(lines)


BLOCK_HEADER = r"""
/* ---- (TICK-063) A64_FPX_BLOCK: straight-line FP runs as one speculative block (a64_block.py) ----
   The lane tests are the default helpers' (A64_VFPX_SUM, a64_vfpx_mul32, a64_vfpx_fma32_fast, recomp_fpx_div32 / sqrt32),
   accumulated into *bad instead of branched on per op; results outside `lanes` are zero (the register's upper lanes). */
#if RECOMP_FPX_HOST == 2 && defined(__FMA__) && defined(__SSSE3__)
#include <immintrin.h>
#define A64_BK 1
#ifndef A64_BK_PROBE
#define A64_BK_PROBE(taken) ((void)0)   /* (tests count the blocks taken and fallen back) */
#endif
typedef uint8_t a64_bk_u8x16 __attribute__((vector_size(16)));
typedef int32_t a64_bk_i32x4 __attribute__((vector_size(16)));
#define A64_BK_L4 _mm_set1_epi32(-1)
#define A64_BK_L2 _mm_setr_epi32(-1, -1, 0, 0)
#define A64_BK_L1 _mm_setr_epi32(-1, 0, 0, 0)
#define A64_BK_F(x) _mm_castsi128_ps(x)
#define A64_BK_I(x) _mm_castps_si128(x)
static inline __attribute__((always_inline)) __m128i a64_bk_lanemask(int m) {
    return _mm_setr_epi32(m & 1 ? -1 : 0, m & 2 ? -1 : 0, m & 4 ? -1 : 0, m & 8 ? -1 : 0);
}
static inline __attribute__((always_inline)) __m128i a64_bk_ld(const uint64_t *s) { return _mm_loadu_si128((const __m128i *)s); }
static inline __attribute__((always_inline)) void a64_bk_st(uint64_t *d, __m128i x) { A64_OPAQUE(x); _mm_storeu_si128((__m128i *)d, x); }
static inline __attribute__((always_inline)) __m128i a64_bk_fzm(const Ctx *c) { return _mm_set1_epi32(RECOMP_FPX_FZ(c) ? -1 : 0); }
static inline __attribute__((always_inline)) __m128i a64_bk_ult(__m128i x, uint32_t k) {
    return _mm_cmplt_epi32(_mm_xor_si128(x, _mm_set1_epi32((int)0x80000000u)), _mm_set1_epi32((int)(k ^ 0x80000000u)));
}
static inline __attribute__((always_inline)) __m128i a64_bk_abs(__m128i x) { return _mm_and_si128(x, _mm_set1_epi32(0x7fffffff)); }
static inline __attribute__((always_inline)) __m128i a64_bk_big(__m128i t) { return a64_bk_ult(_mm_sub_epi32(a64_bk_abs(t), _mm_set1_epi32(0x00800001)), 0x7effffffu); }
static inline __attribute__((always_inline)) __m128i a64_bk_zero(__m128i x) { return _mm_cmpeq_epi32(a64_bk_abs(x), _mm_setzero_si128()); }
static inline __attribute__((always_inline)) __m128i a64_bk_nonfin(__m128i t) { const __m128i e = _mm_set1_epi32(0x7f800000); return _mm_cmpeq_epi32(_mm_and_si128(t, e), e); }
static inline __attribute__((always_inline)) __m128i a64_bk_subn(__m128i x) { return a64_bk_ult(_mm_sub_epi32(_mm_slli_epi32(x, 1), _mm_set1_epi32(1)), 0x00ffffffu); }
static inline __attribute__((always_inline)) __m128i a64_bk_not(__m128i x) { return _mm_andnot_si128(x, _mm_set1_epi32(-1)); }
/* sums (A64_VFPX_SUM): finite; under FZ no subnormal operand and the sum big or zero */
static inline __attribute__((always_inline)) __m128i a64_bk_sum(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i t, __m128i lanes) {
    const __m128i fz = _mm_or_si128(_mm_or_si128(a64_bk_subn(a), a64_bk_subn(b)), a64_bk_not(_mm_or_si128(a64_bk_big(t), a64_bk_zero(t))));
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_or_si128(a64_bk_nonfin(t), _mm_and_si128(fzm, fz)), lanes));
    return _mm_and_si128(t, lanes);
}
/* products (a64_vfpx_mul32, a64_vfpx_fma32_fast): big, or a finite product of a zero; under FZ no subnormal operand */
static inline __attribute__((always_inline)) __m128i a64_bk_prod(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i t, __m128i lanes) {
    const __m128i keep = _mm_or_si128(a64_bk_big(t), _mm_andnot_si128(a64_bk_nonfin(t), _mm_or_si128(a64_bk_zero(a), a64_bk_zero(b))));
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_or_si128(a64_bk_not(keep), _mm_and_si128(fzm, _mm_or_si128(a64_bk_subn(a), a64_bk_subn(b)))), lanes));
    return _mm_and_si128(t, lanes);
}
/* quotients (recomp_fpx_div32): big, or a finite quotient of a zero dividend; under FZ no subnormal operand */
static inline __attribute__((always_inline)) __m128i a64_bk_quot(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i t, __m128i lanes) {
    const __m128i keep = _mm_or_si128(a64_bk_big(t), _mm_andnot_si128(a64_bk_nonfin(t), a64_bk_zero(a)));
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_or_si128(a64_bk_not(keep), _mm_and_si128(fzm, _mm_or_si128(a64_bk_subn(a), a64_bk_subn(b)))), lanes));
    return _mm_and_si128(t, lanes);
}
static inline __attribute__((always_inline)) __m128i a64_bk_vadd(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i lanes) {
    return a64_bk_sum(bad, fzm, a, b, A64_BK_I(_mm_add_ps(A64_BK_F(a), A64_BK_F(b))), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_vsub(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i lanes) {
    return a64_bk_sum(bad, fzm, a, b, A64_BK_I(_mm_sub_ps(A64_BK_F(a), A64_BK_F(b))), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_vmul(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i lanes) {
    return a64_bk_prod(bad, fzm, a, b, A64_BK_I(_mm_mul_ps(A64_BK_F(a), A64_BK_F(b))), lanes); }
/* d + a * b, fused (one rounding, as the Arm op and recomp_fpx_fma32's route); under FZ no subnormal addend either */
static inline __attribute__((always_inline)) __m128i a64_bk_vfma(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i w, __m128i lanes) {
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_and_si128(fzm, a64_bk_subn(w)), lanes));
    return a64_bk_prod(bad, fzm, a, b, A64_BK_I(_mm_fmadd_ps(A64_BK_F(a), A64_BK_F(b), A64_BK_F(w))), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_sadd(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    return a64_bk_sum(bad, fzm, a, b, A64_BK_I(_mm_add_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_ssub(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    return a64_bk_sum(bad, fzm, a, b, A64_BK_I(_mm_sub_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_smul(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    return a64_bk_prod(bad, fzm, a, b, A64_BK_I(_mm_mul_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_sdiv(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    return a64_bk_quot(bad, fzm, a, b, A64_BK_I(_mm_div_ss(A64_BK_F(a), A64_BK_F(b))), A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_sfma(__m128i *bad, __m128i fzm, __m128i a, __m128i b, __m128i w) {
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_and_si128(fzm, a64_bk_subn(w)), A64_BK_L1));
    return a64_bk_prod(bad, fzm, a, b, A64_BK_I(_mm_fmadd_ss(A64_BK_F(a), A64_BK_F(b), A64_BK_F(w))), A64_BK_L1); }
/* square root (recomp_fpx_sqrt32): a zero is itself; else a positive finite operand (under FZ not subnormal) */
static inline __attribute__((always_inline)) __m128i a64_bk_ssqrt(__m128i *bad, __m128i fzm, __m128i a) {
    const __m128i keep = _mm_or_si128(a64_bk_zero(a), a64_bk_ult(_mm_sub_epi32(a, _mm_set1_epi32(1)), 0x7f7fffffu));
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_or_si128(a64_bk_not(keep), _mm_and_si128(fzm, a64_bk_subn(a))), A64_BK_L1));
    return _mm_and_si128(A64_BK_I(_mm_sqrt_ss(A64_BK_F(a))), A64_BK_L1); }
/* scalar FCMP (a64_fcmp32): ordered, under FZ no subnormal operand; the flags as a64_fcmp_set writes them */
static inline __attribute__((always_inline)) uint32_t a64_bk_fcmp(__m128i *bad, __m128i fzm, __m128i a, __m128i b) {
    const __m128 fa = A64_BK_F(a), fb = A64_BK_F(b);
    const __m128i unord = A64_BK_I(_mm_cmpunord_ss(fa, fb));
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_or_si128(unord, _mm_and_si128(fzm, _mm_or_si128(a64_bk_subn(a), a64_bk_subn(b)))), A64_BK_L1));
    return (uint32_t)_mm_comilt_ss(fa, fb) | (uint32_t)_mm_comieq_ss(fa, fb) << 1;
}
static inline __attribute__((always_inline)) void a64_bk_fcmp_commit(Ctx *c, uint32_t fl) {
    c->n = (uint8_t)(fl & 1u); c->z = (uint8_t)(fl >> 1 & 1u); c->c = (uint8_t)!(fl & 1u); c->v = 0;
}
/* SCVTF / UCVTF (recomp_fixed_to_fp, binary32 from 32-bit integers, no fraction bits): in the gate rounded to nearest with
   IXC set, as the host's conversion; UCVTF of a lane >= 2^31 is left to the fallback */
static inline __attribute__((always_inline)) __m128i a64_bk_scvtf(__m128i *bad, __m128i a, int is_signed, __m128i lanes) {
    if (!is_signed) *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_srai_epi32(a, 31), lanes));
    return _mm_and_si128(A64_BK_I(_mm_cvtepi32_ps(a)), lanes); }
/* FCVTZS / FCVTZU (recomp_fp_to_int, toward zero, binary32 to 32-bit): kept when |x| < 2^31 (so not a NaN or an infinity),
   FCVTZU also x >= +0 or |x| < 1 (a magnitude of zero), under FZ no subnormal (IDC); the truncation's IXC is already set */
static inline __attribute__((always_inline)) __m128i a64_bk_fcvtz(__m128i *bad, __m128i fzm, __m128i a, int is_signed, __m128i lanes) {
    __m128i keep = a64_bk_ult(a64_bk_abs(a), 0x4f000000u);
    if (!is_signed) keep = _mm_and_si128(keep, _mm_or_si128(_mm_cmpgt_epi32(a, _mm_set1_epi32(-1)), a64_bk_ult(a64_bk_abs(a), 0x3f800000u)));
    *bad = _mm_or_si128(*bad, _mm_and_si128(_mm_or_si128(a64_bk_not(keep), _mm_and_si128(fzm, a64_bk_subn(a))), lanes));
    return _mm_and_si128(_mm_cvttps_epi32(A64_BK_F(a)), lanes); }
static inline __attribute__((always_inline)) __m128i a64_bk_negs(__m128i x) { return _mm_xor_si128(x, _mm_setr_epi32((int)0x80000000u, 0, 0, 0)); }
static inline __attribute__((always_inline)) __m128i a64_bk_negv(__m128i x) { return _mm_xor_si128(x, _mm_set1_epi32((int)0x80000000u)); }
static inline __attribute__((always_inline)) __m128i a64_bk_neg4(__m128i x) { return a64_bk_negs(x); }
static inline __attribute__((always_inline)) __m128i a64_bk_neg8(__m128i x) { return _mm_xor_si128(x, _mm_setr_epi32(0, (int)0x80000000u, 0, 0)); }
static inline __attribute__((always_inline)) __m128i a64_bk_abs4(__m128i x) { return _mm_and_si128(x, _mm_setr_epi32(0x7fffffff, -1, -1, -1)); }
static inline __attribute__((always_inline)) __m128i a64_bk_abs8(__m128i x) { return _mm_and_si128(x, _mm_setr_epi32(-1, 0x7fffffff, -1, -1)); }
static inline __attribute__((always_inline)) __m128i a64_bk_lo4(__m128i x) { return _mm_and_si128(x, A64_BK_L1); }
static inline __attribute__((always_inline)) __m128i a64_bk_lo8(__m128i x) { return _mm_and_si128(x, A64_BK_L2); }
static inline __attribute__((always_inline)) __m128i a64_bk_bcast(__m128i x, int k) {
    return k == 0 ? _mm_shuffle_epi32(x, 0x00) : k == 1 ? _mm_shuffle_epi32(x, 0x55) : k == 2 ? _mm_shuffle_epi32(x, 0xAA) : _mm_shuffle_epi32(x, 0xFF); }
static inline __attribute__((always_inline)) __m128i a64_bk_ldq(uint64_t a) { return _mm_loadu_si128((const __m128i *)A64_PTR(a)); }
static inline __attribute__((always_inline)) __m128i a64_bk_ld64(uint64_t a) { return _mm_loadl_epi64((const __m128i *)A64_PTR(a)); }
static inline __attribute__((always_inline)) __m128i a64_bk_ld32(uint64_t a) { uint32_t v; memcpy(&v, A64_PTR(a), 4); return _mm_cvtsi32_si128((int)v); }
static inline __attribute__((always_inline)) uint64_t a64_bk_lo32(__m128i x) { return (uint64_t)(uint32_t)_mm_cvtsi128_si32(x); }
static inline __attribute__((always_inline)) uint64_t a64_bk_lo64(__m128i x) { return (uint64_t)_mm_cvtsi128_si64(x); }
static inline __attribute__((always_inline)) uint64_t a64_bk_hi64(__m128i x) { return (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(x, x)); }
static inline __attribute__((always_inline)) __m128i a64_bk_fromx4(uint64_t x) { return _mm_cvtsi32_si128((int)(uint32_t)x); }
static inline __attribute__((always_inline)) __m128i a64_bk_fromx8(uint64_t x) { return _mm_cvtsi64_si128((long long)x); }
static inline __attribute__((always_inline)) __m128i a64_bk_set1_1(uint64_t x) { return _mm_set1_epi8((char)x); }
static inline __attribute__((always_inline)) __m128i a64_bk_set1_2(uint64_t x) { return _mm_set1_epi16((short)x); }
static inline __attribute__((always_inline)) __m128i a64_bk_set1_4(uint64_t x) { return _mm_set1_epi32((int)(uint32_t)x); }
static inline __attribute__((always_inline)) __m128i a64_bk_set1_8(uint64_t x) { return _mm_set1_epi64x((long long)x); }
/* byte permutes: indices 0-15 from a, 16-31 from b (perm2) or zero (perm1); the compiler picks the shuffle */
#define a64_bk_perm1(a, ...) ((__m128i)__builtin_shufflevector((a64_bk_u8x16)(a), (a64_bk_u8x16)_mm_setzero_si128(), __VA_ARGS__))
#define a64_bk_perm2(a, b, ...) ((__m128i)__builtin_shufflevector((a64_bk_u8x16)(a), (a64_bk_u8x16)(b), __VA_ARGS__))
#define a64_bk_andb(x, ...) _mm_and_si128((x), (__m128i)(a64_bk_u8x16){__VA_ARGS__})
/* 4 x 32-bit lanes: indices 0-3 from a, 4-7 from b */
#define a64_bk_shuf4(a, b, ...) ((__m128i)__builtin_shufflevector((a64_bk_i32x4)(a), (a64_bk_i32x4)(b), __VA_ARGS__))
#else
#define A64_BK 0
#endif
"""


def block_header():
    return BLOCK_HEADER
