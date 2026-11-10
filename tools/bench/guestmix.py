"""(TICK-041) Where the lifted guest code's time goes, by x86 instruction class.

python -I tools/bench/guestmix.py <samples.txt> <HeyListen.map> <HeyListen.exe> [--from-ms 0] [--cover 0.6] [--threads regex] [--out dir]

Leaf samples of the job/main threads (HOS_SAMPLE_ALL) are matched to the instruction at the sampled IP, in the
functions that hold `cover` of the guest-code samples. Each instruction is classified (context load/store by kind,
guest memory, host stack spill/reload, flags, poll, FP env checks, calls by kind, ...).
"""
import argparse, bisect, collections, os, re, subprocess, sys

OBJDUMP = os.environ.get("LLVM_OBJDUMP") or os.path.join(os.environ.get("LOCALAPPDATA", ""), "HookshotBuilder", "versions", "v0.1.0-rc1", "HookshotBuilder", "tools", "llvm-mingw", "bin", "llvm-objdump.exe")
BASE = 0x140000000

# Ctx layout (rt/a64.h + nx2/rtgen.py's poll)
def ctx_field(d):
    if d < 0xf8: return "x"
    if d < 0x100: return "sp"
    if d < 0x108: return "pc"
    if d < 0x10c: return "nzcv"
    if d < 0x110: return "halted"
    if d < 0x310: return "vreg"
    if d < 0x320: return "tpidr"
    if d < 0x330: return "fpcr/fpsr"
    if d < 0x350: return "excl"
    if d < 0x358: return "thread"
    if d < 0x360: return "poll"
    return "ctx?"


def load_map(path):
    syms = []
    sect = []
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) >= 4 and p[0].startswith("0001:") and len(p[2]) == 16:
            try:
                syms.append((int(p[2], 16) - BASE, p[1], p[-1]))
            except ValueError:
                pass
        elif len(p) >= 4 and p[0].startswith("0001:") and p[2].startswith(".text"):
            sect.append((int(p[0][5:], 16) + 0x1000, int(p[1].rstrip("H"), 16), p[2]))
    syms.sort()
    sect.sort()
    return syms, sect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples"); ap.add_argument("map"); ap.add_argument("exe")
    ap.add_argument("--from-ms", type=int, default=0)
    ap.add_argument("--cover", type=float, default=0.6)
    ap.add_argument("--threads", default=r"BaseProcExecutor|ModuleSystemWorker|sead::MainThread")
    ap.add_argument("--out", default=None)
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    syms, sect = load_map(a.map)
    addrs = [s[0] for s in syms]
    saddr = [s[0] for s in sect]
    thr = re.compile(a.threads)

    def sym_of(rva):
        i = bisect.bisect_right(addrs, rva) - 1
        return syms[i] if i >= 0 else None

    def sect_of(rva):
        i = bisect.bisect_right(saddr, rva) - 1
        if i >= 0 and rva < sect[i][0] + sect[i][1]:
            return sect[i]
        return None

    leafcat = collections.Counter()
    guest_ip = collections.Counter()     # rva -> samples (guest leaf)
    guest_fn = collections.Counter()
    rt_leaf = collections.Counter()
    total = 0
    for line in open(a.samples, encoding="utf-8", errors="replace"):
        head, _, stack = line.rstrip("\n").partition("|")
        ms, _, thread = head.partition(" ")
        if not ms.isdigit() or int(ms) < a.from_ms or not thr.search(thread):
            continue
        frames = [f for f in stack.split(";") if f and not f.startswith("@")]
        if not frames:
            continue
        total += 1
        leaf = frames[0]
        mod, _, off = leaf.rpartition("+")
        if mod != "HeyListen.exe":
            # system: the innermost exe frame
            leafcat["system"] += 1
            continue
        rva = int(off, 16)
        s = sym_of(rva)
        name = s[1] if s else "?"
        if name.startswith("f_"):
            leafcat["guest"] += 1
            guest_ip[rva] += 1
            sc = sect_of(rva)
            guest_fn[sc[2] if sc else name] += 1
        else:
            leafcat["runtime"] += 1
            rt_leaf[name] += 1
    print("samples %d (threads /%s/, from %d ms)" % (total, a.threads, a.from_ms))
    for k, v in leafcat.most_common():
        print("  %-8s %6.1f%%" % (k, 100.0 * v / total))
    print("runtime leaves:")
    for k, v in rt_leaf.most_common(20):
        print("  %5.2f%%  %s" % (100.0 * v / total, k))
    g = leafcat["guest"]
    # the guest sections covering `cover` of the guest samples
    chosen, acc = [], 0
    for k, v in guest_fn.most_common():
        chosen.append(k); acc += v
        if acc >= a.cover * g:
            break
    print("guest code: %d samples in %d sections; %d sections hold %.0f%%" % (g, len(guest_fn), len(chosen), 100.0 * acc / g))
    print("top sections:")
    for k, v in guest_fn.most_common(a.top):
        print("  %5.2f%% of guest  %s" % (100.0 * v / g, k))
    secbyname = {s[2]: s for s in sect}
    cls_w = collections.Counter()       # weighted by samples
    cls_s = collections.Counter()       # static instruction counts in chosen sections
    calls_s = collections.Counter()
    calls_w = collections.Counter()
    n_insn = 0
    w_total = 0
    out = open(os.path.join(a.out, "guestmix_insns.txt"), "w") if a.out else None
    for name in chosen:
        if name in secbyname:
            st, ln, _ = secbyname[name]
        else:
            i = [k for k, x in enumerate(syms) if x[1] == name][0]
            st, ln = syms[i][0], syms[i + 1][0] - syms[i][0]
        dis = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", "-M", "intel", "--start-address=0x%x" % (BASE + st),
                              "--stop-address=0x%x" % (BASE + st + ln), a.exe], capture_output=True, text=True).stdout
        insns = []
        for l in dis.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", l)
            if m:
                insns.append((int(m.group(1), 16) - BASE, m.group(2).strip()))
        # the context register(s): copies of rcx in the prologue, and registers loaded from one ([ctx]) are guest values
        ctxr = set()
        memr = set()
        for _, t in insns[:60]:
            m = re.match(r"mov\s+(\w+), rcx$", t)
            if m and not ctxr:
                ctxr.add(m.group(1))
        for _, t in insns:
            m = re.match(r"movabs\s+(\w+), 0x100000000000$", t)
            if m:
                memr.add(m.group(1))
        if not ctxr:
            ctxr.add("rcx")
        for rva, t in insns:
            c = classify(t, ctxr, memr)
            n_insn += 1
            cls_s[c] += 1
            w = guest_ip.get(rva, 0)
            cls_w[c] += w
            w_total += w
            if c.startswith("call"):
                m = re.search(r"<([^>+]+)", t)
                tgt = m.group(1) if m else t
                k = c + " " + (tgt if not tgt.startswith("f_") else "f_*")
                calls_s[k] += 1
                calls_w[k] += w
            if out:
                out.write("%s %x %d %s | %s\n" % (name, rva, w, c, t))
    print("\ninstruction classes in those sections: static share / sampled share (%d insns, %d samples)" % (n_insn, w_total))
    for k, _ in sorted(cls_w.items(), key=lambda kv: -kv[1]):
        print("  %-28s %6.1f%%  %6.1f%%" % (k, 100.0 * cls_s[k] / n_insn, 100.0 * cls_w[k] / max(1, w_total)))
    for k in cls_s:
        if k not in cls_w:
            print("  %-28s %6.1f%%  %6.1f%%" % (k, 100.0 * cls_s[k] / n_insn, 0))
    print("\ncalls (static count, samples at the call insn):")
    for k, v in calls_s.most_common(30):
        print("  %6d %5d  %s" % (v, calls_w[k], k))


