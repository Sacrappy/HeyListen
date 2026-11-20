"""Lifted stores mark only pages the GPU can read (nx2/rtgen.py A64_MARK_BITMAP, host/mark_bitmap.c, build.py RX_MARK_BITMAP).

Longshot 35a4a9fc (#1617): every lifted store wrote its page's mark, and the renderer reads the marks of GPU pages only.
The bitmap is a bit per page of the guest window, set when a GPU mapping is made (host/nvdrv.c map_add) and for the page
before it. Generates the runtime header the build generates (rtgen.py), compiles the real host/mark_bitmap.c and the real
A64_MARK macro with a small driver, and checks: a store on a page with no bit marks nothing; on one with a bit it marks
that page; a store that starts on the page before a mapping and crosses into it is marked; HOS_MARK_BITMAP_ALL=1 sets
every bit; and a build without A64_MARK_BITMAP marks every page, as before. RX_MARK_BITMAP=0 must leave the define out.

    python -I -B tests/test_mark_bitmap.py [--cc gcc]

Ported from Longshot 35a4a9fc (#1617); the check is HeyListen's (that PR had none that covers A64_MARK_BITMAP).
"""
import argparse
import ast
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit       # noqa: E402  (the vendored a64-kit's folder)
import rtgen     # noqa: E402

DRIVER = r'''#include <stdio.h>
#include <string.h>
%(defines)s
#include "a64.h"
uint8_t a64_dirty[A64_DIRTY_PAGES];   /* (a64_rt.c's array, not linked here) */
void hos_gpu_pages_set(uint64_t guest, uint64_t size);
static int marked(uint64_t a) { return a64_dirty[A64_DIRTY_AT(a)] != 0; }
static void clear(uint64_t a, uint64_t n) {   /* (only the pages this checks) */
    memset(&a64_dirty[A64_DIRTY_AT(a)], 0, (size_t)((n + 4095) >> 12));
}
static int bad;
static void want(const char *what, int got, int expect) {
    if (got != expect) { printf("FAIL: %%s: %%s, want %%s\n", what, got ? "marked" : "not marked", expect ? "marked" : "not marked"); bad++; }
}
int main(void) {
    const uint64_t base = 0x100000000ULL;              /* (4 GB: far from the window's start) */
    /* nothing mapped: a store marks nothing */
    clear(base, 6 * 4096);
    A64_MARK(base + 8, 8);
    want("a store on a page no mapping covers", marked(base), 0);
    /* a mapping of 3 pages from page 2 sets page 1 (the one before) through page 4 */
    hos_gpu_pages_set(base + 2 * 4096, 3 * 4096);
    want("the page before the mapping", (int)A64_GPU_PAGE(base + 4096), 1);
    want("the mapping's first page", (int)A64_GPU_PAGE(base + 2 * 4096), 1);
    want("the mapping's last page", (int)A64_GPU_PAGE(base + 4 * 4096), 1);
    want("the page after the mapping", (int)A64_GPU_PAGE(base + 5 * 4096), 0);
    want("two pages before the mapping", (int)A64_GPU_PAGE(base), 0);
    clear(base, 6 * 4096);
    A64_MARK(base + 2 * 4096 + 8, 4);
    want("a store inside the mapping", marked(base + 2 * 4096), 1);
    want("  ... and no other page", marked(base), marked(base + 3 * 4096) | marked(base + 4 * 4096) | marked(base + 5 * 4096));
    clear(base, 6 * 4096);
    A64_MARK(base + 2 * 4096 - 8, 16);                  /* (starts on page 1, crosses into page 2) */
    want("a store crossing into the mapping from the page before", marked(base + 4096), 1);
    want("  ... and its last byte's page", marked(base + 2 * 4096), 1);
    clear(base, 6 * 4096);
    A64_MARK(base + 5 * 4096 + 8, 4);
    want("a store past the mapping", marked(base + 5 * 4096), 0);
    return bad != 0;
}
'''

EVERY = r'''#include <stdio.h>
#include <string.h>
%(defines)s
#include "a64.h"
uint8_t a64_dirty[A64_DIRTY_PAGES];
void hos_gpu_pages_set(uint64_t guest, uint64_t size);
int main(void) {
    const uint64_t base = 0x100000000ULL;
    A64_MARK(base + 8, 8);        /* (no mapping at all) */
    A64_MARK(base + 4096 + 8, 8);
    printf("%%d %%d\n", a64_dirty[A64_DIRTY_AT(base)] != 0, a64_dirty[A64_DIRTY_AT(base + 4096)] != 0);
    return 0;
}
'''


