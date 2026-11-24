# Copyright (C) 2026 Sacrappy
"""(ProjectReLink, pixl27; ported by LIFT2) nx2/nativefp.py, game-free: A64_FPX_EASY bit-exact with the software bodies
(result and FPSR), A64_NATIVE_FP's helpers equal to the bodies in value with MXCSR's FTZ / DAZ following FPCR.FZ (the
smallest normal's rounding edge under FZ counted apart), the exact wrappers' values unchanged under that MXCSR, and the
lift's text: the options off leave a64_fpx.h and a hot function's text as they were, to the byte.

Built like tests/test_fpxkeep.py (a64emit from its source, a64_suyu.h / a64_suyu.c by a64_rtgen, a64_fpx.h by a64_lift.fpx_header
with nx2/fpxkeep.py and nx2/nativefp.py installed as nx2/lift.py installs them), and tests/nativefp_test.c compiled against
the real rt/a64.h at the lifted code's FP flags, at -march=x86-64-v3 (a host FMA, the FP blocks) and x86-64-v2. Then
deliberately broken helpers, each built at v3 and run on a smaller set, must fail, and the driver run with MXCSR left at its
default must find mismatches (FTZ and DAZ are needed).

    python -I -B tests/test_native_fp.py [--cc clang] [--cxx clang++] [--n 300000] [--no-faults]

Skipped (exit 0) on a host that is not x86-64 or has no C/C++ compiler; a build this CPU cannot run is skipped.
"""
import argparse
import os
import platform
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import test_fpx_sticky as fs  # noqa: E402  (its compiler, target, build and run helpers; its paths on sys.path)

SEED = 20261123
BUILDS = (("x86-64-v3", 1), ("x86-64-v2", 0))
SHAPES = {("add32", "add32", 2, 2), ("sub32", "sub32", 2, 2), ("mul32", "fma32", 2, 3), ("div32", "div32", 2, 2),
          ("sqrt32", "sqrt32", 1, 1), ("fma32", "fma32", 3, 3), ("add64", "add64", 2, 2), ("sub64", "sub64", 2, 2),
          ("mul64", "fma64", 2, 3), ("div64", "div64", 2, 2), ("sqrt64", "sqrt64", 1, 1), ("fma64", "fma64", 3, 3)}
# (name, text in the generated a64_fpx.h, its broken form, the driver's mode that must fail)
FAULTS = (
    ("easy: an overflow without OFC", "if (m == 0x7f800000u) fl |= 4u | 16u;   /* (overflow: OFC and IXC) */",
     "if (m == 0x7f800000u) fl |= 16u;", "easy"),
    ("easy: inf - inf as the host's default NaN", "t = 0x7fc00000u, fl |= 1u;   /* inf - inf */", "t = 0xffc00000u, fl |= 1u;",
     "easy"),
    ("easy: a tiny product under FZ not flushed", "if (fz) t = sign, fl |= 8u;", "if (fz) fl |= 8u;", "easy"),
    ("native: a scalar NaN result kept", "if (RECOMP_LIKELY(r == r)) goto done;", "goto done;", "native"),
    ("native: a vector NaN lane kept", "if (RECOMP_LIKELY(!(_mm_movemask_ps(odd) & (lanes)))) {", "if (1) {", "native"),
    ("native: a block's NaN result kept",
     "*bad = _mm_or_si128(*bad, _mm_and_si128(A64_BK_I(_mm_cmpunord_ps(A64_BK_F(t), A64_BK_F(t))), lanes));", "(void)bad;",
     "native"),
    ("native: FCVTZS's out-of-range answer kept", "if (RECOMP_LIKELY(r != INT32_MIN)) return (uint32_t)r;",
     "return (uint32_t)r;", "native"),
    ("native: UCVTF from 64 bits by the signed conversion", "        if (is_signed) {\n            const int64_t v = (int64_t)bits;",
     "        {\n            const int64_t v = (int64_t)bits;", "native"),
)


