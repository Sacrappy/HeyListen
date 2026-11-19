"""Three gameplay places that break above a 60 fps cap, as lift hooks (after Longshot 3ab8cae6 / Hookshot 93a8bff3 by PirateKingIsh,
Longshot 8e8a06db by CraftyCorvid, Longshot 4f755527 by CraftyCorvid; their tests, test_saved_turn_rate.py, test_horse_jump_step.py and
test_floor_carry_step.py, are the model of the checks here).

Upstream edits the lifted text in nx2/oot_patches.py; here the hooks are lines nx2/hl_gates.py puts in after anchors (HL_TURN_HOOK,
HL_HORSE_HOOK, HL_FLOOR_HOOK in host/hl_high_fps.c), the functions' hashes unchanged:
- Gohma, f_8263e8e0: the steering update clamps the saved rate (+36) to the turn's magnitude times the step, but the turn was already
  scaled by the step, so a rate-limited update kept rate x step squared. For 0 < step < 1 (above 60 fps) both bounds are the division.
- Epona, f_82c59eb0: the jump state machine takes the blend frames of her JumpReady animation from last frame's plus the step when its
  step is under 1 (slow motion at 60 fps): above 60 fps they grew each frame and she galloped through fences. A step that is the whole
  frame's (the time object's) takes the branch past it.
- The Shadow Temple's barge, f_80159dc0, f_8015d490, f_838baa40: three character modules carry a character by a tenth of a moving
  floor's velocity when the physics' 1/dt is over 100 (above 100 fps always): Link and the enemies slid to its back. Above a 60 cap on
  a frame under a 60 fps one, 1/dt x the frame's step is tested instead.

Authored lifted functions of the real shapes (the anchors are the real text) go through the real hl_gates; the host's functions are cut
from host/hl_high_fps.c and compiled with them. Checked: the hooked code gives the game's own result at a step of 1 or more, at a cap of
60 and for invalid values, the fix above 60; the unhooked functions fail the same checks; a second run changes nothing, another text and
a function an exeFS mod patches are left alone, and a tree that does not ask leaves the lift as it was.

python -I -B tests/test_above60_gameplay.py [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import hl_gates  # noqa: E402

HEAD = hl_gates.HEAD
TURN, HORSE = "8263e8e0", "82c59eb0"
FLOORS = (("80159dc0", "L_8015a39c"), ("8015d490", "L_8015dad4"), ("838baa40", "L_838baf24"))


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


# (Gohma: the two clamps, then the saved rate from them)
LIFTED_TURN = (HEAD % TURN + hl_gates.TURN_MULS + hl_gates.TURN_MULS2 +
               "    { const float rate = as_float(c->vreg[2][0]), lower = as_float(c->vreg[1][0]), upper = as_float(c->vreg[0][0]);\n"
               "      c->saved = rate < lower ? lower : (rate > upper ? upper : rate); }\n}\n")
# (Epona: the step test and its branch past the slow-motion one)
LIFTED_HORSE = (HEAD % HORSE + "    c->slow = 0;\n" + hl_gates.HORSE_TEST +
                "{ c->n = 0; } }\n" + hl_gates.HORSE_BRANCH +
                "    c->slow = 1;   /* (the game's branch for a step under 1) */\nL_82c5ae48:;\n}\n")


def floor_module(addr, label):
    return (HEAD % addr + hl_gates.FLOOR_TEST + hl_gates.FLOOR_FCMP + "{ c->n = 0; c->z = 0; c->c = 1; c->v = 1; } }\n" +
            hl_gates.FLOOR_BRANCH % label + "    c->carry = 1.0f;   /* (the floor's velocity as it is) */\n    return;\n" +
            "%s:;\n    c->carry = 0.1f;   /* (a tenth of it) */\n}\n" % label)


LIFTED_FLOORS = "".join(floor_module(a, l) for a, l in FLOORS)
LIFTED = LIFTED_TURN + LIFTED_HORSE + LIFTED_FLOORS
UNRELATED = (LIFTED_TURN.replace(TURN, "93000000") + LIFTED_HORSE.replace(HORSE, "93000001") +
             floor_module("93000002", "L_8015a39c"))

FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oot_high_fps.h"
#define A64_NOESC
typedef struct {
    uint64_t x[32], vreg[32][2];
    uint8_t n, z, c, v;
    float saved, carry;
    int divides, multiplies, slow;
} Ctx;
static const uint64_t g_module_base = 0;
static float as_float(uint64_t bits) { float f; uint32_t b = (uint32_t)bits; memcpy(&f, &b, 4); return f; }
static uint64_t as_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static void a64_vset64(uint64_t *v, uint64_t bits) { v[0] = bits; v[1] = 0; }
static uint64_t a64_fpx_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) { (void)z; c->multiplies++; return as_bits(as_float(a) * as_float(b)); }
uint64_t a64_fpx_div32_div32(Ctx *c, uint64_t a, uint64_t b) { c->divides++; return as_bits(as_float(a) / as_float(b)); }
/* (as the lift's fcmp: 1 when ordered, n z c v set; 0 for an unordered compare, whose flags the caller sets) */
static int a64_fcmp32(Ctx *c, const uint64_t *a, const uint64_t *b) {
    const float x = as_float(a[0]), y = as_float(b[0]);
    if (x != x || y != y) return 0;
    c->n = x < y, c->z = x == y, c->c = x >= y, c->v = 0;
    return 1;
}
/* (guest memory: the game's time object behind [0x84c9e890] (its step at +0x10), the physics' time object at 0x3000 (1/dt at +40)) */
static uint8_t mem[0x4000];
static uint8_t *guest(uint64_t a) {
    if (a == 0x84c9e890ULL) return mem + 0x100;
    return a < sizeof mem ? mem + a : mem;
}
#define GP(a) ((void *)guest(a))
#define G64(a) (*(uint64_t *)GP(a))
static uint64_t recomp_load32(Ctx *c, uint64_t a) { uint32_t v; (void)c; memcpy(&v, guest(a), 4); return v; }
typedef long long LONG64;
#define InterlockedIncrement64(p) (++*(p))
static void hos_log(const char *f, ...) { (void)f; }
static int fps_log(void) { return 1; }
volatile uint32_t hos_frame_seconds_bits;
@HOST@
@LIFTED@
static int failures, checks;
#define EXPECT(cond, ...) do { checks++; if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); failures++; } } while (0)

/* Gohma: the saved rate of one update */
static void turn(const char *what, float step, float rate, float applied, float expected, int corrected) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.vreg[0][0] = as_bits(fabsf(applied)), c.vreg[1][0] = as_bits(-step), c.vreg[8][0] = as_bits(step), c.vreg[2][0] = as_bits(rate);
    f_8263e8e0(&c);
    const int same = corrected ? fabsf(c.saved - expected) <= 1e-7f : as_bits(c.saved) == as_bits(expected);
    EXPECT(same && c.divides == (corrected ? 2 : 0) && c.multiplies == (corrected ? 0 : 2),
           "turn %s: step %.9g rate %.9g applied %.9g saved %.9g expected %.9g (div %d mul %d)", what, step, rate, applied, c.saved,
           expected, c.divides, c.multiplies);
}
static float old_saved(float step, float rate, float applied) {
    const float lower = fabsf(applied) * -step, upper = fabsf(applied) * step;
    return rate < lower ? lower : (rate > upper ? upper : rate);
}
static void turns(void) {
    const float steps[] = {0.25f, 0.42f, 0.5f, 0.66f, 1.0f, 2.0f};
    for (int k = 0; k < 6; k++)
        for (int sign = -1; sign <= 1; sign += 2) {
            const float step = steps[k], rate = sign * 0.02f;
            for (int target = 0; target < 2; target++) {   /* (a rate-limited and a target-limited turn) */
                const float applied = target ? sign * 0.002f : rate * step;
                turn("a step", step, rate, applied, step < 1 ? (target ? sign * 0.002f / step : rate) : old_saved(step, rate, applied), step < 1);
            }
            turn("no turn", step, rate, 0.0f, step < 1 ? 0.0f : old_saved(step, rate, 0.0f), step < 1);
        }
    turn("0.5, positive", 0.5f, 0.02f, -0.002f, 0.004f, 1);
    turn("0.5, negative", 0.5f, -0.02f, 0.002f, -0.004f, 1);
    const uint32_t others[] = {0, 0x80000000u, 0xbf000000u, 0xc0000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
                               0x3f800000u, 0x3f800001u, 0x40000000u};   /* (zero, negative, infinite, NaN, 1 and over: the game's) */
    for (unsigned k = 0; k < sizeof others / sizeof others[0]; k++)
        for (int sign = -1; sign <= 1; sign += 2) {
            const float step = as_float(others[k]), rate = sign * 0.02f, applied = sign * 0.002f;
            turn("a step the game keeps", step, rate, applied, old_saved(step, rate, applied), 0);
        }
    Ctx rep;   /* (each update's result carried into the next: the retained rate must not decay) */
    memset(&rep, 0, sizeof rep);
    rep.saved = -0.02f;
    for (int k = 0; k < 100; k++) {
        rep.vreg[0][0] = as_bits(fabsf(rep.saved * 0.42f)), rep.vreg[1][0] = as_bits(-0.42f), rep.vreg[8][0] = as_bits(0.42f);
        rep.vreg[2][0] = as_bits(rep.saved);
        f_8263e8e0(&rep);
    }
    EXPECT(fabsf(rep.saved + 0.02f) <= 1e-7f, "turn: the saved rate decays over repeated short steps (%.9g)", rep.saved);
    Ctx other;
    memset(&other, 0, sizeof other);
    other.vreg[0][0] = as_bits(0.01f), other.vreg[1][0] = as_bits(-0.5f), other.vreg[8][0] = as_bits(0.5f), other.vreg[2][0] = as_bits(0.02f);
    f_93000000(&other);
    EXPECT(as_bits(other.saved) == as_bits(0.005f) && !other.divides && other.multiplies == 2, "turn: an unrelated function changed");
}

/* Epona: one frame; the blend frames carried as the game does (the slow-motion branch adds the step to last frame's, 20 when none) */
static float blend_frames;
static int horse_frame(float step, float frame_step, int timed_now) {
    Ctx c;
    memset(&c, 0, sizeof c);
    hos_frame_seconds_bits = timed_now ? as_bits(1.0f / 120) : 0;
    G64(0x84c9e890ULL) = frame_step != 0.0f ? 0x1000 : 0;
    G64(0x1000) = 0x2000;
    const uint32_t f = (uint32_t)as_bits(frame_step);
    memcpy(mem + 0x2010, &f, 4);
    c.vreg[10][0] = as_bits(step);
    f_82c59eb0(&c);
    blend_frames = c.slow ? (blend_frames > 1.0f ? blend_frames : 20.0f) + step : 25.0f;
    return c.slow;
}
static void horse(const char *what, float step, float frame_step, int timed_now, int slow, float expected) {
    blend_frames = 25.0f;
    int slows = 0;
    for (int i = 0; i < 40; i++) slows += horse_frame(step, frame_step, timed_now);
    EXPECT(slows == (slow ? 40 : 0) && fabsf(blend_frames - expected) <= 1e-3f,
           "horse %s: the slow-motion branch ran %d of 40 frames, blend frames %.3f (expected %s, %.3f)", what, slows, blend_frames,
           slow ? "every frame" : "none", expected);
}
static void horses(void) {
    horse("120 fps", 0.5f, 0.5f, 1, 0, 25.0f);
    horse("70 fps", 60.0f / 70.0f, 60.0f / 70.0f, 1, 0, 25.0f);
    horse("144 fps", 60.0f / 144.0f, 60.0f / 144.0f, 1, 0, 25.0f);
    horse("240 fps", 0.25f, 0.25f, 1, 0, 25.0f);
    horse("120 fps, the horse's step a little under the frame's", 0.5f * 0.9995f, 0.5f, 1, 0, 25.0f);
    horse("120 fps, the horse's step a little over the frame's", 0.5f * 1.0005f, 0.5f, 1, 0, 25.0f);
    horse("slow motion at 120 fps", 0.25f, 0.5f, 1, 1, 25.0f + 40 * 0.25f);
    horse("slow motion at a cap of 60", 0.5f, 1.0f, 0, 1, 25.0f + 40 * 0.5f);
    horse("a cap of 60", 1.0f, 1.0f, 0, 0, 25.0f);
    horse("a cap of 60, its time object at 0.5", 0.5f, 0.5f, 0, 1, 25.0f + 40 * 0.5f);
    horse("a frame slower than 60 fps", 1.25f, 1.25f, 1, 0, 25.0f);
    horse("no time object", 0.5f, 0.0f, 1, 1, 25.0f + 40 * 0.5f);
    horse("a NaN step", NAN, 0.5f, 1, 0, 25.0f);
    Ctx c;   /* (an unrelated function keeps the game's branch) */
    memset(&c, 0, sizeof c);
    hos_frame_seconds_bits = as_bits(1.0f / 120);
    c.vreg[10][0] = as_bits(0.5f);
    f_93000001(&c);
    EXPECT(c.slow == 1, "horse: an unrelated function changed");
}

/* the barge: the carry each module chose */
typedef void (*Update)(Ctx *);
static void floors(const char *what, float frame_step, float inv_dt, int timed_now, float expected) {
    const Update updates[3] = {f_80159dc0, f_8015d490, f_838baa40};
    hos_frame_seconds_bits = timed_now ? as_bits(1.0f / 120) : 0;
    G64(0x84c9e890ULL) = frame_step != 0.0f ? 0x1000 : 0;
    G64(0x1000) = 0x2000;
    const uint32_t f = (uint32_t)as_bits(frame_step), i = (uint32_t)as_bits(inv_dt);
    memcpy(mem + 0x2010, &f, 4);
    memcpy(mem + 0x3000 + 40, &i, 4);
    for (int m = 0; m < 3; m++) {
        Ctx c;
        memset(&c, 0, sizeof c);
        c.x[8] = 0x3000;
        updates[m](&c);
        EXPECT(fabsf(c.carry - expected) <= 1e-6f, "floor %s: module %d carried %.1f of the floor's velocity (expected %.1f)", what, m,
               c.carry, expected);
    }
}
static void barge(void) {
    floors("120 fps", 0.5f, 120.0f, 1, 1.0f);
    floors("101 fps", 60.0f / 101.0f, 101.0f, 1, 1.0f);
    floors("144 fps", 60.0f / 144.0f, 144.0f, 1, 1.0f);
    floors("240 fps", 0.25f, 240.0f, 1, 1.0f);
    floors("1000 fps", 0.06f, 1000.0f, 1, 1.0f);
    floors("90 fps", 60.0f / 90.0f, 90.0f, 1, 1.0f);
    floors("120 fps, time at 0.5", 0.5f, 240.0f, 1, 0.1f);   /* (slowed time: the physics' dt slowed, the frame's step not) */
    floors("120 fps, time at 0.3", 0.5f, 400.0f, 1, 0.1f);
    floors("120 fps, time at 0.7", 0.5f, 120.0f / 0.7f, 1, 1.0f);
    floors("240 fps, time at 0.5", 0.25f, 480.0f, 1, 0.1f);
    floors("a cap of 60", 1.0f, 60.0f, 0, 1.0f);
    floors("a cap of 60, time at 0.3", 1.0f, 200.0f, 0, 0.1f);
    floors("a cap of 60, its time object at 0.5", 0.5f, 120.0f, 0, 0.1f);
    floors("a frame slower than 60 fps, time at 0.5", 1.25f, 96.0f, 1, 1.0f);
    floors("a frame slower than 60 fps, time at 0.3", 1.25f, 160.0f, 1, 0.1f);
    floors("no time object", 0.0f, 120.0f, 1, 0.1f);
    floors("a NaN 1/dt", 0.5f, NAN, 1, 1.0f);
    Ctx c;   /* (an unrelated function keeps the game's test) */
    memset(&c, 0, sizeof c);
    hos_frame_seconds_bits = as_bits(1.0f / 120);
    G64(0x84c9e890ULL) = 0x1000, G64(0x1000) = 0x2000;
    { const uint32_t f = (uint32_t)as_bits(0.5f), i = (uint32_t)as_bits(120.0f); memcpy(mem + 0x2010, &f, 4); memcpy(mem + 0x3000 + 40, &i, 4); }
    c.x[8] = 0x3000;
    f_93000002(&c);
    EXPECT(fabsf(c.carry - 0.1f) <= 1e-6f, "floor: an unrelated function changed");
}
int main(void) {
    turns();
    horses();
    barge();
    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS: gohma, epona and the barge (%d checks)\n", checks);
    return 0;
}
'''


def block(src, head):
    """the function that starts with `head` (up to its closing brace) in src"""
    at = src.index(head)
    depth, i = 0, src.index("{", at)
    start = i
    while True:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[at:i + 1]
        i += 1


def host_part():
    """the host's functions for the three hooks, cut from host/hl_high_fps.c"""
    src = (ROOT / "host" / "hl_high_fps.c").read_text(encoding="utf-8")
    heads = ("static int frame_timed(void) {", "static uint32_t frame_step_at(uint64_t slot) {",
             "int hos_oot_horse_whole_step(uint32_t step_bits, uint64_t slot) {",
             "int hos_oot_floor_carry_slow(uint32_t inv_dt_bits, uint64_t slot) {")
    return "\n".join(block(src, h) for h in heads)


