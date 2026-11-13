"""The before/after table of a native-engine step (docs/native-engine/README.md, "How a step is reported"): for each build
and scene, the game's memory in RAM, the process's RAM, VRAM, the main thread's job wait and end-of-frame wait a frame
(the critical chain as the main thread sees it) and fps.

    python -I tools/bench/steptable.py before=<run dir>[,<run dir>...] after=<run dir>[,...]

A run directory is run_bench.py's (bench.json, game_log.txt). The run must have been made with
    HOS_MEM_SPLIT=95 RX_VK_RES_LOG=1 HOS_JOBTRACE=<run dir>/jobtrace.txt HOS_JOBTRACE_AT=75 HOS_JOBTRACE_SECS=6
for its memory lines and its job trace to be there; a column whose line is missing shows "-". Several runs of a build in
one scene are averaged, and the fps column shows each. Numbers of single runs differ by a few fps: compare runs made
interleaved, the same hour.
"""
import collections
import json
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wakelat as W   # noqa: E402

J_LR, E_LR = 0x80080188, 0x83acfcf4   # (the main thread's job-group wait and end-of-frame wait: docs/native-engine/jobs-boundary.md)


def last(pattern, text):
    found = re.findall(pattern, text)
    return found[-1] if found else None


def chain(path):
    """(job wait, end-of-frame wait) of the main thread, ms a frame, from a job trace; None without one"""
    if not os.path.isfile(path):
        return None
    try:
        names, _hosts, recs = W.load(path)
    except Exception:
        return None
    idx = {n: i for i, n in names.items()}
    if "sead::MainThread" not in idx:
        return None
    main = idx["sead::MainThread"]
    begins = set(W.KIND) | {W.SLEEP_B, W.VC_WAIT_B}
    ends = set(W.END_OF) | {W.SLEEP_E, W.VC_WAIT_E}
    blocked, lr, frames, before = 0, None, 0, None
    acc = collections.Counter()
    for t, ty, th, a, b in recs:
        if before is not None and blocked:
            acc["J" if lr == J_LR else "E" if lr == E_LR else "other"] += t - before
        before = t
        if ty == W.MARK and a == 6:
            frames += 1
        if th != main:
            continue
        if ty in begins:
            blocked += 1
            if ty in W.KIND:
                lr = b
        elif ty in ends and blocked > 0:
            blocked -= 1
    if frames < 50:
        return None
    return acc["J"] / 1e6 / frames, acc["E"] / 1e6 / frames


def read(run):
    bench = json.load(open(os.path.join(run, "bench.json"), encoding="utf-8"))
    log = open(os.path.join(run, "game_log.txt"), encoding="utf-8", errors="replace").read()
    game = last(r"memsplit: the game's memory in RAM (\d+) MB", log)
    pools = last(r"memsplit: the game's memory in RAM \d+ MB: GPU pools (\d+) MB", log)
    process = last(r"process memory: private (\d+) MB, working set (\d+) MB", log)
    vram = last(r"vk: VRAM: (\d+) MB used", log)
    system = last(r"heap 1 \(system\): \d+ blocks (\d+) MB", log)
    return {
        "scene": bench.get("scene", "?"), "fps": bench.get("fps") or 0.0,
        "game": int(game) if game else None, "pools": int(pools) if pools else None,
        "ws": int(process[1]) if process else None, "private": int(process[0]) if process else None,
        "vram": int(vram) if vram else None, "vksys": int(system) if system else None,
        "chain": chain(os.path.join(run, "jobtrace.txt")),
    }


def mean(values):
    values = [v for v in values if v is not None]
    return statistics.mean(values) if values else None


def cell(value, fmt="%.0f"):
    return "-" if value is None else fmt % value


def main(argv):
    if not argv or any("=" not in a for a in argv):
        raise SystemExit(__doc__)
    rows = collections.OrderedDict()
    for arg in argv:
        label, dirs = arg.split("=", 1)
        for run in dirs.split(","):
            r = read(run)
            rows.setdefault((r["scene"], label), []).append(r)
    print("| Scene | Build | Game RAM (MB) | of it GPU pools | Process RAM (MB) | Process commit (MB) | VRAM (MB) | "
          "Vulkan in system memory (MB) | Job wait (ms) | End-of-frame wait (ms) | fps |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    for (scene, label), runs in sorted(rows.items(), key=lambda kv: kv[0][0]):
        chains = [r["chain"] for r in runs if r["chain"]]
        fps = [r["fps"] for r in runs]
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            scene, label, cell(mean(r["game"] for r in runs)), cell(mean(r["pools"] for r in runs)),
            cell(mean(r["ws"] for r in runs)), cell(mean(r["private"] for r in runs)), cell(mean(r["vram"] for r in runs)),
            cell(mean(r["vksys"] for r in runs)),
            cell(mean(c[0] for c in chains) if chains else None, "%.2f"),
            cell(mean(c[1] for c in chains) if chains else None, "%.2f"),
            "**%.1f**" % statistics.mean(fps) + (" (%s)" % ", ".join("%.1f" % f for f in fps) if len(fps) > 1 else "")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
