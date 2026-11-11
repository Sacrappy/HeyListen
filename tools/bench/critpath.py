"""(TICK-058) The frame's critical path through the game's hand-offs, from a HOS_JOBTRACE file, walked backwards.

    python -I tools/bench/critpath.py <jobtrace.txt> [--top 25]

From each present's end (JT_MARK 6 on the Presentation Thread) back to the one before: on the current thread, the time since
its last block is work; a block that a signal ended (wakelat.py's matching: the event / condition variable / queue /
mutex signal on the same object after the wait began, the kernel wake naming the thread) jumps to the signaller at the
signal's time, and the gap from the signal to the waiter running is the hand-off (its virtual-core wait apart); a
virtual-core wait on its own (a kick, a yield) stays on the thread; a SleepThread poll episode (back-to-back sleeps)
jumps to the last job end (JT_JOB_E) or signal by another guest thread inside it, the gap from it to the poll's end being
the poll's lag; a block with no signaller found stays on the thread ("unexplained"). Host-only waits (the replay's
segments, the held flush's lock) are inside the Presentation Thread's work: chainb.py splits those.
Per frame (means): the path's time by thread (work) and by kind of wait, and the hand-offs on the path by caller.
"""
import argparse, bisect, collections, statistics, sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import wakelat as W  # noqa: E402

SIGNALS = (W.EV_SIGNAL, W.CV_SIGNAL, W.MX_GIVE, W.Q_SIGNAL)


def build(names, hosts, recs):
    segs = collections.defaultdict(list)    # th -> [(b, e, kind, info)]: kind 'w' wait (info: (S, s, lr, wkind) or None),
    vsegs = collections.defaultdict(list)   # 'p' sleep (info lr); vcore waits apart: th -> [(b, e)]
    last_sig = {}                            # (type, key) -> (t, th)
    last_wake = {}                           # th -> (t, waker th)
    events = []                              # (t, th) job ends and signals by guest threads (poll setters)
    open_w, open_s, open_v = {}, {}, {}
    frames = []
    for t, ty, th, a, b in recs:
        if ty in SIGNALS:
            last_sig[(ty, a)] = (t, th)
            events.append((t, th))
        elif ty == W.K_WAKE:
            i = hosts.get(a)
            if i is not None:
                last_wake[i] = (t, th)
            events.append((t, th))
        elif ty == W.JOB_E:
            events.append((t, th))
        elif ty == W.MARK and a == 6:
            frames.append((t, th))
        elif ty in W.KIND:
            open_w[th] = (t, ty, a, b)
        elif ty in W.END_OF:
            w = open_w.pop(th, None)
            if not w or w[1] != W.END_OF[ty]:
                continue
            t0, bty, key, lr = w
            if bty in (W.EV_WAIT_B, W.Q_WAIT_B, W.K_WAIT_B) and (b & 4):
                segs[th].append((t0, t, "w", None, lr, W.KIND[bty]))   # timed out: no signaller
                continue
            if bty in (W.EV_WAIT_B, W.Q_WAIT_B) and (b & 3) == 0:
                continue                                               # ready at once: no block
            s = last_wake.get(th) if bty == W.K_WAIT_B else last_sig.get((W.SIGNAL_OF[bty], key))
            if s is not None and s[0] < t0:   # a signal from before the wait: not what ended it
                s = None
            segs[th].append((t0, t, "w", s, lr, W.KIND[bty]))
        elif ty == W.SLEEP_B:
            open_s[th] = (t, b)
        elif ty == W.SLEEP_E:
            s = open_s.pop(th, None)
            if s:
                segs[th].append((s[0], t, "p", None, s[1], "poll"))
        elif ty == W.VC_WAIT_B:
            open_v[th] = (t, a, hosts.get(b, -1) if b else -1)
        elif ty == W.VC_WAIT_E:
            s = open_v.pop(th, None)
            if s is not None:
                vsegs[th].append((s[0], t, s[1], s[2]))
    for th in segs:
        segs[th].sort()
    for th in vsegs:
        vsegs[th].sort()
    events.sort()
    # (the module poll's setter) the instants a job thread (main, module workers) stops working: the start of a block that
    # follows work; the serial job's thread, once it has set the polled word, goes to its own wait or poll
    stops = []
    for th, sl in segs.items():
        n = names.get(th, "")
        if n != "sead::MainThread" and not n.startswith("ModuleSystemWorker"):
            continue
        prev_e = None
        for sg in sl:
            if prev_e is None or sg[0] - prev_e > 5000:
                stops.append((sg[0], th))
            prev_e = sg[1] if prev_e is None else max(prev_e, sg[1])
    stops.sort()
    return segs, vsegs, (events, stops), frames


