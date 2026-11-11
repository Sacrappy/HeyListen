"""(TICK-054) A frame-by-frame timeline of the game's threads, the replay and the GPU from a HOS_JOBTRACE file.

    python -I tools/bench/frameline.py <jobtrace.txt> [--frames 4] [--from <ms into the window>] [--row 0.25] [--summary]

Reads the trace of engine/host/hos_jobtrace.c (the guest threads' waits, sleeps and virtual-core waits) and the host's
JT_MARK records around the renderer (host/nvn_native.c, hos_jobtrace.c: a = kind, b = a count):
  1 submit begin  2 submit end  (nvnQueueSubmitCommands on the submitting thread: the capture and the replay of one submit)
  3 a submit held (HOS_NVN_HOLD=2: its capture and replay later, on the held submits' thread)
  5 present begin 6 present end (nvnQueuePresentTexture on the Presentation Thread)
  7 the GPU finished more submissions (the trace thread polls the renderer's timeline every ~2 us)
  9/10 the replay thread waiting for its segments, 11/12 a submit's capture made (the renderer's marks, vk_replay.cpp jt_mark)
(The renderer marks through hos_jt_mark_ext, looked up in the exe: its headers are shader-stamp inputs.) The GPU column is an approximation: busy from
each submit's end to the first completion seen after it.
A frame is cut at the Presentation Thread's present end (or, without marks, at the main thread's end-of-frame wait).
The gantt has one column per thread of interest and one row per --row ms; letters:
  main / module workers / executors: . idle (a blocked wait)  # work  j the main thread's job-group wait (lr 80080188)
  e its end-of-frame wait (lr 83acfcf4)  p a SleepThread poll  s the host's spin before a block  v a virtual-core wait
  Presentation Thread (and the main thread): # work (the game's own code)  S a submit (the capture and the replay)  P the present
  GPU: # busy (a submit ended and no completion seen since)  . idle
The summary gives, per frame, the main thread's phases in order and the lags between the game's hand-over, the replay and
the GPU's completion.
"""
import argparse, bisect, collections, sys

EV_WAIT_B, EV_WAIT_E, EV_SIGNAL, SPIN_E, CV_WAIT_B, CV_WAIT_E, CV_SIGNAL, MX_WAIT_B, MX_WAIT_E, MX_GIVE, SLEEP_B, SLEEP_E, \
    VC_WAIT_B, VC_WAIT_E, Q_WAIT_B, Q_WAIT_E, Q_SIGNAL, K_WAIT_B, K_WAIT_E, K_WAKE, MARK, JOB_B, JOB_E = range(1, 24)
WAIT_B = {EV_WAIT_B: EV_WAIT_E, CV_WAIT_B: CV_WAIT_E, MX_WAIT_B: MX_WAIT_E, Q_WAIT_B: Q_WAIT_E, K_WAIT_B: K_WAIT_E}
LR_JOBGROUP, LR_FRAME_END, LR_POLL = 0x80080188, 0x83acfcf4, 0x8009a478
M_SUBMIT_B, M_SUBMIT_E, M_HELD, M_PRESENT_B, M_PRESENT_E, M_GPU_DONE, M_QUEUED, M_SEGWAIT_B, M_SEGWAIT_E = 1, 2, 3, 5, 6, 7, 8, 9, 10
# (the renderer's own marks, gpu/vk/vk_replay.cpp jt_mark: 11/12 a submit's capture made, 13/14 a dry pass, 15/16 a submission's
# memory work, 17-21 the parallel replay's setup points; tools/bench/chainb.py reads them)
M_CAPTURE_B, M_CAPTURE_E = 11, 12


