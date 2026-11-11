"""(TICK-054) tools/bench/chainb.py: the frame's serial loop from a HOS_JOBTRACE file. A synthetic trace of three presents
(the main thread's held submit, the held flush on a host thread with its capture, the Presentation Thread's submit with a dry
pass, the parallel replay's setup points and a segment wait, then the present) must give two frames cut at the present's end
with each part's time. (TICK-056) The early mirror pre-sync's job on a worker (22/23) and the PT finishing it (24/25).

python -I -B tests/test_chainb.py
"""
from pathlib import Path
import importlib.util
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("chainb", ROOT / "tools" / "bench" / "chainb.py")
chainb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chainb)

US = 1000
MS = 1000 * US


def trace(path):
    lines = ["# jobtrace window 0 100000000", "T 0 0 host:1", "T 1 1 sead::MainThread", "T 2 2 Presentation_Thread",
             "T 3 0 host:mkw_vk_replay_worker"]
    recs = []

    def mark(t, th, kind, v=0):
        recs.append((t, th, kind, v))

    for f in range(3):
        fb = f * 14 * MS
        mark(fb + 3 * MS, 1, 3)                                    # main: the held submit, 3 ms after the present
        mark(fb + 3 * MS + 20 * US, 0, 1), mark(fb + 3 * MS + 30 * US, 0, 11)
        mark(fb + 4 * MS + 30 * US, 0, 12), mark(fb + 5 * MS, 0, 2)  # held flush: capture 1 ms, all 1.98 ms
        mark(fb + 5 * MS, 2, 1)                                    # PT: its submit 5-12.5 ms
        mark(fb + 6 * MS, 2, 13), mark(fb + 6.5 * MS, 2, 14)        # dry pass 0.5
        mark(fb + 6 * MS + 5 * US, 3, 22), mark(fb + 6.7 * MS, 3, 23)  # the pre-sync's job on a worker, 0.695 ms
        mark(fb + 6.5 * MS, 2, 24), mark(fb + 6.7 * MS, 2, 25)      # the PT waits for it 0.2
        mark(fb + 7.0 * MS, 2, 19), mark(fb + 7.1 * MS, 2, 20), mark(fb + 7.2 * MS, 2, 21), mark(fb + 7.3 * MS, 2, 17)
        mark(fb + 8.3 * MS, 2, 18)                                 # own segment 1 ms
        mark(fb + 8.3 * MS, 2, 9), mark(fb + 8.6 * MS, 2, 10)       # seg wait 0.3
        mark(fb + 12.5 * MS, 2, 2)
        mark(fb + 13 * MS, 2, 5), mark(fb + 14 * MS, 2, 6)          # present 13-14
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
        for t, th, k, v in sorted(recs):
            f.write("%d 21 %d %x %x\n" % (t, th, k, v))


class ChainTests(unittest.TestCase):
    def test_parts(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "jobtrace.txt"
            trace(p)
            rows = chainb.frames(*chainb.load(str(p)))
        self.assertEqual(len(rows), 2)
        r = rows[0]
        ms = lambda k: round(r[k] / MS, 3)
        self.assertEqual(ms("frame"), 14.0)
        self.assertEqual(ms("main: present end -> last hold"), 3.0)
        self.assertEqual(ms("held flush: last hold -> PT's first submit"), 2.0)
        self.assertEqual(ms("PT: its submits (first begin -> last end)"), 7.5)
        self.assertEqual(ms("PT: last submit end -> present end"), 1.5)
        self.assertEqual(ms("hold capture made"), 1.0)
        self.assertEqual(ms("PT dry pass"), 0.5)
        self.assertEqual(ms("PT seg wait"), 0.3)
        self.assertEqual(ms("PT setup: mirrors synced"), 0.5)
        self.assertEqual(ms("PT setup: inits, handed out"), 0.1)
        self.assertEqual(ms("PT own segment recorded"), 1.0)
        self.assertEqual(ms("hold presync job"), 0.695)
        self.assertEqual(ms("PT presync finish"), 0.2)


if __name__ == "__main__":
    unittest.main()
