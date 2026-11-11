"""(TICK-058) Wake latency of the game's hand-offs from a HOS_JOBTRACE file: for every wait that slept, the time from
the signal that ended it (on another thread) to the waiter running again, by primitive, caller (lr) and thread.

    python -I tools/bench/wakelat.py <jobtrace.txt> [--top 30] [--thread <substring>]

What a hand-off's latency is here: the waiter's wait-end record minus the last signal of the same object recorded after
the waiter's wait began (events, light events, system events: JT_EV_SIGNAL on the key; condition variables:
JT_CV_SIGNAL; contended nn::os mutexes: JT_MX_GIVE; message queues and semaphores: JT_Q_SIGNAL; the kernel's
arbitration, process-wide keys and address waits: JT_K_WAKE naming the woken HosThread). The signal record is written
before the signaller takes the object's lock, so the latency is the whole hand-off: the signaller's lock and wake call,
the host's wake-up (scheduler, an idle core's exit from its sleep state), the waiter's lock, and its virtual core
(JT_VC_WAIT_B/E inside that span: shown apart as "vcore").
SleepThread polls are listed by caller with the time asked and slept (a poll notices its condition only when it wakes).
Frames: the Presentation Thread's present ends (JT_MARK 6), else the main thread's end-of-frame waits.
"""
import argparse, bisect, collections, statistics, sys

EV_WAIT_B, EV_WAIT_E, EV_SIGNAL, SPIN_E, CV_WAIT_B, CV_WAIT_E, CV_SIGNAL, MX_WAIT_B, MX_WAIT_E, MX_GIVE, SLEEP_B, SLEEP_E, \
    VC_WAIT_B, VC_WAIT_E, Q_WAIT_B, Q_WAIT_E, Q_SIGNAL, K_WAIT_B, K_WAIT_E, K_WAKE, MARK, JOB_B, JOB_E = range(1, 24)
KIND = {EV_WAIT_B: "event", CV_WAIT_B: "condvar", MX_WAIT_B: "mutex", Q_WAIT_B: "queue", K_WAIT_B: "kernel"}
END_OF = {EV_WAIT_E: EV_WAIT_B, CV_WAIT_E: CV_WAIT_B, MX_WAIT_E: MX_WAIT_B, Q_WAIT_E: Q_WAIT_B, K_WAIT_E: K_WAIT_B}
SIGNAL_OF = {EV_WAIT_B: EV_SIGNAL, CV_WAIT_B: CV_SIGNAL, MX_WAIT_B: MX_GIVE, Q_WAIT_B: Q_SIGNAL}
LR_NAMES = {0x80080188: "job-group wait", 0x83acfcf4: "end-of-frame wait", 0x81207a6c: "present wait",
            0x8009a478: "module poll"}


def load(path):
    names, hosts, recs = {}, {}, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line[0] == "#":
                continue
            if line[0] == "T":
                _, i, host, name = line.rstrip("\n").split(" ", 3)
                names[int(i)] = name
                hosts[int(host, 16)] = int(i)
                continue
            t, ty, th, a, b = line.split()
            recs.append((int(t), int(ty), int(th), int(a, 16), int(b, 16)))
    recs.sort()
    return names, hosts, recs


def short(name):
    for p, s in (("BaseProcExecutor", "EX"), ("ModuleSystemWorker", "MW"), ("sead::MainThread", "main"),
                 ("Presentation_Thread", "PT")):
        if name.startswith(p):
            return s + name[len(p):]
    return name


