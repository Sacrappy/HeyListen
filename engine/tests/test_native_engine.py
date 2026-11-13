"""The native engine (engine/native): compiled and run with no game and no GPU. Each engine/native/tests/*_test.cpp is a
program of its own, built with every source of the tree. Also holds the tree to its rule: its own headers and the
standard library only, nothing of the lifted code or of the OS layer.

python -I -B tests/test_native_engine.py --cxx <compiler> [--only <name of a test file>]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "native"


def check_tree():
    """The native tree stands alone: its own headers and the standard library only (no lifted code, no OS layer header)."""
    for path in sorted(NATIVE.rglob("*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        for inc in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', path.read_text(encoding="utf-8"), re.M):
            if "/" in inc and not inc.startswith("hl/"):
                raise SystemExit("%s includes %s: the native tree includes only hl/ and the standard library" % (path, inc))
            if inc in ("a64.h", "hos.h") or inc.startswith("code_"):
                raise SystemExit("%s includes %s: no lifted code or OS layer header in the native tree" % (path, inc))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    check_tree()
    sources = sorted(str(p) for p in (NATIVE / "src").rglob("*.cpp"))
    tests = sorted(p for p in (NATIVE / "tests").glob("*_test.cpp") if not args.only or p.name == args.only)
    if not tests:
        raise SystemExit("no test in %s" % (NATIVE / "tests"))
    with tempfile.TemporaryDirectory(prefix="native-engine-") as tmp:
        # (the tree's sources once, as objects: every test links them)
        objects = []
        for src in sources:
            obj = Path(tmp) / (Path(src).relative_to(NATIVE / "src").as_posix().replace("/", "_")[:-4] + ".o")
            subprocess.run([args.cxx, "-std=c++20", "-O1", "-mavx2", "-mfma", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I", str(NATIVE / "include"), "-c", src, "-o", str(obj)], check=True)
            objects.append(str(obj))
        for test in tests:
            exe = Path(tmp) / (test.stem + ".exe")
            subprocess.run([args.cxx, "-std=c++20", "-O1", "-mavx2", "-mfma", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I", str(NATIVE / "include"), str(test), *objects, "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=600)


if __name__ == "__main__":
    main()