def vc_overlap(vs, b, e, who=None):
    """the vcore wait time in [b, e]; who: a dict (holder idx, a) -> ns to add each wait's part to"""
    i = bisect.bisect_left(vs, (b, 0))
    i = max(0, i - 1)
    tot = 0
    while i < len(vs) and vs[i][0] < e:
        lo, hi = max(vs[i][0], b), min(vs[i][1], e)
        if hi > lo:
            tot += hi - lo
            if who is not None:
                who[(vs[i][3], vs[i][2])] = who.get((vs[i][3], vs[i][2]), 0) + hi - lo
        i += 1
    return tot


def walk(names, segs, vsegs, evs, fb, fe, th0, guest, work):
    """work: (thread, b, e) of the path's work is appended to it"""
    events, stops = evs
    acc = collections.Counter()
    hand = collections.defaultdict(list)
    vwho = {}
    th, t = th0, fe
    ends = {k: [s[1] for s in v] for k, v in segs.items()}
    vends = {k: [s[1] for s in v] for k, v in vsegs.items()}
    steps = 0
    while t > fb and steps < 100000:
        steps += 1
        sl = segs.get(th, [])
        i = bisect.bisect_right(ends.get(th, []), t) - 1
        vl = vsegs.get(th, [])
        j = bisect.bisect_right(vends.get(th, []), t) - 1
        seg = sl[i] if i >= 0 else None
        vseg = vl[j] if j >= 0 else None
        # a lone vcore wait (not inside the wait we are about to take) comes first if it ends later
        if vseg and (not seg or vseg[1] > seg[1]) and not (seg and seg[0] <= vseg[0] and vseg[1] <= seg[1]):
            acc["work " + W.short(names.get(th, "?"))] += t - max(vseg[1], fb)
            work.append((th, max(vseg[1], fb), t))
            d = max(0, min(vseg[1], t) - max(vseg[0], fb))
            acc["vcore wait (no hand-off)"] += d
            k = (th, vseg[3], vseg[2])
            vwho[k] = vwho.get(k, 0) + d
            t = vseg[0]
            continue
        if not seg:
            acc["work " + W.short(names.get(th, "?"))] += t - fb
            work.append((th, fb, t))
            break
        b, e, kind, s, lr, wk = seg
        acc["work " + W.short(names.get(th, "?"))] += t - max(e, fb)
        work.append((th, max(e, fb), t))
        t = min(t, e)
        if t <= fb:
            break
        if kind == "w":
            if s is None or s[1] == th or s[0] >= e:
                acc["blocked, no signaller (%s)" % wk] += t - max(b, fb)
                t = b
                continue
            who = {}
            v = vc_overlap(vl, s[0], e, who)
            for (h, a), d in who.items():
                vwho[(th, h, a)] = vwho.get((th, h, a), 0) + d
            acc["hand-off: vcore"] += v
            acc["hand-off: wake"] += (e - s[0]) - v
            hand[(wk, lr, W.short(names.get(th, "?")), W.short(names.get(s[1], "?")))].append(e - s[0])
            th, t = s[1], s[0]
            continue
        # a poll episode: back-to-back sleeps
        k = i
        while k > 0 and sl[k - 1][2] == "p" and sl[k][0] - sl[k - 1][1] < 10000:
            k -= 1
        pb = sl[k][0]
        setter = None
        # (a poll looks every ~20 us: what ended it happened within its last step; earlier, it was not traced)
        last_step = max(pb, sl[i][0] - 5000)
        for lst in (stops, events):   # a job thread stopping work first, else a signal or job end
            x = bisect.bisect_left(lst, (e, -1)) - 1
            while x >= 0 and lst[x][0] >= last_step:
                if lst[x][1] != th and lst[x][1] in guest:
                    setter = lst[x]
                    break
                x -= 1
            if setter:
                break
        if setter:
            acc["poll lag"] += e - setter[0]
            hand[("poll", lr, W.short(names.get(th, "?")), W.short(names.get(setter[1], "?")))].append(e - setter[0])
            th, t = setter[1], setter[0]
        else:
            acc["poll, setter not traced"] += t - max(pb, fb)
            t = pb
    return acc, hand, vwho


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--anchor", choices=("present", "main"), default="present",
                    help="cut frames at the presents' ends (the loop through the replay), or at the ends of the main "
                         "thread's end-of-frame waits (lr 83acfcf4: the game's own chain)")
    ap.add_argument("--samples", help="a HOS_SAMPLE_ALL file of the same run, taken with HOS_SAMPLE_NS=1")
    ap.add_argument("--maps", help="the folder with that build's HeyListen.map and hookshot_vk.map")
    args = ap.parse_args()
    names, hosts, recs = W.load(args.trace)
    segs, vsegs, events, frames = build(names, hosts, recs)
    guest = {i for i, n in names.items() if not n.startswith("host:")}
    if args.anchor == "main":
        main_th = next(i for i, n in names.items() if n == "sead::MainThread")
        frames = [(sg[1], main_th) for sg in segs[main_th] if sg[2] == "w" and sg[4] == 0x83acfcf4]
    if len(frames) < 3:
        sys.exit("fewer than 3 presents in the trace")
    tot = collections.Counter()
    hands = collections.defaultdict(list)
    vwhos = collections.Counter()
    work = []
    lens = []
    for (fb, _), (fe, th) in zip(frames, frames[1:]):
        acc, hand, vwho = walk(names, segs, vsegs, events, fb, fe, th, guest, work)
        tot.update(acc)
        vwhos.update(vwho)
        for k, v in hand.items():
            hands[k].extend(v)
        lens.append(fe - fb)
    n = len(lens)
    print("%d frames, %.2f ms a frame (mean), the path's time a frame:" % (n, statistics.mean(lens) / 1e6))
    for k, v in sorted(tot.items(), key=lambda kv: -kv[1]):
        if v / n >= 5000:
            print("  %-36s %7.3f ms" % (k, v / n / 1e6))
    print("\nhand-offs and polls on the path, by caller, waiter <- signaller (count a frame, mean us, us a frame):")
    rows = sorted(hands.items(), key=lambda kv: -sum(kv[1]))
    for (wk, lr, who, frm), v in rows[:args.top]:
        print("  %-8s lr %-9x %-18s %-6s <- %-24s %5.2f  %7.1f  %7.1f" % (wk, lr, W.LR_NAMES.get(lr, ""), who, frm[:24], len(v) / n,
                                                               statistics.mean(v) / 1e3, sum(v) / n / 1e3))
    print("\nvirtual-core waits on the path, by waiter and the holder of its first allowed core (us a frame):")
    for (th, h, a), v in sorted(vwhos.items(), key=lambda kv: -kv[1])[:args.top]:
        if v / n < 2000:
            break
        print("  %-12s (prio %3d, cores %#04x) behind %-28s (prio %3d): %7.1f" % (
            W.short(names.get(th, "?")), a >> 8 & 0xff, a & 0xff, W.short(names.get(h, "?")) if h >= 0 else "-",
            a >> 16 & 0xff, v / n / 1e3))

    if args.samples:
        path_samples(args, names, work)


