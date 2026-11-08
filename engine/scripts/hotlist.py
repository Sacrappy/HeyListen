"""Hot lifted functions from an all-thread sample (HOS_SAMPLE_ALL, scripts/crit.py): for each sample of a game thread
running in hookshot.exe, the innermost lifted function (f_...) is charged. Writes the functions that make up the
first <share> of those samples, hottest first, one a line, for the lift's A64_HOT_FUNCS (their FP ops call the
helpers' inline fast paths; with A64_HOT_CHUNK=1 they are compiled together in code_hot.c).

    python -I scripts/hotlist.py work/runs/<run>_all.txt <out> [share 0.85] [max 1500]
"""
import bisect, collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prof  # noqa: E402

addrs, names = prof.load_map(os.path.join(prof.BUILD, "hookshot.map"), 0x140000000)
share = float(sys.argv[3]) if len(sys.argv) > 3 else 0.85
cap = int(sys.argv[4]) if len(sys.argv) > 4 else 1500
c = collections.Counter()
total = 0
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    head, _, st = line.rstrip("\n").partition("|")
    if not st or head.split(" ", 1)[-1].startswith("mkw_vk"):
        continue
    frames = st.split(";")
    if not frames[0].startswith("hookshot.exe+"):
        continue
    for f in frames:
        mod, _, off = f.rpartition("+")
        if mod != "hookshot.exe":
            continue
        i = bisect.bisect_right(addrs, int(off, 16)) - 1
        n = names[i] if i >= 0 else ""
        if n.startswith("f_"):
            c[n] += 1
            total += 1
            break
out, acc = [], 0
for n, k in c.most_common():
    if acc >= share * total or len(out) >= cap:
        break
    out.append(n)
    acc += k
open(sys.argv[2], "w").write("".join(n + "\n" for n in out))
print("%d samples in %d functions; %d functions cover %.0f%%" % (total, len(c), len(out), 100.0 * acc / max(1, total)))
