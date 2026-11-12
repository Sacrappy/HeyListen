"""Counts the places in the lifted code that match the current thread's id against the BaseProc manager's six executor ids
(the table at [manager + 128]: ids at +32..+52, their current procs at +56.., a seventh id at +104 with its proc at +112):
a 32-bit load at +32, +36, +40, +44, +48 and +52 of one register within a short window. usage: idsites.py [-v]"""
import os, re, sys

GEN = os.path.join(os.environ["LOCALAPPDATA"], "HookshotBuilder", "sa", "gen")
verbose = "-v" in sys.argv
LOAD = re.compile(r"recomp_load32\(c,c->x\[(\d+)\] \+ (\d+)\)")
LDP = re.compile(r"_b=c->x\[(\d+)\]; int64_t _o=(\d+); \{ uint64_t _p0,_p1; recomp_ldp32")
HEAD = re.compile(r"^void (f_[0-9a-f]+)\(Ctx")
sites = {}
for name in sorted(os.listdir(GEN)):
    if not (name.startswith("code_") and name.endswith(".c")):
        continue
    lines = open(os.path.join(GEN, name), encoding="utf-8", errors="replace").read().split("\n")
    fn = None
    recent = []   # (line, reg, offset)
    for i, line in enumerate(lines):
        m = HEAD.match(line)
        if m:
            fn = m.group(1)
            recent = []
            continue
        for m in LOAD.finditer(line):
            recent.append((i, m.group(1), int(m.group(2))))
        m = LDP.search(line)
        if m:
            o = int(m.group(2))
            recent.append((i, m.group(1), o))
            recent.append((i, m.group(1), o + 4))
        recent = [r for r in recent if i - r[0] < 40]
        for reg in set(r[1] for r in recent):
            offs = set(r[2] for r in recent if r[1] == reg)
            if {32, 36, 40, 44, 48, 52} <= offs:
                first = min(r[0] for r in recent if r[1] == reg)
                key = (fn, name)
                if not sites.get(key) or first - sites[key][-1] > 40:
                    sites.setdefault(key, []).append(first)
                recent = [r for r in recent if r[1] != reg]
total = sum(len(v) for v in sites.values())
print("%d sites in %d functions" % (total, len(sites)))
for (fn, name), where in sorted(sites.items()):
    print(fn, name, len(where))
