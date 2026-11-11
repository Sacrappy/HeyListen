"""(HeyListen TICK-055) A64_FPX_XMM=1 (a64_lift.xmm_text / xmm_header) and A64_LR_SCALAR=1 (LR_STP), game-free: bit-exact
with the default lowering.

Guest words and short sequences of them (hand-encoded: scalar FADD / FSUB / FMUL / FNMUL / FDIV / FMADD / FMSUB / FNMADD /
FNMSUB S, aliased registers too; LDR S / D / Q, LDP S / Q, pre- and post-index; FNEG / FABS / FMOV S and D; dependent chains
where one op reads the register the previous one wrote, as a whole or as a lane) go through a64emit.translate,
a64_lift.op_text and hot_text twice: with the switch off (the default lowering) and on. Both versions are compiled in one
file against the header the switch writes (which keeps every default helper) and the build's own guest model
(nx2/rtgen.py: the 64-bit window, the soft write watch, the polls). tests/fpx_xmm_test.c runs both on random registers
(zeros, subnormals, infinities, quiet and signalling NaNs, the extreme normals, binary32 midpoints, random bits in the
upper lanes), random guest memory (the same kinds of values, aligned and unaligned addresses), random FPCR (FZ, DN, the
rounding modes, other control bits) and FPSR states, and compares the whole Ctx and the guest memory. Built at
-march=x86-64-v3 and -march=x86-64-v2 (no FMA3: the FMAs take the default route) with the build's FP flags.

It also checks that the switch off leaves the lifter's output as before: fpx_header and hot_text give the text they gave
without the switch.

    python -I -B tests/test_fpx_xmm.py [--cases 3000000] [--cc clang] [--cxx clang++]

It has teeth (each by hand, then restored; 200,000 runs): the add's keep test without its FZ sum test: 447 mismatches
(subnormal sums under FZ); the FNMUL's negation dropped: 12,861; a64_vld32 loading 8 bytes: 31,735 (the upper lanes);
the FMA's FZ test without the addend's: 224 at v3 (v2 has no FMA3: the default route, 0).
Skipped (exit 0) on a host that is not x86-64, without a C/C++ compiler, or on a CPU without x86-64-v2.
"""
import argparse
import os
import platform
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "nx2"))
import test_fpx_sticky as sticky  # noqa: E402  (its compiler search, flags and header generation)

CASES, SEED = 3000000, 20261111
# (name, words, whether the switch must change its text)
GUEST = (
    ("FADD s0, s1, s2", [0x1E222820], 1), ("FSUB s0, s1, s2", [0x1E223820], 1), ("FMUL s0, s1, s2", [0x1E220820], 1),
    ("FNMUL s0, s1, s2", [0x1E228820], 1), ("FDIV s0, s1, s2", [0x1E221820], 1), ("FMUL s1, s1, s1", [0x1E210821], 1),
    ("FADD s2, s2, s2", [0x1E222842], 1), ("FMADD s0, s1, s2, s3", [0x1F020C20], 1), ("FMSUB s0, s1, s2, s3", [0x1F028C20], 1),
    ("FNMADD s0, s1, s2, s3", [0x1F220C20], 1), ("FNMSUB s0, s1, s2, s3", [0x1F228C20], 1),
    ("FMADD s3, s3, s3, s3", [0x1F030C63], 1),
    ("LDR s1, [x0, #4]", [0xBD400401], 1), ("LDR d1, [x0]", [0xFD400001], 1), ("LDR q1, [x0, #16]", [0x3DC00401], 1),
    ("LDP s1, s2, [x0]", [0x2D400801], 1), ("LDP q0, q1, [x0]", [0xAD400400], 1), ("LDR s0, [x0], #4", [0xBC404400], 1),
    ("LDR q0, [x0], #16", [0x3CC10400], 1), ("LDR s0, [x0, #4]!", [0xBC404C00], 1),
    ("FNEG s0, s1", [0x1E214020], 1), ("FABS s0, s1", [0x1E20C020], 1), ("FMOV s0, w1", [0x1E270020], 1),
    ("FNEG d0, d1", [0x1E614020], 1), ("FMOV d0, x1", [0x9E670020], 1),
    # chains: each op reads what the one before wrote
    ("FMUL s0,s0,s1; FADD s0,s0,s2; FMADD s0,s0,s1,s3; FSUB s0,s0,s2", [0x1E210800, 0x1E222800, 0x1F010C00, 0x1E223800], 1),
    ("LDP s4,s5,[x0]; LDR s6,[x0,#8]; FMUL s7,s4,s1; FMADD s7,s5,s2,s7; FMADD s7,s6,s3,s7; STR s7,[x1]",
     [0x2D401404, 0xBD400806, 0x1E210887, 0x1F021CA7, 0x1F031CC7, 0xBD000027], 1),
    ("FNEG s0,s1; FADD v2.4s,v0.4s,v3.4s; FNEG s1,s2", [0x1E214020, 0x4E23D402, 0x1E214041], 1),
    ("LDR s0,[x0]; FADD s0,s0,s1; STR s0,[x0]", [0xBD400000, 0x1E212800, 0xBD000000], 1),
    ("LDR q0,[x0]; FADD s1,s0,s2; FMUL v3.4s,v0.4s,v1.4s", [0x3DC00000, 0x1E222801, 0x6E21DC03], 1),
    ("FDIV s0,s0,s1; FMUL s0,s0,s1; FNMUL s2,s0,s0", [0x1E211800, 0x1E210800, 0x1E208802], 1),
    # A64_LR_SCALAR: the frame record's STP
    ("STP x29, x30, [sp, #-16]!", [0xA9BF7BFD], 1), ("SUB sp, sp, #0x60; STP x29, x30, [sp, #0x50]", [0xD10183FF, 0xA9057BFD], 1),
    ("STP x29, x30, [sp, #-16]!; STP x19, x20, [sp, #-16]!", [0xA9BF7BFD, 0xA9BF53F3], 1),
)
SWITCHES = ("A64_FPX_XMM", "A64_LR_SCALAR")


