"""(TICK-058) tools/bench/wakelat.py and critpath.py on a synthetic HOS_JOBTRACE: the hand-off latency of a slept wait (signal
to wait end, its virtual-core wait apart), and the critical path walked back between the main thread's end-of-frame waits
(work by thread, hand-offs, a vcore wait with its holder, a poll ended by a job thread stopping in its last step).

python -I -B tests/test_critpath.py
"""
from pathlib import Path
import importlib.util
import io
import sys
import tempfile
import contextlib
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "bench"))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / "bench" / (name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


wakelat = load("wakelat")
critpath = load("critpath")
US = 1000
EV_WAIT_B, EV_WAIT_E, EV_SIGNAL = 1, 2, 3
SLEEP_B, SLEEP_E, VC_WAIT_B, VC_WAIT_E = 11, 12, 13, 14
LR_EOF, LR_POLL = 0x83acfcf4, 0x8009a478


def trace(path):
    # threads: 0 main (host 0x10), 1 EX0 (0x20), 2 MW1 (0x30)
    lines = ["# jobtrace window 0 100000000", "T 0 0000000000000010 sead::MainThread",
             "T 1 0000000000000020 BaseProcExecutor0", "T 2 0000000000000030 ModuleSystemWorker1"]
    recs = []
    for f in range(3):
        b = f * 1000 * US
        # MW1 works, stops at b+100 (polls after); main polls from b+0 to b+110 (the setter: MW1 stopping at b+100)
        recs.append((b + 100 * US, SLEEP_B, 2, 20000, LR_POLL))
        recs.append((b + 900 * US, SLEEP_E, 2, 20000, LR_POLL))
        for k in range(5):
            recs.append((b + k * 22 * US, SLEEP_B, 0, 20000, LR_POLL))
            recs.append((b + k * 22 * US + 21 * US, SLEEP_E, 0, 20000, LR_POLL))
        # main works 110-200, then waits at its end of frame (event 0xE) from 200 on
        recs.append((b + 200 * US, EV_WAIT_B, 0, 0xE, LR_EOF))
        # EX0 waits on event 0xA from 50; main signals it at 150; EX0 waits 152-172 for its core (held by main), runs at 175
        recs.append((b + 50 * US, EV_WAIT_B, 1, 0xA, 0x8004af4c))
        recs.append((b + 150 * US, EV_SIGNAL, 0, 0xA, 0))
        recs.append((b + 152 * US, VC_WAIT_B, 1, 0x1 | 16 << 8 | 16 << 16, 0x10))
        recs.append((b + 172 * US, VC_WAIT_E, 1, 0, 0))
        recs.append((b + 175 * US, EV_WAIT_E, 1, 0xA, 2))
        # EX0 works 175-990, signals 0xE at 990; main runs again at 997
        recs.append((b + 990 * US, EV_SIGNAL, 1, 0xE, 0))
        recs.append((b + 997 * US, EV_WAIT_E, 0, 0xE, 2))
    lines += ["%d %d %d %x %x" % r for r in sorted(recs)]
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


class T(unittest.TestCase):
    def test_wakelat(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "jt.txt"
            trace(p)
            names, hosts, recs = wakelat.load(str(p))
            frames, lat, how_n, sleeps = wakelat.analyze(names, hosts, recs)
            v = lat[("event", 0x8004af4c, 1)]
            self.assertEqual(len(v), 3)
            self.assertEqual(v[0], (25 * US, 20 * US))          # signal 150 -> running 175, 20 us of it the core
            self.assertEqual(lat[("event", LR_EOF, 0)][0], (7 * US, 0))
            self.assertEqual(len(sleeps[(LR_POLL, 0)]), 15)

    def test_critpath(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "jt.txt"
            trace(p)
            out = io.StringIO()
            sys_argv = sys.argv
            sys.argv = ["critpath.py", str(p), "--anchor", "main"]
            try:
                with contextlib.redirect_stdout(out):
                    critpath.main()
            finally:
                sys.argv = sys_argv
            text = out.getvalue()
            # frames 1 and 2: from main's end-of-frame wait end (997) back to the previous one (997 of the frame before):
            # main's EOF wait <- EX0 (7 us), EX0 work 990-175 = 815, EX0's wait <- main at 150 (25 us, 20 of them vcore),
            # main's work 110..150 (the last poll ended at 109), its poll episode <- MW1 stopping at 100 (lag 9 us), MW1 work
            self.assertIn("2 frames, 1.00 ms a frame", text)
            row = lambda k: re.search(r"^  %s +([0-9.]+) ms$" % re.escape(k), text, re.M).group(1)
            self.assertEqual(row("work EX0"), "0.815")
            self.assertEqual(row("work main"), "0.041")             # 150-110 (+1 us after the last poll's end at 109)
            self.assertEqual(row("hand-off: vcore"), "0.020")
            self.assertEqual(row("hand-off: wake"), "0.012")         # 5 (EX0) + 7 (main)
            self.assertEqual(row("poll lag"), "0.009")
            self.assertIn("behind main", text)


if __name__ == "__main__":
    unittest.main()
