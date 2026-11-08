"""The SDK surface the game's modules use: every symbol main and the subsdk* modules import that the sdk defines,
grouped by namespace, and (with --hle <file of mangled names>) which of them an existing HLE layer already covers
(a64-kit's hos/, TOTKPort).

Usage:  python -I nx2/imports.py [--hle names.txt] [--list]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit
import image as eden_image


def namespace(name):
    """nn::os, nn::fs, ... from an Itanium mangled name (the first two nested names), or the C name"""
    m = re.match(r"_Z(?:N|L)?(?:\d+)?", name)
    parts, s = [], name[2:] if name.startswith("_Z") else None
    if s is None:
        return "(C) " + name.split("_")[0][:12]
    if s.startswith("N"):
        s = s[1:]
    while s and len(parts) < 2:
        m = re.match(r"(\d+)", s)
        if not m:
            break
        n = int(m.group(1))
        s = s[len(m.group(1)):]
        parts.append(s[:n])
        s = s[n:]
    return "::".join(parts) or name[:20]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--hle")
    p.add_argument("--list", action="store_true")
    a = p.parse_args()
    img = eden_image.EdenImage(kit.need_exefs())
    sdk = next(m for m in img.modules if m.name == "sdk")
    sdk_defs = {s["name"] for s in sdk.symbols if s["defined"] and s["name"]}
    used = set()
    for m in img.modules:
        if m.name in ("sdk", "rtld"):
            continue
        for s in m.symbols:
            if not s["defined"] and s["name"] in sdk_defs:
                used.add(s["name"])
    hle = set(open(a.hle).read().split()) if a.hle else set()
    by_ns = collections.defaultdict(lambda: [0, 0])
    for n in used:
        by_ns[namespace(n)][0] += 1
        by_ns[namespace(n)][1] += n in hle
    print("sdk symbols the game modules import: %d; covered by the HLE list: %d" % (len(used), len(used & hle)))
    for ns, (n, h) in sorted(by_ns.items(), key=lambda kv: -kv[1][0]):
        print("  %-28s %5d  covered %4d" % (ns, n, h))
    if a.list:
        for n in sorted(used):
            print(("  * " if n in hle else "    ") + n)


if __name__ == "__main__":
    main()
