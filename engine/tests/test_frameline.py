"""(TICK-054) tools/bench/frameline.py: the frame timeline from a HOS_JOBTRACE file. A synthetic trace of two frames (the main
thread's job-group wait and end-of-frame wait, a poll, the Presentation Thread's queue wait, its submit and present marks, the
GPU's completion marks) must give frames cut at the present's end, the main thread's phases in order, the Presentation Thread's
spans and a GPU busy time.

python -I -B tests/test_frameline.py
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "bench" / "frameline.py"

MS = 1_000_000
EV_WAIT_B, EV_WAIT_E, SLEEP_B, SLEEP_E, Q_WAIT_B, Q_WAIT_E, MARK = 1, 2, 11, 12, 15, 16, 21


def trace(path):
    t0 = 1000 * MS
    lines = ["# jobtrace window %d %d" % (t0, t0 + 40 * MS), "T 0 1 sead::MainThread", "T 1 2 Presentation_Thread",
             "T 2 3 BaseProcExecutor0", "T 3 4 ModuleSystemWorker1"]
    recs = []

    def frame(fb, n):
        # main: work 2 ms (its submit 1 ms inside), job group 6 ms, work 1, polls 1, end-of-frame wait 2, work 1
        recs.append((fb + 1 * MS, MARK, 0, 1, n))
        recs.append((fb + 2 * MS, MARK, 0, 2, n))
        recs.append((fb + 2 * MS, EV_WAIT_B, 0, 0x10, 0x80080188))
        recs.append((fb + 8 * MS, EV_WAIT_E, 0, 0x10, 2))
        recs.append((fb + 9 * MS, SLEEP_B, 0, 20000, 0x8009a478))
        recs.append((fb + 10 * MS, SLEEP_E, 0, 20000, 0x8009a478))
        recs.append((fb + 10 * MS, EV_WAIT_B, 0, 0x20, 0x83acfcf4))
        recs.append((fb + 12 * MS, EV_WAIT_E, 0, 0x20, 2))
        # executor: works during the job group
        recs.append((fb, Q_WAIT_B, 2, 0x30, 0))
        recs.append((fb + 2 * MS, Q_WAIT_E, 2, 0x30, 2))
        recs.append((fb + 8 * MS, Q_WAIT_B, 2, 0x30, 0))
        recs.append((fb + 13 * MS, Q_WAIT_E, 2, 0x30, 2))
        # presentation thread: idle until the hand-over at 2 ms, a submit 2.5-8.5, present 12.5-13
        recs.append((fb, Q_WAIT_B, 1, 0x40, 0))
        recs.append((fb + 2 * MS, Q_WAIT_E, 1, 0x40, 2))
        recs.append((fb + 2.5 * MS, MARK, 1, 1, n))
        recs.append((fb + 8.5 * MS, MARK, 1, 2, n))
        recs.append((fb + 9 * MS, Q_WAIT_B, 1, 0x40, 0))
        recs.append((fb + 12 * MS, Q_WAIT_E, 1, 0x40, 2))
        recs.append((fb + 12.5 * MS, MARK, 1, 5, n))
        recs.append((fb + 13 * MS, MARK, 1, 6, n))
        # the GPU finished the submit 3 ms after it was queued
        recs.append((fb + 11.5 * MS, MARK, 3, 7, n))

    frame(t0 + 1 * MS, 1)
    frame(t0 + 14 * MS, 2)
    frame(t0 + 27 * MS, 3)
    for t, ty, th, a, b in sorted(recs):
        lines.append("%d %d %d %x %x" % (int(t), ty, th, a, b))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class FramelineTests(unittest.TestCase):
    def run_script(self, *args):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "jobtrace.txt"
            trace(p)
            out = subprocess.run([sys.executable, "-I", str(SCRIPT), str(p), *args], capture_output=True, text=True,
                                 encoding="utf-8")
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout

    def test_frames_cut_at_the_present_and_phases_in_order(self):
        out = self.run_script("--frames", "1", "--from", "5", "--row", "1", "--summary")
        self.assertIn("frame cut at the present's end", out)
        self.assertIn("-- frame 0: 13.00 ms", out)
        # the summary: one frame from the first present's end to the next: W1.00 (then S1.00) J6.00 W1.00 P1.00 E2.00 W1.00
        summary = [l for l in out.splitlines() if l.startswith("f0 ")]
        self.assertEqual(len(summary), 1)
        self.assertIn("W2.00 J6.00 W1.00 P1.00 E2.00 W1.00", summary[0])
        self.assertIn("PT idle 5.00 S 6.00 P 0.50", summary[0])
        self.assertIn("GPU busy 9.50", summary[0])   # (from each submit end to the next completion: the main thread's at 1-2 too)

    def test_gantt_letters(self):
        out = self.run_script("--frames", "1", "--from", "5", "--row", "1")
        gantt = out[out.index("-- frame"):out.index("\n\n", out.index("-- frame"))]
        rows = [l.split() for l in gantt.splitlines() if l and l[0] == " "]
        # at 1.5 ms of the frame: main in its submit (S), the Presentation Thread idle, the executor idle
        by_t = {float(r[0]): r for r in rows}
        self.assertEqual(by_t[1.0][1], "S")
        self.assertEqual(by_t[1.0][2], ".")
        self.assertEqual(by_t[3.0][1], "j")
        self.assertEqual(by_t[3.0][2], "S")
        self.assertEqual(by_t[3.0][3], "#")
        self.assertEqual(by_t[9.0][1], "p")
        self.assertEqual(by_t[11.0][1], "e")
        self.assertEqual(by_t[12.0][2], "#")
        self.assertEqual(by_t[9.0][-1], "#")   # the GPU: a submit ended at 8.5, its completion at 11.5
        self.assertEqual(by_t[12.0][-1], ".")

    def test_means(self):
        out = self.run_script("--frames", "1", "--from", "5")
        line = next(l for l in out.splitlines() if l.startswith("## means over"))
        self.assertIn("frame 13.00 ms", line)
        self.assertIn("J 6.00 E 2.00 P 1.00", line)


if __name__ == "__main__":
    unittest.main()
