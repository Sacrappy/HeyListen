"""Game counters that add the frame step, or an amount of it, cut to whole frames keep counting above 60 fps (after upstream
Hookshot f915af93: host/frame_whole.h, host/hl_step.c hos_frame_whole; the hooks put in by nx2/hl_gates.py).

The pick-up indicator above Link's head (f_80a87e50) moves the count it shows toward the wallet's by (int)(step * per) and fades
when they meet; the time of day (f_803edf70, after upstream f0c40fc96) adds (int)(2 * step) game seconds a frame. Under a
Frame rate cap above 60 the step is the frame's own time, under 1 (0.42 at 144 fps): cut to 0 every frame, the count never
moved and the indicator stayed, and the clock stood still. The water renderer (f_800672e0, after upstream 6df80a8d) runs its
ripple simulation (int)step times a frame the same way, its remainder kept per water object: forty surfaces stepped in turn
each still count their 60 frames a second (64 slots). With the frames' own time as the step (carry), frame_whole adds the
amount to the counter's own remainder and hands out its whole frames: 144 frames of 60/144 are 60; an amount of 1 or more keeps
its fraction too; two counters keep their own remainders; NaN and amounts below zero give 0. Without it, the amount is cut as
the game cuts it (60 fps as before).

The hooks: host/hl_step.c asks nx2/hl_gates.py for one a site (HL_WHOLE_HOOK: the function, its hash, the register of the cut,
the counter's address), a line after the site's cut; the function's hash is unchanged, a second run changes nothing, another
text, a function an exeFS mod patches and a tree that does not ask leave the lift as it was, and the same cut in another
function stays, as does the clock's second cut (the one not stored to its seconds). The hooked made-up sites, compiled with the host's hos_frame_whole, count 60 frames a second at 144 fps, each
object its own; at a cap of 60 and with HOS_FPS_WHOLE=0 they count as the game does; without the hooks the same checks fail.

python -I -B tests/test_frame_whole.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import hl_gates  # noqa: E402

# (the sites host/hl_step.c asks for: the register the function cuts into, the counter's address, and whether the function
#  makes the same cut once more elsewhere, so that the cut is the one stored to the counter)
SITES = {
    "80a87e50": (12, "c->x[19] + 864", False),   # the pick-up indicator's shown count
    "803edf70": (8, "c->x[19] + 60", True),      # the time of day's seconds added a frame (its cut once more elsewhere)
    # (the water's ripple substeps, f_800672e0: its hook hands out the substeps too ("t8"): tests/test_above60_ledge_zora_song.py)
    "80652cb0": (22, "c->x[19] + 2296", False),  # the floor web's simulation steps (tests/test_web_floor_steps.py)
}
HOST_ANCHOR = "uint32_t hos_frame_whole(uint64_t at, uint32_t amount_bits, uint32_t game) {"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(source, marker):
    check(marker in source, "host/hl_step.c has no '%s'" % marker)
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


def lifted_site(addr, reg, key, twice):
    """a made-up function with the site's cut of vreg 0's amount; out: what the count is after it. A function with the cut
    twice stores the second to another field"""
    cut = hl_gates.CUT % reg
    s = hl_gates.HEAD % addr + "    c->x[8]=1;\n" + cut
    if twice:
        s += hl_gates.WHOLE_STORE % (key, reg)
    s += "    out = (uint32_t)c->x[%d];\n" % reg
    if twice:
        s += cut + "    recomp_store32(c,c->x[19] + 12,c->x[%d]);\n" % reg
    return s + "return;\n}\n"


LIFTED = '#include "a64.h"\n' + "".join(lifted_site(a, *v) for a, v in SITES.items()) + \
    hl_gates.HEAD % "90000000" + "".join(hl_gates.CUT % v[0] for v in SITES.values()) + "}\n"

HELPER = r"""
static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static void helper(void) {
    static FrameWhole w;
    const uint64_t a = 0x1000, b = 0x2000;
    uint32_t n = 0;
    for (int i = 0; i < 144; i++) n += frame_whole(&w, a, bits(60.0f / 144.0f), 1);
    CHECK(n == 60 || n == 59);   /* (a second at 144 fps: 60 frames, give or take the float's last one) */
    n = 0;
    for (int i = 0; i < 1440; i++) n += frame_whole(&w, a, bits(60.0f / 144.0f), 1);
    CHECK(n >= 599 && n <= 601);
    CHECK(frame_whole(&w, b, bits(0.42f), 1) == 0 && frame_whole(&w, b, bits(0.42f), 1) == 0 && frame_whole(&w, b, bits(0.42f), 1) == 1);
    n = 0;
    for (int i = 0; i < 4; i++) n += frame_whole(&w, b + 8, bits(1.25f), 1);   /* (the step times 3: its fractions kept) */
    CHECK(n == 5);
    n = 0;
    for (int i = 0; i < 144; i++) n += frame_whole(&w, b + 16, bits(2.0f * 60.0f / 144.0f), 1);   /* (the time of day) */
    CHECK(n == 120 || n == 119);   /* (a second at 144 fps: the 120 game seconds of one at 60) */
    CHECK(frame_whole(&w, a, bits(0.42f), 0) == 0 && frame_whole(&w, a, bits(1.5f), 0) == 1 && frame_whole(&w, a, bits(2.0f), 0) == 2);
    CHECK(frame_whole(&w, a, bits(-1.0f), 1) == 0 && frame_whole(&w, a, bits(NAN), 1) == 0 && frame_whole(&w, a, bits(0.0f), 1) == 0);
    for (uint64_t k = 0; k < 100; k++) frame_whole(&w, 0x9000 + k, bits(0.5f), 1);   /* (more counters than slots: the oldest go) */
    CHECK(frame_whole(&w, 0x9000 + 99, bits(0.5f), 1) == 1);   /* (a slot still held: 0.5 + 0.5) */
    CHECK(frame_whole(&w, 0x9000, bits(0.5f), 1) == 0);   /* (the oldest's slot was reused: it starts again from 0) */
    /* (a scene's water surfaces, each stepped once a frame at 144 fps: 40 of them, more than the old 16 slots, each still counts
       its 60 frames a second) */
    {
        static FrameWhole water;
        uint32_t passes[40] = {0};
        for (int f = 0; f < 144; f++)
            for (int k = 0; k < 40; k++) passes[k] += frame_whole(&water, 0x7000000 + (uint64_t)k * 0x400, bits(60.0f / 144.0f), 1);
        for (int k = 0; k < 40; k++) CHECK(passes[k] == 60 || passes[k] == 59);
    }
}
"""

FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame_whole.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* (the lock as a flag: taken once, given back once) */
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static void AcquireSRWLockExclusive(SRWLOCK *l) { CHECK(!*l); *l = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { CHECK(*l); *l = 0; }
volatile uint32_t hos_frame_seconds_bits;
@HOST@
@HELPER@
/* (the lifted code's helpers, as the lift has them for these lines, on a small guest) */
#define A64_NOESC
typedef struct { uint64_t x[32], vreg[32][2]; uint32_t fpcr, fpsr; } Ctx;
#define BASE 0x10000ULL
static uint8_t guest[0x4000] __attribute__((aligned(16)));
static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { (void)c; memcpy(guest + (a - BASE), &v, 4); }
static uint64_t recomp_fp_to_int(uint64_t b, int iw, int ow, int s, int mode, uint32_t fpcr, uint32_t *fpsr) {
    (void)iw, (void)ow, (void)s, (void)mode, (void)fpcr, (void)fpsr;
    float f;
    const uint32_t low = (uint32_t)b;
    memcpy(&f, &low, 4);
    return (uint32_t)(f != f ? 0 : (int32_t)f);
}
static uint32_t out;
@LIFTED@
typedef void (*Site)(Ctx *);
static const Site sites[] = {@SITES@};
enum { SITES = sizeof sites / sizeof sites[0] };
/* (one update of a site for an object at BASE + 0x1000 * object (x19, and x21 the same), an amount of the step in vreg 0) */
static uint32_t update(int site, int object, float amount) {
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.x[19] = ctx.x[21] = BASE + 0x1000 * (uint64_t)object;
    ctx.vreg[0][0] = bits(amount);
    sites[site](&ctx);
    return out;
}
static void seconds(double s) { hos_frame_seconds_bits = s > 0 ? bits((float)s) : 0; }
int main(int argc, char **argv) {
    const int off = argc > 1 && !strcmp(argv[1], "off"), only = argc > 2 ? atoi(argv[2]) : -1;   /* (only: one site) */
    helper();
    for (int k = 0; k < SITES; k++) {
        if (only >= 0 && k != only) continue;
        /* a cap of 60: the game's cut */
        seconds(0);
        CHECK(update(k, 1, 0.42f) == 0 && update(k, 1, 1.5f) == 1 && update(k, 1, 2.0f) == 2);
        /* 144 fps: two objects, 60 frames a second each (the game's cut: 0); then HOS_FPS_WHOLE=0: the game's */
        seconds(1.0 / 144);
        uint32_t one = 0, two = 0;
        for (int i = 0; i < 144; i++) one += update(k, 1, 60.0f / 144.0f), two += update(k, 2, 60.0f / 144.0f);
        if (off) CHECK(one == 0 && two == 0);
        else CHECK((one == 60 || one == 59) && (two == 60 || two == 59));
        /* 40 fps: one and a half frames an update, the half kept (the game's cut: 1) */
        seconds(1.0 / 40);
        one = 0;
        for (int i = 0; i < 40; i++) one += update(k, 3, 1.5f);
        CHECK(off ? one == 40 : (one == 60 || one == 59));
    }
    printf("PASS: %d site(s)%s\n", SITES, off ? ", off" : "");
    return 0;
}
'''


def hook_text():
    """The hooks in a made-up lift: put in after each site's cut, once, with the hashes unchanged; idempotent; left out for
    another text, a function an exeFS mod patches, or a tree that does not ask; an older tree's stale-gate rule takes them
    out; the same cut in another function stays as it is."""
    hashes = {a: hl_gates.text_hash(LIFTED, a) for a in SITES}
    lines = {a: hl_gates.lift_lines("whole", a, (reg, key))[0] for a, (reg, key, _) in SITES.items()}
    with tempfile.TemporaryDirectory(prefix="frame-whole-hook-") as tmp:
        gen, host = Path(tmp) / "gen", Path(tmp) / "host"
        gen.mkdir()
        host.mkdir()
        code = gen / "code_000.c"
        code.write_text(LIFTED, encoding="utf-8", newline="")
        (host / "hl_w.c").write_text("".join('HL_WHOLE_HOOK(%s, "%s", %d, "%s");\n' % (a, hashes[a], reg, key)
                                             for a, (reg, key, _) in SITES.items()), encoding="utf-8")
        hl_gates.apply(str(gen), str(host), quiet=True)
        s = code.read_text(encoding="utf-8")
        for a, (reg, key, twice) in SITES.items():
            line = lines[a]
            check(s.count(line) == 1 and "/* hl gate %s */" % hl_gates.step_id(a) in s, "f_%s's count is not the host's, once" % a)
            check(hl_gates.CUT % reg + line + (hl_gates.WHOLE_STORE % (key, reg) if twice else "") in s,
                  "f_%s's hook is not right after its cut" % a)
            check(hl_gates.text_hash(s, a) == hashes[a], "the hook changed f_%s's hash" % a)
        check(s.count("= hos_frame_whole(") == len(SITES), "another cut hooked (the other function, or a second cut)")
        old = re.compile(r"^.*/\* hl gate ([0-9a-f]{8}) \*/\n", re.M).sub("", s)
        check(old == LIFTED, "an older tree's gates script does not restore the lift")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == s, "a second run changed a hooked file")
        first = next(iter(SITES))
        hl_gates.apply(str(gen), str(host), quiet=True, skip={first: "some_mod"})
        t = code.read_text(encoding="utf-8")
        check(lines[first] not in t and t.count("= hos_frame_whole(") == len(SITES) - 1, "a patched function was hooked")
        other = LIFTED.replace("32,32,", "32,31,0,")   # (another game version's code: longer too, so a filesystem with a
                                                          #  coarse mtime still reads the file again: hl_gates' stamp)
        code.write_text(other, encoding="utf-8", newline="")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == other, "a function whose cut differs was hooked")
        code.write_text(LIFTED, encoding="utf-8", newline="")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == s, "hooked again")
        os.remove(host / "hl_w.c")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == LIFTED, "none asked: the hooks stay")
    asked = {a: v for a, (h, k, v) in hl_gates.wanted_lifts().items() if k == "whole" and len(v) == 2}   # (the unsigned cuts: tests/test_scarecrow_timing.py)
    check(asked == {a: (reg, key) for a, (reg, key, _) in SITES.items()},
          "host/hl_step.c asks for other whole sites than this test knows: %s" % sorted(asked))
    return s


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    hooked = hook_text().replace('#include "a64.h"\n', "")
    step = (ROOT / "host" / "hl_step.c").read_text(encoding="utf-8")
    fixture = FIXTURE.replace("@HOST@", block(step, HOST_ANCHOR)).replace("@HELPER@", HELPER) \
        .replace("@SITES@", ", ".join("f_" + a for a in SITES))
    with tempfile.TemporaryDirectory(prefix="frame-whole-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        cmd = [a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(ROOT / "host"), str(c),
               "-o", str(exe), "-lm"]
        c.write_text(fixture.replace("@LIFTED@", hooked), encoding="utf-8")
        subprocess.run(cmd, check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}
        subprocess.run([str(exe)], env=env, check=True)
        subprocess.run([str(exe)], env=dict(env, HOS_FPS_WHOLE="1"), check=True)   # (read by its value: 1 is on)
        subprocess.run([str(exe), "off"], env=dict(env, HOS_FPS_WHOLE="0"), check=True)
        r = subprocess.run([str(exe), "off"], env=env, capture_output=True, text=True)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "the counts are the game's own without HOS_FPS_WHOLE=0")
        # (the negative control: the game's own cut fails the same checks at 144 fps)
        c.write_text(fixture.replace("@LIFTED@", LIFTED.replace('#include "a64.h"\n', "")), encoding="utf-8")
        subprocess.run(cmd, check=True)
        for k, addr in enumerate(SITES):
            r = subprocess.run([str(exe), "on", str(k)], env=env, capture_output=True, text=True)
            check(r.returncode != 0 and "FAIL:" in r.stderr, "f_%s unhooked passes the checks: they prove nothing" % addr)
    print("frame whole: ok")


if __name__ == "__main__":
    main()
