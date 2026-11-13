"""The native engine's first module (engine/native: the job system, the lanes guest jobs run under, the main loop, the job
bridge's table): compiled and run with no game and no GPU. Also holds the tree to its rule: nothing of the lifted code is
included from it.

python -I -B tests/test_native_engine.py --cxx <compiler>
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
    args = parser.parse_args()
    check_tree()
    sources = sorted(str(p) for p in (NATIVE / "src").rglob("*.cpp")) + [str(NATIVE / "tests" / "jobs_test.cpp")]
    with tempfile.TemporaryDirectory(prefix="native-engine-") as tmp:
        exe = Path(tmp) / "native-engine.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(NATIVE / "include"),
                        *sources, "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=300)


if __name__ == "__main__":
    main()