def generate(cxx, tmp, flags):
    """a64_suyu.h / .c and a64_fpx.h (fpxkeep, A64_NATIVE_FP, A64_FPX_EASY on), with the checks of the options off and of the
    hot text's rewrite"""
    dll = os.path.join(tmp, "a64emit.dll" if os.name == "nt" else "liba64emit.so")
    fs.build([cxx, "-std=c++20", "-O0", "-shared", "-Wno-everything", "-o", dll, os.path.join(fs.KIT, "lift", "a64emit.cpp")]
             + flags + ([] if os.name == "nt" else ["-fPIC"]), "a64emit")
    import build_emit
    build_emit.build = lambda force=False: dll
    import a64_lift
    import a64_rtgen
    import a64emit
    import fpxkeep
    import nativefp
    fpxkeep.install()
    base = a64_lift.fpx_header(SHAPES)
    base_hot = a64_lift.hot_text
    saved = {k: os.environ.get(k) for k in ("A64_NATIVE_FP", "A64_FPX_EASY", "A64_LOCAL_VREGS")}
    try:
        for k in saved:   # (on by default since 2026-11-23: unset is on, =0 off)
            os.environ.pop(k, None)
        if not (nativefp.native_fp() and nativefp.fpx_easy() and nativefp.local_vregs()):
            raise SystemExit("nativefp: A64_NATIVE_FP / A64_FPX_EASY / A64_LOCAL_VREGS are not on by default")
        for k in saved:
            os.environ[k] = "0"
        if nativefp.native_fp() or nativefp.fpx_easy() or nativefp.local_vregs():
            raise SystemExit("nativefp: =0 does not turn an option off")
        nativefp.install()
        if a64_lift.fpx_header(SHAPES) != base or a64_lift.hot_text is not base_hot:
            raise SystemExit("nativefp: the options off changed a64_fpx.h or the hot text")
        nativefp._installed = False
        os.environ["A64_NATIVE_FP"] = "1"
        os.environ["A64_FPX_EASY"] = "1"
        nativefp.install()
        header = a64_lift.fpx_header(SHAPES)
        check_text(nativefp)
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    if header == base:
        raise SystemExit("nativefp: not on")
    for (fast, exact), (_, _, call) in nativefp.EASY.items():
        line = [x for x in header.split("\n") if x.startswith("A64_FPX_FN uint64_t a64_fpx_%s_%s(" % (fast, exact))]
        if len(line) != 1 or (call + "} return recomp_fpe_") not in line[0]:
            raise SystemExit("nativefp: the wrapper a64_fpx_%s_%s has no easy rescue last: %s" % (fast, exact, line))
    try:
        a64_rtgen.write(tmp)
    finally:
        if a64emit._lib is not None:
            import _ctypes
            (_ctypes.FreeLibrary if os.name == "nt" else _ctypes.dlclose)(a64emit._lib._handle)
            a64emit._lib = None
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(header)
    return header