def lift_text(a64emit, a64_lift, words, used):
    tx, fl = a64emit.translate([(0x1000 + 4 * i, w) for i, w in enumerate(words)])
    if any(fl):
        raise SystemExit("fpx xmm: a64emit did not translate %s: %s" % (["%08x" % w for w in words], fl))
    return "\n".join(a64_lift.op_text(t, used) for t in tx)


def generate(cxx, tmp, flags):
    dll = os.path.join(tmp, "a64emit.dll" if os.name == "nt" else "liba64emit.so")
    sticky.build([cxx, "-std=c++20", "-O0", "-shared", "-Wno-everything", "-o", dll, os.path.join(sticky.KIT, "lift", "a64emit.cpp")]
                 + flags + ([] if os.name == "nt" else ["-fPIC"]), "a64emit")
    import build_emit
    build_emit.build = lambda force=False: dll
    import a64_lift
    import a64_rtgen
    import a64emit
    import rtgen
    used = set()
    out = ["/* Generated by tests/test_fpx_xmm.py */", '#include <math.h>', '#include "a64.h"']
    try:
        rtgen.write(tmp)
        a64_rtgen.write(tmp)
        for k in SWITCHES:
            os.environ[k] = "0"
        olds = [a64_lift.hot_text(lift_text(a64emit, a64_lift, w, used)) for _, w, _ in GUEST]
        for k in SWITCHES:
            os.environ[k] = "1"
        news = [a64_lift.hot_text(lift_text(a64emit, a64_lift, w, used)) for _, w, _ in GUEST]
    finally:
        for k in SWITCHES:
            os.environ[k] = "0"
        if a64emit._lib is not None:
            import _ctypes
            (_ctypes.FreeLibrary if os.name == "nt" else _ctypes.dlclose)(a64emit._lib._handle)
            a64emit._lib = None
    for (name, _, change), o, n in zip(GUEST, olds, news):
        if change and n == o:
            raise SystemExit("fpx xmm: the switch did not change %s:\n%s" % (name, o))
        if not change and n != o:
            raise SystemExit("fpx xmm: the switch changed %s" % name)
    for i, (o, n) in enumerate(zip(olds, news)):
        out.append("static void g_old%d(Ctx *__restrict c) {\n%s\n}" % (i, o))
        out.append("static void g_new%d(Ctx *__restrict c) {\n%s\n}" % (i, n))
    out.append("typedef void (*GuestFn)(Ctx *);")
    out.append("const GuestFn fn_old[] = {%s};" % ", ".join("g_old%d" % i for i in range(len(GUEST))))
    out.append("const GuestFn fn_new[] = {%s};" % ", ".join("g_new%d" % i for i in range(len(GUEST))))
    out.append("const char *const fn_names[] = {%s};" % ", ".join('"%s"' % g[0] for g in GUEST))
    out.append("const int fn_count = %d;" % len(GUEST))
    with open(os.path.join(tmp, "xmm_cases.c"), "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    used |= {("add32", "add32", 2, 2), ("sub32", "sub32", 2, 2), ("mul32", "fma32", 2, 3), ("fma32", "fma32", 3, 3),
             ("div32", "div32", 2, 2)}
    # the switch off: the header and the hot text as without it
    for k in SWITCHES:
        os.environ.pop(k, None)
    plain = a64_lift.fpx_header(used)
    for k in SWITCHES:
        os.environ[k] = "0"
    off = a64_lift.fpx_header(used)
    for k in SWITCHES:
        os.environ[k] = "1"
    try:
        on = a64_lift.fpx_header(used)
    finally:
        for k in SWITCHES:
            os.environ[k] = "0"
    if off != plain or "TICK-055" in off or "TICK-055" not in on or not on.startswith(off.rstrip("\n")):
        raise SystemExit("fpx xmm: the switch off must leave a64_fpx.h as it was, and on only add to it")
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(on)


