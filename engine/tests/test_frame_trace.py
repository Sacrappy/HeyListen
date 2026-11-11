"""(TICK-051) The per-frame trace (RX_VK_FRAME_TRACE=1, hookshot_frames.csv): its header names as many columns as each line
prints, the TICK-051 columns are there, and the counters behind them keep running totals the trace takes differences of.

python -I -B tests/test_frame_trace.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def c_strings(text):
    """The C string literals of text, joined (adjacent literals concatenate)."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text))


class FrameTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
        cls.tick = block(cls.replay, "void status_tick() {")

    def header(self):
        start = self.tick.index('if (f) fprintf(f, "frame,ms,')
        end = self.tick.index(");", start)
        text = re.sub(r"//[^\n]*", "", self.tick[start + len("if (f) fprintf(f, "):end])
        return c_strings(text).replace("\\n", "").split(",")

    def line(self):
        start = self.tick.index('fprintf(trace, "%llu,')
        end = self.tick.index(");", start)
        call = self.tick[start + len("fprintf(trace, "):end]
        fmt_end = call.index('",\n') + 1
        fmt = c_strings(call[:fmt_end]).replace("\\n", "")
        args = call[fmt_end + 2:]
        depth, n = 0, 1
        for ch in args:
            depth += ch in "(["
            depth -= ch in ")]"
            n += ch == "," and depth == 0
        return fmt, n

    def test_header_and_line_agree(self):
        cols = self.header()
        fmt, nargs = self.line()
        specs = re.findall(r"%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[duxfgs]", fmt)
        self.assertEqual(len(cols), len(specs), "the header and the line's format differ in their columns")
        self.assertEqual(len(specs), nargs, "the line's format and its arguments differ in number")

    def test_tick051_columns(self):
        cols = self.header()
        for c in ("t_s", "gpu_ms", "late_wait_ms", "late_waits", "submit_ms", "present_ms", "qsubmit_ms", "astc",
                  "tex_made", "scatter_mb", "vram_mb", "astc_again", "astc_mtexels"):
            self.assertIn(c, cols)
        self.assertEqual(cols[:2], ["frame", "ms"], "the first columns moved: tools reading the trace would misread it")

    def test_trace_is_off_by_default(self):
        self.assertIn('if (!getenv("RX_VK_FRAME_TRACE") || !atoi(getenv("RX_VK_FRAME_TRACE"))) return (FILE*)nullptr;', self.tick)
        tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
        note = block(tex, "void astc_trace_note(")
        self.assertIn('getenv("RX_VK_FRAME_TRACE")', note)
        self.assertLess(note.index("if (!on) return;"), note.index("std::lock_guard"), "the churn set is taken with the trace off")

    def test_late_wait_running_totals(self):
        mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
        take = block(mem, "bool take_capture(")
        late = block(take, "if (c->late_value && c->map)")
        wait = late.index("vk_timeline_wait(c->late_value);")
        self.assertIn("st_tr_late_ns.fetch_add(", late[wait:])
        self.assertIn("st_tr_late_n.fetch_add(1", late[wait:])
        self.assertNotIn("st_tr_late_ns = ", mem, "the trace's totals are reset: its differences go negative")
        counts = block(mem, "void memory_trace_counts(")
        self.assertIn("st_scatter_bytes.load(", counts)


if __name__ == "__main__":
    unittest.main()