def check_text(nativefp):
    """the hot text's rewrite: names and the blocks' gate only, nothing in a function the text edits read (fastvec.PLAIN0),
    and MSR FPCR followed by the host's sync"""
    import fastvec
    gate = "if (RECOMP_LIKELY(RECOMP_FPX_OPEN(c))) {\nconst __m128i _fzm = a64_bk_fzm(c); __m128i _bad = _mm_setzero_si128();"
    text = ("void f_80001000(Ctx *__restrict A64_NOESC c) {\n"
            "    { uint64_t _a=0,_b=0,_v; _v=a64_fpx_hot_add32_add32(c,_a,_b); }\n"
            "    a64_vfpx_mul32(c,c->vreg[1],c->vreg[2],c->vreg[3],4); a64_vfpx_mul32e(c,c->vreg[1],c->vreg[2],c->vreg[3],1);\n"
            "    c->x[8]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);\n"
            "{ /* (TICK-063) FP block: 2 ops, 2 arithmetic */\n#if A64_BK\nint _bk = 0;\n" + gate + "\n"
            "_bv1 = a64_bk_vadd(&_bad, _fzm, _bv2, _bv3, A64_BK_L4);\n_bv1 = a64_bk_sfma(&_bad, _fzm, _bv1, _bv2, _bv3);\n"
            "_bfl = a64_bk_fcmp(&_bad, _fzm, _bv1, _bv2);\n}\n#endif\n}\n}\n")
    out = nativefp.native_text(text)
    want = (text.replace("a64_fpx_hot_add32_add32(c,", "a64_fpx_nat_add32_add32(c,")
            .replace("a64_vfpx_mul32(c,", "a64_vfpx_nat_mul32(c,").replace("recomp_fp_to_int(", "a64_nat_fp_to_int(")
            .replace("RECOMP_FPX_OPEN(c)", "A64_NAT_OPEN_BK(c)")
            .replace("a64_bk_vadd(&_bad", "a64_bk_nvadd(&_bad").replace("a64_bk_sfma(&_bad", "a64_bk_nsfma(&_bad"))
    if out != want:
        raise SystemExit("nativefp: the hot text's rewrite:\n%s\nwanted:\n%s" % (out, want))
    import a64_lift
    fastvec.PLAIN0.add(0x80001000)
    try:
        kept = a64_lift.hot_text(text)
        if "a64_fpx_nat_" in kept or "A64_NAT_OPEN" in kept:
            raise SystemExit("nativefp: a function the text edits read (PLAIN0) was given the native names")
    finally:
        fastvec.PLAIN0.discard(0x80001000)
    # (LIFT3, A64_NATIVE_FP_COLD) a cold function's text: its outlined exact helpers take the native names; nothing else changes
    cold = ("void f_80002000(Ctx *__restrict A64_NOESC c) {\n"
            "    { uint64_t _a=0,_b=0,_z=0,_v; _v=a64_fpx_mul32_fma32(c,_a,_b,_z); _v=a64_fpx_fma64_fma64(c,_a,_b,_z); }\n"
            "    a64_vfpx_add32(c,c->vreg[1],c->vreg[2],c->vreg[3],2); a64_fpx_sticky32(c,0,0,0,1,&_v);\n"
            "    c->x[8]=recomp_fixed_to_fp(c->x[9],32,32,0,1,c->fpcr,&c->fpsr);\n}\n")
    want = (cold.replace("a64_fpx_mul32_fma32(c,", "a64_fpx_nat_mul32_fma32(c,").replace("a64_fpx_fma64_fma64(c,", "a64_fpx_nat_fma64_fma64(c,")
            .replace("a64_vfpx_add32(c,", "a64_vfpx_nat_add32(c,").replace("recomp_fixed_to_fp(", "a64_nat_fixed_to_fp("))
    if nativefp.cold_text(cold) != want:
        raise SystemExit("nativefp: the cold text's rewrite:\n%s\nwanted:\n%s" % (nativefp.cold_text(cold), want))
    if nativefp.cold_text(text) != text.replace("a64_vfpx_mul32(c,", "a64_vfpx_nat_mul32(c,").replace("recomp_fp_to_int(", "a64_nat_fp_to_int("):
        raise SystemExit("nativefp: the cold rewrite touched a hot helper's name or a block")
    msr = "    c->fpcr = (x5_ & 0xffffffffULL) | (c->fpcr & RECOMP_FPX_INHIBIT);\n"
    if nativefp.fpcr_text(msr) != msr.rstrip("\n") + " { extern void hos_fpcr_sync(Ctx *A64_NOESC); hos_fpcr_sync(c); }\n":
        raise SystemExit("nativefp: MSR FPCR not followed by hos_fpcr_sync: %r" % nativefp.fpcr_text(msr))


