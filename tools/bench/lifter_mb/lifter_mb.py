"""(TICK-055) Micro-benchmarks of the lifter's lowering on x86-64, game-free: synthetic AArch64 kernels (hand-encoded words)
lifted by the lifter's own pipeline (a64emit.translate -> a64_lift.op_text -> hot_text), once with the lift switches off
and once with them on, compiled with the hot pieces' flags against the build's guest model (nx2/rtgen.py) and timed.

    python -I tools/bench/lifter_mb/lifter_mb.py <out dir> [--switches A64_FPX_XMM,A64_LR_SCALAR] [--build-only]

Writes <out>/lifter_mb.exe (run it there or copy it to another PC: `lifter_mb.exe [calls]`). Each kernel repeats its words
8 times in one function; the driver checks that both lowerings leave the same Ctx and guest memory, then times them
interleaved (best of 41 runs of <calls> calls) and prints ns per guest instruction. The FPX gate is marked likely in
both (the PGO profile lays the game's hot code out that way). Windows (VirtualAlloc of the guest window); needs
HOOKSHOT_MINGW (llvm-mingw).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.normpath(os.path.join(HERE, "..", "..", "..", "engine"))
sys.path[:0] = [os.path.join(ENGINE, "third_party", "a64-kit", "lift"), os.path.join(ENGINE, "third_party", "a64-kit", "tools"),
                os.path.join(ENGINE, "nx2")]
import toolchain  # noqa: E402

R = 8
KER = {
    # one dependent scalar FP chain on s0: FMUL s0,s0,s1; FADD s0,s0,s2; FMADD s0,s0,s1,s3; FSUB s0,s0,s2
    "fpchain": [0x1E210800, 0x1E222800, 0x1F010C00, 0x1E223800],
    # vec3 dot from memory: LDP s4,s5,[x0]; LDR s6,[x0,#8]; FMUL s7,s4,s1; FMADD s7,s5,s2,s7; FMADD s7,s6,s3,s7; STR s7,[x1]
    "dot3": [0x2D401404, 0xBD400806, 0x1E210887, 0x1F021CA7, 0x1F031CC7, 0xBD000027],
    # FDIV s0,s0,s1; FMUL s0,s0,s1
    "divmul": [0x1E211800, 0x1E210800],
    # FNEG s0,s1; FADD v2.4s,v0.4s,v3.4s; FNEG s1,s2 (a scalar result read whole)
    "fnegvec": [0x1E214020, 0x4E23D402, 0x1E214041],
    # LDR s0,[x0]; FADD s0,s0,s1; STR s0,[x0] (through guest memory)
    "ldst": [0xBD400000, 0x1E212800, 0xBD000000],
    # LDR q0,[x0]; FADD v1.4s,v0.4s,v1.4s (a Q load read whole)
    "ldq": [0x3DC00000, 0x4E21D401],
}
DRIVER = r"""
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "a64.h"
uint8_t a64_dirty[A64_DIRTY_PAGES];
void a64_poll(Ctx *c) { c->poll = 0; }
#define KER(n, k) void K_##n##_base(Ctx *); void K_##n##_new(Ctx *);
#include "kernels.h"
#undef KER
typedef void (*Fn)(Ctx *);
static double now(void) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart / f.QuadPart; }
static void init(Ctx *c) {
    memset(c, 0, sizeof *c);
    c->fpcr = 0x01000000; c->fpsr = 0x9f;
    float v[8] = {1.5f, 0.999f, 0.001f, 0.5f, 1.25f, 2.5f, 3.75f, 0.0f};
    for (int i = 0; i < 8; i++) memcpy(&c->vreg[i][0], &v[i], 4);
    c->x[0] = 0x1000; c->x[1] = 0x2000; c->x[31] = 0x8000;
    float m[4] = {0.25f, 0.5f, 0.75f, 1.0f}; memcpy(A64_PTR(0x1000), m, 16); memset(A64_PTR(0x2000), 0, 64);
}
static double once(Fn f, int n) { Ctx c; init(&c); double t0 = now(); for (int i = 0; i < n; i++) f(&c); return now() - t0; }
int main(int argc, char **argv) {
    if (!VirtualAlloc((void *)A64_PTR(0), 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { printf("no guest window\n"); return 2; }
    SetThreadAffinityMask(GetCurrentThread(), 1 << 2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const int n = argc > 1 ? atoi(argv[1]) : 100000;
    int bad = 0;
#define KER(name, k) { Ctx a, b; uint8_t ma[64], mb[64]; \
    init(&a); for (int i = 0; i < 100; i++) K_##name##_base(&a); memcpy(ma, A64_PTR(0x1000), 64); \
    init(&b); for (int i = 0; i < 100; i++) K_##name##_new(&b); memcpy(mb, A64_PTR(0x1000), 64); \
    const int same = !memcmp(&a, &b, sizeof a) && !memcmp(ma, mb, 64); bad += !same; \
    double tb = 1e9, tn = 1e9; \
    for (int r = 0; r < 41; r++) { double x = once(K_##name##_base, n), y = once(K_##name##_new, n); if (x < tb) tb = x; if (y < tn) tn = y; } \
    printf("%-10s base %6.3f ns/insn  new %6.3f ns/insn  (%+.1f%%)  state %s\n", #name, tb * 1e9 / ((double)n * k), \
           tn * 1e9 / ((double)n * k), 100.0 * (tn - tb) / tb, same ? "same" : "DIFFERENT"); }
#include "kernels.h"
    return bad != 0;
}
"""


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--switches", default="A64_FPX_XMM,A64_LR_SCALAR")
    p.add_argument("--build-only", action="store_true")
    a = p.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    import build_emit
    dll = os.path.join(out, "a64emit.dll")
    cxx, cc = toolchain.find("clang++", host=True), toolchain.find("clang", host=True)
    if not os.path.exists(dll):
        subprocess.run([cxx, "-std=c++20", "-O2", "-shared", "-o", dll, os.path.join(ENGINE, "third_party", "a64-kit", "lift", "a64emit.cpp"),
                        "-Wno-everything"] + toolchain.native_target(host=True) + toolchain.native_link(host=True), check=True)
    build_emit.build = lambda force=False: dll
    import a64emit
    import a64_lift
    import a64_rtgen
    import rtgen
    rtgen.write(out)
    a64_rtgen.write(out)
    path = os.path.join(out, "a64_suyu.h")
    h = open(path, encoding="utf-8").read()
    old = "#define RECOMP_FPX_OPEN(c) (((c)->fpsr & 16u) && !((c)->fpcr & RECOMP_FPX_FPCR_MASK))"
    assert old in h, "the FPX gate's definition moved"
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(h.replace(old, "#define RECOMP_FPX_OPEN(c) __builtin_expect(((c)->fpsr & 16u) && !((c)->fpcr & RECOMP_FPX_FPCR_MASK), 1)"))
    switches = [s for s in a.switches.split(",") if s]
    used = {("add32", "add32", 2, 2), ("sub32", "sub32", 2, 2), ("mul32", "fma32", 2, 3), ("fma32", "fma32", 3, 3),
            ("div32", "div32", 2, 2)}
    texts = {}
    for var, on in (("base", "0"), ("new", "1")):
        for s in switches:
            os.environ[s] = on
        tx = {}
        for k, words in KER.items():
            t, fl = a64emit.translate([(0x1000 + 4 * i, w) for i, w in enumerate(words)])
            assert not any(fl), (k, fl)
            tx[k] = "\n".join(a64_lift.hot_text(a64_lift.op_text(x, used)) for x in t)
        texts[var] = tx
    header = a64_lift.fpx_header(used)   # (the switches still on: the header with every helper)
    for s in switches:
        os.environ[s] = "0"
    with open(os.path.join(out, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(header)
    for var, tx in texts.items():
        src = ["#include <math.h>", '#include "a64.h"']
        for k, t in tx.items():
            src.append("__attribute__((noinline)) void K_%s_%s(Ctx *__restrict A64_NOESC c) {\n%s\n}" % (k, var, "\n".join([t] * R)))
        with open(os.path.join(out, "cases_%s.c" % var), "w", newline="\n") as f:
            f.write("\n".join(src) + "\n")
    with open(os.path.join(out, "kernels.h"), "w") as f:
        f.write("".join("KER(%s,%d)\n" % (k, len(v) * R) for k, v in KER.items()))
    with open(os.path.join(out, "driver.c"), "w", newline="\n") as f:
        f.write(DRIVER)
    fl = toolchain.native_target(host=True) + ["-march=x86-64-v3", "-ffp-contract=off", "-fno-strict-aliasing", "-O2", "-g0", "-mcx16",
                                               "-mllvm", "-rotation-max-header-size=0", "-mllvm",
                                               "-capture-tracking-max-uses-to-explore=5000", "-DA64_POLLING", "-DA64_WRITE_TRACK",
                                               "-Wno-everything", "-I" + out]
    objs = []
    for s in ("cases_base.c", "cases_new.c", "a64_suyu.c"):
        o = os.path.join(out, s[:-2] + ".o")
        subprocess.run([cc] + fl + ["-c", os.path.join(out, s), "-o", o], check=True)
        objs.append(o)
    exe = os.path.join(out, "lifter_mb.exe")
    subprocess.run([cc] + fl + toolchain.native_link(host=True) + [os.path.join(out, "driver.c")] + objs + ["-o", exe], check=True)
    print("built", exe)
    if not a.build_only:
        sys.exit(subprocess.run([exe]).returncode)


if __name__ == "__main__":
    main()
