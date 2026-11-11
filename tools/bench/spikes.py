"""(TICK-051) The spikes of a run's frame trace, by cause.

    python -I spikes.py <hookshot_frames.csv | run folder> [...] [--cut 66] [--factor 2] [--list N]

Reads the per-frame trace that RX_VK_FRAME_TRACE=1 writes beside the game (hookshot_frames.csv, the TICK-051 columns: t_s
and the ones after it). Frames from --cut seconds after launch on; a spike is a frame over --factor times the median frame.

Each spike's excess (its time minus the median frame) is set against each cause's excess (the cause's time that frame minus
the cause's median). The cause with the largest excess takes the spike when it covers at least 40% of the frame's excess;
otherwise the spike is "unattributed" (the game's own CPU work, or waits no counter sees). The causes' columns are times
summed over the threads that did them, so a cause can exceed the frame; the 40% rule is a heuristic, not a critical path.
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

CAUSES = {
    # cause: (columns summed, columns subtracted)
    "late pages (capture waits for the GPU)": (["late_wait_ms"], []),
    "memory sync (other than the late wait)": (["memsync_ms"], ["late_wait_ms"]),
    "pipelines / programs / shader translation": (["pipelines_ms", "programs_ms", "pipe_wait_ms", "translate_ms"], []),
    "texture uploads / transcodes": (["uploads_ms"], []),
    "texture lookups and creation (not uploads)": (["tex_lookup_ms"], ["uploads_ms"]),
    "replay CPU (draws, descriptors, commands)": (["draw_ms"], ["tex_lookup_ms", "pipelines_ms", "programs_ms"]),
    "driver: queue submit": (["qsubmit_ms"], []),
    "GPU behind / slot waits": (["gpu_behind_ms", "slot_wait_ms"], []),
    "compute dispatch": (["dispatch_ms"], []),
}
UNATTR = "unattributed (game CPU / other waits)"


def load(path: Path):
    if path.is_dir():
        path = path / "hookshot_frames.csv"
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items() if k and v not in (None, "")})
            except ValueError:
                pass   # (a line cut by a forced stop)
    return rows


def cause_value(r, cols):
    add, sub = cols
    return sum(r.get(c, 0.0) for c in add) - sum(r.get(c, 0.0) for c in sub)


def classify(rows, cut, factor):
    rows = [r for r in rows if r.get("t_s", 0.0) >= cut and r.get("ms", 0.0) > 0]
    if not rows:
        return None
    med = statistics.median(r["ms"] for r in rows)
    meds = {c: statistics.median(cause_value(r, cols) for r in rows) for c, cols in CAUSES.items()}
    spikes = []
    for r in rows:
        if r["ms"] <= factor * med:
            continue
        excess = r["ms"] - med
        best, best_x = UNATTR, 0.0
        for c, cols in CAUSES.items():
            x = cause_value(r, cols) - meds[c]
            if x > best_x:
                best, best_x = c, x
        if best_x < 0.4 * excess:
            best = UNATTR
        spikes.append((r, best, excess, best_x))
    ms = sorted(r["ms"] for r in rows)
    p99 = ms[min(len(ms) - 1, int(0.99 * len(ms)))]
    worst = ms[-max(1, len(ms) // 100):]
    return {"frames": len(rows), "median": med, "p99": p99, "low1": 1000.0 / statistics.mean(worst),
            "fps": 1000.0 * len(rows) / sum(ms), "spikes": spikes, "meds": meds,
            "spike_ms": sum(r["ms"] - med for r, *_ in spikes)}


def main() -> int:
    args, cut, factor, listn = [], 66.0, 2.0, 0
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--cut": cut = float(next(it))
        elif a == "--factor": factor = float(next(it))
        elif a == "--list": listn = int(next(it))
        else: args.append(Path(a))
    total = defaultdict(lambda: [0, 0.0])
    nframes = 0
    for p in args:
        res = classify(load(p), cut, factor)
        if not res:
            print(p, ": no frames after the cut"); continue
        nframes += res["frames"]
        print(f"{p}: {res['frames']} frames, {res['fps']:.1f} fps, median {res['median']:.1f} ms, p99 {res['p99']:.1f} ms, "
              f"1% low {res['low1']:.1f} fps; {len(res['spikes'])} spikes over {factor:g}x the median "
              f"({res['spike_ms']:.0f} ms of excess)")
        by = defaultdict(lambda: [0, 0.0])
        for r, c, ex, _ in res["spikes"]:
            by[c][0] += 1; by[c][1] += ex
            total[c][0] += 1; total[c][1] += ex
        for c, (n, ex) in sorted(by.items(), key=lambda kv: -kv[1][1]):
            print(f"    {c:45s} {n:4d} spikes, {ex:7.0f} ms excess ({ex / max(n, 1):.1f} ms each)")
        for r, c, ex, cx in sorted(res["spikes"], key=lambda s: -s[0]["ms"])[:listn]:
            parts = " ".join(f"{k}={r.get(k, 0):.1f}" for k in ("late_wait_ms", "memsync_ms", "uploads_ms", "tex_lookup_ms",
                                                              "pipelines_ms", "programs_ms", "draw_ms", "qsubmit_ms",
                                                              "gpu_behind_ms", "slot_wait_ms", "gpu_ms", "astc", "tex_made",
                                                              "scatter_mb", "vram_mb"))
            print(f"      t={r['t_s']:.2f} {r['ms']:.1f} ms -> {c} ({cx:.1f}) | {parts}")
    if len(args) > 1:
        print(f"all ({nframes} frames):")
        for c, (n, ex) in sorted(total.items(), key=lambda kv: -kv[1][1]):
            print(f"    {c:45s} {n:4d} spikes, {ex:7.0f} ms excess")
    return 0


if __name__ == "__main__":
    sys.exit(main())
