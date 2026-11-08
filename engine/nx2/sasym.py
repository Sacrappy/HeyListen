"""Symbolize guest addresses by the image's symbol names: a standalone run's crash backtrace (work/runs/<run>/log.txt),
or addresses given on the command line.

Usage:  python -I nx2/sasym.py <run> | <hex address> ...
"""
import bisect
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit
import image as eden_image
import toolchain


def main():
    if sys.argv[1].startswith("0x"):
        addrs = [int(a, 16) for a in sys.argv[1:]]
    else:
        log = open(os.path.join(kit.WORK, "runs", sys.argv[1], "log.txt"), errors="replace").read()
        if "guest backtrace: lr " not in log:
            sys.exit("no crash in that run")
        addrs = [int(x, 16) for x in log.split("guest backtrace: lr ")[1].split("\n")[0].split() if x != "0"]
    img = eden_image.EdenImage(kit.need_exefs())
    ks = sorted(img.names)
    rows = []
    for a in addrs:
        j = bisect.bisect_right(ks, a) - 1
        rows.append((a, img.names[ks[j]] if j >= 0 else "?", a - ks[j] if j >= 0 else 0))
    dm = subprocess.run([toolchain.find("llvm-cxxfilt")], input="\n".join(r[1] for r in rows), capture_output=True,
                        text=True).stdout.splitlines()
    for (a, _, off), d in zip(rows, dm):
        print("%#x  %s +%#x" % (a, d[:110], off))


if __name__ == "__main__":
    main()
