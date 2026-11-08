"""Median frame rate and replay time of the gameplay windows of a log (scripts/bench-replay.sh)."""
import re
import statistics
import sys

rows = []
sub = None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.search(r"replay time \(ms\): submit ([\d.]+)", line)
    if m:
        sub = float(m.group(1))
    m = re.search(r"vk: (\d+) presents, ([\d.]+) fps; a frame: (\d+) draws", line)
    if m and sub is not None and int(m.group(3)) >= 1500:
        rows.append((float(m.group(2)), sub, int(m.group(3))))


def med(i):
    return statistics.median(r[i] for r in rows) if rows else 0


print("gameplay windows %d: median fps %.1f, median replay %.2f ms, median draws %d" % (len(rows), med(0), med(1), med(2)))
