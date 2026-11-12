"""Which lifted functions read the game's "current core + 1" TLS slot (the slot's variable at 0x84e9f7ac, or the GOT entry
at 0x84ca78d0 that points at it), and what they do with the value next: usage tlsusers.py [addr hex ...] [-v]"""
import os, re, sys

GEN = os.path.join(os.environ["LOCALAPPDATA"], "HookshotBuilder", "sa", "gen")
targets = [int(a, 16) for a in sys.argv[1:] if not a.startswith("-")] or [0x84e9f7ac, 0x84ca78d0]
verbose = "-v" in sys.argv
ADRP = re.compile(r"c->x\[(\d+)\]=g_module_base \+ 0x([0-9a-f]+)ULL \+ \(int64_t\)(-?\d+);")
ADD = re.compile(r"\{ uint64_t _a=c->x\[(\d+)\], _b=(\d+)ULL; uint64_t _r=_a\+_b; c->x\[(\d+)\]=_r; \}")
LOAD = re.compile(r"c->x\[(\d+)\]=recomp_load(?:32|64)\(c,c->x\[(\d+)\] \+ (\d+)\);")
HEAD = re.compile(r"^void (f_[0-9a-f]+)\(Ctx")
hits = {}
for name in sorted(os.listdir(GEN)):
    if not (name.startswith("code_") and name.endswith(".c")):
        continue
    lines = open(os.path.join(GEN, name), encoding="utf-8", errors="replace").read().split("\n")
    fn = None
    for i, line in enumerate(lines):
        m = HEAD.match(line)
        if m:
            fn = m.group(1)
            continue
        m = ADRP.search(line)
        if not m:
            continue
        reg, base = m.group(1), int(m.group(2), 16) + int(m.group(3))
        for k in range(i + 1, min(i + 4, len(lines))):   # the add or the load right after
            a = ADD.search(lines[k])
            l = LOAD.search(lines[k])
            addr = None
            if a and a.group(1) == reg and a.group(3) == reg:
                addr = base + int(a.group(2))
            elif l and l.group(2) == reg:
                addr = base + int(l.group(3))
            if addr in targets:
                hits.setdefault(fn, []).append((name, i, addr))
                break
print("%d functions use %s" % (len(hits), " / ".join("%#x" % t for t in targets)))
for fn, where in sorted(hits.items()):
    print(fn, where[0][0], len(where), "uses")
    if verbose:
        name, i, _ = where[0]
        lines = open(os.path.join(GEN, name), encoding="utf-8", errors="replace").read().split("\n")
        for l in lines[i:i + 14]:
            print("     ", l[:170])
