"""(TICK-041) python -I tools/bench/guestmix_post.py <out>/guestmix_insns.txt (from guestmix.py --out): guestmix_insns.txt -> skid-corrected shares (a timer sample's IP is the instruction after the one that held
retirement), lane loops (FPX per-lane loops: a back edge over a 4*idx store to the host stack) found per section."""
import collections, re, sys

recs = []
for l in open(sys.argv[1]):
    head, _, ins = l.rstrip("\n").partition(" | ")
    p = head.split(" ", 3)
    recs.append([p[0], int(p[1], 16), int(p[2]), p[3], ins])

# lane loops: per section, backward jumps whose range has a [rsp + 4*reg + d] store
inloop = set()
lanestore_disp = collections.defaultdict(set)
bysec = collections.defaultdict(list)
for i, r in enumerate(recs):
    bysec[r[0]].append(i)
for sec, idx in bysec.items():
    addr2i = {recs[i][1]: i for i in idx}
    for i in idx:
        t = recs[i][4]
        m = re.match(r"j\w*\s+0x([0-9a-f]+)", t)
        if not m:
            continue
        tgt = int(m.group(1), 16) - 0x140000000
        if tgt >= recs[i][1] or tgt not in addr2i:
            continue
        j0 = addr2i[tgt]
        if i - j0 > 80:
            continue
        body = range(j0, i + 1)
        ds = set()
        for k in body:
            mm = re.search(r"dword ptr \[rsp \+ 4\*\w+ \+ (0x[0-9a-f]+)\], ", recs[k][4])
            if mm:
                ds.add(int(mm.group(1), 16))
        if ds:
            for k in body:
                inloop.add(k)
            lanestore_disp[sec] |= ds
# the wide reload of the lane results after the loop (store-forwarding stall)
wide = set()
for sec, idx in bysec.items():
    for i in idx:
        mm = re.search(r"xmmword ptr \[rsp \+ (0x[0-9a-f]+)\]", recs[i][4])
        if mm and recs[i][4].split()[0] in ("vmovaps", "vmovups", "vmovdqa", "vmovdqu") and "], " not in recs[i][4].split(None,1)[1][:40] \
                and int(mm.group(1), 16) in lanestore_disp[sec]:
            wide.add(i)

group = {
    "guest mem load": "guest memory", "guest mem store": "guest memory",
    "host stack load": "host stack (spill/reload)", "host stack store": "host stack (spill/reload)",
    "host push/pop": "call/ret/prologue", "ret": "call/ret/prologue",
    "fp/vector alu": "FP/vector ops and checks", "fp env check": "FP env (fpcr/fpsr)", "fp env load": "FP env (fpcr/fpsr)",
    "fp env store": "FP env (fpcr/fpsr)",
    "poll (ctx.poll)": "poll", "poll (cmp ctx.poll)": "poll",
    "setcc/cmov": "int compute", "int alu": "int compute", "cmp/test": "int compute", "reg move/lea": "int compute",
    "branch": "branch",
    "host global load": "host globals (dirty marks, site cache)", "host global store": "host globals (dirty marks, site cache)",
    "other mem load": "other memory (derived ptrs)", "other mem store": "other memory (derived ptrs)", "nop": "int compute",
}
def grp(c):
    if c.startswith("ctx") and "nzcv" in c: return "ctx flags (nzcv)"
    if c.startswith("ctx"): return "ctx registers (x/sp/vreg)"
    if c.startswith("call") or c.startswith("tail"): return "call/ret/prologue"
    return group.get(c, c)

cls = collections.Counter(); g = collections.Counter(); lane = 0; widew = 0; tot = 0
for i, r in enumerate(recs):
    w = r[2]
    if not w:
        continue
    j = i - 1 if i > 0 and recs[i - 1][0] == r[0] else i
    tot += w
    cls[recs[j][3]] += w
    g[grp(recs[j][3])] += w
    if j in inloop or i in inloop: lane += w
    if j in wide: widew += w
print("samples %d (skid-corrected: charged to the instruction before the IP)" % tot)
for k, v in g.most_common():
    print("  %-40s %5.1f%%" % (k, 100.0 * v / tot))
print("  -- of which FPX lane loops (body)          %5.1f%%" % (100.0 * lane / tot))
print("  -- of which the lanes' 16-byte reload       %5.1f%%" % (100.0 * widew / tot))
print("static: %d lane loops' insns, %d wide reloads" % (len(inloop), len(wide)))
