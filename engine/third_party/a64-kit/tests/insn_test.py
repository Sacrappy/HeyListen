"""Every instruction form a game uses, run in Unicorn and as lifted C, compared (Rexcomp's test_insns.py method).

Samples real encodings from a game's code (up to --per encodings per mnemonic form, NEON keyed by arrangement),
gives each --states random register states (FPCR 0 and FPCR.FZ, as TOTK runs), runs one instruction in Unicorn's
AArch64 and in C from a64emit + rt/a64.h, and compares X0-X30, SP, NZCV, FPSR, V0-V31 and the scratch memory.
Memory operands get a base register inside the scratch area and a small index, so loads and stores are tested too.

Usage:  python insn_test.py <image_dir from nso.py> [--modules main,sdk] [--per 6] [--states 12] [--seed 1]
        [--fpsr] (also compare FPSR cumulative flags) [--list] (show every failing form)
Needs: unicorn, capstone, emsdk clang (native). Builds in a temp folder.
"""
import argparse
import collections
import hashlib
import json
import os
import random
import struct
import subprocess
import sys
import tempfile

import capstone
import unicorn
from unicorn import arm64_const as A

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(KIT, "lift"))
sys.path.insert(0, os.path.join(KIT, "tools"))
import a64emit, a64_lift, a64_rtgen, toolchain

