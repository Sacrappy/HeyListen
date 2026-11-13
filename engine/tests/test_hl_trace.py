"""The crash recorder's core (host/hl_trace_core.c): rings under eight writers, the live and freed blocks' tables, the
file and the crash report's lines (tests/hl_trace_test.c). Windows only (its test threads): skipped elsewhere."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    if os.name != "nt":
        print("skipped: the test's threads are Windows ones")
        return 0
    with tempfile.TemporaryDirectory(prefix="hookshot-hltrace-") as temp:
        exe = Path(temp) / "hl_trace_test.exe"
        subprocess.run([args.cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "host"),
                        str(ROOT / "tests/hl_trace_test.c"), str(ROOT / "host/hl_trace_core.c"), "-o", str(exe)],
                       check=True)
        return subprocess.run([str(exe)], timeout=300).returncode


if __name__ == "__main__":
    sys.exit(main())
