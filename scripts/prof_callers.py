"""Who spends the time: for the samples of a profile (scripts/prof.py's input) whose leaf is in a given module (default nvoglv64.dll, the
NVIDIA driver), the nearest frame of hookshot_vk.dll above it, so the cost of the driver is charged to the back end's function that
called it; and the same for all samples (the nearest named function) as 'self including callees in other modules'.

    HOOKSHOT_BUILD=<folder with hookshot_vk.map> python -I scripts/prof_callers.py <stacks file> [skip] [leaf module] [top]
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prof  # noqa: E402


def main():
    path = sys.argv[1]
    skip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    leaf_mod = sys.argv[3] if len(sys.argv) > 3 else "nvoglv64.dll"
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    maps = {}
    for mod in ("hookshot_vk.dll", "hookshot.exe"):
        mp = os.path.join(prof.BUILD, mod[:-4] + ".map")
        if os.path.exists(mp):
            maps[mod] = prof.load_map(mp, 0x180000000 if mod.endswith(".dll") else 0x140000000)
    import bisect

    def name(frame):
        mod, _, off = frame.rpartition("+")
        if mod in maps:
            addrs, names = maps[mod]
            i = bisect.bisect_right(addrs, int(off, 16)) - 1
            if i >= 0:
                return mod.split(".")[0] + ":" + prof.demangle(names[i])
        return None

    lines = open(path, encoding="utf-8", errors="replace").readlines()
    lines = lines[int(len(lines) * skip):]
    total = 0
    by_driver, by_leaf_vk = collections.Counter(), collections.Counter()
    for line in lines:
        frames = line.strip().split(";")
        if not frames or not frames[0]:
            continue
        total += 1
        first_vk = next((n for n in (name(f) for f in frames) if n and n.startswith("hookshot_vk")), None)
        if frames[0].rpartition("+")[0] == leaf_mod:
            by_driver[first_vk or "?"] += 1
        elif frames[0].rpartition("+")[0] == "hookshot_vk.dll":
            by_leaf_vk[name(frames[0]) or "?"] += 1
    print("%d samples" % total)
    print("-- leaf in %s, by the back end's function that called it" % leaf_mod)
    for k, v in by_driver.most_common(top):
        print("%6.2f%%  %s" % (100.0 * v / total, k[:150]))
    print("-- leaf in hookshot_vk.dll, by function")
    for k, v in by_leaf_vk.most_common(top):
        print("%6.2f%%  %s" % (100.0 * v / total, k[:150]))


if __name__ == "__main__":
    main()