def host_asks():
    """host/hl_high_fps.c asks for the five hooks, with the labels"""
    asked = hl_gates.wanted_lifts()
    check(asked.get(TURN, (None, None))[1] == "turn", "host/hl_high_fps.c does not ask for the steering hook")
    check(asked.get(HORSE, (None, None))[1] == "horse", "host/hl_high_fps.c does not ask for the horse hook")
    for addr, label in FLOORS:
        check(asked.get(addr, (None, None, None))[1:] == ("floor", (label,)), "host/hl_high_fps.c does not ask for the %s floor hook" % addr)


def hook_text():
    """The hooks in a made-up lift: once each, after their anchors, hashes unchanged, idempotent; another text, a function an exeFS mod
    patches and a tree that does not ask leave the lift as it was; an older tree's stale-gate rule takes them out."""
    lifted = LIFTED + UNRELATED
    addrs = (TURN, HORSE) + tuple(a for a, _ in FLOORS)
    hashes = {a: hl_gates.text_hash(lifted, a) for a in addrs}
    with tempfile.TemporaryDirectory(prefix="above60-hook-") as tmp:
        gen, host = Path(tmp) / "gen", Path(tmp) / "host"
        gen.mkdir()
        host.mkdir()
        code = gen / "code_000.c"
        code.write_text(lifted, encoding="utf-8", newline="")
        asks = ('HL_TURN_HOOK(%s, "%s");\n' % (TURN, hashes[TURN]) + 'HL_HORSE_HOOK(%s, "%s");\n' % (HORSE, hashes[HORSE]) +
                "".join('HL_FLOOR_HOOK(%s, "%s", "%s");\n' % (a, hashes[a], l) for a, l in FLOORS))
        (host / "hl_f.c").write_text(asks, encoding="utf-8")
        hl_gates.apply(str(gen), str(host), quiet=True)
        s = code.read_text(encoding="utf-8")
        turn = hl_gates.lift_lines("turn", TURN, ())
        check(s.count("L_hl_8263e8e1_a:;") == 1 and s.count("L_hl_8263e8e1_b:;") == 1 and s.count("a64_fpx_div32_div32") == 2,
              "the steering hook is not once")
        check(s.count(turn[0] + hl_gates.TURN_MULS + turn[1] + turn[2] + hl_gates.TURN_MULS2 + turn[3]) == 1,
              "the steering hook's lines are not before and after each multiply, in order")
        horse = hl_gates.lift_lines("horse", HORSE, ())[0]
        check(s.count(horse) == 1 and s.count(horse + hl_gates.HORSE_BRANCH) == 1, "the horse hook is not right before its branch")
        for addr, label in FLOORS:
            before, after = hl_gates.lift_lines("floor", addr, (label,))
            check(s.count(before + hl_gates.FLOOR_BRANCH % label + after) == 1, "the %s floor hook is not around its branch" % addr)
        check(s.count("hos_oot_floor_carry_slow(") == 6 and "hl gate 9300" not in s, "the unrelated functions were hooked")
        check(all(hl_gates.text_hash(s, a) == h for a, h in hashes.items()), "a hook changed its function's hash")
        old = re.compile(r"^.*/\* hl gate ([0-9a-f]{8}) \*/\n", re.M).sub("", s)
        check(old == lifted, "an older tree's gates script does not restore the lift")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == s, "a second run changed a hooked file")
        (gen / "hl_gates.state").unlink()
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == s, "a hook of an older form is not made again")
        hl_gates.apply(str(gen), str(host), quiet=True, skip={HORSE: "some_mod"})
        check("hos_oot_horse_whole_step" not in code.read_text(encoding="utf-8"), "a patched function was hooked")
        for what, other, marker in (
                ("another anchor (steering)", lifted.replace("c->vreg[8],4);_z", "c->vreg[9],4);_z", 1), "a64_fpx_div32_div32"),
                ("another label (horse)", lifted.replace("goto L_82c5ae48; }\n    c->slow = 1", "goto L_82c5ae50; }\n    c->slow = 1", 1),
                 "hos_oot_horse_whole_step"),
                ("another threshold (barge)", lifted.replace("0x42c80000ULL", "0x42c60000ULL"), "hos_oot_floor_carry_slow")):
            code.write_text(other, encoding="utf-8", newline="")
            hl_gates.apply(str(gen), str(host), quiet=True)
            check(marker not in code.read_text(encoding="utf-8"), "a function with %s was hooked" % what)
        code.write_text(lifted, encoding="utf-8", newline="")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check("hos_oot_horse_whole_step" in code.read_text(encoding="utf-8"), "hooked again")
        (host / "hl_f.c").unlink()
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == lifted, "none asked: the hooks stay")
    return s


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    host_asks()
    hooked = hook_text()
    fixture = FIXTURE.replace("@HOST@", host_part())
    with tempfile.TemporaryDirectory(prefix="above60-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        cmd = [a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-const-variable", "-I",
               str(ROOT / "host"), str(c), "-o", str(exe), "-lm"]
        c.write_text(fixture.replace("@LIFTED@", hooked), encoding="utf-8")
        subprocess.run(cmd, check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end="")
        check(r.returncode == 0, "the hooked functions fail their checks")
        # (the negative control: the game's own functions fail the same checks above 60 fps)
        c.write_text(fixture.replace("@LIFTED@", LIFTED + UNRELATED), encoding="utf-8")
        subprocess.run(cmd, check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode != 0 and "turn" in r.stdout and "horse" in r.stdout and "floor" in r.stdout,
              "the unhooked functions pass the checks (or fail only some): they prove nothing\n" + r.stdout)
    print("above 60 gameplay hooks: ok")


if __name__ == "__main__":
    main()
