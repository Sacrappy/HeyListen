"""(HeyListen TICK-063) A64_FPX_BLOCK=1 (a64_block.block_text / block_header), game-free: bit-exact with the default lowering.

Synthetic AArch64 sequences (assembled by llvm-mingw's clang for aarch64, no game code) go through a64emit.translate,
a64_lift.op_text and hot_text twice: with the switch off (the default lowering) and on (the runs of FP statements as
speculative blocks). Both versions are compiled in one file against the header the switch writes (which keeps every default
helper) and the build's own guest model (nx2/rtgen.py: the 64-bit window, the soft write watch, the polls).
tests/fpx_block_test.c runs both on random registers (all 32 vector registers: zeros, subnormals, infinities, quiet and
signalling NaNs, the extreme normals, binary32 midpoints), random guest memory (aligned and unaligned addresses), random
FPCR (FZ, DN, the rounding modes, other control bits, the host inhibit bit) and FPSR states, and compares the whole Ctx and
the guest memory. Built at -march=x86-64-v3 (the blocks: FMA3) and x86-64-v2 (no FMA3: the blocks compile out, the
fallback alone). The sequences cover every shape the pass knows (vector FADD / FSUB / FMUL, by element, FMLA / FMLS,
scalar FADD / FSUB / FMUL / FNMUL / FDIV / FMADD / FMSUB / FNMADD / FNMSUB / FSQRT, FADDP, DUP, INS, EXT, ZIP / UZP / TRN,
ORR / AND / EOR, FNEG / FABS / FMOV, LDR S / D / Q, LDP S / D / Q), the NavMesh crowd-step kernel's own sequence, aliased
registers, runs broken by stores and compares, and a run that reads what the one before wrote.

It also checks that the switch off leaves the lifter's output as before (fpx_header and hot_text give the text they gave
without the switch), and that the block path was taken in a fair share of the runs (not only its fallback).

    python -I -B tests/test_fpx_block.py [--cases 2000000] [--cc clang] [--cxx clang++]

Teeth (each by hand, then restored; 100,000 runs at v3): the product keep test without its FZ operand test: 100 mismatches;
FMLS's negation dropped: 122; EXT's byte map shifted by one: 577; the sum's FZ "big or zero" test dropped: 4; the scalar FMA's
addend FZ test dropped: 3; FCMP's unordered test dropped: 100; FCMP's FZ subnormal test dropped: 18; a general register
written in a block but not committed: 981.
Skipped (exit 0) on a host that is not x86-64, without llvm-mingw's clang (the AArch64 assembler), or on a CPU without x86-64-v2.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "nx2"))
import test_fpx_sticky as sticky  # noqa: E402  (its compiler search, flags and header generation)

CASES, SEED = 2000000, 20261112
SWITCH = "A64_FPX_BLOCK"
# (name, assembly, whether the switch must change its text: 1 yes, 0 no, 2 either (A64_FPX_BLOCK_ST's cases))
GUEST = (
    ("vadd/vsub/vmul .4s chain", "fsub v1.4s, v21.4s, v20.4s\nfmul v2.4s, v7.4s, v1.4s\nfadd v3.4s, v2.4s, v1.4s\nfsub v4.4s, v3.4s, v2.4s", 1),
    ("vadd/vsub .2s", "fadd v1.2s, v2.2s, v3.2s\nfsub v4.2s, v1.2s, v2.2s\nfmul v5.2s, v4.2s, v1.2s", 1),
    ("fmul by element", "fmul v1.4s, v2.4s, v3.s[0]\nfmul v4.4s, v1.4s, v3.s[3]\nfmul v5.4s, v4.4s, v1.s[2]", 1),
    ("fmla/fmls by element", "fmla v1.4s, v2.4s, v3.s[1]\nfmls v4.4s, v1.4s, v3.s[2]\nfmla v4.4s, v4.4s, v4.s[0]", 1),
    ("fmla/fmls vector", "fmla v1.4s, v2.4s, v3.4s\nfmls v1.4s, v4.4s, v5.4s\nfmla v6.4s, v1.4s, v1.4s", 1),
    ("scalar mul/div/add/sub", "fmul s0, s1, s2\nfdiv s3, s0, s1\nfadd s4, s3, s2\nfsub s5, s4, s0\nfnmul s6, s5, s4", 1),
    ("scalar fma forms", "fmadd s0, s1, s2, s3\nfmsub s4, s0, s2, s3\nfnmadd s5, s4, s1, s0\nfnmsub s6, s5, s2, s4\nfmadd s3, s3, s3, s3", 1),
    ("fsqrt", "fmul s0, s1, s2\nfsqrt s3, s0\nfdiv s4, s1, s3\nfsqrt s5, s5", 1),
    ("faddp 2s/4s/scalar", "faddp v0.2s, v1.2s, v2.2s\nfaddp v3.4s, v0.4s, v4.4s\nfaddp s5, v3.2s\nfadd s6, s5, s0", 1),
    ("dup/ins/ext/zip", "dup v0.4s, v1.s[2]\ndup v2.2s, v1.s[1]\nins v0.s[1], v3.s[0]\next v4.16b, v0.16b, v3.16b, #12\n"
                        "zip1 v5.4s, v4.4s, v0.4s\nzip2 v6.4s, v4.4s, v0.4s\nuzp1 v7.4s, v5.4s, v6.4s\ntrn2 v8.4s, v7.4s, v5.4s\n"
                        "fmul v9.4s, v8.4s, v7.4s\nfadd v10.4s, v9.4s, v6.4s", 1),
    ("dup 2d/8b, zip 2d", "dup v0.2d, v1.d[1]\nzip1 v2.2d, v0.2d, v3.2d\ndup v4.8b, v3.b[5]\ndup s5, v1.s[3]\ndup d6, v1.d[0]\n"
                          "fadd v7.4s, v2.4s, v0.4s\nfmul v8.4s, v7.4s, v5.4s", 1),
    ("orr/and/eor/mov", "orr v0.16b, v1.16b, v2.16b\nand v3.16b, v0.16b, v4.16b\neor v5.16b, v3.16b, v1.16b\nmov v6.16b, v5.16b\n"
                        "fadd v7.4s, v6.4s, v0.4s\nfsub v8.4s, v7.4s, v3.4s", 1),
    ("fneg/fabs/fmov s and d", "fneg s0, s1\nfabs s2, s0\nfmov s3, s2\nfneg d4, d5\nfabs d6, d4\nfmov d7, d6\nfmov s8, w4\nfmov d9, x5\n"
                               "fadd s10, s0, s2\nfmul s11, s10, s8", 1),
    ("ins from zero", "fmul v0.4s, v1.4s, v2.4s\nmov v0.s[1], wzr\nfadd v3.4s, v0.4s, v2.4s", 1),
    ("loads q/s/d, ldp", "ldr q0, [x0]\nldr s1, [x1, #4]\nldr d2, [x2, #8]\nldp q3, q4, [x3, #16]\nldp s5, s6, [x0, #8]\nldp d7, d8, [x1, #16]\n"
                         "fmul v9.4s, v0.4s, v3.4s\nfadd v10.4s, v9.4s, v4.4s\nfmul s11, s1, s5\nfadd s12, s11, s6\nfmadd s13, s2, s7, s8", 1),
    ("loads with register offsets", "ldr q0, [x0, x8, lsl #4]\nldr q1, [x1, x9, lsl #4]\nfsub v2.4s, v0.4s, v1.4s\nfmul v3.4s, v2.4s, v2.4s", 1),
    ("navmesh crowd step", "ldr q20, [x1]\nldr q21, [x2]\nfsub v1.4s, v21.4s, v20.4s\nldr d17, [x3]\nldp q7, q6, [sp, #32]\n"
                           "fmul v2.4s, v7.4s, v1.4s\nfmul v1.4s, v6.4s, v1.4s\ndup v3.2s, v2.s[2]\nfaddp v2.2s, v2.2s, v2.2s\n"
                           "fadd v22.2s, v2.2s, v3.2s\ndup v2.2s, v1.s[2]\nfaddp v1.2s, v1.2s, v1.2s\nfadd v24.2s, v1.2s, v2.2s\n"
                           "fmul v1.2s, v22.2s, v22.2s\nfmul v2.2s, v24.2s, v24.2s\nfadd v1.2s, v1.2s, v2.2s", 1),
    ("vec3 kernel (a2 style)", "ldr q1, [x0]\nldr q0, [x1]\nldr q2, [x2]\nfsub v3.4s, v0.4s, v1.4s\nfsub v4.4s, v2.4s, v1.4s\n"
                               "ext v0.16b, v3.16b, v3.16b, #12\next v2.16b, v4.16b, v4.16b, #12\next v0.16b, v0.16b, v3.16b, #8\n"
                               "ext v2.16b, v2.16b, v4.16b, #8\nfmul v2.4s, v3.4s, v2.4s\nfmul v0.4s, v4.4s, v0.4s\nfsub v0.4s, v2.4s, v0.4s\n"
                               "mov v7.16b, v0.16b\next v2.16b, v0.16b, v0.16b, #4\nfmul v6.2s, v0.2s, v0.2s\nmov v7.s[1], wzr\n"
                               "mov v0.16b, v2.16b\nfmul v16.2s, v2.2s, v2.2s\nfaddp s8, v6.2s\nfadd s9, s8, s16", 1),
    # runs broken by a store, a compare, a general-register write: two blocks, the statements between left alone
    ("store between runs", "fmul v0.4s, v1.4s, v2.4s\nfadd v3.4s, v0.4s, v1.4s\nstr q3, [x1]\nfsub v4.4s, v3.4s, v0.4s\nfmul v5.4s, v4.4s, v4.4s\nstr s5, [x1, #16]", 1),
    ("compare between runs", "fmul s0, s1, s2\nfadd s3, s0, s1\nfcmp s3, s2\nfsub s4, s3, s0\nfmul s5, s4, s4\nfcmp s5, #0.0\nfadd s6, s5, s1\nfmul s7, s6, s2", 1),
    ("fmov to gpr between runs", "fmul v0.4s, v1.4s, v2.4s\nfadd v3.4s, v0.4s, v1.4s\nfmov w4, s3\nfsub v4.4s, v3.4s, v0.4s\nfmul v5.4s, v4.4s, v4.4s\nfmov s6, w4\nfadd s7, s6, s5", 1),
    ("aliased registers", "fmul v0.4s, v0.4s, v0.4s\nfadd v0.4s, v0.4s, v0.4s\nfmla v0.4s, v0.4s, v0.s[1]\nfaddp v0.2s, v0.2s, v0.2s\nfmul s0, s0, s0\nfmadd s0, s0, s0, s0", 1),
    ("read back through memory", "fmul v0.4s, v1.4s, v2.4s\nfadd v3.4s, v0.4s, v1.4s\nstr q3, [x0]\nldr q4, [x0]\nfmul v5.4s, v4.4s, v3.4s\nfsub v6.4s, v5.4s, v4.4s", 1),
    ("fcmp ends a block", "fmul s0, s1, s2\nfadd s3, s0, s1\nfcmp s3, s2\nfsub s4, s3, s0\nfmul s5, s4, s4\nfcmp s5, #0.0\nfadd s6, s5, s1\nfmul s7, s6, s2\nfcmp s7, s6", 1),
    ("fcmp of registers the block did not compute", "fmul s0, s1, s2\nfadd s3, s0, s1\nfcmp s4, s5\nfmul s6, s3, s1\nfsub s7, s6, s0\nfcmp s8, #0.0", 1),
    ("fmov immediates","fmov s0, #1.0\nfmov d1, #2.5\nmovi v2.2d, #0\nfmov v3.4s, #-0.5\nfmul s4, s0, s5\nfadd s6, s4, s3\nfmul v7.4s, v3.4s, v2.4s\nfadd v8.4s, v7.4s, v1.4s", 1),
    ("zip/ext 8-byte, fneg vector", "zip2 v0.2s, v1.2s, v2.2s\nzip1 v3.2s, v1.2s, v2.2s\next v4.8b, v0.8b, v3.8b, #4\nfneg v5.4s, v4.4s\nfmul v6.2s, v5.2s, v0.2s\nfadd v7.2s, v6.2s, v3.2s", 1),
    ("scalar fmul/fmla by element", "fmul s0, s1, v2.s[1]\nfmla s3, s0, v2.s[3]\nfmls s4, s3, v1.s[0]\nfmul s5, s4, v4.s[2]", 1),
    # general-register statements inside a block: pointer loads, address arithmetic, immediates, a conditional select
    ("gpr between FP ops", "fmul v0.4s, v1.4s, v2.4s\nldr x8, [x0, #8]\nand x8, x8, #0xff0\nadd x9, x0, x8\nfadd v3.4s, v0.4s, v1.4s\n"
                           "ldr q4, [x0, x8]\nmov w10, #0x1d60\nadd x10, x0, x10\nfmul v5.4s, v4.4s, v3.4s\nldrb w11, [x9, #3]\nsxtw x12, w11\n"
                           "fsub v6.4s, v5.4s, v0.4s\nldr s7, [x0, #12]\nfmul s8, s7, s6\nlsl w13, w11, #2\ncsel x14, x12, x13, eq\nfadd s9, s8, s7", 1),
    ("gpr loads the FP ops read", "ldr x8, [x0]\nand x8, x8, #0xff0\nfmul v0.4s, v1.4s, v2.4s\nadd x9, x0, x8\nldr q3, [x9]\nfadd v4.4s, v3.4s, v0.4s\n"
                                  "ldr x9, [x0, #8]\nand x9, x9, #0xff0\nadd x9, x0, x9\nldr q5, [x9, #16]\nfmul v6.4s, v5.4s, v4.4s\nstr x9, [x1]", 1),
    # (A64_FPX_BLOCK_ST) stores inside runs: S / D / Q, STP S / D, through SP, register offsets that may overlap, a register
    # stored then overwritten, a base register moved between stores, a load after a store (a new run), a gpr load after one
    ("stores inside a run", "fmul v0.4s, v1.4s, v2.4s\nstr q0, [x1]\nfadd v3.4s, v0.4s, v1.4s\nstr s3, [x1, #16]\nstp s3, s0, [x2, #8]\n"
                            "fmul v4.4s, v3.4s, v3.4s\nstp d0, d4, [sp, #16]\nstr d4, [sp, #40]\nfadd s5, s4, s3\nstr s5, [sp, #4]\nfmul s6, s5, s5", 2),
    ("stores that overlap", "fmul v0.4s, v1.4s, v2.4s\nstr q0, [x0, x8, lsl #4]\nfadd v3.4s, v0.4s, v1.4s\nstr q3, [x0, x9, lsl #4]\n"
                            "str s3, [x0, #4]\nfmul v4.4s, v3.4s, v0.4s\nstr d4, [x0]\nfsub v5.4s, v4.4s, v3.4s", 2),
    ("stored then overwritten", "fmul v0.4s, v1.4s, v2.4s\nstr q0, [x1]\nfadd v0.4s, v0.4s, v1.4s\nstr q0, [x1, #16]\nfmul s0, s0, s2\n"
                                "str s0, [x1, #32]\nfsub v0.4s, v0.4s, v3.4s\nstp s0, s0, [x1, #36]", 2),
    ("base moved between stores", "fmul v0.4s, v1.4s, v2.4s\nstr q0, [x1]\nadd x1, x1, #32\nfadd v3.4s, v0.4s, v1.4s\nstr q3, [x1]\n"
                                  "mov x2, x1\nfmul v4.4s, v3.4s, v3.4s\nstr s4, [x2, #20]\nfadd v5.4s, v4.4s, v0.4s", 2),
    ("load after a store", "fmul v0.4s, v1.4s, v2.4s\nfadd v3.4s, v0.4s, v1.4s\nstr q3, [x0]\nfmul v6.4s, v3.4s, v3.4s\nldr s4, [x0, #8]\n"
                           "fmul s5, s4, s3\nfadd s7, s5, s6\nstr s7, [x0, #4]\nldp s8, s9, [x0]\nfmul s10, s8, s9\nfadd s11, s10, s8", 2),
    ("gpr load after a store", "fmul v0.4s, v1.4s, v2.4s\nfadd v3.4s, v0.4s, v1.4s\nstr q3, [x0]\nfsub v6.4s, v3.4s, v0.4s\nldr x8, [x0]\n"
                               "and x8, x8, #0x70\nfmul v4.4s, v6.4s, v3.4s\nldr q5, [x0, x8]\nfadd v7.4s, v5.4s, v4.4s\nfmul v8.4s, v7.4s, v7.4s", 2),
    ("one op alone", "fmul v0.4s, v1.4s, v2.4s\nstr q0, [x0]\nfadd s3, s1, s2\nstr s3, [x1]", 0),
    ("moves alone", "dup v0.4s, v1.s[2]\nins v0.s[1], v3.s[0]\next v4.16b, v0.16b, v3.16b, #4", 0),
)
# (A64_FPX_BLOCK_ST=1 must change these, against the switch alone)
ST_CHANGES = {"store between runs", "stores inside a run", "stores that overlap", "stored then overwritten", "base moved between stores",
              "load after a store", "gpr load after a store", "read back through memory", "one op alone"}
NEEDS = {("add32", "add32", 2, 2), ("sub32", "sub32", 2, 2), ("mul32", "fma32", 2, 3), ("fma32", "fma32", 3, 3),
         ("div32", "div32", 2, 2), ("sqrt32", "sqrt32", 1, 1)}


def assemble(clang, objdump, tmp, name, src):
    s = os.path.join(tmp, "case.s")
    o = os.path.join(tmp, "case.o")
    with open(s, "w") as f:
        f.write(src + "\n")
    sticky.build([clang, "--target=aarch64-w64-mingw32", "-c", s, "-o", o], "the assembly of " + name)
    out = subprocess.run([objdump, "-d", o], capture_output=True, text=True, check=True).stdout
    words = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0].endswith(":") and len(p[1]) == 8:
            words.append(int(p[1], 16))
    if len(words) != len([l for l in src.splitlines() if l.strip()]):
        raise SystemExit("fpx block: %s assembled to %d words" % (name, len(words)))
    return words


def lift_text(a64emit, a64_lift, words, used):
    tx, fl = a64emit.translate([(0x1000 + 4 * i, w) for i, w in enumerate(words)])
    if any(fl):
        raise SystemExit("fpx block: a64emit did not translate %s: %s" % (["%08x" % w for w in words], fl))
    return "\n".join(a64_lift.op_text(t, used) for t in tx)


def generate(cc, cxx, tmp, flags):
    dll = os.path.join(tmp, "a64emit.dll" if os.name == "nt" else "liba64emit.so")
    sticky.build([cxx, "-std=c++20", "-O0", "-shared", "-Wno-everything", "-o", dll, os.path.join(sticky.KIT, "lift", "a64emit.cpp")]
                 + flags + ([] if os.name == "nt" else ["-fPIC"]), "a64emit")
    import build_emit
    build_emit.build = lambda force=False: dll
    import a64_lift
    import a64_rtgen
    import a64emit
    import rtgen
    objdump = os.path.join(os.path.dirname(cc), "llvm-objdump" + (".exe" if os.name == "nt" else ""))
    used = set()
    out = ["/* Generated by tests/test_fpx_block.py */", '#include <math.h>', '#include "a64.h"',
           "long a64_bk_taken, a64_bk_fallen;"]
    try:
        rtgen.write(tmp)
        a64_rtgen.write(tmp)
        words = [assemble(cc, objdump, tmp, name, src) for name, src, _ in GUEST]
        os.environ[SWITCH] = "0"
        olds = [a64_lift.hot_text(lift_text(a64emit, a64_lift, w, used)) for w in words]
        os.environ[SWITCH] = "1"
        news = [a64_lift.hot_text(lift_text(a64emit, a64_lift, w, used)) for w in words]
        os.environ["A64_FPX_BLOCK_ST"] = "1"   # (the stores inside blocks: every case again)
        sts = [a64_lift.hot_text(lift_text(a64emit, a64_lift, w, used)) for w in words]
        import a64_block
        print(a64_block.report())
    finally:
        os.environ[SWITCH] = "0"
        os.environ.pop("A64_FPX_BLOCK_ST", None)
        if a64emit._lib is not None:
            import _ctypes
            (_ctypes.FreeLibrary if os.name == "nt" else _ctypes.dlclose)(a64emit._lib._handle)
            a64emit._lib = None
    for (name, _, change), o, n in zip(GUEST, olds, news):
        if change == 1 and n == o:
            raise SystemExit("fpx block: the switch did not change %s:\n%s" % (name, o))
        if change == 0 and n != o:
            raise SystemExit("fpx block: the switch changed %s" % name)
    for (name, _, _), n, t in zip(GUEST, news, sts):
        if (name in ST_CHANGES) != (t != n):
            raise SystemExit("fpx block: A64_FPX_BLOCK_ST=1 %s %s" % ("did not change" if name in ST_CHANGES else "changed", name))
    n_g = len(GUEST)
    for i, (o, n, t) in enumerate(zip(olds, news, sts)):
        out.append("static void g_old%d(Ctx *__restrict c) {\n%s\n}" % (i, o))
        out.append("static void g_new%d(Ctx *__restrict c) {\n%s\n}" % (i, n))
        out.append("static void g_st%d(Ctx *__restrict c) {\n%s\n}" % (i, t))
    out.append("typedef void (*GuestFn)(Ctx *);")
    # (the cases twice: the switch alone, then with A64_FPX_BLOCK_ST=1, both against the default lowering)
    out.append("const GuestFn fn_old[] = {%s};" % ", ".join(["g_old%d" % i for i in range(n_g)] * 2))
    out.append("const GuestFn fn_new[] = {%s};" % ", ".join(["g_new%d" % i for i in range(n_g)] + ["g_st%d" % i for i in range(n_g)]))
    out.append("const char *const fn_names[] = {%s};" % ", ".join(['"%s"' % g[0] for g in GUEST] + ['"%s [st]"' % g[0] for g in GUEST]))
    out.append("const int fn_count = %d;" % (2 * n_g))
    with open(os.path.join(tmp, "block_cases.c"), "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    used |= NEEDS
    # the switch off: the header as without it; on: only added to
    os.environ.pop(SWITCH, None)
    plain = a64_lift.fpx_header(used)
    os.environ[SWITCH] = "0"
    off = a64_lift.fpx_header(used)
    os.environ[SWITCH] = "1"
    try:
        on = a64_lift.fpx_header(used)
    finally:
        os.environ[SWITCH] = "0"
    if off != plain or "TICK-063" in off or "TICK-063" not in on or not on.startswith(off.rstrip("\n")):
        raise SystemExit("fpx block: the switch off must leave a64_fpx.h as it was, and on only add to it")
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(on)


def main():
    # (the TICK-045 defaults on, as the lift has them; TICK-055's switches off)
    for k in ("A64_FPX_SLOWPATH", "A64_FPX_FMA3", "A64_VFPX_ELEM", "A64_FPX_XMM", "A64_LR_SCALAR"):
        os.environ.pop(k, None)
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    p.add_argument("--cases", type=int, default=CASES)
    a = p.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        print("fpx block: skipped (not an x86-64 host)")
        return
    if not os.environ.get("HOOKSHOT_MINGW") and os.name == "nt":
        w = shutil.which("clang++")
        if w and os.path.isfile(os.path.join(os.path.dirname(w), "x86_64-w64-mingw32-clang++.exe")):
            os.environ["HOOKSHOT_MINGW"] = os.path.dirname(os.path.dirname(w))
    tc_flags = None
    cc = cxx = None
    if not a.cc and not a.cxx:
        try:
            tcc, tcxx = sticky.toolchain.find("clang", host=True), sticky.toolchain.find("clang++", host=True)
            if os.path.isfile(tcc) and os.path.isfile(tcxx):
                cc, cxx = tcc, tcxx
                tc_flags = sticky.toolchain.native_target(host=True) + sticky.toolchain.native_link(host=True)
        except Exception:
            tc_flags = None
    if tc_flags is None:
        cc = sticky.compiler(a.cc, "CC", ("clang",), "clang")
        cxx = sticky.compiler(a.cxx, "CXX", ("clang++",), "clang++")
    if not cc or not cxx or not os.path.isfile(os.path.join(os.path.dirname(cc), "llvm-objdump" + (".exe" if os.name == "nt" else ""))):
        print("fpx block: skipped (needs clang with its llvm-objdump: the AArch64 assembler)")
        return
    flags_of = (lambda tmp: tc_flags) if tc_flags is not None else (lambda tmp: sticky.target_flags(cc, tmp))
    t0 = time.time()
    failed, ran = [], []
    build_fl = ["-mllvm", "-capture-tracking-max-uses-to-explore=5000", "-DA64_WRITE_TRACK", "-DA64_POLLING", "-mcx16",
                "-DA64_BK_PROBE(k)=((k)?a64_bk_taken++:a64_bk_fallen++)"]
    with tempfile.TemporaryDirectory(prefix="hookshot-fpx-block-") as tmp:
        generate(cc, cxx, tmp, flags_of(tmp))
        for march in ("x86-64-v3", "x86-64-v2"):
            exe = os.path.join(tmp, "fpx_block_test_%s.exe" % march)
            sticky.build([cc] + flags_of(tmp) + ["-march=" + march] + sticky.FLAGS + build_fl +
                         ["-D_CRT_SECURE_NO_WARNINGS", "-I" + tmp, os.path.join(HERE, "fpx_block_test.c"),
                          os.path.join(tmp, "block_cases.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe], "the driver at " + march)
            r = sticky.run([exe, str(a.cases), str(SEED)])
            print(r.stdout.rstrip())
            if r.returncode == 3:
                print("fpx block: %s skipped (this CPU is not %s)" % (march, march))
                continue
            ran.append(march)
            if r.returncode:
                failed.append(march)
            if "block path taken" not in r.stdout:
                failed.append(march + " (the driver exited with %d and no summary)" % r.returncode)
                continue
            if march == "x86-64-v3":
                taken = [int(x) for x in r.stdout.split("block path taken")[1].replace(",", " ").split() if x.isdigit()]
                if not taken or taken[0] < a.cases // 10:
                    failed.append(march + " (the block path was taken in %s runs: the test exercised the fallback only)" % (taken[:1] or "?"))
    if failed:
        raise SystemExit("fpx block: FAILED at %s" % ", ".join(failed))
    print("fpx block: ok (%s, %.1f s)" % (", ".join(ran) or "nothing ran", time.time() - t0))


if __name__ == "__main__":
    main()
