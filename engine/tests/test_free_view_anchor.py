"""The free camera's hook goes at the head of the view's loop in f_804e48c0, whichever way the lift wrote that head.

nx2/poll.py puts the interrupt poll before each backward goto, so the loop's head is a bare label; C lifted before that had
the poll after the label. nx2/oot_patches.py's patch_view finds either, once, and a function without the label is left alone.

python -I -B tests/test_free_view_anchor.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

HOOK = "    hos_oot_free_view(c, c->x[20]);\n"


def lifted(head):
    return ('#include "a64.h"\n' + oot_patches.VIEW_HEAD + "    c->x[20]=c->x[0];\n" + head +
            "    c->x[8]=recomp_load64(c,c->x[20] + 8);\n"
            "if ((uint32_t)c->x[8] != 0) { { A64_POLL(c); goto L_804e5068; } }\n"
            "return;\n}\n")


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
    check(oot_patches.patch_view(lifted("L_804e5070:;\n")) is None, "a function without the label was patched")
    check(oot_patches.patch_view(lifted(oot_patches.VIEW_ANCHORS[0]).replace("f_804e48c0", "f_804e48d0")) is None,
          "a file without the function was patched")
    print("PASS: the free camera's hook finds the view loop's head with or without a poll after its label")


if __name__ == "__main__":
    main()
