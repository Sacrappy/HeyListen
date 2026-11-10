"""nx2/oot_patches.py, TICK-047's job order hooks: patch_job_order puts one timing pair around each of the dispatcher's two
indirect job calls and one x0 statement before each level's push, once, at the places TICK-047 chose (never after the
level-0 / level-2 retry labels), and leaves the text alone when its anchors are not each found once. The lifted text is a
stand-in with the same anchor lines. No game needed.

python -I -B tests/test_job_order_patch.py
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

RUN = ("void f_8004ab40(Ctx *__restrict A64_NOESC c) {\n"
       "    c->x[21]=c->x[0];\n"
       "{ uint64_t _t = c->x[8]; c->x[30] = 0x8004b3f4ULL; A64_ICALL(c, _t); }\n"
       "L_8004b3f4:; A64_POLL(c);\n"
       "{ uint64_t _t = c->x[9]; c->x[30] = 0x8004b564ULL; A64_ICALL(c, _t); }\n"
       "return;\n}\n")
PUSH = ("void f_8004fa40(Ctx *__restrict A64_NOESC c) {\n"
        "    c->x[9]=recomp_load64(c,c->x[0] + 272);\n"
        "if (c->x[9] == 0) { goto L_8004fa78; }\n"
        "    c->x[9]=recomp_load16(c,c->x[1] + 12);\n"
        "if ((uint32_t)c->x[9] == 0) { goto L_8004fc80; }\n"
        "L_8004fa78:;\n"
        "if (c->x[8] == 0) { goto L_8004fa88; }\n"
        "if (c->z) { goto L_8004fd88; }\n"
        "L_8004fa88:; A64_POLL(c);\n"
        "    c->x[9]=recomp_load64(c,c->x[0] + 208);\n"
        "return;\n"
        "L_8004fc80:; A64_POLL(c);\n"
        "return;\n"
        "L_8004fd88:; A64_POLL(c);\n"
        "return;\n}\n")
SRC = '#include "a64.h"\n' + RUN + "\n" + PUSH


class JobOrderPatch(unittest.TestCase):
    def test_patched_once_in_place(self):
        out, n = oot_patches.patch_job_order(SRC)
        self.assertEqual(n, 2)
        self.assertEqual(out.count(oot_patches.JO_DECL), 1)
        self.assertEqual(out.count("hos_jo_begin()"), 2)
        self.assertEqual(out.count("hos_jo_end(c, c->x[21], _jq)"), 2)
        lines = out.splitlines()
        shift = [i for i, l in enumerate(lines) if "hos_jo_shift(" in l and "c->x[0] -=" in l]
        self.assertEqual(len(shift), 3)
        # level 2 before its branch to the level-2 push, level 0 before its branch, level 1 after the label
        self.assertIn(", 2);", lines[shift[0]])
        self.assertTrue(lines[shift[0] + 1].startswith("if ((uint32_t)c->x[9] == 0) { goto L_8004fc80; }"))
        self.assertIn(", 0);", lines[shift[1]])
        self.assertEqual(lines[shift[1] + 1], "if (c->z) { goto L_8004fd88; }")
        self.assertIn(", 1);", lines[shift[2]])
        self.assertEqual(lines[shift[2] - 1], "L_8004fa88:; A64_POLL(c);")
        # nothing after the retry targets
        for label in ("L_8004fc80:", "L_8004fd88:"):
            i = next(i for i, l in enumerate(lines) if l.startswith(label))
            self.assertNotIn("hos_jo_", lines[i + 1])
        # no new branch: the PGO records' hash stays the same
        for word in ("if (", "goto ", "while", "for (", "?"):
            self.assertEqual(out.count(word), SRC.count(word), word)

    def test_idempotent(self):
        out, _ = oot_patches.patch_job_order(SRC)
        again, n = oot_patches.patch_job_order(out)
        self.assertEqual(again, out)
        self.assertEqual(n, 2)

    def test_missing_anchor_leaves_function(self):
        src = SRC.replace("if (c->z) { goto L_8004fd88; }\n", "")
        out, n = oot_patches.patch_job_order(src)
        self.assertEqual(n, 1)
        self.assertNotIn("c->x[0] -= hos_jo_shift(", out)

    def test_other_text(self):
        self.assertIsNone(oot_patches.patch_job_order('#include "a64.h"\nvoid f_80000000(Ctx *c) {\n}\n'))


if __name__ == "__main__":
    unittest.main()