CODE = 0x10000
SCRATCH = 0x100000
SCRATCH_SIZE = 0x10000
MEM_SIZE = SCRATCH + SCRATCH_SIZE
FZ = 1 << 24
XREG = [getattr(A, "UC_ARM64_REG_X%d" % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
QREG = [getattr(A, "UC_ARM64_REG_Q%d" % i) for i in range(32)]
STATE = struct.Struct("<32Q4B4xQQ64Q")   # x0-x30, sp | n z c v | fpcr fpsr | v0-v31 (lo, hi)


def sample(image_dir, modules, per, rng):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    groups = collections.defaultdict(list)
    for mod in modules:
        meta = json.load(open(os.path.join(image_dir, mod + ".json")))
        img = open(os.path.join(image_dir, mod + ".bin"), "rb").read()
        t = next(s for s in meta["segments"] if s["name"] == ".text")
        words = set(struct.unpack_from("<%dI" % (t["size"] // 4), img, t["offset"]))
        for w in words:
            if a64_lift.classify(w, CODE)[0] != "op":
                continue
            ins = next(md.disasm(struct.pack("<I", w), CODE), None)
            if ins is None or ins.mnemonic in ("msr", "mrs", "sys", "dc", "ic", "hint", "yield", "wfe", "wfi",
                                               "sev", "sevl", "udf", "brk", "hlt", "prfm", "prfum", "clrex",
                                               "dmb", "dsb", "isb", "nop"):
                continue
            arr = sorted(set(op.vas for op in ins.operands if getattr(op, "vas", 0)))
            groups[(ins.mnemonic, tuple(arr))].append(w)
    picked = []
    for key in sorted(groups):
        ws = groups[key]
        rng.shuffle(ws)
        picked += [(key, w) for w in ws[:per]]
    return picked


def mem_regs(w):
    """(base register number or 'sp', index register number or None) of a memory operand, or None"""
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    ins = next(md.disasm(struct.pack("<I", w), CODE), None)
    for op in ins.operands if ins else ():
        if op.type == capstone.arm64.ARM64_OP_MEM:
            base = ins.reg_name(op.mem.base) if op.mem.base else None
            idx = ins.reg_name(op.mem.index) if op.mem.index else None
            num = lambda r: None if r is None else ("sp" if r in ("sp", "wsp") else int(r[1:]) if r[1:].isdigit() else None)
            return num(base), num(idx)
    return None


def rand_state(rng, w):
    x = [rng.getrandbits(64) if rng.random() < 0.7 else rng.choice([0, 1, 0xFFFFFFFF, 0x80000000, 2 ** 63, 2 ** 64 - 1, rng.getrandbits(8)])
         for _ in range(31)]
    sp = SCRATCH + 0x8000 + 16 * rng.randrange(128)
    m = mem_regs(w)
    if m:
        base, idx = m
        addr = SCRATCH + 0x4000 + 16 * rng.randrange(512)
        if base == "sp":
            sp = addr
        elif base is not None:
            x[base] = addr
        if idx is not None and idx != base:
            x[idx] = rng.randrange(64)
    v = []
    for _ in range(32):
        kind = rng.random()
        if kind < 0.5:
            v += [rng.getrandbits(64), rng.getrandbits(64)]
        else:   # lanes of "reasonable" floats, so FP paths see normal values too
            lanes = [struct.unpack("<I", struct.pack("<f", rng.uniform(-1000, 1000)))[0] for _ in range(4)]
            v += [lanes[0] | lanes[1] << 32, lanes[2] | lanes[3] << 32]
    nzcv = [rng.getrandbits(1) for _ in range(4)]
    fpcr = rng.choice([0, FZ])
    return x + [sp], nzcv, fpcr, 0x10 if rng.random() < 0.5 else 0, v   # fpsr: IXC set half the time (FPX1 gate)


def run_unicorn(w, state, mem):
    x, nzcv, fpcr, fpsr, v = state
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    uc.mem_map(0, MEM_SIZE)
    uc.mem_write(CODE, struct.pack("<I", w))
    uc.mem_write(SCRATCH, mem)
    for i in range(31):
        uc.reg_write(XREG[i], x[i])
    uc.reg_write(A.UC_ARM64_REG_SP, x[31])
    uc.reg_write(A.UC_ARM64_REG_NZCV, nzcv[0] << 31 | nzcv[1] << 30 | nzcv[2] << 29 | nzcv[3] << 28)
    uc.reg_write(A.UC_ARM64_REG_FPCR, fpcr)
    uc.reg_write(A.UC_ARM64_REG_FPSR, fpsr)
    for i in range(32):
        uc.reg_write(QREG[i], v[2 * i] | v[2 * i + 1] << 64)
    try:
        uc.emu_start(CODE, CODE + 4, count=1)
    except unicorn.UcError as e:
        return None, str(e)
    ox = [uc.reg_read(XREG[i]) for i in range(31)] + [uc.reg_read(A.UC_ARM64_REG_SP)]
    f = uc.reg_read(A.UC_ARM64_REG_NZCV)
    on = [(f >> 31) & 1, (f >> 30) & 1, (f >> 29) & 1, (f >> 28) & 1]
    ov = []
    for i in range(32):
        q = uc.reg_read(QREG[i])
        ov += [q & (2 ** 64 - 1), q >> 64]
    return (ox, on, uc.reg_read(A.UC_ARM64_REG_FPCR), uc.reg_read(A.UC_ARM64_REG_FPSR), ov,
            hashlib.sha1(bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE))).digest()), None


def pack(state):
    x, nzcv, fpcr, fpsr, v = state
    return STATE.pack(*(x + nzcv + [fpcr, fpsr] + v))


def build_harness(tmp, picked):
    a64_rtgen.write(tmp)
    texts, flags = a64emit.translate([(CODE, w) for _, w in picked])
    used = set()
    texts = [a64_lift.outline_fpx(t, used) for t in texts]   # the same FPX helpers the lifter emits
    open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n").write(a64_lift.fpx_header(used))
    src = ['#include "a64.h"', "#include <stdio.h>", "#include <stdlib.h>", "#include <windows.h>"]
    for i, ((key, w), t, f) in enumerate(zip(picked, texts, flags)):
        body = "c->halted = 7;" if f else t
        src.append("static void t%d(Ctx *__restrict c) { %s }" % (i, body))
    src.append("static void (*const T[])(Ctx *) = {%s};" % ",".join("t%d" % i for i in range(len(picked))))
    src.append(r"""
void a64_trap(Ctx *c, uint64_t pc, uint32_t insn) { (void)pc; (void)insn; c->halted = 9; }
void recomp_unhandled(GuestContext *c, uint32_t i, uint64_t pc) { (void)i; (void)pc; c->halted = 7; }
void recomp_barrier(void) {}
void recomp_ic_ivau(GuestContext *c, uint64_t a, int v) { (void)c; (void)a; (void)v; }
uint64_t recomp_cntpct(GuestContext *c) { (void)c; return 0; }
static uint64_t ex_addr; static int ex_valid;
uint64_t recomp_ldxr(GuestContext *c, uint64_t a, uint32_t s) { uint64_t v = 0; memcpy(&v, A64_PTR(a), s); ex_addr = a; ex_valid = 1; return v; }
uint32_t recomp_stxr(GuestContext *c, uint64_t a, uint32_t s, uint64_t v) { if (!ex_valid || ex_addr != a) return 1; ex_valid = 0; memcpy(A64_PTR(a), &v, s); return 0; }
void recomp_ldxp(GuestContext *c, uint64_t a, uint32_t s, uint64_t *lo, uint64_t *hi) { *lo = *hi = 0; memcpy(lo, A64_PTR(a), s); memcpy(hi, A64_PTR(a + s), s); ex_addr = a; ex_valid = 1; }
uint32_t recomp_stxp(GuestContext *c, uint64_t a, uint32_t s, uint64_t lo, uint64_t hi) { if (!ex_valid || ex_addr != a) return 1; ex_valid = 0; memcpy(A64_PTR(a), &lo, s); memcpy(A64_PTR(a + s), &hi, s); return 0; }
void recomp_clrex(GuestContext *c) { ex_valid = 0; }
int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (VirtualAlloc(A64_MEM, %d, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != A64_MEM) return 3;
    static uint8_t state[%d];
    unsigned idx;
    while (fread(&idx, 4, 1, in) == 1) {
        fread(state, sizeof state, 1, in);
        fread(A64_MEM + %d, %d, 1, in);
        Ctx c; memset(&c, 0, sizeof c);
        memcpy(c.x, state, 256);
        memcpy(&c.n, state + 256, 4);
        memcpy(&c.fpcr, state + 264, 8); memcpy(&c.fpsr, state + 272, 8);
        memcpy(c.vreg, state + 280, 512);
        ex_valid = 0;
        T[idx](&c);
        memcpy(state, c.x, 256); memcpy(state + 256, &c.n, 4);
        memcpy(state + 264, &c.fpcr, 8); memcpy(state + 272, &c.fpsr, 8); memcpy(state + 280, c.vreg, 512);
        fwrite(&c.halted, 4, 1, out);
        fwrite(state, sizeof state, 1, out);
        fwrite(A64_MEM + %d, %d, 1, out);
    }
    fclose(out);
    return 0;
}""" % (MEM_SIZE, STATE.size, SCRATCH, SCRATCH_SIZE, SCRATCH, SCRATCH_SIZE))
    path = os.path.join(tmp, "harness.c")
    open(path, "w", newline="\n").write("\n".join(src))
    exe = os.path.join(tmp, "harness.exe")
    cmd = [toolchain.need(toolchain.find("clang"))] + toolchain.native_target() + toolchain.native_link() + \
          ["-O1", "-g0", "-Wno-everything", "-I" + tmp, "-I" + os.path.join(KIT, "rt"), path,
           os.path.join(tmp, "a64_suyu.c"), "-o", exe]
    subprocess.run(cmd, check=True)
    return exe, flags


def main():
    p = argparse.ArgumentParser()
    p.add_argument("image_dir")
    p.add_argument("--modules", default="main,sdk")
    p.add_argument("--per", type=int, default=6)
    p.add_argument("--states", type=int, default=12)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--fpsr", action="store_true")
    p.add_argument("--list", action="store_true")
    a = p.parse_args()
    rng = random.Random(a.seed)
    picked = sample(a.image_dir, a.modules.split(","), a.per, rng)
    print("%d encodings in %d forms" % (len(picked), len(set(k for k, _ in picked))), flush=True)
    tmp = tempfile.mkdtemp(prefix="a64_insn_")
    exe, flags = build_harness(tmp, picked)
    cases, expect = [], []
    base_mem = bytes(rng.getrandbits(8) for _ in range(4096)) * (SCRATCH_SIZE // 4096)
    for i, (key, w) in enumerate(picked):
        for _ in range(a.states):
            st = rand_state(rng, w)
            res, err = run_unicorn(w, st, base_mem)
            if res is None:
                continue          # Unicorn faulted (e.g. an unmapped address): not a comparable case
            cases.append((i, st))
            expect.append(res)
    inp = os.path.join(tmp, "in.bin")
    with open(inp, "wb") as f:
        for i, st in cases:
            f.write(struct.pack("<I", i) + pack(st) + base_mem)
    outp = os.path.join(tmp, "out.bin")
    subprocess.run([exe, inp, outp], check=True)
    fails = collections.defaultdict(list)
    passes = collections.Counter()
    rec = 4 + STATE.size + SCRATCH_SIZE
    with open(outp, "rb") as f:
        for (i, st), exp in zip(cases, expect):
            blob = f.read(rec)
            halted = struct.unpack_from("<i", blob, 0)[0]
            vals = STATE.unpack_from(blob, 4)
            mem = hashlib.sha1(blob[4 + STATE.size:]).digest()
            key, w = picked[i]
            got = (list(vals[:32]), list(vals[32:36]), vals[36], vals[37], list(vals[38:]), mem)
            ex_x, ex_n, ex_fpcr, ex_fpsr, ex_v, ex_mem = exp
            diffs = []
            if halted:
                diffs.append("halted %d (unhandled)" % halted)
            else:
                for r in range(32):
                    if got[0][r] != ex_x[r]:
                        diffs.append("%s %#x != %#x" % ("sp" if r == 31 else "x%d" % r, got[0][r], ex_x[r]))
                if got[1] != ex_n:
                    diffs.append("nzcv %s != %s" % (got[1], ex_n))
                for r in range(64):
                    if got[4][r] != ex_v[r]:
                        diffs.append("v%d.%s %#x != %#x" % (r // 2, "hi" if r & 1 else "lo", got[4][r], ex_v[r]))
                if a.fpsr and (got[3] & 0x9F) != (ex_fpsr & 0x9F):
                    diffs.append("fpsr %#x != %#x" % (got[3], ex_fpsr))
                if mem != ex_mem:
                    diffs.append("memory differs")
            if diffs:
                fails[key].append((w, st[2], diffs[:4]))
            else:
                passes[key] += 1
    forms = set(k for k, _ in picked)
    bad = sorted(fails, key=lambda k: -len(fails[k]))
    print("%d cases, %d forms pass, %d forms with failures" % (len(cases), len(forms) - len(bad), len(bad)))
    for k in bad if a.list else bad[:40]:
        w, fpcr, d = fails[k][0]
        name = k[0] + (" (%s)" % ",".join(map(str, k[1])) if k[1] else "")
        print("  FAIL %-28s %3d/%-3d e.g. %08x fpcr=%#x: %s" % (name, len(fails[k]), len(fails[k]) + passes[k], w,
                                                               fpcr, "; ".join(d)))
    print("PASS" if not bad else "FAIL")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
