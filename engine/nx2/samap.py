"""HeyListen.exe (hookshot.exe before) offset -> lifted function (f_<guest address>) + offset, from work/sa/build/hookshot.map (build.py --standalone).

Usage:  python -I nx2/samap.py <hex offset> [...]
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit

rows = []
_build = os.path.join(kit.WORK, "sa", "build")
_map = next((os.path.join(_build, m) for m in ("HeyListen.map", "hookshot.map") if os.path.exists(os.path.join(_build, m))),
            os.path.join(_build, "HeyListen.map"))   # (HeyListen.exe since TICK-034; hookshot.map in older builds)
for line in open(_map, errors="replace"):
    m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})", line)
    if m:
        rows.append((int(m.group(2), 16), m.group(1)))
rows.sort()
keys = [r[0] for r in rows]
for a in sys.argv[1:]:
    t = 0x140000000 + int(a, 16)
    i = bisect.bisect_right(keys, t) - 1
    print("%s: %s+%#x" % (a, rows[i][1], t - rows[i][0]))
