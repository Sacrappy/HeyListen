"""Switch 2 instruction forms for a64-kit: the ARMv8.1-8.5 encodings suyu's translator (lift/suyu/arm64_to_c.h)
does not know, which Switch 2 code uses everywhere (MKW: ~121k RETAB, ~39k LDAPR, ~32k LSE, ~7k CAS).

install() wraps the two entry points a64_lift.py and tests/insn_test.py use, without editing the vendored kit:
  - a64_lift.classify: RETAA/RETAB are returns; BRAA*/BRAB* and BLRAA*/BLRAB* are BR/BLR through Xn (the
    modifier is ignored);
  - a64emit.translate: the forms below get C from emit() here; everything else goes to suyu as before.

The C is written in the same dialect as suyu's (c->x[n], A64_PTR, A64_MARK), so the lifter's later passes
(dead flags, register localisation, dirty-register sets) see ordinary register reads and writes.

Model:
  - LSE atomics, SWP, CAS: one host atomic read-modify-write on the guest address (__atomic builtins, seq_cst; on
    x86-64 every locked RMW is a full barrier anyway, so the A/L ordering bits cost nothing and are all honoured).
    MIN/MAX are a compare-exchange loop. Rt = XZR (the ST* aliases) discards the old value.
  - CASP: 64-bit or 128-bit compare-exchange (the 128-bit one needs cmpxchg16b: build with -mcx16 / x86-64-v2+).
  - LDAPR*: acquire load (a plain load on x86-64, ordered for the compiler).
  - Pointer authentication is the identity, as in Eglantine's dynarmic: PAC*/AUT* (register forms) and XPAC* leave
    Xd unchanged, PACGA writes 0. Nothing in the game's own code can tell, since no pointer is ever signed.
  - MTE: tags are always 0. STG/ST2G only do their base-register writeback.
"""
import a64emit
import a64_lift

LSE_OPS = ["add", "clr", "eor", "set", "smax", "smin", "umax", "umin"]
UTYPE = {0: "uint8_t", 1: "uint16_t", 2: "uint32_t", 3: "uint64_t"}
STYPE = {0: "int8_t", 1: "int16_t", 2: "int32_t", 3: "int64_t"}
MASK = {0: "0xFFULL", 1: "0xFFFFULL", 2: "0xFFFFFFFFULL", 3: "0xFFFFFFFFFFFFFFFFULL"}


def xr(n):
    """C for reading Xn where 31 is XZR"""
    return "0ULL" if n == 31 else "c->x[%d]" % n


def xw(n, value):
    """C for writing Xn where 31 is XZR (the value is dropped)"""
    return "" if n == 31 else "c->x[%d] = %s;" % (n, value)


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# ---- control flow ----------------------------------------------------------------------------------------
def classify_ext(w, pc):
    if w in (0xD65F0BFF, 0xD65F0FFF):                 # RETAA / RETAB
        return ("ret", 30)
    if (w & 0xFEFFF800) == 0xD61F0800:                # BRAA, BRAB, BRAAZ, BRABZ
        return ("br", (w >> 5) & 31)
    if (w & 0xFEFFF800) == 0xD63F0800:                # BLRAA, BLRAB, BLRAAZ, BLRABZ
        return ("blr", (w >> 5) & 31)
    return None


# ---- data ----------------------------------------------------------------------------------------------
def lse(w):
    size, a, r = w >> 30, (w >> 23) & 1, (w >> 22) & 1
    rs, o3, opc, rn, rt = (w >> 16) & 31, (w >> 15) & 1, (w >> 12) & 7, (w >> 5) & 31, w & 31
    T, S = UTYPE[size], STYPE[size]
    n = 1 << size
    head = "{ uint64_t _a = c->x[%d]; %s *_p = (%s *)A64_PTR(_a); " % (rn, T, T)
    if o3 == 1 and opc == 4 and a == 1 and r == 0 and rs == 31:          # LDAPR(B/H)
        return head + "%s }" % xw(rt, "(uint64_t)__atomic_load_n(_p, __ATOMIC_ACQUIRE)")
    if o3 == 1 and opc == 0:                                             # SWP
        return head + "%s _o = __atomic_exchange_n(_p, (%s)%s, __ATOMIC_SEQ_CST); A64_MARK(_a, %d); %s }" % (
            T, T, xr(rs), n, xw(rt, "(uint64_t)_o"))
    if o3 != 0:
        return None
    op = LSE_OPS[opc]
    s = "(%s)%s" % (T, xr(rs))
    if op in ("add", "clr", "eor", "set"):
        fn, arg = {"add": ("__atomic_fetch_add", s), "clr": ("__atomic_fetch_and", "(%s)~%s" % (T, s)),
                   "eor": ("__atomic_fetch_xor", s), "set": ("__atomic_fetch_or", s)}[op]
        body = "%s _o = %s(_p, %s, __ATOMIC_SEQ_CST);" % (T, fn, arg)
    else:
        cmp = {"smax": "(%s)_o > (%s)_s" % (S, S), "smin": "(%s)_o < (%s)_s" % (S, S),
               "umax": "_o > _s", "umin": "_o < _s"}[op]
        body = ("%s _s = %s, _o = __atomic_load_n(_p, __ATOMIC_RELAXED), _n; "
                "do { _n = (%s) ? _o : _s; } while (!__atomic_compare_exchange_n(_p, &_o, _n, 1, __ATOMIC_SEQ_CST, "
                "__ATOMIC_RELAXED));" % (T, s, cmp))
    return head + body + " A64_MARK(_a, %d); %s }" % (n, xw(rt, "(uint64_t)_o"))


