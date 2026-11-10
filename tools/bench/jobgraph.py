"""The game's job graph from a HOS_JOBTRACE taken with a job-marked build (TICK-042).

A job-marked build is a diagnostic lift: tools/bench/jobmarks.py wraps the job dispatcher's calls (f_8004ab40 and its
runners, f_8007fe60, f_80df25c0) in hos_jt_job() marks, which the trace records as JT_JOB_B / JT_JOB_E (22 / 23):
the job object (x0), the job function (the call's target) and the call site.

    python -I tools/bench/jobgraph.py <jobtrace.txt> [--samples samples.txt --maps <build folder>]
        [--gantt <frame index>] [--step-ms 0.1] [--job <target>[:<obj>] ...]

Prints:
  - the frame (from the main thread's frame-end wait, lr 0x83acfcf4) split by what runs: the main thread alone, the
    main thread with job threads, the main thread waiting while 0..6 executors are in a job;
  - per job function: time a frame and how many executors ran it in a frame (a stage limited to fewer executors);
  - the long jobs (one object, > 300 us): how late in its stage each one starts and how long it then runs alone;
  - --gantt N: frame N in steps (each executor, module worker and the main thread: the job function's low 16 bits and
    the busy share of the step; ".." busy outside a job);
  - --job T[:O]: with --samples / --maps (HOS_SAMPLE_ALL + HOS_SAMPLE_NS of the same run), where the job spends its
    time (self, inclusive, the chain below the job function), its blocking waits and who ended them.
"""
import argparse, bisect, collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jobtrace as J

JOB_B, JOB_E = 22, 23
FRAME_LR = 0x83acfcf4


def load(path):
    names, host, window, recs = J.load(path)
    waits, sleeps, vcs, signals = J.build(names, host, recs)
    jobs = collections.defaultdict(list)   # thread -> (begin, end, target, site, obj, depth)
    stk = collections.defaultdict(list)
    for t, ty, th, a, b in recs:
        if ty == JOB_B:
            stk[th].append((t, b & 0xffffffff, b >> 32, a))
        elif ty == JOB_E and stk[th]:
            b0, tg, site, obj = stk[th].pop()
            jobs[th].append((b0, t, tg, site, obj, len(stk[th])))
    for v in jobs.values():
        v.sort()
    return names, waits, sleeps, signals, jobs, recs[-1][0] if recs else 0


def idle_of(th, waits, sleeps):
    return sorted([(w.b, w.e) for w in waits.get(th, []) if w.how & 3] + [(b, e) for b, e, _, _ in sleeps.get(th, [])])


def frames_of(names, waits, tend):
    m = next(i for i, n in names.items() if n == "sead::MainThread")
    fr = [w.e for w in waits[m] if w.lr == FRAME_LR and w.e < tend]
    return m, fr


def breakdown(names, waits, sleeps, jobs, m, fr):
    JT = [i for i, n in names.items() if n.startswith(("BaseProcExecutor", "ModuleSystemWorker"))]
    EX = [i for i, n in names.items() if n.startswith("BaseProcExecutor")]
    ev = []
    for th in JT + [m]:
        for b, e in idle_of(th, waits, sleeps):
            ev.append((b, -1, th))
            ev.append((e, 1, th))
    for th in EX:
        for y in jobs.get(th, []):
            if y[5] == 0:
                ev.append((y[0], 2, th))
                ev.append((y[1], -2, th))
    ev.sort()
    running, injob, c, prev = set(JT + [m]), collections.Counter(), collections.Counter(), fr[0]
    for t, d, th in ev:
        if t > fr[-1]:
            break
        if t > prev >= fr[0]:
            dt, njt = t - prev, sum(1 for x in JT if x in running)
            if m in running:
                c["main thread running, no job thread working" if not njt else "main thread running, job threads working"] += dt
            else:
                c["main thread waiting, %d executors in a job" % sum(1 for x in EX if injob[x] > 0)] += dt
        if d == -1:
            running.discard(th)
        elif d == 1:
            running.add(th)
        else:
            injob[th] += 1 if d == 2 else -1
        prev = t
    nf = len(fr) - 1
    print("## the frame: %d frames, %.2f ms a frame" % (nf, (fr[-1] - fr[0]) / nf / 1e6))
    for k in sorted(c):
        print("  %-55s %5.2f ms" % (k, c[k] / nf / 1e6))


