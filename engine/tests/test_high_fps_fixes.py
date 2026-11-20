"""Above a 60 fps cap the sound job runs at 60 Hz and the physics solver keeps whole-frame steps (after upstream Hookshot
8fcc8c22b and 9df055f23: host/oot_high_fps.h, host/hl_high_fps.c; the hooks put in by nx2/hl_gates.py).

Sound: under a Frame rate cap above 60 the frame step is the frame's own time (0.5 at 120 fps). The sound manager's job
(f_80435300) fed steps under 1 left sounds unreleased at area changes until the game's voice budget was full and sounds, then
the music, stopped. The gate runs phase 0 once a whole step has built up (120 frames of 0.5: 60 runs; 144 of 60/144: 60), the
other phases follow phase 0's choice, the engine is handed a whole step then, and a step of 1 or more, 0 or below, NaN, or no
frame time at all (a cap of 60) runs every frame with the game's own step. A step of 1 or more keeps the remainder, so around
60 fps (0.98 and 1.02 alternating, 0.9 and 1.1, or mean 1 with jitter) the sound clock keeps the real one; a cap of 60 runs
every phase, before the first phase 0 too and after a phase 0 that held the job.

Solver: the game cuts frame step * n + 0.001 to its solver steps (n a 60 fps frame: 8 at 60 fps, 3 or 4 at 120 as the step
wanders; with n = 4, 0 above 240). With the frames' own time and a step under 1 it is rounded up (10% leeway for the frame
time's jitter) and at least 1, never under the game's; a step of 1 or more, or no frame time (a cap of 60), keeps the game's.

Music manager (after Longshot eb5ba6e2, Hookshot bf802502 by CraftyCorvid; HL_BGM_HOOK): its update (f_83373400, game::Bgm) runs inside
the gated job but reads the step (+16) and the seconds (+24) from the game's time object itself: its clock runs at 60 / fps of the
real one unless a whole step and that step's seconds are handed to it (120 and 240 fps; 60 fps, a slow frame and a cap of 60 read the
game's own). The battle manager sets a byte of change flags ([[0x84c9dda0]]+0x4898: bit 5 the battle music's name, bit 1 its distance
volume, 2, 6) for one game frame, and the update syncs a property for each bit set: held to 60 Hz it saw a frame's flags only on the
frames it ran, so half the fight ends were lost at 120 fps (the battle music played on). The hooked update, with the host's code, handles a
bit set for one frame exactly once, whether the gate ran the job on that frame or held it (72 cases: 3 rates, 8 offsets, 3 bits).

The hooks: host/hl_high_fps.c asks nx2/hl_gates.py for them (HL_SOUND_HOOK, HL_SOLVER_HOOK, HL_BGM_HOOK). The sound job's gate goes at its
head and its step after the job's own load of it, the solver's count after its cut; each function's hash is unchanged, a
second run changes nothing, another text, a function an exeFS mod patches and a tree that does not ask leave the lift as it
was. The hooked made-up functions, compiled with the host's code, run the job at 60 Hz and round the count up at 120 fps, as
the game at a cap of 60, and as the game with HOS_FPS_SOUND=0 / HOS_FPS_SOLVER=0; without the hooks the same checks fail.

python -I -B tests/test_high_fps_fixes.py [--cc gcc]
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

SOUND, SOLVERS, BGM = "80435300", ("838ceb80", "838cfc10"), "83373400"
HEAD = hl_gates.HEAD
# (the job: phase 0 alone loads the step; what it hands the engine is kept, with how many times it ran. The next function
#  has the same load, and stays as it is)
LIFTED_SOUND = ('#include "a64.h"\n' + HEAD % SOUND + "    /* nop/hint */\n"
                "if ((uint32_t)c->x[2] != 0) { goto L_1; }\n" + hl_gates.SOUND_LOAD +
                "    fed += oot_bits_float((uint32_t)c->vreg[0][0]);\n"
                "L_1:;\n    ran[(uint32_t)c->x[2]]++;\n    return;\n}\n" +
                HEAD % "80435360" + hl_gates.SOUND_LOAD + "}\n")
SOLVER_CUT = hl_gates.CUT % 1 + hl_gates.SOLVER_STORE
LIFTED_SOLVER = '#include "a64.h"\n' + "".join(HEAD % a + "    c->x[8]=1;\n" + SOLVER_CUT + "}\n" for a in SOLVERS)
# (the music manager's update as the lift writes it: the time object in x8, its step into s8 and seconds into s9, then the four reads
#  of the change flags, each followed by a handler call the stand-in counts by bit; the next function has the same loads and stays)
LIFTED_BGM = (HEAD % BGM + "    c->x[8]=BASE + 0x2000;\n" + hl_gates.BGM_TIME + "    c->x[26]=BASE + 0x1000;\n" +
              "".join(hl_gates.BGM_FLAGS + "    if ((c->x[8] >> %d) & 1) { handled[%d]++; }\n" % (b, b) for b in (5, 1, 2, 6)) +
              "    clock_steps += oot_bits_float((uint32_t)c->vreg[8][0]);\n    clock_secs += oot_bits_float((uint32_t)c->vreg[9][0]);\n}\n" +
              HEAD % "83373460" + hl_gates.BGM_TIME + "}\n")
HOST_ANCHOR = "extern volatile uint32_t hos_frame_seconds_bits;"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


LOGIC = r"""
static uint32_t bits(float f) { return oot_float_bits(f); }
static int runs(float step, int frames, int timed) {
    OotSoundGate g = OOT_SOUND_GATE_INIT;
    int n = 0;
    for (int i = 0; i < frames; i++) {
        const int run = oot_sound_gate(&g, 0, step, timed);
        CHECK(oot_sound_gate(&g, 1, step, timed) == run);   /* (phase 1 follows phase 0) */
        n += run;
    }
    return n;
}
/* (the sound engine's time over these frames' steps against the real time: what it is handed each frame the job runs) */
static double sound_clock(const float *steps, int n) {
    OotSoundGate g = OOT_SOUND_GATE_INIT;
    double real = 0, sound = 0;
    for (int i = 0; i < n; i++) {
        real += steps[i];
        if (oot_sound_gate(&g, 0, steps[i], 1)) sound += oot_bits_float(oot_sound_step(oot_float_bits(steps[i]), 1));
    }
    return sound / real;
}
static void logic(void) {
    CHECK(runs(0.5f, 120, 1) == 60);
    const int n144 = runs(60.0f / 144.0f, 144, 1);
    CHECK(n144 == 60 || n144 == 59);
    CHECK(runs(60.0f / 1000.0f, 1000, 1) >= 59 && runs(60.0f / 1000.0f, 1000, 1) <= 60);
    CHECK(runs(0.5f, 120, 0) == 120);   /* (a cap of 60: every frame) */
    CHECK(runs(1.0f, 60, 1) == 60 && runs(1.5f, 60, 1) == 60);
    CHECK(runs(0.0f, 60, 1) == 60 && runs(-1.0f, 60, 1) == 60 && runs(NAN, 60, 1) == 60);
    {   /* (a whole step after partial ones runs with the game's step and keeps the remainder) */
        OotSoundGate g = OOT_SOUND_GATE_INIT;
        CHECK(oot_sound_gate(&g, 0, 0.75f, 1) == 0 && oot_sound_gate(&g, 2, 0.75f, 1) == 0);
        CHECK(oot_sound_gate(&g, 0, 1.0f, 1) == 1 && g.part == 0.75f && oot_sound_gate(&g, 1, 1.0f, 1) == 1);
        CHECK(oot_sound_gate(&g, 0, 0.75f, 1) == 1 && g.part == 0.5f);
        CHECK(oot_sound_gate(&g, 0, 0.0f, 1) == 1 && g.part == 0.0f);   /* (a step of 0: as the game has it, the remainder cleared) */
    }
    {   /* (around 60 fps the sound clock keeps the real one: dropping the remainder at each whole step made it 0.51 of it) */
        static float steps[20000];
        for (int i = 0; i < 20000; i++) steps[i] = i & 1 ? 1.02f : 0.98f;
        double r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
        for (int i = 0; i < 20000; i++) steps[i] = i & 1 ? 1.1f : 0.9f;
        r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
        uint32_t x = 12345;   /* (mean 1, about 0.03 apart: a sum of uniforms) */
        for (int i = 0; i < 20000; i++) {
            float u = 0;
            for (int k = 0; k < 12; k++) x = x * 1664525u + 1013904223u, u += (float)(x >> 8) / 16777216.0f;
            steps[i] = 1.0f + (u - 6.0f) * 0.03f;
        }
        r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
        for (int i = 0; i < 20000; i++) steps[i] = 0.5f;
        r = sound_clock(steps, 20000);
        CHECK(r > 0.999 && r < 1.001);
    }
    {   /* (a cap of 60 runs every phase: before any phase 0, and after a phase 0 that held the job at a cap above 60) */
        OotSoundGate g = OOT_SOUND_GATE_INIT;
        CHECK(oot_sound_gate(&g, 1, 1.0f, 0) == 1 && oot_sound_gate(&g, 2, 1.0f, 0) == 1);
        OotSoundGate h = OOT_SOUND_GATE_INIT;
        CHECK(oot_sound_gate(&h, 1, 1.0f, 1) == 1);   /* (above 60, before the first phase 0: it runs) */
        CHECK(oot_sound_gate(&h, 0, 0.5f, 1) == 0 && oot_sound_gate(&h, 1, 0.5f, 0) == 1 && h.part == 0.0f);   /* (the cap changed) */
        CHECK(oot_sound_gate(&h, 2, 0.5f, 0) == 1 && oot_sound_gate(&h, 0, 0.5f, 0) == 1);
    }
    CHECK(oot_sound_step(bits(0.5f), 1) == bits(1.0f));
    CHECK(oot_sound_step(bits(0.5f), 0) == bits(0.5f));
    CHECK(oot_sound_step(bits(1.25f), 1) == bits(1.25f) && oot_sound_step(bits(0.0f), 1) == bits(0.0f));
    /* (the game's: (int)(step * n + 0.001), n the steps of a 60 fps frame) */
    #define GAME(step, n) ((int32_t)((step) * (n) + 0.001f))
    #define OURS(step, n, timed) oot_solver_steps(GAME(step, n), bits((step) * (n) + 0.001f), bits(n), timed)
    CHECK(OURS(1.0f, 4.0f, 1) == 4 && OURS(1.0f, 8.0f, 1) == 8 && OURS(1.0f, 8.0f, 0) == 8);
    CHECK(OURS(0.5f, 4.0f, 1) == 2 && OURS(0.49f, 4.0f, 1) == 2 && OURS(0.52f, 4.0f, 1) == 2 && GAME(0.49f, 4.0f) == 1);
    CHECK(OURS(0.49f, 8.0f, 1) == 4 && OURS(0.5f, 8.0f, 1) == 4 && GAME(0.49f, 8.0f) == 3);
    CHECK(OURS(0.25f, 4.0f, 1) == 1 && OURS(0.24f, 4.0f, 1) == 1 && OURS(0.06f, 4.0f, 1) == 1 && GAME(0.24f, 4.0f) == 0);
    CHECK(OURS(60.0f / 144.0f, 4.0f, 1) == 2 && OURS(60.0f / 90.0f, 4.0f, 1) == 3);
    CHECK(OURS(0.49f, 4.0f, 0) == 1 && OURS(0.06f, 4.0f, 0) == 0);   /* (a cap of 60: the game's count) */
    CHECK(OURS(1.6f, 4.0f, 1) == 6 && OURS(1.708f, 8.0f, 1) == 13);   /* (a slow frame: the game's count) */
    CHECK(oot_solver_steps(3, bits(NAN), bits(4.0f), 1) == 3 && oot_solver_steps(0, bits(-1.0f), bits(4.0f), 1) == 0);
    CHECK(oot_solver_steps(1, bits(2.0f), bits(0.0f), 1) == 1);   /* (no steps a frame: the game's) */
}
"""

FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oot_high_fps.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* (the host's surroundings: a small guest, the lock as a flag taken once and given back once) */
#define A64_NOESC
typedef struct { uint64_t x[32], vreg[32][2]; uint32_t fpcr, fpsr; } Ctx;
#define BASE 0x10000ULL
static uint8_t guest[0x4000] __attribute__((aligned(16)));
#define GP(a) ((void *)(guest + ((uint64_t)(a) - BASE)))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define HOS_HEAP_BASE 0x1000ULL   /* (hos_oot_note_link's walk: this fixture never calls it) */
static void *hos_guest_range(uint64_t a, uint64_t n) { (void)n; return GP(a); }
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static void AcquireSRWLockExclusive(SRWLOCK *l) { CHECK(!*l); *l = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { CHECK(*l); *l = 0; }
typedef int64_t LONG64;
#define InterlockedIncrement64(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
static void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
volatile uint32_t hos_frame_seconds_bits;
@HOST@
@LOGIC@
/* (the lifted code's helpers, as the lift has them for these lines) */
static const uint64_t g_module_base = BASE - 0x80435000ULL - 75927552ULL;   /* (the job's slot at guest + 0) */
static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; return G64(a); }
static uint64_t recomp_load32(Ctx *c, uint64_t a) { (void)c; return G32(a); }
static uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return *(uint8_t *)GP(a); }
static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { (void)c; G32(a) = (uint32_t)v; }
static void a64_vset64(uint64_t *v, uint64_t x) { v[0] = x, v[1] = 0; }
static uint64_t recomp_fp_to_int(uint64_t b, int iw, int ow, int s, int mode, uint32_t fpcr, uint32_t *fpsr) {
    (void)iw, (void)ow, (void)s, (void)mode, (void)fpcr, (void)fpsr;
    const float f = oot_bits_float((uint32_t)b);
    return (uint32_t)(f != f ? 0 : (int32_t)f);
}
static double fed, clock_steps, clock_secs;
static int ran[3], handled[8];
@LIFTED@
static void seconds(double s) { hos_frame_seconds_bits = s > 0 ? oot_float_bits((float)s) : 0; }
/* (a frame of the job: phases 0, 1 and 2, the game step at 60 x the frame's seconds; the job's slot at guest + 2192 points to
   an object at + 0x1000 whose first word points to the time object at + 0x2000, its step at + 0x10) */
static void frames(int n, float step) {
    G64(BASE + 2192) = BASE + 0x1000, G64(BASE + 0x1000) = BASE + 0x2000, G32(BASE + 0x2010) = oot_float_bits(step);
    Ctx ctx;
    for (int i = 0; i < n; i++)
        for (uint64_t phase = 0; phase < 3; phase++) {
            memset(&ctx, 0, sizeof ctx);
            ctx.x[2] = phase;
            f_80435300(&ctx);
        }
}
/* the music manager: a frame of step (60 / fps; no frame time given at a cap of 60) with the flag byte, the job's phases as the game
   calls them (the update on phase 1 when the gate runs it); the flag byte is cleared after the frame, as the battle manager does */
static Ctx bctx;
static void bgm_frame(float step, int timed, uint8_t flags) {
    const float secs = step / 60.0f;
    hos_frame_seconds_bits = timed ? oot_float_bits(secs) : 0;
    G64(BASE + 2192) = BASE + 0x1000, G64(BASE + 0x1000) = BASE + 0x2000;
    G32(BASE + 0x2010) = oot_float_bits(step), G32(BASE + 0x2018) = oot_float_bits(secs);
    *(uint8_t *)GP(BASE + 0x1000 + 2200) = flags;
    for (uint64_t phase = 0; phase < 3; phase++)
        if (hos_oot_sound_gate(&bctx, (uint32_t)phase, BASE + 2192) && phase == 1) f_83373400(&bctx);
    *(uint8_t *)GP(BASE + 0x1000 + 2200) = 0;
}
static void bgm_checks(void) {
    const float steps[] = {0.5f, 0.25f, 1.0f, 1.5f};
    const int timed[] = {1, 1, 0, 0};
    int lost = 0;
    for (int i = 0; i < 4; i++) {   /* (the update's clock against the real one; a step of 0.5 at a cap of 60 runs every frame with it) */
        clock_steps = clock_secs = 0;
        for (int f = 0; f < 2400; f++) bgm_frame(steps[i], timed[i], 0);
        CHECK(fabs(clock_steps / (steps[i] * 2400) - 1.0) < 0.01 && fabs(clock_secs / (steps[i] / 60.0f * 2400) - 1.0) < 0.01);
    }
    const int bits[] = {5, 1, 6};
    for (int i = 0; i < 3; i++)
        for (int w = 0; w < 8; w++)
            for (int k = 0; k < 3; k++) {
                const float step = i == 2 ? 1.0f : i == 1 ? 0.25f : 0.5f;
                const int t = i != 2;
                for (int f = 0; f < 4; f++) bgm_frame(step, t, 0);
                memset(handled, 0, sizeof handled);
                for (int f = 0; f < w; f++) bgm_frame(step, t, 0);
                bgm_frame(step, t, (uint8_t)(1u << bits[k]));
                for (int f = 0; f < 8; f++) bgm_frame(step, t, 0);
                lost += handled[bits[k]] != 1;
            }
    CHECK(lost == 0);
}
static int32_t solver(void (*f)(Ctx *), float step, float n) {
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.x[0] = BASE + 0x3000;
    ctx.vreg[0][0] = oot_float_bits(step * n + 0.001f), ctx.vreg[1][0] = oot_float_bits(n);
    f(&ctx);
    return (int32_t)G32(BASE + 0x3000 + 712);
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "on";
    if (!strcmp(mode, "bgm")) {
        bgm_checks();
        printf("PASS: %s\n", mode);
        return 0;
    }
    const int sound_on = !strcmp(mode, "on") || !strcmp(mode, "solver-off");
    const int solver_on = !strcmp(mode, "on") || !strcmp(mode, "sound-off");
    logic();
    if (sound_on) bgm_checks();
    /* a cap of 60: every phase of every frame with the game's step, the game's solver count */
    seconds(0);
    frames(60, 1.0f);
    CHECK(ran[0] == 60 && ran[1] == 60 && ran[2] == 60 && fed == 60.0);
    CHECK(solver(f_838ceb80, 0.49f, 8.0f) == 3 && solver(f_838cfc10, 0.49f, 8.0f) == 3);
    /* a cap of 120: the job at 60 Hz, handed whole steps (as the game: every frame, 0.5 each), all its phases together */
    seconds(1.0 / 120);
    memset(ran, 0, sizeof ran), fed = 0;
    frames(120, 0.5f);
    if (sound_on) CHECK(ran[0] == 60 && ran[1] == 60 && ran[2] == 60 && fed == 60.0);
    else CHECK(ran[0] == 120 && ran[1] == 120 && ran[2] == 120 && fed == 60.0);
    /* the solver count rounded up at a step under 1 (as the game: cut) */
    if (solver_on) CHECK(solver(f_838ceb80, 0.49f, 8.0f) == 4 && solver(f_838cfc10, 0.49f, 8.0f) == 4 &&
                         solver(f_838ceb80, 0.24f, 4.0f) == 1);
    else CHECK(solver(f_838ceb80, 0.49f, 8.0f) == 3 && solver(f_838cfc10, 0.49f, 8.0f) == 3);
    CHECK(solver(f_838ceb80, 1.6f, 4.0f) == 6);   /* (a slow frame: the game's) */
    /* back at a cap of 60 */
    seconds(0);
    memset(ran, 0, sizeof ran), fed = 0;
    frames(10, 1.0f);
    CHECK(ran[0] == 10 && ran[1] == 10 && ran[2] == 10 && fed == 10.0);
    printf("PASS: %s\n", mode);
    return 0;
}
'''


def host_part():
    """host/hl_high_fps.c from the frame's seconds on: the functions the hooks call, their switches and logs"""
    src = (ROOT / "host" / "hl_high_fps.c").read_text(encoding="utf-8")
    check(HOST_ANCHOR in src, "host/hl_high_fps.c has no '%s'" % HOST_ANCHOR)
    return src[src.index(HOST_ANCHOR):]


