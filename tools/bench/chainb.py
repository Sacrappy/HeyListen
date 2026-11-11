"""(TICK-054) The frame's serial loop from a HOS_JOBTRACE file: per frame (cut at the Presentation Thread's present end), the
main thread's part up to its last held submit, the held flush, the Presentation Thread's submits and present, and the time
of each traced step on each side (the renderer's marks, gpu/vk/vk_replay.cpp jt_mark).

    python -I tools/bench/chainb.py <jobtrace.txt>

Marks (JT_MARK records, a = kind): 1/2 a submit's capture and replay, 3 a submit held (HOS_NVN_HOLD=2), 5/6 the present;
the renderer's: 9/10 the replay thread waiting for a segment, 11/12 a capture made, 13/14 a dry pass, 15/16 a submission's
memory work (the capture taken), 17 the segments handed out, 18 the replay thread's own segment done, 19/20/21 the mirrors
synced before the segments, what came before submitted, the segments' recordings begun; (TICK-056) 22/23 the early mirror
pre-sync's job (RX_VK_PRESYNC_EARLY, on a worker or taken back by the PT), 24/25 the PT finishing it after the dry pass. Threads: "PT" (the Presentation
Thread), "main" (sead::MainThread), "hold" (any other: the held submits' host thread).
"""
import collections
import sys

SPAN = {1: "submit", 9: "seg wait", 11: "capture made", 13: "dry pass", 15: "memory work", 22: "presync job",
        24: "presync finish"}
SETUP = {19: ("dry", "setup: mirrors synced"), 20: (19, "setup: earlier work submitted"),
         21: (20, "setup: recordings begun"), 17: (21, "setup: inits, handed out"), 18: (17, "own segment recorded")}


def load(path):
    recs, names = [], {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line[0] == "#":
                continue
            if line[0] == "T":
                _, i, _, n = line.rstrip("\n").split(" ", 3)
                names[int(i)] = n
                continue
            t, ty, th, a, b = line.split()
            if ty == "21" and a != "7":
                recs.append((int(t), int(th), int(a, 16), int(b, 16)))
    recs.sort()
    return recs, names


def frames(recs, names):
    pt = next((i for i, n in names.items() if n.startswith("Presentation")), None)
    main = next((i for i, n in names.items() if n == "sead::MainThread"), None)
    who = lambda th: "PT" if th == pt else "main" if th == main else "hold"
    opened, spans = {}, []
    for t, th, k, v in recs:
        if k in (1, 5, 9, 11, 13, 15, 22, 24):
            opened[(th, k)] = t
        elif k in (2, 6, 10, 12, 14, 16, 23, 25):
            b = opened.pop((th, k - 1), None)
            if b is not None:
                spans.append((b, t, who(th), k - 1))
        elif k == 3 or (k in SETUP and th == pt):
            spans.append((t, t, who(th), k))
    spans.sort()
    pres = [s for s in spans if s[2] == "PT" and s[3] == 5]
    rows = []
    for i in range(1, len(pres)):
        fb, fe = pres[i - 1][1], pres[i][1]
        ins = [s for s in spans if fb <= s[0] < fe]
        holds = [s[0] for s in ins if s[3] == 3]
        subs = [s for s in ins if s[2] == "PT" and s[3] == 1]
        if not holds or not subs:
            continue
        d = collections.OrderedDict()
        d["frame"] = fe - fb
        d["main: present end -> last hold"] = holds[-1] - fb
        d["held flush: last hold -> PT's first submit"] = subs[0][0] - holds[-1]
        d["PT: its submits (first begin -> last end)"] = subs[-1][1] - subs[0][0]
        d["PT: last submit end -> present end"] = fe - subs[-1][1]
        for s in ins:
            if s[3] in SPAN:
                key = "%s %s" % (s[2], SPAN[s[3]])
                d[key] = d.get(key, 0) + s[1] - s[0]
        last = {}
        for s in ins:
            if s[2] != "PT":
                continue
            if s[3] == 13:
                last = {"dry": s[1]}
            elif s[3] in SETUP and "dry" in last:
                prev, name = SETUP[s[3]]
                if prev in last:
                    d["PT " + name] = d.get("PT " + name, 0) + s[0] - last[prev]
                last[s[3]] = s[0]
        rows.append(d)
    return rows


def main():
    rows = frames(*load(sys.argv[1]))
    n = len(rows)
    print("frames", n)
    keys = []
    for r in rows:
        keys += [k for k in r if k not in keys]
    for k in keys:
        v = sorted(r.get(k, 0) / 1e6 for r in rows)
        print("  %-46s mean %6.2f  p50 %6.2f  p90 %6.2f" % (k, sum(v) / n, v[n // 2], v[int(n * 0.9)]))


if __name__ == "__main__":
    main()
