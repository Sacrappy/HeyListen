"""Game-free: the native engine's entry gates (nx2/hl_gates.py) on lifted code made up here: a gate is put at the head of
the function a host source asks for when its text hashes as recorded, the hash is the same with and without the gate, a
second run changes nothing and reads nothing, another hash leaves the function alone, and a gate nobody asks for any more
is taken out; and the gated file compiles and the gate answers (with a C compiler).

python -I -B tests/test_hl_gates.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import hl_gates  # noqa: E402

CODE = '''#include "a64.h"
void f_80001000(Ctx *A64_NOESC);
void f_80001000(Ctx *__restrict A64_NOESC c) {
    c->x[0] = c->x[0] + 1;
return;
}
void f_80002000(Ctx *__restrict A64_NOESC c) {
    c->x[0] = c->x[0] * 2;
    f_80001000(c);
return;
}
'''
A64 = '''#include <stdint.h>
#define A64_NOESC
typedef struct { uint64_t x[32]; } Ctx;
'''
HOSTC = '''#include "a64.h"
#define HL_GATE(addr, hash) volatile int hl_gate_on_##addr; int hl_gate_##addr(Ctx *c)
HL_GATE(80002000, "%s") { c->x[0] = 1000 + c->x[0]; return 1; }
'''
MAIN = '''#include <stdio.h>
#include "a64.h"
void f_80002000(Ctx *);
extern volatile int hl_gate_on_80002000;
int main(void) {
    Ctx c = {{0}};
    c.x[0] = 5; f_80002000(&c);
    if (c.x[0] != 11) { printf("off: %llu\\n", (unsigned long long)c.x[0]); return 1; }
    hl_gate_on_80002000 = 1;
    c.x[0] = 5; f_80002000(&c);
    if (c.x[0] != 1005) { printf("on: %llu\\n", (unsigned long long)c.x[0]); return 1; }
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hl-gates-") as tmp:
        gen, host = Path(tmp) / "gen", Path(tmp) / "host"
        gen.mkdir()
        host.mkdir()
        code = gen / "code_000.c"
        code.write_text(CODE, encoding="utf-8", newline="")
        (gen / "code_001.c").write_text('#include "a64.h"\n', encoding="utf-8", newline="")
        (gen / "a64.h").write_text(A64, encoding="utf-8")
        h = hl_gates.text_hash(CODE, "80002000")
        check(h and len(h) == 16 and h != hl_gates.text_hash(CODE, "80001000"), "a function's hash")
        check(hl_gates.text_hash(CODE, "80003000") is None, "no hash for a function that is not there")
        (host / "hl_x.c").write_text(HOSTC % h, encoding="utf-8")
        check(hl_gates.wanted(str(host)) == {"80002000": h}, "the gate read from the host source")
        check(hl_gates.apply(str(gen), str(host), quiet=True) == 1, "one gate in place")
        gated = code.read_text(encoding="utf-8")
        check(gated.count("/* hl gate 80002000 */") == 1 and "hl gate 80001000" not in gated, "the gate's line, once")
        check(gated.index("hl gate 80002000") > gated.index("void f_80002000(Ctx *__restrict"), "at the head of its function")
        check(gated.index("hl gate 80002000") < gated.index("c->x[0] * 2"), "before the function's first statement")
        check(hl_gates.text_hash(gated, "80002000") == h, "the hash does not count the gate")
        # a second run: nothing written, nothing read (the state file answers)
        before = code.stat().st_mtime_ns
        check(hl_gates.apply(str(gen), str(host), quiet=True) == 1 and code.stat().st_mtime_ns == before, "a second run writes nothing")
        check(code.read_text(encoding="utf-8") == gated, "and changes nothing")
        # it compiles, and the gate answers when its switch is on
        (gen / "main.c").write_text(MAIN, encoding="utf-8")
        exe = Path(tmp) / "t.exe"
        subprocess.run([args.cc, "-O1", "-I", str(gen), str(code), str(host / "hl_x.c"), str(gen / "main.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
        # the lifted text changes (another lift): the gate goes, the function is left alone
        changed = gated.replace("c->x[0] * 2", "c->x[0] * 3")
        code.write_text(changed, encoding="utf-8", newline="")
        check(hl_gates.apply(str(gen), str(host), quiet=True) == 0, "another text is not gated")
        check("hl gate" not in code.read_text(encoding="utf-8"), "and its stale gate is taken out")
        # back to the recorded text, then nobody asks any more
        code.write_text(CODE, encoding="utf-8", newline="")
        check(hl_gates.apply(str(gen), str(host), quiet=True) == 1, "gated again")
        os.remove(host / "hl_x.c")
        check(hl_gates.apply(str(gen), str(host), quiet=True) == 0, "no gate wanted")
        check(code.read_text(encoding="utf-8") == CODE, "the lifted code is as the lift wrote it")
    print("hl gates: ok")


if __name__ == "__main__":
    main()