MEMOP = re.compile(r"(byte|word|dword|qword|xmmword|ymmword) ptr \[([^\]]+)\]")


def classify(t, ctxr, memr):
    op = t.split()[0]
    if op == "call":
        if "<f_" in t: return "call guest (direct)"
        if "qword ptr" in t or re.match(r"call\s+r\w+$", t): return "call indirect"
        m = re.search(r"<([^>+]+)", t)
        n = m.group(1) if m else ""
        if n.startswith("a64_fpx") or n.startswith("recomp_fp") or "fpe" in n: return "call soft-FP"
        if "icall" in n or "ijump" in n or "lookup" in n: return "call icall/lookup"
        return "call runtime"
    if op == "jmp" and "<f_" in t and "+" not in t.split("<")[1]: return "tail call guest"
    if op.startswith("j"): return "branch"
    if op in ("push", "pop") : return "host push/pop"
    if op in ("ret",): return "ret"
    m = MEMOP.search(t)
    if m:
        parts = [p.strip() for p in re.split(r"\s*([+-])\s*", m.group(2))]
        regs = [p for p in parts if re.match(r"^[a-z][a-z0-9]*$", p) and p not in ("+", "-")]
        regs = [r.split("*")[-1] for r in regs]
        disp = 0
        mm = re.search(r"([+-])\s*(0x[0-9a-f]+)\s*$", m.group(2))
        if mm:
            disp = int(mm.group(2), 16) * (-1 if mm.group(1) == "-" else 1)
        # is the operand a destination?
        dst = t.split(None, 1)[1].startswith(m.group(0)) if len(t.split(None, 1)) > 1 else False
        if op in ("cmp", "test") : dst = False
        rw = "store" if dst else "load"
        if op in ("cmp", "test") and len(regs) == 1 and regs[0] in ctxr:
            f = ctx_field(disp)
            if f == "poll": return "poll (cmp ctx.poll)"
            if f == "fpcr/fpsr": return "fp env check"
            return "ctx load %s (cmp)" % f
        if "rip" in regs:
            return "host global " + rw
        if regs and regs[0] in ("rsp",) and len(regs) == 1:
            return "host stack " + rw
        if len(regs) == 1 and regs[0] in ctxr and 0 <= disp < 0x360:
            f = ctx_field(disp)
            if f == "poll": return "poll (ctx.poll)"
            if f == "fpcr/fpsr": return "fp env " + rw
            return "ctx %s %s" % (rw, f)
        if any(r in memr for r in regs) or len(regs) == 2:
            return "guest mem " + rw
        return "other mem " + rw
    if op.startswith("set") or op.startswith("cmov"): return "setcc/cmov"
    if op in ("cmp", "test"): return "cmp/test"
    if op.startswith("v") or op.startswith("cvt"): return "fp/vector alu"
    if op in ("mov", "movzx", "movsx", "movsxd", "movabs", "lea"): return "reg move/lea"
    if op == "nop" or op.startswith("nop"): return "nop"
    return "int alu"


if __name__ == "__main__":
    main()