def host_asks():
    """{address: hash} host/hl_high_fps.c asks for"""
    asked = hl_gates.wanted_lifts()
    check(asked.get(SOUND, (None, None))[1] == "sound", "host/hl_high_fps.c does not ask for the sound hook")
    check(all(asked.get(a, (None, None))[1] == "solver" for a in SOLVERS), "host/hl_high_fps.c does not ask for both solver hooks")
    check(asked.get(BGM, (None, None))[1] == "bgm", "host/hl_high_fps.c does not ask for the music manager hook")
    return asked


def hook_text():
    """The hooks in a made-up lift: put in after their anchors, once, with the hashes unchanged; idempotent; left out for
    another text, a function an exeFS mod patches, or a tree that does not ask; an older tree's stale-gate rule takes them out."""
    lifted = LIFTED_SOUND + LIFTED_SOLVER.replace('#include "a64.h"\n', "") + LIFTED_BGM
    hashes = {a: hl_gates.text_hash(lifted, a) for a in (SOUND, BGM) + SOLVERS}
    gate, step = hl_gates.lift_lines("sound", SOUND, ())
    with tempfile.TemporaryDirectory(prefix="high-fps-hook-") as tmp:
        gen, host = Path(tmp) / "gen", Path(tmp) / "host"
        gen.mkdir()
        host.mkdir()
        code = gen / "code_000.c"
        code.write_text(lifted, encoding="utf-8", newline="")
        asks = 'HL_SOUND_HOOK(%s, "%s");\n' % (SOUND, hashes[SOUND]) + \
            "".join('HL_SOLVER_HOOK(%s, "%s");\n' % (a, hashes[a]) for a in SOLVERS) + 'HL_BGM_HOOK(%s, "%s");\n' % (BGM, hashes[BGM])
        (host / "hl_f.c").write_text(asks, encoding="utf-8")
        hl_gates.apply(str(gen), str(host), quiet=True)
        s = code.read_text(encoding="utf-8")
        head = s.index(HEAD % SOUND) + len(HEAD % SOUND)
        check(s[head:].startswith(gate), "the sound gate is not at the job's head")
        check(s.count(gate) == 1 and s.count(step) == 1, "the sound hook not once")
        check(s.index(step) == s.index(hl_gates.SOUND_LOAD) + len(hl_gates.SOUND_LOAD), "the step not right after the job's load")
        check(s.count(hl_gates.SOUND_LOAD + step) == 1, "the next function's load hooked too")
        check(s.count("hos_oot_solver_steps((uint32_t)c->x[1]") == 2 and "/* hl gate 838ceb81 */" in s and
              "/* hl gate 838cfc11 */" in s, "the solver hooks not once each")
        check(s.count(hl_gates.CUT % 1 + hl_gates.lift_lines("solver", SOLVERS[0], ())[0] + hl_gates.SOLVER_STORE) == 1,
              "the solver hook is not between the cut and the store")
        bgm = hl_gates.lift_lines("bgm", BGM, ())
        body = s[s.index(HEAD % BGM):s.index(HEAD % "83373460")]
        check(body.startswith(HEAD % BGM + bgm[0]) and body.count(bgm[1]) == 1 and body.count(bgm[2]) == 4, "the music manager hooks are not once each")
        check(body.count(hl_gates.BGM_TIME + bgm[1]) == 1 and body.count(hl_gates.BGM_FLAGS + bgm[2]) == 4,
              "the music manager hooks are not right after their loads")
        check(bgm[1] not in s[s.index(HEAD % "83373460"):], "the next function's loads hooked too")
        check(all(hl_gates.text_hash(s, a) == h for a, h in hashes.items()), "a hook changed its function's hash")
        old = re.compile(r"^.*/\* hl gate ([0-9a-f]{8}) \*/\n", re.M).sub("", s)
        check(old == lifted, "an older tree's gates script does not restore the lift")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == s, "a second run changed a hooked file")
        # (a line of an older form is made again, not doubled)
        code.write_text(s.replace(step, step.replace("hos_oot_sound_step(", "hos_oot_sound_step_old(")), encoding="utf-8",
                        newline="")
        (gen / "hl_gates.state").unlink()
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == s, "a hook of an older form is not made again")
        # (an exeFS mod patches the sound job: its hook out, the solver's stay)
        hl_gates.apply(str(gen), str(host), quiet=True, skip={SOUND: "some_mod"})
        t = code.read_text(encoding="utf-8")
        check(gate not in t and step not in t and t.count("hos_oot_solver_steps((uint32_t)") == 2 and "hos_oot_bgm" in t,
              "a patched function was hooked")
        hl_gates.apply(str(gen), str(host), quiet=True, skip={BGM: "some_mod"})
        check("hos_oot_bgm" not in code.read_text(encoding="utf-8"), "a patched music manager was hooked")
        # (another game version: the functions' text differs, they are left alone)
        for what, other, line in (("another slot", lifted.replace("+ 2192);", "+ 2200);", 1), "hos_oot_sound_gate("),
                                  ("another field", lifted.replace("+ 712,", "+ 716,"), "hos_oot_solver_steps("),
                                  ("another flag byte", lifted.replace("c->x[26] + 2200", "c->x[26] + 2208", 1), "hos_oot_bgm"),
                                  ("another time field", lifted.replace("(c->x[8]+24ULL)", "(c->x[8]+28ULL)", 1), "hos_oot_bgm")):
            code.write_text(other, encoding="utf-8", newline="")
            hl_gates.apply(str(gen), str(host), quiet=True)
            check(line not in code.read_text(encoding="utf-8"), "a function with %s was hooked" % what)
        # (the recorded hashes, then nobody asks any more: the lift as it was)
        code.write_text(lifted, encoding="utf-8", newline="")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(gate in code.read_text(encoding="utf-8"), "hooked again")
        os.remove(host / "hl_f.c")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == lifted, "none asked: the hooks stay")
    return s


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    host_asks()
    hooked = hook_text().replace('#include "a64.h"\n', "")
    plain = (LIFTED_SOUND + LIFTED_SOLVER).replace('#include "a64.h"\n', "") + LIFTED_BGM
    fixture = FIXTURE.replace("@HOST@", host_part()).replace("@LOGIC@", LOGIC)
    with tempfile.TemporaryDirectory(prefix="high-fps-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        cmd = [a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(ROOT / "host"), str(c),
               "-o", str(exe), "-lm"]
        c.write_text(fixture.replace("@LIFTED@", hooked), encoding="utf-8")
        subprocess.run(cmd, check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}
        subprocess.run([str(exe)], env=env, check=True)
        subprocess.run([str(exe)], env=dict(env, HOS_FPS_SOUND="1", HOS_FPS_SOLVER="1"), check=True)   # (read by value: 1 is on)
        subprocess.run([str(exe), "sound-off"], env=dict(env, HOS_FPS_SOUND="0"), check=True)
        subprocess.run([str(exe), "solver-off"], env=dict(env, HOS_FPS_SOLVER="0"), check=True)
        subprocess.run([str(exe), "bgm"], env=env, check=True)
        r = subprocess.run([str(exe), "sound-off"], env=env, capture_output=True, text=True)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "the sound job is the game's own without HOS_FPS_SOUND=0")
        # (the negative control: the game's own job and count fail the same checks at 120 fps)
        c.write_text(fixture.replace("@LIFTED@", plain), encoding="utf-8")
        subprocess.run(cmd, check=True)
        r = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "the unhooked job passes the checks: they prove nothing")
        r = subprocess.run([str(exe), "sound-off"], env=env, capture_output=True, text=True)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "the unhooked solver count passes the checks: they prove nothing")
        r = subprocess.run([str(exe), "bgm"], env=env, capture_output=True, text=True)
        check(r.returncode != 0 and "FAIL:" in r.stderr, "the unhooked music manager passes the checks: they prove nothing")
    print("high fps fixes: ok")


if __name__ == "__main__":
    main()
