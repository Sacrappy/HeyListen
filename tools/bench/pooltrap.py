"""Resolve and sum up a HOS_POOL_TRAP dump (engine/host/hos_memsplit.c).

usage: python tools/bench/pooltrap.py <pooltrap.txt> <bin dir with HeyListen.exe, HeyListen.map, hookshot_vk.dll> [top]

The dump's frames are e+<rva> (HeyListen.exe: resolved with HeyListen.map's publics) and v+<rva> (hookshot_vk.dll: resolved
with llvm-nm, from HOOKSHOT_MINGW or the builder's tools). A guest-thread place is the game's code when one of its frames is a
lifted function (f_<guest address>); otherwise it is host code running on a guest thread (an HLE call, nn_fs, NVN).
"""
import bisect
import collections
import os
import re
import subprocess
import sys

PAGE_MB = 4096 / (1 << 20)


def exe_symbols(map_path):
    syms = []
    in_publics = False
    with open(map_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Publics by Value" in line:
                in_publics = True
                continue
            if not in_publics:
                continue
            parts = line.split()
            if len(parts) >= 3 and re.fullmatch(r"[0-9a-fA-F]{16}", parts[2]):
                syms.append((int(parts[2], 16) - 0x140000000, parts[1]))
    syms.sort()
    return syms


def dll_symbols(dll):
    cands = []
    if os.environ.get("HOOKSHOT_MINGW"):
        cands.append(os.path.join(os.environ["HOOKSHOT_MINGW"], "bin", "llvm-nm.exe"))
    la = os.environ.get("LOCALAPPDATA", "")
    for b in ("HeyListenBuilder", "HookshotBuilder"):
        root = os.path.join(la, b, "versions")
        if os.path.isdir(root):
            for v in sorted(os.listdir(root), reverse=True):
                cands.append(os.path.join(root, v, "HookshotBuilder", "tools", "llvm-mingw", "bin", "llvm-nm.exe"))
    nm = next((c for c in cands if os.path.isfile(c)), None)
    if not nm or not os.path.isfile(dll):
        return []
    out = subprocess.run([nm, "-n", "-C", dll], capture_output=True, text=True, errors="replace").stdout
    syms = []
    for line in out.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16) - 0x180000000, p[2]))
    syms.sort()
    return syms


def resolver(syms):
    keys = [a for a, _ in syms]

    def name(rva):
        i = bisect.bisect_right(keys, rva) - 1
        return syms[i][1] if i >= 0 else "?"
    return name


def main():
    dump, bindir = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
    exe = resolver(exe_symbols(os.path.join(bindir, "HeyListen.map")))
    dll = resolver(dll_symbols(os.path.join(bindir, "hookshot_vk.dll")))

    def frame_name(fr):
        if fr.startswith("e+"):
            return exe(int(fr[2:], 16))
        if fr.startswith("v+"):
            return "vk:" + dll(int(fr[2:], 16))
        if fr.startswith("hookshot_vk.dll+"):
            return "vk:" + dll(int(fr[len("hookshot_vk.dll+"):], 16))
        return fr

    totals = []
    by_kind = collections.defaultdict(lambda: [0, 0])          # (flags, kind) -> [read pages, write pages]
    by_fn = collections.defaultdict(lambda: [0, 0, set()])     # (flags, kind, function) -> [r, w, threads]
    with open(dump, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("total "):
                totals.append(line.split())
                continue
            m = re.match(r'site (\d+) (\w+) (0x[0-9a-f]+|0) (\w) "([^"]*)"(.*)', line)
            if not m:
                continue
            pages, cls, flags, rw, thread, rest = int(m[1]), m[2], m[3], m[4], m[5], m[6].split()
            names = [frame_name(fr) for fr in rest if fr != "-"]
            if cls == "kernel":
                kind, fn = "kernel", thread
            elif cls == "guest":
                # (the copy routines a touch can be in, skipped: a lifted function calling memcpy is the game's own touch)
                own = [n for n in names if not re.match(r"(ucrtbase\.dll|msvcrt\.dll|ntdll\.dll|vk:)?(mem(cpy|set|move)|__mem|.*\.dll\+)", n)
                       or n.startswith("vk:")]
                lifted = next((n for n in names if n.startswith("f_")), None)
                if own and own[0].startswith("f_"):
                    kind = "game"          # (lifted code, or a copy routine it called)
                elif lifted:
                    kind = "game-via-host"  # (a host function the game called, an HLE or NVN call, touched it)
                else:
                    kind = "host-on-guest"  # (the back end's or the host's own work on a guest thread: the replay, a present)
                fn = lifted if kind == "game" else (own[0] if own else names[0] if names else "?")
                if kind == "game-via-host":
                    fn = f"{own[0] if own else '?'} <- {lifted}"
            else:
                kind = "backend"
                fn = next((n for n in names if n.startswith("vk:") and "memcpy" not in n and "memset" not in n), names[0] if names else "?")
            by_kind[(flags, kind)][rw == "w"] += pages
            e = by_fn[(flags, kind, fn)]
            e[rw == "w"] += pages
            e[2].add(thread)
    for t in totals:
        kv = dict(zip(t[2::2], t[3::2]))
        print(f"flags {t[1]}: trapped {int(kv['trapped']) * PAGE_MB:.0f} MB (in imported chunks {int(kv['imported']) * PAGE_MB:.0f}), "
              f"pool ended {int(kv['gone']) * PAGE_MB:.0f} MB, untouched {int(kv['untouched']) * PAGE_MB:.0f} MB"
              + (f", pools made {int(kv['made']) * PAGE_MB:.0f} MB, in RAM when made {int(kv['resident_when_made']) * PAGE_MB:.0f} MB"
                 if "made" in kv else ""))
    print()
    for (flags, kind), (r, w) in sorted(by_kind.items()):
        print(f"{flags} {kind:14s} read {r * PAGE_MB:8.1f} MB  write {w * PAGE_MB:8.1f} MB")
    print()
    for (flags, kind, fn), (r, w, th) in sorted(by_fn.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:top]:
        print(f"{flags} {kind:14s} {fn:60s} r {r * PAGE_MB:7.1f} MB w {w * PAGE_MB:7.1f} MB  {', '.join(sorted(th))[:80]}")


if __name__ == "__main__":
    main()
