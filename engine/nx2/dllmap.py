"""hookshot_game.dll offset -> lifted function (f_<guest address>) + offset, from the linker map (build.py --dll).

Usage:  python -I nx2/dllmap.py <hex offset> [...]
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit

BASE = 0x180000000


def load():
    rows = []
    for line in open(os.path.join(kit.WORK, "build", "hookshot_game.map"), errors="replace"):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})", line)
        if m:
            rows.append((int(m.group(2), 16), m.group(1)))
    rows.sort()
    return rows


def main():
    rows = load()
    keys = [r[0] for r in rows]
    for a in sys.argv[1:]:
        t = BASE + int(a, 16)
        i = bisect.bisect_right(keys, t) - 1
        print("%s: %s+%#x" % (a, rows[i][1], t - rows[i][0]) if i >= 0 else "%s: ?" % a)


if __name__ == "__main__":
    main()