def load(path):
    names, recs, window = {}, [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line[0] == "#":
                p = line.split()
                if len(p) >= 5 and p[1] == "jobtrace":
                    window = (int(p[3]), int(p[4]))
                continue
            if line[0] == "T":
                _, i, _, name = line.rstrip("\n").split(" ", 3)
                names[int(i)] = name
                continue
            t, ty, th, a, b = line.split()
            recs.append((int(t), int(ty), int(th), int(a, 16), int(b, 16)))
    recs.sort()
    return names, window, recs


def segments(recs, names):
    """per thread: [(b, e, code, lr)] of its blocked intervals (waits, sleeps, vcore waits, spins); marks: [(t, kind, value, th)]"""
    segs = collections.defaultdict(list)
    open_w, open_s, open_v, spin_at = {}, {}, {}, {}
    marks = []
    for t, ty, th, a, b in recs:
        if ty in WAIT_B:
            open_w[th] = (t, a, b, ty)
        elif ty == SPIN_E:
            spin_at[th] = t
        elif ty in WAIT_B.values():
            w = open_w.pop(th, None)
            if not w:
                continue
            b0, key, lr, ty0 = w
            if ty0 == EV_WAIT_B and (b & 3) == 0:   # ready at once: no block
                continue
            code = "j" if lr == LR_JOBGROUP else "e" if lr == LR_FRAME_END else "."
            sp = spin_at.pop(th, None)
            if sp and b0 <= sp <= t:
                segs[th].append((b0, sp, "s", lr))
                segs[th].append((sp, t, code, lr))
            else:
                segs[th].append((b0, t, code, lr))
        elif ty == SLEEP_B:
            open_s[th] = (t, b)
        elif ty == SLEEP_E:
            s = open_s.pop(th, None)
            if s:
                segs[th].append((s[0], t, "p", s[1]))
        elif ty == VC_WAIT_B:
            open_v[th] = t
        elif ty == VC_WAIT_E:
            s = open_v.pop(th, None)
            if s is not None:
                segs[th].append((s, t, "v", 0))
        elif ty == MARK:
            marks.append((t, a, b, th))
    for th in segs:
        segs[th].sort()
    return segs, marks


def state_at(segs, t):
    """the code of the thread's blocked segment covering t, or '#' (work)"""
    i = bisect.bisect_right(segs, (t, float("inf"))) - 1
    if i >= 0 and segs[i][0] <= t < segs[i][1]:
        return segs[i][2]
    return "#"


class Spans:
    """closed intervals with a code, for the renderer's marks"""

    def __init__(self):
        self.items = []

    def add(self, b, e, code):
        self.items.append((b, e, code))

    def at(self, t):
        for b, e, code in self.items:
            if b <= t < e:
                return code
        return None


def build_marks(marks, names):
    """spans per thread from the marks (submit, capture, present, segment waits), the queued/done lists for the GPU"""
    spans = collections.defaultdict(Spans)
    open_m = {}
    queued, done = [], []   # (t, value)
    for t, kind, v, th in marks:
        if kind in (M_SUBMIT_B, M_CAPTURE_B, M_PRESENT_B, M_SEGWAIT_B):
            open_m[(th, kind)] = t
        elif kind in (M_SUBMIT_E, M_CAPTURE_E, M_PRESENT_E, M_SEGWAIT_E):
            b = open_m.pop((th, kind - 1), None)
            if b is not None:
                spans[th].add(b, t, {M_SUBMIT_E: "S", M_CAPTURE_E: "C", M_PRESENT_E: "P", M_SEGWAIT_E: "w"}[kind])
                if kind == M_SUBMIT_E:
                    queued.append((t, 0))
        elif kind == M_GPU_DONE:
            done.append((t, v))
        elif kind == M_QUEUED:
            queued.append((t, v))
    # (the capture over the submit: a submit's span holds the capture's)
    for th, sp in spans.items():
        sp.items.sort(key=lambda x: ({"C": 0, "w": 0, "S": 1, "P": 1}[x[2]], x[0]))
    return spans, queued, done


def gpu_busy(queued, done):
    """(b, e) intervals from each submit's end to the first completion seen after it (the GPU had that submit's work)"""
    dts = [t for t, v in done]
    out = []
    for t, v in queued:
        i = bisect.bisect_right(dts, t)
        if i < len(dts):
            out.append((t, dts[i]))
    out.sort()
    merged = []
    for b, e in out:
        if merged and b <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], e))
        else:
            merged.append((b, e))
    return merged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--from", dest="from_ms", type=float, default=500.0, help="ms into the window the gantt starts at")
    ap.add_argument("--row", type=float, default=0.25, help="ms a row")
    ap.add_argument("--summary", action="store_true", help="per-frame numbers for the whole window")
    args = ap.parse_args()
    names, window, recs = load(args.trace)
    segs, marks = segments(recs, names)
    spans, queued, done = build_marks(marks, names)
    busy = gpu_busy(queued, done)
    by_name = {}
    for i, n in names.items():
        by_name.setdefault(n, i)
    main_th = by_name.get("sead::MainThread")
    pt = by_name.get("Presentation_Thread")
    if main_th is None:
        sys.exit("no sead::MainThread in the trace")
    t0 = window[0] if window else recs[0][0]
    t1 = window[1] if window else recs[-1][0]
    # frame boundaries: present ends on the Presentation Thread when there are marks, else the main thread's end-of-frame waits
    bounds = []
    if pt is not None and spans.get(pt) and any(c == "P" for _, _, c in spans[pt].items):
        bounds = sorted(e for _, e, c in spans[pt].items if c == "P")
    else:
        bounds = [e for b, e, c, lr in segs[main_th] if c == "e"]
    cols = [("MAIN", main_th), ("PRES", pt)]
    for k in range(6):
        cols.append(("EX%d" % k, by_name.get("BaseProcExecutor%d" % k)))
    for k in range(1, 6):
        cols.append(("MS%d" % k, by_name.get("ModuleSystemWorker%d" % k)))
    for n in sorted(names.values()):
        if n.startswith("host:") and by_name[n] in spans:
            cols.append((n[:8], by_name[n]))

    def cell(th, t):
        if th is None:
            return " "
        sp = spans.get(th)
        if sp:
            c = sp.at(t)
            if c:
                return c
        return state_at(segs.get(th, []), t)

    def gpu_cell(t):
        i = bisect.bisect_right(busy, (t, float("inf"))) - 1
        return "#" if i >= 0 and busy[i][0] <= t < busy[i][1] else "."

    # ---- gantt ----
    start = t0 + int(args.from_ms * 1e6)
    i0 = bisect.bisect_left(bounds, start)
    if i0 + args.frames >= len(bounds):
        sys.exit("not enough frames after --from")
    row = int(args.row * 1e6)
    print("gantt from %.1f ms into the window, %d frames, a row = %.2f ms (frame cut at %s)" %
          (args.from_ms, args.frames, args.row, "the present's end" if pt is not None and spans.get(pt) else "the main thread's end-of-frame wait"))
    print("%8s %s %s" % ("ms", " ".join("%-4s" % c[0] for c in cols), "GPU"))
    for fi in range(i0, i0 + args.frames):
        fb, fe = bounds[fi], bounds[fi + 1]
        print("-- frame %d: %.2f ms" % (fi, (fe - fb) / 1e6))
        t = fb
        while t < fe:
            line = ["%8.2f" % ((t - fb) / 1e6)]
            for _, th in cols:
                line.append("%-4s" % cell(th, t))
            line.append(gpu_cell(t))
            print(" ".join(line))
            t += row

    # ---- summary ----
    print("\n## per frame (ms): the main thread's phases in order (W work, J job-group wait, E end-of-frame wait, P polls, "
          "O other waits), then the Presentation Thread (idle in its queue, replay (S), present (P), waits for segments (w)) "
          "and the GPU (busy)")
    rows = []
    for fi in range(len(bounds) - 1):
        fb, fe = bounds[fi], bounds[fi + 1]
        if fb < t0 or fe > t1:
            continue
        # main thread phases
        phases = []
        t = fb
        for b, e, c, lr in segs[main_th]:
            if e <= fb or b >= fe:
                continue
            b, e = max(b, fb), min(e, fe)
            if b > t:
                phases.append(("W", b - t))
            code = {"j": "J", "e": "E", "p": "P", "s": "P", "v": "O", ".": "O"}[c]
            phases.append((code, e - b))
            t = e
        if t < fe:
            phases.append(("W", fe - t))
        # collapse runs of the same code, and phases under 40 us into their neighbour
        col = []
        for c, d in phases:
            if col and (col[-1][0] == c or d < 40000):
                col[-1] = (col[-1][0], col[-1][1] + d)
            elif col and col[-1][1] < 40000:
                col[-1] = (c, col[-1][1] + d)
            else:
                col.append((c, d))
        # "alone": the main thread working while no executor and no module worker works
        alone = 0
        others = [th for n, th in cols[2:] if th is not None]
        t = fb
        step = 20000
        while t < fe:
            if cell(main_th, t) in "#SC" and all(state_at(segs.get(th, []), t) != "#" for th in others):
                alone += step
            t += step
        tot = collections.Counter()
        for c, d in phases:   # (the totals from the phases before the collapse)
            tot[c] += d
        pres = ""
        if pt is not None:
            idle = sum(min(e, fe) - max(b, fb) for b, e, c, lr in segs.get(pt, []) if e > fb and b < fe and c == ".")
            sp = spans.get(pt)
            rep = sum(min(e, fe) - max(b, fb) for b, e, c in (sp.items if sp else []) if e > fb and b < fe and c == "S")
            prs = sum(min(e, fe) - max(b, fb) for b, e, c in (sp.items if sp else []) if e > fb and b < fe and c == "P")
            sw = sum(min(e, fe) - max(b, fb) for b, e, c in (sp.items if sp else []) if e > fb and b < fe and c == "w")
            pres = " | PT idle %.2f S %.2f P %.2f w %.2f" % (idle / 1e6, rep / 1e6, prs / 1e6, sw / 1e6)
        g = sum(min(e, fe) - max(b, fb) for b, e in busy if e > fb and b < fe)
        tot["A"] = alone
        rows.append((fi, (fe - fb) / 1e6, col, tot, pres, g / 1e6))
    if args.summary:
        for fi, ms, col, tot, pres, g in rows:
            print("f%-4d %6.2f  %s%s | GPU busy %.2f" % (fi, ms, " ".join("%s%.2f" % (c, d / 1e6) for c, d in col), pres, g))
    n = len(rows)
    if n:
        print("\n## means over %d frames: frame %.2f ms; main: W %.2f (alone %.2f) J %.2f E %.2f P %.2f O %.2f; GPU busy %.2f" % (
            n, sum(r[1] for r in rows) / n,
            *[sum(r[3][c] for r in rows) / n / 1e6 for c in "WAJEPO"], sum(r[5] for r in rows) / n))
        if pt is not None:
            for key in ("idle", "S", "P", "w"):
                vals = []
                for r in rows:
                    for part in r[4].split():
                        pass
            # (the Presentation Thread's means from the strings)
            acc = collections.Counter()
            for r in rows:
                parts = r[4].replace("|", "").split()
                for k, v in zip(parts[1::2], parts[2::2]):
                    acc[k] += float(v)
            print("   Presentation Thread: " + " ".join("%s %.2f" % (k, v / n) for k, v in acc.items()))
        # the main thread's own phases, by typical order: the mean length of the k-th phase of each code
        print("   main thread phases in order (mean of the runs; a frame starts at the present's end):")
        seq = collections.Counter()
        order = collections.defaultdict(list)
        for r in rows:
            for k, (c, d) in enumerate(r[2]):
                order[(k, c)].append(d / 1e6)
        for (k, c), v in sorted(order.items()):
            if len(v) >= n * 0.5:
                print("     %2d %s %.2f ms (in %d frames)" % (k, c, sum(v) / len(v), len(v)))


if __name__ == "__main__":
    main()
