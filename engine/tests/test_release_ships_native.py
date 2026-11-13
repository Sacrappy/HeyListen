"""The builders ship what nx2/build.py compiles into the game: the native engine (native/) and zstd's decoder
(third_party/zstd/lib, for host/hl_stream.c). A Windows release whose package left native/ out stopped every player's
build at its first step; the AppImage carried neither.

    python -I -B tests/test_release_ships_native.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    bad = []
    build = read("nx2/build.py")
    if '"native"' not in build or '"zstd"' not in build:
        bad.append("nx2/build.py no longer names native/ and third_party/zstd: this check is out of date")
    parts = read("scripts/release_parts.py")
    m = re.search(r"def package\(a\):.*?for f in tracked\(([^)]*)\)", parts, re.S)
    shipped = set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()
    for top in ("host", "native", "nx2", "third_party"):
        if top not in shipped:
            bad.append("scripts/release_parts.py package(): %s/ is not shipped (%s)" % (top, sorted(shipped)))
    app = read("scripts/linux/appimage.sh")
    if not re.search(r"^cp -a [^\n]*\bnative\b", app, re.M):
        bad.append("scripts/linux/appimage.sh does not copy native/")
    if not re.search(r"^cp -a third_party/zstd/lib\b", app, re.M):
        bad.append("scripts/linux/appimage.sh does not copy third_party/zstd/lib")
    for b in bad:
        print("FAIL: " + b)
    print("release ships native: %s" % ("FAILED" if bad else "ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
