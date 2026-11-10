"""The game's job chain from a HOS_JOBTRACE file (engine/host/hos_jobtrace.c, TICK-039).

    python -I tools/bench/jobtrace.py <trace.txt> [--present <present_trace.csv>] [--lr 80080188]

Per thread: the share of the window working, waiting (by kind and guest caller), spinning, in SleepThread polls and
waiting for a virtual core. Then the main thread (sead::MainThread) per frame. Then, for each main-thread wait from
the given caller (the job group, f_8007fe60, by default the one with the most time), the path back from its wake-up:
who woke it, what that thread did before (work, its own wake-up, ...), until the start of the wait. Each path
segment is one of:
  work       a thread running between its waits (guest code and the runtime under it); a wait that did not block
             (the event already set) counts as work of the thread that made the call
  wake       from a signal to the woken thread returning from its wait (Windows wake-up, lock hand-off, virtual core)
  poll       a SleepThread poll on the path (the thread looks again after each sleep; who it waits for is unknown)
  vcore      a wait for a virtual core on the path
  unknown    a blocked wait whose waker is not in the trace (a host thread, or before the window)
A thread that blocked is left at its wake-up for the thread that woke it. Spinning in the host's 20 us spin before a
block is part of the wait, not work.
"""
import argparse, bisect, collections, os, sys

EV_WAIT_B, EV_WAIT_E, EV_SIGNAL, SPIN_E, CV_WAIT_B, CV_WAIT_E, CV_SIGNAL, MX_WAIT_B, MX_WAIT_E, MX_GIVE, SLEEP_B, SLEEP_E, \
    VC_WAIT_B, VC_WAIT_E, Q_WAIT_B, Q_WAIT_E, Q_SIGNAL, K_WAIT_B, K_WAIT_E, K_WAKE, MARK = range(1, 22)
WAIT_PAIRS = {EV_WAIT_B: (EV_WAIT_E, "event"), CV_WAIT_B: (CV_WAIT_E, "condvar"), MX_WAIT_B: (MX_WAIT_E, "mutex"),
              Q_WAIT_B: (Q_WAIT_E, "queue"), K_WAIT_B: (K_WAIT_E, "kernel")}
SIGNAL_OF = {"event": EV_SIGNAL, "condvar": CV_SIGNAL, "mutex": MX_GIVE, "queue": Q_SIGNAL}


class Wait:
    __slots__ = ("kind", "key", "lr", "b", "e", "spin_e", "how", "waker", "wake_t")

    def __init__(self, kind, key, lr, b):
        self.kind, self.key, self.lr, self.b = kind, key, lr, b
        self.e = self.spin_e = None
        self.how = 0
        self.waker = self.wake_t = None


def load(path):
    names, host = {}, {}
    window = None
    recs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line[0] == "#":
                p = line.split()
                if len(p) >= 5 and p[1] == "jobtrace":
                    window = (int(p[3]), int(p[4]))
                continue
            if line[0] == "T":
                _, i, ptr, name = line.rstrip("\n").split(" ", 3)
                names[int(i)] = name
                host[int(ptr, 16)] = int(i)
                continue
            t, ty, th, a, b = line.split()
            recs.append((int(t), int(ty), int(th), int(a, 16), int(b, 16)))
    recs.sort()
    return names, host, window, recs


