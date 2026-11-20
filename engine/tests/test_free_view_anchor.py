"""The free camera's hook goes at the head of the view's loop in f_804e48c0, whichever way the lift wrote that head.

nx2/poll.py puts the interrupt poll before each backward goto, so the loop's head is a bare label; C lifted before that had
the poll after the label. nx2/oot_patches.py's patch_view finds either, once, and a function without the label is left alone.
The block lift (A64_FPX_BLOCK, mkg.ps1 -Block, the shipped builds) also writes whole FPX regions whose closing '}' stands
alone on a line inside the function; the view's loop sits 0x7a8 into f_804e48c0, past the first of them, so taking the first
such line for the function's end left the anchor out and every fresh -Block lift said "the free camera was found 0 times"
(the hud agent's build the first to re-lift). The end is the next function's head now, as in patch_text_speed, and the
block-lift shape is checked here for both anchors, with a next function after and at the file's end.

python -I -B tests/test_free_view_anchor.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

HOOK = "    hos_oot_free_view(c, c->x[20]);\n"

# (a whole FPX region as the block lift writes it: closed by a '}' alone on a line, inside the function, before the loop)
FPX = ("if (RECOMP_LIKELY(RECOMP_FPX_OPEN(c))) {\n"
       "const __m128i _fzm = a64_bk_fzm(c); __m128i _bad = _mm_setzero_si128();\n"
       "__m128i _bv8 = a64_bk_ld(c->vreg[8]);\n"
       "uint64_t _bx20 = c->x[20], _bx8 = c->x[8];\n"
       "if (RECOMP_LIKELY(!_mm_movemask_epi8(_bad))) { a64_bk_st(c->vreg[8], _bv8); c->x[8] = _bx8; _bk = 1; } else "
       "A64_BK_PROBE(0);\n"
       "}\n")

NEXT = "void f_804ea3e0(Ctx *__restrict A64_NOESC c) {\n    return;\n}\n"   # (the head that ends the function)


def lifted(head, fpx="", next_head=""):
    return ('#include "a64.h"\n' + oot_patches.VIEW_HEAD + "    c->x[20]=c->x[0];\n" + fpx + head +
            "    c->x[8]=recomp_load64(c,c->x[20] + 8);\n"
            "if ((uint32_t)c->x[8] != 0) { { A64_POLL(c); goto L_804e5068; } }\n"
            "return;\n}\n" + next_head)


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    check(len(oot_patches.VIEW_ANCHORS) == 2 and len(set(oot_patches.VIEW_ANCHORS)) == 2, "two distinct anchors expected")
    for head in oot_patches.VIEW_ANCHORS:
        out = oot_patches.patch_view(lifted(head))
        check(out is not None and out.count(HOOK) == 1, "the hook is not in the view's loop once: %r" % head)
        check(head + HOOK in out, "the hook does not follow the loop's head: %r" % head)
        check("extern void hos_oot_free_view(Ctx *c, uint64_t view);" in out, "the lifted file does not declare the hook")
        check("A64_POLL(c); goto L_804e5068;" in out, "the back-edge's poll was lost")
        for shape, where in ((lifted(head, FPX, NEXT), "past an FPX block"),
                             (lifted(head, FPX), "past an FPX block, the function last in its file")):
            out = oot_patches.patch_view(shape)
            check(out is not None and out.count(HOOK) == 1,
                  "the hook is not in the view's loop once %s: %r" % (where, head))
            check(head + HOOK in out, "the hook does not follow the loop's head %s: %r" % (where, head))
            check(NEXT not in shape or NEXT in out, "the function after the view's was lost %s" % where)
    check(oot_patches.patch_view(lifted("L_804e5070:;\n", FPX, NEXT)) is None,
          "a function without the label was patched")
    check(oot_patches.patch_view(lifted(oot_patches.VIEW_ANCHORS[0], FPX).replace("f_804e48c0", "f_804e48d0")) is None,
          "a file without the function was patched")
    print("PASS: the free camera's hook finds the view loop's head with or without a poll after its label, past the "
          "block lift's FPX regions")


if __name__ == "__main__":
    main()