def main():
    # (the TICK-045 defaults on, as the lift has them)
    for k in ("A64_FPX_SLOWPATH", "A64_FPX_FMA3", "A64_VFPX_ELEM"):
        os.environ.pop(k, None)
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    p.add_argument("--cases", type=int, default=CASES)
    a = p.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        print("fpx xmm: skipped (not an x86-64 host)")
        return
    tc_flags = None
    if not os.environ.get("HOOKSHOT_MINGW") and os.name == "nt":
        w = shutil.which("clang++")
        if w and os.path.isfile(os.path.join(os.path.dirname(w), "x86_64-w64-mingw32-clang++.exe")):
            os.environ["HOOKSHOT_MINGW"] = os.path.dirname(os.path.dirname(w))
    if not a.cc and not a.cxx:
        try:
            tcc, tcxx = sticky.toolchain.find("clang", host=True), sticky.toolchain.find("clang++", host=True)
            if os.path.isfile(tcc) and os.path.isfile(tcxx):
                cc, cxx = tcc, tcxx
                tc_flags = sticky.toolchain.native_target(host=True) + sticky.toolchain.native_link(host=True)
        except Exception:
            tc_flags = None
    if tc_flags is None:
        cc = sticky.compiler(a.cc, "CC", ("clang", "gcc", "cc"), "clang")
        cxx = sticky.compiler(a.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
    flags_of = (lambda tmp: tc_flags) if tc_flags is not None else (lambda tmp: sticky.target_flags(cc, tmp))
    if not cc or not cxx:
        print("fpx xmm: skipped (no C/C++ compiler)")
        return
    t0 = time.time()
    failed, ran = [], []
    # (the lifted code's flags from nx2/build.py: the guest model's soft write watch and polls, the capture-tracking limit)
    build_fl = ["-mllvm", "-capture-tracking-max-uses-to-explore=5000", "-DA64_WRITE_TRACK", "-DA64_POLLING", "-mcx16"]
    with tempfile.TemporaryDirectory(prefix="hookshot-fpx-xmm-") as tmp:
        generate(cxx, tmp, flags_of(tmp))
        for march in ("x86-64-v3", "x86-64-v2"):
            exe = os.path.join(tmp, "fpx_xmm_test_%s.exe" % march)
            sticky.build([cc] + flags_of(tmp) + ["-march=" + march] + sticky.FLAGS + build_fl +
                         ["-D_CRT_SECURE_NO_WARNINGS", "-I" + tmp, os.path.join(HERE, "fpx_xmm_test.c"),
                          os.path.join(tmp, "xmm_cases.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe], "the driver at " + march)
            r = sticky.run([exe, str(a.cases), str(SEED)])
            print(r.stdout.rstrip())
            if r.returncode == 3:
                print("fpx xmm: %s skipped (this CPU is not %s)" % (march, march))
                continue
            ran.append(march)
            if r.returncode:
                failed.append(march)
    if failed:
        raise SystemExit("fpx xmm: FAILED at %s" % ", ".join(failed))
    print("fpx xmm: ok (%s, %.1f s)" % (", ".join(ran) or "nothing ran", time.time() - t0))


if __name__ == "__main__":
    main()