def build(names, host, recs):
    """per thread: its waits (blocking calls), sleeps, vcore waits; signals by (type, key) and kernel wakes by thread"""
    waits = collections.defaultdict(list)
    sleeps = collections.defaultdict(list)
    vcs = collections.defaultdict(list)
    open_w, open_s, open_v = {}, {}, {}
    signals = collections.defaultdict(list)   # (type, key) -> [(t, thread, lr)]
    kwakes = collections.defaultdict(list)    # thread idx -> [(t, waker thread)]
    end_type = {v[0]: k for k, v in WAIT_PAIRS.items()}
    for t, ty, th, a, b in recs:
        if ty in WAIT_PAIRS:
            open_w[th] = Wait(WAIT_PAIRS[ty][1], a, b, t)
        elif ty == SPIN_E:
            w = open_w.get(th)
            if w and w.key == a:
                w.spin_e = t
        elif ty in end_type:
            w = open_w.pop(th, None)
            if w and w.key == a:
                w.e, w.how = t, b
                waits[th].append(w)
        elif ty in (EV_SIGNAL, CV_SIGNAL, MX_GIVE, Q_SIGNAL):
            signals[(ty, a)].append((t, th, b))
        elif ty == K_WAKE:
            if a in host:
                kwakes[host[a]].append((t, th))
        elif ty == SLEEP_B:
            open_s[th] = (t, a, b)
        elif ty == SLEEP_E:
            s = open_s.pop(th, None)
            if s:
                sleeps[th].append((s[0], t, s[1], s[2]))
        elif ty == VC_WAIT_B:
            open_v[th] = t
        elif ty == VC_WAIT_E:
            s = open_v.pop(th, None)
            if s is not None:
                vcs[th].append((s, t))
    # who woke each blocked wait: the first signal on its object after it began (kernel: the first wake of the thread)
    sig_t = {k: [x[0] for x in v] for k, v in signals.items()}
    kw_t = {k: [x[0] for x in v] for k, v in kwakes.items()}
    for th, ws in waits.items():
        for w in ws:
            if w.kind == "kernel":
                lst, ts = kwakes.get(th, []), kw_t.get(th, [])
            else:
                key = (SIGNAL_OF[w.kind], w.key)
                lst, ts = signals.get(key, []), sig_t.get(key, [])
            i = bisect.bisect_right(ts, w.b)
            if w.how & 3 or w.kind in ("kernel", "condvar", "mutex"):   # blocked (spun or slept)
                if i < len(ts) and ts[i] <= w.e:
                    w.wake_t, w.waker = lst[i][0], lst[i][1]
            else:   # ready: the signal before it
                if i > 0:
                    w.wake_t, w.waker = lst[i - 1][0], lst[i - 1][1]
    return waits, sleeps, vcs, signals


def blocked(w):
    return w.wake_t is not None and w.wake_t > w.b


def map_objects(path):
    """(TICK-040) f_<addr> -> the object file it was linked from (the map's last column); {} without a map"""
    objs = {}
    if not os.path.exists(path):
        return objs
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) >= 4 and p[1].startswith("f_") and p[-1].endswith(".o"):
                objs[p[1]] = p[-1]
    return objs


