"""(TICK-061, D2/D3) RX_VK_DRY_LEAN: keeping only the dry pass's points next to the planned boundaries picks the same boundaries.

Two parts.
- By the text of gpu/vk/vk_replay.cpp: the candidates are each planned target's two neighbouring multiples of the point
  interval (w / pe * pe and that + pe), the plan comes from count_work's count with the same segment_count / balanced_targets
  calls as the boundary search after the dry pass, the dry pass keeps a point only when lean_points has it, lean_points is
  cleared right after the dry pass, and replay_handle_early (RX_VK_SEG_EARLY) does not set it.
- A model of the boundary search (replay_handle: the nearest point to each target, ties to the first, or the first at or after
  it; a point at or before the last boundary, or at or past the end, is no boundary), run over random counts, point intervals,
  segment counts and targets, with every point and with only the candidates: the same boundaries whenever the dry pass reaches
  the counted end, and boundaries that are kept points when it stops sooner.

python -I -B tests/test_dry_lean.py
"""
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def search(points, total, wants, balance):
    """replay_handle's boundary search over the points (works, ascending): the boundaries' works"""
    out, last = [], 0
    for want in wants:
        at = None
        if balance:
            for q in points:
                if at is None or abs(q - want) < abs(at - want):
                    at = q
            if at is not None and (at <= last or at >= total):
                at = None
        else:
            for q in points:
                if q >= want:
                    at = q
                    break
        if at is None or at == last:
            continue
        out.append(at)
        last = at
    return out


def main():
    src = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    handle = function(src, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop) {")
    early = function(src, "const uint8_t* replay_handle_early(")
    for need in ("const int64_t T = count_work(h);",
                 "mkwvk::segment_plan::segment_count(T, per, recorders, kMaxBatch, one_round);",
                 "mkwvk::segment_plan::balanced_targets(profile, nplan, T, want);",
                 "const int64_t w = balance ? want[k - 1] : T * k / nplan;",
                 "lean.work[lean.n++] = lo, lean.work[lean.n++] = lo + pe;",
                 "lean_points = &lean;",
                 "mkwvk::segment_plan::segment_count(total, per, recorders, kMaxBatch, one_round);",
                 "if (balance) mkwvk::segment_plan::balanced_targets(profile, nseg, total, wants);"):
        if need not in handle:
            raise SystemExit("replay_handle's lean plan is not as this check knows it: " + need)
    if handle.index("lean_points = &lean;") > handle.index("replay(none, state, h);") or \
            handle.index("lean_points = nullptr;") < handle.index("replay(none, state, h);"):
        raise SystemExit("lean_points is not set just for the dry pass")
    if "lean_points" in early:
        raise SystemExit("replay_handle_early must keep every point")
    if "lean_points && !lean_points->has(dry_work)" not in src:
        raise SystemExit("the dry pass does not filter its points by lean_points")
    count = function(src, "int64_t count_work(const uint8_t* p) {")
    for op in ("NOP_DRAW ", "NOP_DRAW_IDX", "NOP_DRAW_INDIRECT", "NOP_DRAW_MESH", "NOP_DISPATCH ", "NOP_DISPATCH_INDIRECT"):
        if op.strip() + " " not in count.replace("||", " ").replace(";", " ") and op.strip() not in count:
            raise SystemExit("count_work does not count " + op)

    rnd = random.Random(61)
    for _ in range(20000):
        pe = rnd.choice([8, 16, 32, 64, 256])
        total = rnd.randrange(0, 3000)
        nseg = rnd.randrange(2, 7)
        balance = rnd.random() < 0.7
        if balance:
            wants = sorted(rnd.randrange(0, total + 1) for _ in range(nseg - 1))
        else:
            wants = [total * k // nseg for k in range(1, nseg)]
        cand = set()
        for w in wants:
            cand |= {w // pe * pe, w // pe * pe + pe}
        stop = total if rnd.random() < 0.8 else rnd.randrange(0, total + 1)   # (a dry pass that stops sooner)
        every = [q for q in range(pe, stop + 1, pe)]
        kept = [q for q in every if q in cand]
        a, b = search(every, stop, wants, balance), search(kept, stop, wants, balance)
        if stop == total and a != b:
            raise SystemExit("FAIL: pe %d total %d wants %s balance %s: %s against %s" % (pe, total, wants, balance, a, b))
        if any(q not in kept for q in b):
            raise SystemExit("FAIL: a boundary not at a kept point")
    print("PASS: the lean dry pass keeps the points the boundaries are at (same boundaries when it reaches its end)")


if __name__ == "__main__":
    main()