def cas(w):
    size, rs, rn, rt = w >> 30, (w >> 16) & 31, (w >> 5) & 31, w & 31
    T = UTYPE[size]
    return ("{ uint64_t _a = c->x[%d]; %s _e = (%s)%s; __atomic_compare_exchange_n((%s *)A64_PTR(_a), &_e, (%s)%s, 0, "
            "__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); A64_MARK(_a, %d); %s }"
            % (rn, T, T, xr(rs), T, T, xr(rt), 1 << size, xw(rs, "(uint64_t)_e")))


def casp(w):
    sz, rs, rn, rt = (w >> 30) & 1, (w >> 16) & 31, (w >> 5) & 31, w & 31
    if rs & 1 or rt & 1:
        return None
    if not sz:   # two 32-bit registers as one 64-bit word, the first register at the lower address
        return ("{ uint64_t _a = c->x[%d]; uint64_t _e = (uint32_t)%s | (uint64_t)(uint32_t)%s << 32; "
                "uint64_t _n = (uint32_t)%s | (uint64_t)(uint32_t)%s << 32; "
                "__atomic_compare_exchange_n((uint64_t *)A64_PTR(_a), &_e, _n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); "
                "A64_MARK(_a, 8); %s %s }" % (rn, xr(rs), xr(rs + 1), xr(rt), xr(rt + 1),
                                              xw(rs, "(uint32_t)_e"), xw(rs + 1, "_e >> 32")))
    return ("{ uint64_t _a = c->x[%d]; unsigned __int128 _e = (unsigned __int128)%s | (unsigned __int128)%s << 64; "
            "unsigned __int128 _n = (unsigned __int128)%s | (unsigned __int128)%s << 64; "
            "__atomic_compare_exchange_n((unsigned __int128 *)A64_PTR(_a), &_e, _n, 0, __ATOMIC_SEQ_CST, "
            "__ATOMIC_SEQ_CST); A64_MARK(_a, 16); %s %s }" % (rn, xr(rs), xr(rs + 1), xr(rt), xr(rt + 1),
                                                               xw(rs, "(uint64_t)_e"), xw(rs + 1, "(uint64_t)(_e >> 64)")))


def stg(w):
    """STG / ST2G with writeback (tags are not modelled: the only effect is the base register update)"""
    opc, imm, op2, rn = (w >> 22) & 3, sx((w >> 12) & 0x1FF, 9) << 4, (w >> 10) & 3, (w >> 5) & 31
    if opc not in (0, 2) or op2 == 0:
        return None
    if op2 == 2:   # signed offset, no writeback
        return "/* stg: memory tags are not modelled */"
    return "c->x[%d] += (uint64_t)(int64_t)%d; /* st%sg, writeback: tags are not modelled */" % (
        rn, imm, "2" if opc == 2 else "")


# ---- the few SIMD forms suyu lacks that MKW's sdk uses (62 sites) -------------------------------------
# Saturations set FPSR.QC (bit 27), as the hardware does.
QC = "c->fpsr |= 1u << 27;"