def path_samples(args, names, work):
    """the samples (HOS_SAMPLE_NS instants) that fall in the path's work, by leaf (host function, or guest code) and by
    innermost guest function"""
    import os
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "scripts"))
    import prof
    maps = {}
    for mod in ("HeyListen.exe", "hookshot_vk.dll"):
        mp = os.path.join(args.maps or ".", mod[:-4] + ".map")
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
    byth = collections.defaultdict(list)
    for th, b, e in work:
        byth[th].append((b, e))
    for th in byth:
        byth[th].sort()
    starts = {th: [x[0] for x in v] for th, v in byth.items()}
    byname = {}
    for i, n in names.items():
        byname.setdefault(n.replace("_", " "), i)
        byname.setdefault(n, i)
    leaf, inner, host = collections.Counter(), collections.Counter(), collections.Counter()
    n = 0
    for line in open(args.samples, encoding="utf-8", errors="replace"):
        head, _, stack = line.rstrip(chr(10)).partition("|")
        ms, _, thread = head.partition(" ")
        if not stack:
            continue
        th = byname.get(thread)
        if th is None or th not in byth:
            continue
        fr = stack.split(";")
        if not fr[-1].startswith("@"):
            continue
        t = int(fr.pop()[1:])
        i = bisect.bisect_right(starts[th], t) - 1
        if i < 0 or byth[th][i][1] < t:
            continue
        n += 1
        named = [name(f) for f in fr]
        lf = named[0]
        if lf.startswith("HeyListen:f_"):
            leaf["guest code"] += 1
        else:
            leaf[lf] += 1
        g = next((x for x in named if x.startswith("HeyListen:f_")), "?")
        inner[g] += 1
        h = next((x for x in named if x.startswith("HeyListen:") and not x.startswith("HeyListen:f_")), None)
        if h:
            host[h] += 1
    print()
    print("samples on the path's work: %d" % n)
    print("  by leaf:")
    for k, v in leaf.most_common(args.top):
        print("    %5.1f%%  %s" % (100 * v / max(n, 1), k))
    print("  by innermost host (runtime) function anywhere in the stack:")
    for k, v in host.most_common(args.top):
        print("    %5.1f%%  %s" % (100 * v / max(n, 1), k))
    print("  by innermost guest function:")
    for k, v in inner.most_common(args.top):
        print("    %5.1f%%  %s" % (100 * v / max(n, 1), k))


if __name__ == "__main__":
    main()