def stages(names, jobs, fr, top):
    EX = [i for i, n in names.items() if n.startswith("BaseProcExecutor")]
    per, tt = collections.defaultdict(collections.Counter), collections.Counter()
    for k in range(len(fr) - 1):
        by = collections.defaultdict(set)
        for x in EX:
            for y in jobs.get(x, []):
                if fr[k] <= y[0] < fr[k + 1] and y[5] == 0:
                    key = "%08x@%08x" % (y[2], y[3])
                    by[key].add(x)
                    tt[key] += y[1] - y[0]
        for key, s in by.items():
            per[key][len(s)] += 1
    nf = len(fr) - 1
    print("\n## executor jobs by function (function@call site: ms a frame; executors that ran it in a frame: frames)")
    for key, v in tt.most_common(top):
        print("  %-20s %6.3f ms   %s" % (key, v / nf / 1e6, dict(sorted(per[key].items()))))


def long_jobs(names, jobs, fr, top):
    EX = [i for i, n in names.items() if n.startswith("BaseProcExecutor")]
    alljobs = sorted((y[0], y[1], y[2], y[4], x) for x in EX for y in jobs.get(x, []) if y[5] == 0)
    nf = len(fr) - 1
    d, c = collections.Counter(), collections.Counter()
    for b, e, t, o, x in alljobs:
        if e - b > 300000:
            d[(t, o)] += e - b
            c[(t, o)] += 1
    print("\n## long jobs (one object, runs over 300 us): ms a frame, runs; start after its stage's first job of that function;"
          " rank; time it runs on after the other jobs of that function ended (medians)")
    byt = collections.defaultdict(list)
    for j in alljobs:
        byt[j[2]].append(j)
    for (t, o), v in d.most_common(top):
        delays, ranks, tails, n = [], [], [], []
        for b, e, t_, o_, x in byt[t]:
            if o_ != o:
                continue
            near = [j for j in byt[t] if b - 3000000 <= j[0] <= e and j[3] != o]
            st = b
            for j in sorted((j for j in near if j[0] <= b), key=lambda j: -j[0]):
                if st - j[0] < 400000:
                    st = j[0]
            same = [j for j in near if j[0] >= st]
            delays.append(b - st)
            ranks.append(sum(1 for j in same if j[0] < b))
            n.append(len(same) + 1)
            tails.append(max(0, e - max((j[1] for j in same), default=st)))
        med = lambda a: sorted(a)[len(a) // 2]
        print("  %08x obj %x: %.3f ms, %.2f a frame, %.0f us each; starts %.0f us late, as #%d of %d; alone after: %.0f us" % (
            t, o, v / nf / 1e6, c[(t, o)] / nf, v / c[(t, o)] / 1e3, med(delays) / 1e3, med(ranks) + 1, med(n), med(tails) / 1e3))


def gantt(names, waits, sleeps, jobs, m, fr, k, step):
    T0, T1 = fr[k], fr[k + 1]
    TH = sorted([i for i, n in names.items() if n.startswith(("BaseProcExecutor", "ModuleSystemWorker"))], key=lambda i: names[i]) + [m]
    idle = {th: idle_of(th, waits, sleeps) for th in TH}
    short = lambda n: n.replace("BaseProcExecutor", "EX").replace("ModuleSystemWorker", "MS").replace("sead::MainThread", "MAIN")
    print("\n## frame %d: %.2f ms (job function's low 16 bits, busy %% of each %.2f ms step; '..' busy outside a job)" % (
        k, (T1 - T0) / 1e6, step / 1e6))
    print("  ms    " + " ".join("%-7s" % short(names[t]) for t in TH))
    t = T0
    while t < T1:
        e, row = t + step, []
        for th in TH:
            bt, best = 0, None
            for y in jobs.get(th, []):
                if y[5] == 0 and y[1] > t and y[0] < e:
                    dd = min(y[1], e) - max(y[0], t)
                    if dd > bt:
                        bt, best = dd, y
            bz = step - sum(min(e, y) - max(t, x) for x, y in idle[th] if y > t and x < e)
            bz = max(0, min(99, 100 * bz // step))
            row.append("%04x%3d" % (best[2] & 0xffff, bz) if best and bt > step * 0.3 else ("  ..%3d" % bz if bz > 10 else "       "))
        print("%6.2f  %s" % ((t - T0) / 1e6, " ".join(row)))
        t = e


def job_profile(a, names, waits, signals, jobs, fr, spec):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "scripts"))
    import prof
    tg, _, ob = spec.partition(":")
    tg, ob = int(tg, 16), int(ob, 16) if ob else None
    iv = collections.defaultdict(list)
    for th, v in jobs.items():
        for y in v:
            if y[2] == tg and (ob is None or y[4] == ob):
                iv[names[th]].append((y[0], y[1]))
    for k in iv:
        iv[k].sort()
    ivb = {k: [x[0] for x in v] for k, v in iv.items()}
    nf = len(fr) - 1
    tot = sum(e - b for v in iv.values() for b, e in v)
    print("\n## job %s: %.3f ms a frame" % (spec, tot / nf / 1e6))
    inside = lambda n, b, e: (lambda i: i >= 0 and iv[n][i][1] >= e)(bisect.bisect_right(ivb[n], b) - 1)
    wt, who = collections.Counter(), collections.Counter()
    for th, ws in waits.items():
        n = names[th]
        if n not in iv:
            continue
        for w in ws:
            if w.how & 3 and inside(n, w.b, w.e):
                wt["%s lr %08x" % (w.kind, w.lr)] += w.e - w.b
                if w.waker is not None:
                    who[names[w.waker]] += w.e - w.b
    print("  blocking waits inside: %s" % ", ".join("%s %.3f ms" % (k, v / nf / 1e6) for k, v in wt.most_common(5)))
    if who:
        print("  ended by: %s" % ", ".join("%s %.3f ms" % (k, v / nf / 1e6) for k, v in who.most_common(5)))
    if not (a.samples and a.maps):
        return
    addrs, nm = prof.load_map(os.path.join(a.maps, "HeyListen.map"), 0x140000000)
    cache = {}

    def name(f):
        if f not in cache:
            mod, _, off = f.rpartition("+")
            r = mod
            if mod == "HeyListen.exe":
                i = bisect.bisect_right(addrs, int(off, 16)) - 1
                r = nm[i] if i >= 0 else mod
            cache[f] = r
        return cache[f]
    selfc, incl, chain, n = collections.Counter(), collections.Counter(), collections.Counter(), 0
    fn = "f_%08x" % tg
    for line in open(a.samples, encoding="utf-8", errors="replace"):
        head, _, stack = line.rstrip("\n").partition("|")
        thn = head.partition(" ")[2]
        if thn not in iv:
            continue
        fs = stack.split(";")
        if not fs[-1].startswith("@"):
            continue
        t = int(fs.pop()[1:])
        i = bisect.bisect_right(ivb[thn], t) - 1
        if i < 0 or iv[thn][i][1] < t:
            continue
        nmd = [name(f) for f in fs]
        g = [x for x in nmd if x.startswith("f_")]
        n += 1
        lf = nmd[0]
        selfc[lf if not lf.endswith(".dll") else lf + " <- " + (g[0] if g else "?")] += 1
        for x in set(g):
            incl[x] += 1
        if fn in g:
            j = g.index(fn)
            chain[" > ".join(reversed(g[max(0, j - 4):j + 1]))] += 1
    if not n:
        return
    print("  %d samples; self:" % n)
    for k, v in selfc.most_common(a.top):
        print("    %5.1f%%  %s" % (100 * v / n, k))
    print("  inclusive:")
    for k, v in incl.most_common(a.top):
        print("    %5.1f%%  %s" % (100 * v / n, k))
    print("  below the job function:")
    for k, v in chain.most_common(8):
        print("    %5.1f%%  %s" % (100 * v / n, k))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--samples")
    ap.add_argument("--maps")
    ap.add_argument("--gantt", type=int, action="append", default=[])
    ap.add_argument("--step-ms", type=float, default=0.1)
    ap.add_argument("--job", action="append", default=[])
    ap.add_argument("--top", type=int, default=16)
    a = ap.parse_args()
    names, waits, sleeps, signals, jobs, tend = load(a.trace)
    if not any(jobs.values()):
        sys.exit("no job marks in the trace: take it with a job-marked build (tools/bench/jobmarks.py)")
    m, fr = frames_of(names, waits, tend)
    breakdown(names, waits, sleeps, jobs, m, fr)
    stages(names, jobs, fr, a.top)
    long_jobs(names, jobs, fr, a.top)
    for k in a.gantt:
        gantt(names, waits, sleeps, jobs, m, fr, k, int(a.step_ms * 1e6))
    for spec in a.job:
        job_profile(a, names, waits, signals, jobs, fr, spec)


if __name__ == "__main__":
    main()