def sqdmlal(w):
    """SQDMLAL{2} (vector and by element): Vd.(2e) = sat(Vd + sat(2 * Vn.e * Vm.e))"""
    q, size, rn, rd = (w >> 30) & 1, (w >> 22) & 3, (w >> 5) & 31, w & 31
    if size not in (1, 2):
        return None
    if (w & 0xBF20FC00) == 0x0E209000:      # vector
        rm, idx = (w >> 16) & 31, None
    else:                                   # by element
        L, M, H = (w >> 21) & 1, (w >> 20) & 1, (w >> 11) & 1
        if size == 1:
            rm, idx = (w >> 16) & 15, H << 2 | L << 1 | M
        else:
            rm, idx = (w >> 16) & 31, H << 1 | L
    e, n = (16, 4) if size == 1 else (32, 2)
    T, W = ("int16_t", "int32_t") if size == 1 else ("int32_t", "int64_t")
    lo = n * q    # the "2" form reads the upper half of Vn (and Vm, vector form)
    b = "_m[%d]" % idx if idx is not None else "_m[%d + _i]" % lo
    return ("{ %s _n[%d], _m[%d]; %s _d[%d]; memcpy(_n, c->vreg[%d], 16); memcpy(_m, c->vreg[%d], 16); "
            "memcpy(_d, c->vreg[%d], 16); for (int _i = 0; _i < %d; _i++) { %s _p; "
            "if (_n[%d + _i] == (%s)(-%dLL - 1) && %s == (%s)(-%dLL - 1)) { _p = (%s)((1ULL << %d) - 1); %s } "
            "else _p = (%s)2 * _n[%d + _i] * %s; "
            "if (__builtin_add_overflow(_d[_i], _p, &_d[_i])) { _d[_i] = _p < 0 ? (%s)(-%dLL - 1) - 0 : (%s)((1ULL << %d) - 1); %s } } "
            "memcpy(c->vreg[%d], _d, 16); }"
            % (T, 2 * n, 2 * n, W, n, rn, rm, rd, n, W, lo, T, (1 << (e - 1)) - 1, b, T, (1 << (e - 1)) - 1,
               W, 2 * e - 1, QC, W, lo, b, W, (1 << (2 * e - 1)) - 1, W, 2 * e - 1, QC, rd))