def pct(v, q):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def analyze(names, hosts, recs):
    last_sig = {}                      # (signal type, key) -> time of the last signal
    last_wake = {}                     # thread idx -> time of the last kernel wake naming it
    open_w, open_s, open_v = {}, {}, {}
    vc_in = collections.defaultdict(list)   # thread -> [(b, e)] virtual-core waits
    lat = collections.defaultdict(list)     # (kind, lr, thread) -> [(total ns, vcore ns)]
    how_n = collections.defaultdict(lambda: [0, 0, 0])   # (kind, lr, thread) -> ready, spun, slept
    sleeps = collections.defaultdict(list)  # (lr, thread) -> [(asked, slept)]
    frames = 0
    for t, ty, th, a, b in recs:
        if ty in (EV_SIGNAL, CV_SIGNAL, MX_GIVE, Q_SIGNAL):
            last_sig[(ty, a)] = t
        elif ty == K_WAKE:
            i = hosts.get(a)
            if i is not None:
                last_wake[i] = t
        elif ty == MARK and a == 6:
            frames += 1
        elif ty in KIND:
            open_w[th] = (t, ty, a, b)
        elif ty in END_OF:
            w = open_w.pop(th, None)
            if not w or w[1] != END_OF[ty]:
                continue
            t0, bty, key, lr = w
            k = (KIND[bty], lr, th)
            how = b & 3
            if bty == EV_WAIT_B or bty == Q_WAIT_B:
                if b & 4:              # timed out: no hand-off
                    continue
                if how in (0, 1):
                    how_n[k][how] += 1
                    continue
            if bty == K_WAIT_B:
                if b & 4:
                    continue
                s = last_wake.get(th)
            else:
                s = last_sig.get((SIGNAL_OF[bty], key))
            how_n[k][2] += 1
            if s is None or s < t0:
                continue
            v = sum(min(e, t) - max(vb, s) for vb, e in vc_in[th] if e > s and vb < t)
            lat[k].append((t - s, max(0, v)))
        elif ty == VC_WAIT_B:
            open_v[th] = t
        elif ty == VC_WAIT_E:
            vb = open_v.pop(th, None)
            if vb is not None:
                vc_in[th].append((vb, t))
                if len(vc_in[th]) > 64:
                    del vc_in[th][:32]
        elif ty == SLEEP_B:
            open_s[th] = (t, a, b)
        elif ty == SLEEP_E:
            s = open_s.pop(th, None)
            if s:
                sleeps[(s[2], th)].append((s[1], t - s[0]))
    return frames, lat, how_n, sleeps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--thread", default="")
    args = ap.parse_args()
    names, hosts, recs = load(args.trace)
    frames, lat, how_n, sleeps = analyze(names, hosts, recs)
    span = (recs[-1][0] - recs[0][0]) / 1e9 if recs else 1
    nf = frames or 1
    print("window %.2f s, %d frames (%.2f ms a frame)" % (span, frames, span * 1e3 / nf))
    rows = []
    for k, v in lat.items():
        if args.thread and args.thread not in names.get(k[2], ""):
            continue
        tot = [x[0] for x in v]
        rows.append((sum(tot) / nf, k, v))
    rows.sort(reverse=True)
    print("\nhand-offs that slept, by the time a frame from signal to running (us); vcore = of it, waiting for a virtual core")
    print("%-8s %-10s %-24s %-8s %7s %7s %7s %7s %7s %7s %7s %9s" % ("kind", "lr", "what", "thread", "/frame", "slept", "ready",
                                                                "mean", "p50", "p90", "vcore", "us/frame"))
    for per, k, v in rows[:args.top]:
        kind, lr, th = k
        tot = [x[0] / 1e3 for x in v]
        vc = [x[1] / 1e3 for x in v]
        r, sp, sl = how_n[k]
        print("%-8s %-10x %-24s %-8s %7.2f %7d %7d %7.1f %7.1f %7.1f %7.1f %9.1f" % (
            kind, lr, LR_NAMES.get(lr, ""), short(names.get(th, "?")), len(v) / nf, sl, r + sp, statistics.mean(tot),
            pct(tot, .5), pct(tot, .9), statistics.mean(vc), per / 1e3))
    allv = [x for _, _, v in rows for x in v]
    if allv:
        t = [x[0] / 1e3 for x in allv]
        print("\nall: %d hand-offs (%.1f a frame), mean %.1f us, p50 %.1f, p90 %.1f, p99 %.1f; of it vcore %.1f us mean" % (
            len(t), len(t) / nf, statistics.mean(t), pct(t, .5), pct(t, .9), pct(t, .99),
            statistics.mean(x[1] / 1e3 for x in allv)))
        for who in ("main", "MW", "EX", "PT"):
            sel = [x for _, k, v in rows for x in v if short(names.get(k[2], "")).startswith(who)]
            if sel:
                t = [x[0] / 1e3 for x in sel]
                print("  %-4s %6d (%.1f a frame) mean %.1f us p50 %.1f p90 %.1f, %.0f us a frame" % (
                    who, len(t), len(t) / nf, statistics.mean(t), pct(t, .5), pct(t, .9), sum(t) / nf))
    print("\nSleepThread by caller (asked / slept, us)")
    srows = sorted(sleeps.items(), key=lambda kv: -sum(x[1] for x in kv[1]))
    for (lr, th), v in srows[:args.top]:
        if args.thread and args.thread not in names.get(th, ""):
            continue
        print("  lr %-10x %-24s %-8s %7.2f a frame, asked %7.1f, slept mean %7.1f p90 %7.1f, %8.1f us a frame" % (
            lr, LR_NAMES.get(lr, ""), short(names.get(th, "?")), len(v) / nf, statistics.mean(x[0] for x in v) / 1e3,
            statistics.mean(x[1] for x in v) / 1e3, pct([x[1] / 1e3 for x in v], .9), sum(x[1] for x in v) / 1e3 / nf))


if __name__ == "__main__":
    main()
