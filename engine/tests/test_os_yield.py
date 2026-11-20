"""nn::os::YieldThread gives up the virtual core only (host/hos_thread.c, os_yield; HOS_OS_YIELD=1: SwitchToThread too).

Longshot #1663: OoT yields after every job and every actor callback, and each yield also asked the OS to switch threads,
which on few cores took a share of the frame. Cuts os_yield out of the source and builds it with a driver under each
value of the switch, and reads the two call sites (YieldThread and a zero-length sleep) to check both take it.

    python -I -B tests/test_os_yield.py [--cc gcc]

Ported from Longshot d10b62b3 (#1663); the check is HeyListen's.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "host/hos_thread.c"

DRIVER = r'''#include <stdio.h>
#include <stdlib.h>
%(fn)s
int main(void) { printf("%%d\n", os_yield()); return 0; }
'''

# (what the switch means: unset, empty and "0..." off; anything else on, as env_on's rule)
CASES = [(None, "0"), ("", "0"), ("0", "0"), ("0x", "0"), ("1", "1"), ("true", "1"), ("yes", "1")]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="clang")
    a = p.parse_args()
    text = SRC.read_text(encoding="utf-8")
    start = text.index("static int os_yield(void) {")
    fn = text[start:text.index("\n}", start) + 2]
    if "HOS_OS_YIELD" not in fn:
        raise SystemExit("FAIL: os_yield does not read HOS_OS_YIELD")
    yield_hle = text[text.index("HLE(_ZN2nn2os11YieldThreadEv) {"):]
    yield_hle = yield_hle[:yield_hle.index("\n}")]
    if "hos_vcore_yield();" not in yield_hle or "if (os_yield()) SwitchToThread();" not in yield_hle:
        raise SystemExit("FAIL: YieldThread no longer keeps the virtual-core yield and gates SwitchToThread on os_yield")
    zero = re.search(r"static void sleep_away\(int64_t ns, int guest\) \{\n    if \(ns <= 0\) \{\n(.*?)\n        return;", text, re.S)
    if not zero or "hos_vcore_yield();" not in zero.group(1) or "if (os_yield()) SwitchToThread();" not in zero.group(1):
        raise SystemExit("FAIL: sleep_away's zero-length sleep (SleepThread(0)) no longer gates SwitchToThread on os_yield")
    with tempfile.TemporaryDirectory(prefix="os-yield-") as tmp:
        src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
        src.write_text(DRIVER % {"fn": fn}, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Werror", str(src), "-o", str(exe)], check=True)
        bad = 0
        for value, want in CASES:
            env = {k: v for k, v in os.environ.items() if k != "HOS_OS_YIELD"}
            if value is not None:
                env["HOS_OS_YIELD"] = value
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=env)
            got = r.stdout.strip()
            if r.returncode or got != want:
                print("FAIL: HOS_OS_YIELD=%s: %r, want %r" % ("(unset)" if value is None else value, got, want))
                bad += 1
    if bad:
        raise SystemExit(1)
    print("PASS: the yield keeps the virtual core and calls SwitchToThread only with HOS_OS_YIELD set (YieldThread and "
          "SleepThread(0))")


if __name__ == "__main__":
    sys.exit(main())