def compile_driver(cc, tmp, march, inc, name):
    exe = os.path.join(tmp, "nativefp_test_%s.exe" % name)
    fs.build([cc] + fs.target_flags(cc, tmp) + ["-march=" + march] + fs.FLAGS +
             ["-D_CRT_SECURE_NO_WARNINGS", "-I" + inc, "-I" + tmp, "-I" + os.path.join(fs.KIT, "rt"),
              os.path.join(ROOT, "tests", "nativefp_test.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe],
             "the driver " + name)
    return exe


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    p.add_argument("--n", type=int, default=300000)
    p.add_argument("--no-faults", action="store_true")
    a = p.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        fs.skip("not an x86-64 host: %s" % platform.machine())
    if not os.environ.get("HOOKSHOT_MINGW") and os.name == "nt":   # (an llvm-mingw on PATH, as test_fpx_sticky.py)
        w = shutil.which("clang++")
        if w and os.path.isfile(os.path.join(os.path.dirname(w), "x86_64-w64-mingw32-clang++.exe")):
            os.environ["HOOKSHOT_MINGW"] = os.path.dirname(os.path.dirname(w))
    cc = fs.compiler(a.cc, "CC", ("clang", "gcc", "cc"), "clang")
    cxx = fs.compiler(a.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
    if not cc or not cxx:
        fs.skip("no C compiler" if not cc else "no C++ compiler")
    t0 = time.time()
    failed, ran = [], []
    with tempfile.TemporaryDirectory(prefix="lift2-nativefp-") as tmp:
        probe = os.path.join(tmp, "fpxkeep_cpu.exe")
        fs.build([cc] + fs.target_flags(cc, tmp) + ["-march=x86-64", os.path.join(ROOT, "tests", "fpxkeep_cpu.c"), "-o", probe],
                 "the CPU feature probe")
        supported = {march: fs.run([probe, str(fma)]).returncode == 0 for march, fma in BUILDS}
        header = generate(cxx, tmp, fs.target_flags(cxx, tmp))
        for march, _ in BUILDS:
            if not supported[march]:
                print("%-10s skipped (this CPU or OS does not support %s)" % (march, march))
                continue
            exe = compile_driver(cc, tmp, march, tmp, march)
            for mode in ("easy", "native"):
                r = fs.run([exe, mode, str(a.n), str(SEED)], timeout=3600)
                print("%-10s %s" % (march, r.stdout.rstrip()))
                ran.append(march)
                if r.returncode:
                    failed.append("%s %s (exit %d)" % (march, mode, r.returncode))
            r = fs.run([exe, "nomxcsr", str(a.n // 4), str(SEED)], timeout=3600)
            print("%-10s %s" % (march, r.stdout.rstrip().splitlines()[-1] if r.stdout.strip() else r.returncode))
            if r.returncode != 1:
                failed.append("%s: native ops with MXCSR at its default found no mismatch (exit %d)" % (march, r.returncode))
        if not a.no_faults and "x86-64-v3" in ran:
            for i, (what, old, new, mode) in enumerate(FAULTS):
                if header.count(old) < 1:
                    raise SystemExit("nativefp: the fault %r: its text is not in a64_fpx.h" % what)
                d = os.path.join(tmp, "fault%d" % i)
                os.makedirs(d)
                with open(os.path.join(d, "a64_fpx.h"), "w", newline="\n") as f:
                    f.write(header.replace(old, new))
                exe = compile_driver(cc, tmp, "x86-64-v3", d, "fault%d" % i)
                code = fs.run([exe, mode, str(max(20000, a.n // 10)), str(SEED)], timeout=3600).returncode
                print("fault %-55s %s (exit %d)" % (what, "caught" if code == 1 else "NOT CAUGHT", code))
                if code != 1:
                    failed.append("fault not caught: " + what)
    if not ran:
        fs.skip("this CPU is not x86-64-v2")
    if failed:
        raise SystemExit("nativefp: FAIL (%s) in %.1f s" % (", ".join(failed), time.time() - t0))
    print("nativefp: pass in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
