"""The guest window's substrate (host/hos_guestpages.c): a small window reserved the way the guest window is, granules
dropped and restored atomically while threads touch them (tests/guestpages_test.c). Windows only: skipped elsewhere."""
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
        print("skipped: the substrate is Windows only (placeholders)")
        return 0
    with tempfile.TemporaryDirectory(prefix="hookshot-guestpages-") as temp:
        exe = Path(temp) / "guestpages_test.exe"
        subprocess.run([args.cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "host"),
                        str(ROOT / "tests/guestpages_test.c"), str(ROOT / "host/hos_guestpages.c"), "-o", str(exe)],
                       check=True)
        return subprocess.run([str(exe)], timeout=300).returncode


if __name__ == "__main__":
    sys.exit(main())