def sqshl_imm(w):
    """SQSHL / SQSHLU (vector, immediate)"""
    q, u, immh, immb, rn, rd = (w >> 30) & 1, (w >> 29) & 1, (w >> 19) & 15, (w >> 16) & 7, (w >> 5) & 31, w & 31
    if not immh or (immh & 8 and not q):
        return None
    e = 8 << (immh.bit_length() - 1)
    shift = (immh << 3 | immb) - e
    lanes = (16 if q else 8) * 8 // e
    S = {8: "int8_t", 16: "int16_t", 32: "int32_t", 64: "int64_t"}[e]
    U = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}[e]
    smax, smin, umax = (1 << (e - 1)) - 1, -(1 << (e - 1)), (1 << e) - 1
    if u:
        body = ("if (_x < 0) { _r[_i] = 0; if (_x) %s } else { __int128 _v = (__int128)_x * ((__int128)1 << %d); "
                "if (_v > (__int128)%dULL) { _r[_i] = (%s)%dULL; %s } else _r[_i] = (%s)_v; }"
                % (QC, shift, umax, U, umax, QC, U))
        out = U
    else:
        body = ("__int128 _v = (__int128)_x * ((__int128)1 << %d); if (_v > (__int128)%dLL) { _r[_i] = (%s)%dLL; %s } "
                "else if (_v < (__int128)(%dLL - 1)) { _r[_i] = (%s)(%dLL - 1); %s } else _r[_i] = (%s)_v;"
                % (shift, smax, S, smax, QC, smin + 1, S, smin + 1, QC, S))
        out = S
    return ("{ %s _a[%d]; %s _r[%d]; memcpy(_a, c->vreg[%d], 16); for (int _i = 0; _i < %d; _i++) { %s _x = _a[_i]; %s } "
            "c->vreg[%d][0] = 0; c->vreg[%d][1] = 0; memcpy(c->vreg[%d], _r, %d); }"
            % (S, 128 // e, out, 128 // e, rn, lanes, S, body, rd, rd, rd, lanes * e // 8))


def fneg_h(w):
    """FNEG (vector, half precision): flip each lane's sign bit (no exceptions, NaNs keep their payload)"""
    q, rn, rd = (w >> 30) & 1, (w >> 5) & 31, w & 31
    m = "0x8000800080008000ULL"
    return ("{ uint64_t _lo = c->vreg[%d][0] ^ %s, _hi = %s; c->vreg[%d][0] = _lo; c->vreg[%d][1] = _hi; }"
            % (rn, m, ("c->vreg[%d][1] ^ %s" % (rn, m)) if q else "0", rd, rd))


def dot(w):
    """SDOT / UDOT (vector, and by element; FEAT_DotProd): each 32-bit lane of Vd += the sum of the four products of
    the matching bytes of Vn and Vm (Vm's lane `index` for the element form), wrapping. OoT: 4 sites in main."""
    q, u, rm, rn, rd = (w >> 30) & 1, (w >> 29) & 1, (w >> 16) & 31, (w >> 5) & 31, w & 31
    elem = (w >> 24) & 1
    T = "uint8_t" if u else "int8_t"
    if elem:
        index = ((w >> 11) & 1) << 1 | ((w >> 21) & 1)   # (H:L; Rm is M:Rm, all 5 bits, for size 10)
        mb = "_m[4 * %d + _j]" % index
    else:
        mb = "_m[4 * _i + _j]"
    lanes = 4 if q else 2
    return ("{ %s _n[16], _m[16]; uint32_t _d[4]; memcpy(_n, c->vreg[%d], 16); memcpy(_m, c->vreg[%d], 16); "
            "memcpy(_d, c->vreg[%d], 16); for (int _i = 0; _i < %d; _i++) { int32_t _s = 0; "
            "for (int _j = 0; _j < 4; _j++) _s += (int32_t)_n[4 * _i + _j] * (int32_t)%s; _d[_i] += (uint32_t)_s; } "
            "c->vreg[%d][1] = 0; memcpy(c->vreg[%d], _d, %d); }"
            % (T, rn, rm, rd, lanes, mb, rd, rd, lanes * 4))


def ldar_stlr(w):
    """LDAR / STLR (and the LO forms LDLAR / STLLR) as host atomics.

    suyu translates them as plain loads and stores: right for x86's own ordering, but the C compiler may then move
    other guest accesses across them, or hoist an LDAR out of a spin loop. And an STLR followed by an LDAR must not be
    reordered (AArch64's release/acquire are RCsc), which x86 does to a store and a later load unless the store is
    locked. So: LDAR = seq_cst load (a plain mov), STLR = seq_cst store (xchg). MKW: 1,900 LDAR, 1,321 STLR."""
    size, load, rn, rt = w >> 30, (w >> 22) & 1, (w >> 5) & 31, w & 31
    T = UTYPE[size]
    if load:
        v = "(uint64_t)__atomic_load_n((%s *)A64_PTR(_a), __ATOMIC_SEQ_CST)" % T
        return "{ uint64_t _a = c->x[%d]; %s }" % (rn, xw(rt, v) if rt != 31 else "(void)" + v + ";")
    return ("{ uint64_t _a = c->x[%d]; __atomic_store_n((%s *)A64_PTR(_a), (%s)%s, __ATOMIC_SEQ_CST); A64_MARK(_a, %d); }"
            % (rn, T, T, xr(rt), 1 << size))


def emit(w):
    """C for one of the forms above, or None (then suyu translates it)"""
    if (w & 0x3FBF7C00) == 0x089F7C00:                # LDAR, STLR, LDLAR, STLLR (o0 = bit 15)
        return ldar_stlr(w)
    if (w & 0xBF20FC00) == 0x0E209000 or (w & 0xBF00F400) == 0x0F003000:
        return sqdmlal(w)
    if (w & 0xBF80FC00) in (0x0F007400, 0x2F006400):
        return sqshl_imm(w)
    if (w & 0xBFFFFC00) == 0x2EF8F800:
        return fneg_h(w)
    if (w & 0x9FE0FC00) == 0x0E809400 or (w & 0x9FC0F400) == 0x0F80E000:   # SDOT / UDOT (vector, by element)
        return dot(w)
    if (w & 0x3F200C00) == 0x38200000:
        return lse(w)
    if (w & 0x3FA07C00) == 0x08A07C00:
        return cas(w)
    if (w & 0xBFA07C00) == 0x08207C00:
        return casp(w)
    if (w & 0xFFE0FC00) == 0x9AC03000:                # PACGA Xd, Xn, Xm
        return xw(w & 31, "0") or ";"
    if (w & 0xFFFFFBE0) == 0xDAC143E0:                # XPACI / XPACD
        return "/* xpac: pointers carry no PAC */"
    if (w & 0xFFFFC000) == 0xDAC10000:                # PACIA/IB/DA/DB, AUTIA/IB/DA/DB (+ Z forms)
        return "/* pac/aut: pointer authentication is the identity */"
    if (w & 0xFF200000) == 0xD9200000:
        return stg(w)
    return None


def is_identity_model(w):
    """Forms whose result here is by design not the hardware's (PAC, MTE): instruction tests skip them."""
    return ((w & 0xFFE0FC00) == 0x9AC03000 or (w & 0xFFFFFBE0) == 0xDAC143E0 or (w & 0xFFFFC000) == 0xDAC10000
            or (w & 0xFF200000) == 0xD9200000)


# ---- install -------------------------------------------------------------------------------------------
_installed = False


def install():
    global _installed
    if _installed:
        return
    _installed = True
    base_classify = a64_lift.classify
    base_translate = a64emit.translate

    def classify(w, pc):
        return classify_ext(w, pc) or base_classify(w, pc)

    def translate(items, batch=65536):
        mine = {}
        rest = []
        for k, (pc, w) in enumerate(items):
            t = emit(w)
            if t is None:
                rest.append(k)
            else:
                mine[k] = t
        texts, flags = [None] * len(items), [0] * len(items)
        if rest:
            tx, fl = base_translate([items[k] for k in rest], batch)
            for k, t, f in zip(rest, tx, fl):
                texts[k], flags[k] = t, f
        for k, t in mine.items():
            texts[k] = t + "\n"
        return texts, flags

    a64_lift.classify = classify
    a64emit.translate = translate