def mark_bitmap_flag(env):
    """build.py's MARK_BITMAP as its own source has it, under RX_MARK_BITMAP=env (None: unset)"""
    tree = ast.parse((ROOT / "nx2/build.py").read_text(encoding="utf-8"))
    node = next(n for n in tree.body if isinstance(n, ast.Assign) and
                any(isinstance(t, ast.Name) and t.id == "MARK_BITMAP" for t in n.targets))
    e = {k: v for k, v in os.environ.items() if k != "RX_MARK_BITMAP"}
    if env is not None:
        e["RX_MARK_BITMAP"] = env
    ns = {"os": type("Os", (), {"environ": e})}
    exec(compile(ast.Module(body=[node], type_ignores=[]), "build.py", "exec"), ns)
    return ns["MARK_BITMAP"]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="clang")
    a = p.parse_args()
    for value, on in ((None, False), ("", False), ("0", False), ("off", False), ("false", False), ("no", False), ("NO", False),
                      ("1", True), ("on", True), ("true", True), ("yes", True)):
        got = mark_bitmap_flag(value)
        if got != (["-DA64_MARK_BITMAP"] if on else []):
            raise SystemExit("FAIL: build.py RX_MARK_BITMAP=%r: %r" % (value, got))
    build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
    if "+ WRITE_TRACK + MARK_BITMAP" not in build:
        raise SystemExit("FAIL: build.py's lifted-code flags no longer carry MARK_BITMAP")
    with tempfile.TemporaryDirectory(prefix="mark-bitmap-") as tmp:
        tmp = Path(tmp)
        rt = rtgen.write(str(tmp / "rt64"))
        for stub in ("a64_suyu.h", "a64_fpx.h"):
            (Path(rt) / stub).write_text("/* (the lift's generated headers, in gen: nothing the store paths use) */\n",
                                         encoding="utf-8")
        src = tmp / "t.c"
        exe = tmp / "t"
        def run(defines, text, env=None):
            src.write_text(text % {"defines": defines}, encoding="utf-8")
            flag = ["-DA64_MARK_BITMAP"] if "A64_MARK_BITMAP" in defines else []
            subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Werror", "-I", str(rt), *flag, str(src),
                            str(ROOT / "host/mark_bitmap.c"), *flag, "-o", str(exe)], check=True)
            e = {k: v for k, v in os.environ.items() if k != "HOS_MARK_BITMAP_ALL"}
            e.update(env or {})
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, env=e)
            return r
        defs = "#define A64_WRITE_TRACK 1\n#define A64_MARK_BITMAP 1"
        r = run(defs, DRIVER)
        if r.returncode:
            raise SystemExit(r.stdout + r.stderr)
        if not r.stdout.startswith("") or "FAIL" in r.stdout:
            raise SystemExit(r.stdout + r.stderr)
        # every page marks, as before the bitmap: a build without the define, and HOS_MARK_BITMAP_ALL=1
        r = run("#define A64_WRITE_TRACK 1", EVERY)
        if r.returncode or r.stdout.split() != ["1", "1"]:
            raise SystemExit("FAIL: without A64_MARK_BITMAP a store does not mark every page: %r %r" % (r.stdout, r.stderr))
        r = run(defs, EVERY, {"HOS_MARK_BITMAP_ALL": "1"})
        if r.returncode or r.stdout.split() != ["1", "1"]:
            raise SystemExit("FAIL: HOS_MARK_BITMAP_ALL=1 does not set every page's bit: %r %r" % (r.stdout, r.stderr))
        r = run(defs, EVERY)
        if r.returncode or r.stdout.split() != ["0", "0"]:
            raise SystemExit("FAIL: with the bitmap and no mapping a store marks a page: %r %r" % (r.stdout, r.stderr))
    print("PASS: a lifted store marks only a GPU-mapped page (and the page before a mapping); RX_MARK_BITMAP=0 and "
          "HOS_MARK_BITMAP_ALL=1 mark every page, as before")


if __name__ == "__main__":
    sys.exit(main())
