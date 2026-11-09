"""Hot-function map of the frame's critical chain from an all-thread sample (HOS_SAMPLE_ALL, the input of scripts/crit.py).

    RX_MAPS=<build folder with hookshot.map> python -I scripts/hotmap.py <all-thread file> [top] [wait function]

The chain is the main thread's own work (sead::MainThread running) plus every game thread running at an instant when the
main thread is blocked in the job group's wait (default f_8007fe60, the WaitLightEvent of the job group, TICK-011/TICK-025).
OptimizeChangeBanc (a background thread that always runs, TICK-025) is listed apart and never counted in the chain.

For each guest function f_<addr> (names from hookshot.map): inclusive samples (in the stack at all, once per stack), self
samples (the innermost f_ frame of a running stack, so runtime helpers it calls count to it), the threads, the top callers
(the next f_ frame outward) and callees (the next f_ frame inward).
"""
import bisect
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prof  # noqa: E402

RUN_MODS = prof.GAME_EXES + ("hookshot_vk.dll",)
BACKGROUND = ("OptimizeChangeBanc",)


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    wait_fn = sys.argv[3] if len(sys.argv) > 3 else "f_8007fe60"
    mp = prof.game_map(os.environ.get("RX_MAPS", prof.BUILD))
    addrs, names = prof.load_map(mp, 0x140000000)
    cache = {}

    def name(frame):
        r = cache.get(frame)
        if r is None:
            mod, _, off = frame.rpartition("+")
            r = mod
            if mod in prof.GAME_EXES:
                i = bisect.bisect_right(addrs, int(off, 16)) - 1
                r = prof.demangle(names[i]) if i >= 0 else mod
            cache[frame] = r
        return r

    ticks = collections.defaultdict(dict)
    for line in open(path, encoding="utf-8", errors="replace"):
        head, _, stack = line.rstrip("\n").partition("|")
        ms, _, thread = head.partition(" ")
        if not stack or not ms.isdigit():
            continue
        frames = stack.split(";")
        running = frames[0].rpartition("+")[0] in RUN_MODS
        ticks[int(ms)][thread] = (running, [name(f) for f in frames])

    incl, self_, thr = collections.Counter(), collections.Counter(), collections.defaultdict(collections.Counter)
    callers, callees = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    bg_self = collections.Counter()
    chain = main_own = 0
    for t in ticks.values():
        m = t.get("sead::MainThread")
        main_waits = m is not None and not m[0] and any(f == wait_fn for f in m[1])
        for th, (running, fr) in t.items():
            if not running:
                continue
            gf = [f for f in fr if f.startswith("f_")]
            if th.startswith(BACKGROUND):
                if gf:
                    bg_self[gf[0]] += 1
                continue
            if th == "sead::MainThread":
                main_own += 1
            elif not main_waits:
                continue
            chain += 1
            if not gf:
                continue
            self_[gf[0]] += 1
            for f in set(gf):
                incl[f] += 1
                thr[f][re.sub(r"\d+$", "", th)] += 1
            for i, f in enumerate(gf):
                if i + 1 < len(gf):
                    callers[f][gf[i + 1]] += 1
                if i > 0:
                    callees[f][gf[i - 1]] += 1
    print("%d instants, chain samples %d (main thread running %d, others while main waits in %s %d)"
          % (len(ticks), chain, main_own, wait_fn, chain - main_own))

    def row(f):
        cs = ", ".join("%s %d" % kv for kv in callers[f].most_common(3))
        ce = ", ".join("%s %d" % kv for kv in callees[f].most_common(3))
        th = ", ".join("%s %d" % kv for kv in thr[f].most_common(2))
        return "%6.2f%% %6.2f%%  %-12s  [%s]  callers: %s | callees: %s" % (
            100.0 * incl[f] / chain, 100.0 * self_[f] / chain, f, th, cs, ce)

    print("-- by self (incl% self% of chain samples)")
    for f, _ in self_.most_common(top):
        print(row(f))
    print("-- by inclusive")
    for f, _ in incl.most_common(top):
        print(row(f))
    total_bg = sum(bg_self.values())
    print("-- %s (not in the chain), self, %d samples" % (BACKGROUND[0], total_bg))
    for f, n in bg_self.most_common(8):
        print("%6.2f%%  %s" % (100.0 * n / max(total_bg, 1), f))


if __name__ == "__main__":
    main()