def path_samples(a, names, segs, frames):
    """the HOS_SAMPLE_ALL samples (same run) of the threads on the path while they are on it: guest code, the runtime
    under it, the system (a leaf outside the exe and the dll), by leaf and by innermost guest function"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "scripts"))
    import prof
    maps = {}
    for mod in ("HeyListen.exe", "hookshot_vk.dll"):
        mp = os.path.join(a.maps, mod[:-4] + ".map")
        if os.path.exists(mp):
            maps[mod] = prof.load_map(mp, 0x180000000 if mod.endswith(".dll") else 0x140000000)
    cache = {}

    def name(frame):
        if frame not in cache:
            mod, _, off = frame.rpartition("+")
            r = mod
            if mod in maps:
                addrs, nm = maps[mod]
                i = bisect.bisect_right(addrs, int(off, 16)) - 1
                if i >= 0:
                    r = mod.split(".")[0] + ":" + prof.demangle(nm[i])
            cache[frame] = r
        return cache[frame]
    byname = {v: k for k, v in names.items()}
    starts = {th: [s[0] for s in sorted(v)] for th, v in segs.items()}
    srt = {th: sorted(v) for th, v in segs.items()}
    t0 = a.sample_t0
    cat = collections.Counter()
    leaf = collections.Counter()
    fn = collections.Counter()
    hc = collections.Counter()
    coldfn = collections.Counter()
    objs = map_objects(os.path.join(a.maps, "HeyListen.map"))
    n = 0
    for line in open(a.samples, encoding="utf-8", errors="replace"):
        head, _, stack = line.rstrip("\n").partition("|")
        ms, _, thread = head.partition(" ")
        if not stack or not ms.isdigit():
            continue
        th = byname.get(thread)
        if th is None or th not in srt:
            continue
        frames_ = stack.split(";")
        if frames_[-1].startswith("@"):   # (HOS_SAMPLE_NS: the sample's own instant)
            t = int(frames_.pop()[1:])
        elif t0 is not None:
            t = t0 + int(ms) * 1000000   # (1 ms coarse: only for long segments)
        else:
            continue
        i = bisect.bisect_right(starts[th], t) - 1
        if i < 0 or srt[th][i][1] < t:
            continue
        n += 1
        named = [name(f) for f in frames_]
        lf = named[0]
        if lf.startswith("HeyListen:"):
            c = "guest code" if lf.startswith("HeyListen:f_") else "runtime"
        elif lf.startswith("hookshot_vk:"):
            c = "renderer"
        else:
            c = "system"
        cat[c] += 1
        leaf[(c, lf if c != "system" else lf + " <- " + next((x for x in named if x.startswith("HeyListen:")), "?"))] += 1
        inner = next((x for x in named if x.startswith("HeyListen:f_")), "?")
        fn[inner] += 1
        if objs:   # (TICK-040) where the innermost guest function was compiled: code_hot_*.o (RX_HOT_OPT + profile) or a chunk (-O1)
            where = "hot" if objs.get(inner[10:], "").startswith("code_hot") else "cold" if inner != "?" else "none"
            hc[(c, where)] += 1
            if where == "cold":
                coldfn[inner] += 1
    print("\n## samples on the path's work (%d samples)" % n)
    for k, v in cat.most_common():
        print("  %-12s %5.1f%%" % (k, 100 * v / max(n, 1)))
    print("  leaves:")
    for (c, k), v in leaf.most_common(a.top * 2):
        print("    %5.1f%%  %-10s %s" % (100 * v / max(n, 1), c, k))
    print("  innermost guest function:")
    for k, v in fn.most_common(a.top * 2):
        print("    %5.1f%%  %s" % (100 * v / max(n, 1), k))
    if objs:
        print("  hot / cold (TICK-040: where the innermost guest function was compiled, code_hot_*.o or an -O1 chunk):")
        for c in ("guest code", "runtime", "system"):
            h, k = hc[(c, "hot")], hc[(c, "cold")]
            if h + k:
                print("    %-10s hot %5.1f%%  cold %5.1f%%  of the path's samples (%.0f%% of these hot)" % (
                    c, 100 * h / n, 100 * k / n, 100 * h / (h + k)))
        h = sum(v for (c, w), v in hc.items() if w == "hot")
        k = sum(v for (c, w), v in hc.items() if w == "cold")
        print("    all        hot %5.1f%%  cold %5.1f%%" % (100 * h / max(n, 1), 100 * k / max(n, 1)))
        print("  cold functions on the path:")
        for k, v in coldfn.most_common(a.top * 2):
            print("    %5.1f%%  %s" % (100 * v / max(n, 1), k))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--present", help="present_trace.csv of the run (frames in the window)")
    ap.add_argument("--lr", help="the main thread's job-group wait caller (hex); default: its largest event wait")
    ap.add_argument("--main", default="sead::MainThread")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--samples", help="a HOS_SAMPLE_ALL file of the same run")
    ap.add_argument("--sample-t0", type=int, help="its t0 (ns), from the log's 'prof: all-thread sampling ... (t0 N ns)'")
    ap.add_argument("--maps", help="the folder with that build's HeyListen.map and hookshot_vk.map")
    a = ap.parse_args()
    names, host, window, recs = load(a.trace)
    t0, t1 = window if window else (recs[0][0], recs[-1][0])
    span = (t1 - t0) / 1e6
    waits, sleeps, vcs, signals = build(names, host, recs)
    frames = None
    if a.present:
        n = 0
        for line in open(a.present):
            p = line.strip().split(",")
            if p[0].isdigit() and t0 <= int(p[1]) <= t1:
                n += 1
        frames = n
    print("window %.0f ms, %d records, %s frames" % (span, len(recs), frames))
    per = span / frames if frames else None

    # ---- per thread states
    print("\n## threads (ms in the window; per frame in brackets)")
    rows = []
    for th, name in names.items():
        wt = collections.Counter()
        spin = 0
        for w in waits.get(th, []):
            if w.how & 3:
                wt[w.kind] += w.e - w.b
                if w.spin_e:
                    spin += w.spin_e - w.b
        sl = sum(e - b for b, e, _, _ in sleeps.get(th, []))
        vc = sum(e - b for b, e in vcs.get(th, []))
        tot = sum(wt.values()) + sl
        rows.append((name, wt, spin, sl, vc, tot))
    rows.sort(key=lambda r: r[5])
    for name, wt, spin, sl, vc, tot in rows:
        if not tot and not sl:
            continue
        work = span - tot / 1e6
        f = lambda v: "%.1f" % (v / 1e6) + (" (%.2f)" % (v / 1e6 / frames) if frames else "")
        print("%-32s work %6.1f ms (%3.0f%%)  waits %s  spin %s  sleep %s  vcore %s" % (
            name, work, 100 * work / span, ", ".join("%s %s" % (k, f(v)) for k, v in wt.most_common()), f(spin), f(sl), f(vc)))

    main_th = [i for i, n in names.items() if n == a.main]
    if not main_th:
        return
    m = main_th[0]
    print("\n## %s: blocking waits by caller (ms, count, how: ready/spun/slept)" % a.main)
    by = collections.defaultdict(lambda: [0, 0, collections.Counter()])
    for w in waits[m]:
        k = (w.kind, w.lr)
        by[k][0] += w.e - w.b
        by[k][1] += 1
        by[k][2][w.how & 3] += 1
    for (kind, lr), (ns, n, how) in sorted(by.items(), key=lambda x: -x[1][0])[:a.top]:
        print("  %-8s lr %08x: %8.1f ms%s  %5d waits  %s" % (kind, lr, ns / 1e6, " (%.2f/frame)" % (ns / 1e6 / frames) if frames else "",
                                                           n, dict(how)))
    sl = collections.defaultdict(lambda: [0, 0, 0])
    for b, e, ask, lr in sleeps[m]:
        sl[lr][0] += e - b
        sl[lr][1] += 1
        sl[lr][2] += ask
    for lr, (ns, n, ask) in sorted(sl.items(), key=lambda x: -x[1][0])[:6]:
        print("  sleep    lr %08x: %8.1f ms  %5d sleeps, asked %.1f us, slept %.1f us each" % (lr, ns / 1e6, n, ask / n / 1e3, ns / n / 1e3))

    lr = int(a.lr, 16) if a.lr else max(((k, v) for k, v in by.items() if k[0] == "event"), key=lambda x: x[1][0])[0][1]
    group = [w for w in waits[m] if w.kind == "event" and w.lr == lr and w.how & 3]
    print("\n## the path of the main thread's waits from lr %08x (%d blocked waits, %.1f ms)" % (
        lr, len(group), sum(w.e - w.b for w in group) / 1e6))

    # indexes for the walk back
    wait_ends = {th: [w.e for w in ws] for th, ws in waits.items()}
    sleep_ends = {th: [s[1] for s in ss] for th, ss in sleeps.items()}
    vc_ends = {th: [v[1] for v in vv] for th, vv in vcs.items()}

    def walk(th, t, start, out, depth=0):
        """the path on thread th from `start` up to t (th is running at t, or returning from a wait at t)"""
        last = None
        while t > start and depth < 1000000:
            depth += 1
            if last is not None and t >= last:
                t = last - 1   # (a block of no length: step past it)
            last = t
            # the last block of th that ended at or before t: a wait, a sleep or a vcore wait
            cand = None
            ws = wait_ends.get(th, [])
            i = bisect.bisect_right(ws, t) - 1
            while i >= 0 and not (waits[th][i].how & 3 or blocked(waits[th][i])):
                i -= 1   # (waits that did not block are work)
            if i >= 0:
                cand = ("wait", waits[th][i].e, waits[th][i])
            ss = sleep_ends.get(th, [])
            j = bisect.bisect_right(ss, t) - 1
            if j >= 0 and (not cand or ss[j] > cand[1]):
                cand = ("sleep", ss[j], sleeps[th][j])
            vv = vc_ends.get(th, [])
            k = bisect.bisect_right(vv, t) - 1
            if k >= 0 and (not cand or vv[k] > cand[1]):
                cand = ("vcore", vv[k], vcs[th][k])
            if not cand or cand[1] <= start:
                out.append(("work", th, t - start, t))
                return
            kind, e, obj = cand
            if t > e:
                out.append(("work", th, t - e, t))
            t = e
            if kind == "sleep":
                b = max(obj[0], start)
                out.append(("poll", th, t - b, obj[3]))
                t = b
            elif kind == "vcore":
                b = max(obj[0], start)
                out.append(("vcore", th, t - b, None))
                t = b
            else:
                w = obj
                if w.waker is None or w.wake_t is None or w.wake_t <= w.b:
                    b = max(w.b, start)
                    out.append(("unknown", th, t - b, (w.kind, w.lr)))
                    t = b
                    continue
                ws_ = max(w.wake_t, start)
                out.append(("wake", th, t - ws_, (w.kind, w.lr, w.how & 3)))
                if w.wake_t <= start:
                    return
                th, t = w.waker, w.wake_t
        return

    agg = collections.Counter()
    agg_th = collections.Counter()
    agg_wake = collections.Counter()
    agg_unknown = collections.Counter()
    agg_poll = collections.Counter()
    total = 0
    segs = collections.defaultdict(list)   # thread -> path work segments (begin, end)
    for w in group:
        out = []
        if w.waker is None:
            agg["unattributed"] += w.e - w.b
            total += w.e - w.b
            continue
        # the main thread's own wake-up, then the signaller's path
        out.append(("wake", m, w.e - w.wake_t, (w.kind, w.lr, w.how & 3)))
        walk(w.waker, w.wake_t, w.b, out)
        for kind, th, d, info in out:
            if kind == "work" and d > 0:
                segs[th].append((info - d, info))
            agg[kind] += d
            agg_th[(kind, names[th])] += d
            if kind == "wake":
                agg_wake[(names[th], info[0], info[2])] += d
            if kind == "unknown":
                agg_unknown[(names[th],) + info] += d
            if kind == "poll":
                agg_poll[(names[th], info)] += d
        total += w.e - w.b
    print("  path covered: %.1f ms of %.1f ms" % (sum(agg.values()) / 1e6, total / 1e6))
    f = (lambda v: "%7.1f ms  %.2f ms/frame" % (v / 1e6, v / 1e6 / frames)) if frames else (lambda v: "%7.1f ms" % (v / 1e6))
    for k, v in agg.most_common():
        print("  %-12s %s  %4.1f%%" % (k, f(v), 100 * v / max(total, 1)))
    print("  by thread:")
    for (k, n), v in agg_th.most_common(25):
        print("    %-8s %-30s %s" % (k, n, f(v)))
    print("  wake-ups on the path (thread, object, 1 spun / 2 slept):")
    for (n, kind, how), v in agg_wake.most_common(12):
        print("    %-30s %-8s %d  %s" % (n, kind, how, f(v)))
    if agg_poll:
        print("  polls on the path:")
        for (n, lr_), v in agg_poll.most_common(8):
            print("    %-30s lr %08x  %s" % (n, lr_, f(v)))
    if agg_unknown:
        print("  waits with no waker found:")
        for k, v in agg_unknown.most_common(8):
            print("    %s %s" % (k, f(v)))

    if a.samples:
        path_samples(a, names, segs, frames)

    # how many job threads work while the main thread waits for the group (a chain of dependent jobs, or not enough
    # threads for the work): a sweep over every thread's work/idle edges inside the group's waits
    jobs = [i for i, n in names.items() if n.startswith(("BaseProcExecutor", "ModuleSystemWorker", "NavMesh"))]
    edges = []
    for th in jobs:
        idle = [(w.b, w.e) for w in waits.get(th, []) if w.how & 3] + [(b, e) for b, e, _, _ in sleeps.get(th, [])]
        idle.sort()
        for b, e in idle:
            edges.append((b, -1, th))
            edges.append((e, +1, th))
    edges.sort()
    hist = collections.Counter()
    ex_hist = collections.Counter()
    gw = sorted((w.b, w.e) for w in group)
    gi = 0
    working = set(jobs)   # (a thread is working unless inside an idle interval)
    prev = t0
    for t, d, th in edges:
        # time from prev to t: count it if inside a group wait
        while gi < len(gw) and gw[gi][1] <= prev:
            gi += 1
        k = gi
        while k < len(gw) and gw[k][0] < t:
            lo, hi = max(prev, gw[k][0]), min(t, gw[k][1])
            if hi > lo:
                hist[len(working)] += hi - lo
                ex_hist[sum(1 for x in working if names[x].startswith("BaseProc"))] += hi - lo
            k += 1
        if d < 0:
            working.discard(th)
        else:
            working.add(th)
        prev = t
    tot = sum(hist.values())
    if tot:
        print("\n## job threads working during the group's waits (share of that time)")
        print("  all job threads: " + "  ".join("%d: %.0f%%" % (k, 100 * hist[k] / tot) for k in sorted(hist)))
        print("  executors (of 6): " + "  ".join("%d: %.0f%%" % (k, 100 * ex_hist[k] / tot) for k in sorted(ex_hist)))
        print("  mean executors working: %.2f" % (sum(k * v for k, v in ex_hist.items()) / tot))

    # wake latency overall: blocked waits, signal -> return, by thread
    print("\n## wake latency (blocked waits with a waker: signal -> back from the wait), by thread")
    for th, name in sorted(names.items(), key=lambda x: x[1]):
        lat = [w.e - w.wake_t for w in waits.get(th, []) if blocked(w) and w.how & 3]
        if len(lat) < 50:
            continue
        lat.sort()
        slept = [w.e - w.wake_t for w in waits.get(th, []) if blocked(w) and w.how & 3 == 2]
        print("  %-32s %6d  mean %5.1f us  p50 %5.1f  p90 %6.1f  p99 %6.1f  (slept %d, mean %.1f us)  sum %.1f ms" % (
            name, len(lat), sum(lat) / len(lat) / 1e3, lat[len(lat) // 2] / 1e3, lat[len(lat) * 9 // 10] / 1e3,
            lat[len(lat) * 99 // 100] / 1e3, len(slept), sum(slept) / max(len(slept), 1) / 1e3, sum(lat) / 1e6))

    # the busiest signallers (calls a second): host overhead of signals nobody waits for
    print("\n## signals by thread and type (calls in the window)")
    c = collections.Counter()
    for (ty, key), lst in signals.items():
        for t, th, lr_ in lst:
            c[(names[th], ty, lr_)] += 1
    tyname = {EV_SIGNAL: "event", CV_SIGNAL: "condvar", MX_GIVE: "mutex give", Q_SIGNAL: "queue/sema"}
    for (n, ty, lr_), v in c.most_common(a.top):
        print("  %-30s %-10s lr %08x  %8d" % (n, tyname[ty], lr_, v))


if __name__ == "__main__":
    main()
