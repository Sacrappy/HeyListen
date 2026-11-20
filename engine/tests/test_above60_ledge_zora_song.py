"""Four more places that break above a 60 fps cap, as lift hooks (after Longshot d30b9479, 6aa5d653, 01d0263d and 10010630; their
tests, test_drive_target.py, test_loop_step.py, test_water_substeps.py and test_song_note_cue.py, are the model of the checks here).

Upstream edits the lifted text in nx2/oot_patches.py; here the hooks are lines nx2/hl_gates.py puts in after anchors (HL_DRIVE_HOOK,
HL_LOOP_HOOK, HL_POSE_HOOK, HL_SONG_HOOK in host/hl_high_fps.c and host/hl_step.c, the water's "t8" mode of HL_WHOLE_HOOK), the
functions' hashes unchanged:
- Link's pull-up onto a ledge (f_804f5030, a rigid body's update before the physics step): above 60 fps Havok delivers part of a driven
  body's move and the shortfall was never made up; Link's body's target carries last update's shortfall (host/oot_high_fps.h).
- The frozen Zoras (f_802cfca0, the clock of an animation entry; f_80944aa0, the time a skeleton is drawn at): a loop of one frame or
  less takes whole frames of the step and is drawn at its start (host/loop_step.h).
- The water's ripples (f_800672e0): the whole substeps are handed out, not all of a frame's or none.
- The ocarina's notes (f_832b2af0, f_832b2dc0): a set note flag read again before the sound job ran again is the same note.

Authored lifted functions of the real shapes (the anchors are the real text) go through the real hl_gates; the host's code is compiled
as it is in the tree (host/hl_high_fps.c whole, the functions of host/hl_step.c cut out of it) with stand-ins for guest memory, the
lock and the log. Checked: each fix above 60 fps, the game's own result at a cap of 60 and at a step of 1 or more, each off switch,
the unhooked functions failing the same checks (or the switch off), a second run changing nothing, another text and a function an exeFS
mod patches left alone.

python -I -B tests/test_above60_ledge_zora_song.py [--cc gcc]
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
sys.path.insert(0, str(ROOT / "tests"))   # (python -I leaves the check's own folder off the path)
import hl_gates  # noqa: E402
from srccut import block  # noqa: E402

HEAD = hl_gates.HEAD
DRIVE, LOOP, POSE, WATER = "804f5030", "802cfca0", "80944aa0", "800672e0"
SONGS = ("832b2af0", "832b2dc0")


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def hooked(text, addr, kind, args=()):
    out = hl_gates.apply_lift(text, addr, kind, args)
    check(out is not None, "the %s hook's anchors are not found in the authored f_%s" % (kind, addr))
    return out


# ---- the authored functions (the real lines at the anchors, the rest made up)
READ = hl_gates.DRIVE_READ
KEEP = "    c->read[0] = (uint32_t)c->vreg[2][0]; c->read[1] = (uint32_t)c->vreg[1][0]; c->read[2] = (uint32_t)c->vreg[0][0];\n"
UPDATE = (HEAD % DRIVE +
          "    c->x[8]=recomp_load64(c,c->x[20] + 256);\n"
          "if (!((c->x[8] >> 7) & 1)) { goto L_804f5d10; }\n"
          "if (c->second) { goto L_804f5e9c; }\n" + READ % 23 + KEEP +
          "{ goto L_804f5d10; }\n"
          "L_804f5e9c:;\n" + READ % 24 + KEEP +
          "L_804f5d10:;\n"
          "}\n")
UNRELATED_UPDATE = UPDATE.replace("void f_804f5030(", "void f_904f5030(")

SITE_1 = (hl_gates.LOOP_SITES[0][0] + hl_gates.LOOP_MUL % 1 +
          "    _v=a64_fpx_mul32_fma32(c,_a,_b,_z);\n"
          "    a64_vset64(c->vreg[2],_v);}\n")
SITE_2 = (hl_gates.LOOP_SITES[1][0] + hl_gates.LOOP_MUL % 0 +
          "    _v=a64_fpx_hot_mul32_fma32(c,_a,_b,_z);\n"   # (a hot function's multiply: the lift writes either)
          "    a64_vset64(c->vreg[2],_v);}\n")
CLOCK = (HEAD % LOOP +
         "    c->x[9]=recomp_load32(c,c->x[20] + 0);\n"
         "    if (c->x[10] == 1) { goto L_802d077c; }\n" + SITE_1 +
         "goto L_advance;\n"
         "L_802d077c:;\n"
         "    c->x[9] = 0;\n" + SITE_2 +
         "goto L_advance;\n"
         "L_802d07f4:;\n"
         "    a64_vset64(c->vreg[2],0);\n"
         "L_advance:;\n"
         "    clock_advance(c);\n"
         "}\n")
POSE_TEXT = HEAD % POSE + hl_gates.POSE_STORE + "}\n"
UNRELATED_LOOP = (HEAD % "90000000").replace("90000000", "90000000") + "    c->x[9]=recomp_load32(c,c->x[20] + 0);\n" + SITE_1 + "L_802d077c:;\n}\n"

PLAYED = (HEAD % SONGS[0] + "    { uint64_t _a=c->x[8], _b=32ULL; uint64_t _r=_a+_b; c->x[0]=_r; }\n" + hl_gates.SONG_CALLS[SONGS[0]]
          + "if (!((c->x[0] >> 0) & 1)) { goto L_832b2c94; }\n"
          + "    next_note(0);   /* (the game's: the next note when one was waiting) */\n"
          + "L_832b2c94:;\n}\n")
OTHER = (HEAD % SONGS[1] + "    { uint64_t _a=c->x[8], _b=32ULL; uint64_t _r=_a+_b; c->x[0]=_r; }\n" + hl_gates.SONG_CALLS[SONGS[1]]
         + "if (!((c->x[0] >> 0) & 1)) { goto L_832b3044; }\n"
         + "    next_note(1);\n"
         + "L_832b3044:;\n}\n")
UNRELATED_SONG = PLAYED.replace("f_832b2af0", "f_932b2af0").replace("next_note(0)", "unrelated++")

WATER_LOAD = hl_gates.WHOLE_TIMES_LOAD % 8
WATER_CUT = hl_gates.CUT % 9
WATER_REST = ("    c->x[27]=g_module_base + 0x80067000ULL + (int64_t)79933440;\n"
              "    { uint64_t _r = (uint64_t)0 + (c->x[8] * c->x[9]); _r &= 0xFFFFFFFFULL; c->x[8] = _r; }\n"
              "    recomp_store32_sp(c,c->x[31] + 52,c->x[8]);\n"
              "    return;\n}\n")
WATER_TEXT = HEAD % WATER + WATER_LOAD + WATER_CUT + WATER_REST
WATER_ARGS = (9, "c->x[21]", "t8")


# ---- the host's code, as the tree has it
def host_file(name):
    """a host file without its includes (the fixtures' stand-ins give what they declare) but the logic's headers"""
    text = (ROOT / "host" / name).read_text(encoding="utf-8")
    keep = ('#include "oot_high_fps.h"', '#include "frame_whole.h"', '#include "loop_step.h"')
    out = "\n".join(line for line in text.split("\n") if not line.startswith("#include") or line in keep)
    return re.sub(r"^HL_[A-Z]+_HOOK\(.*\);.*$", "", out, flags=re.M)


STANDINS = r'''#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define A64_NOESC
typedef struct {
    uint64_t x[32], vreg[32][2];
    uint8_t n, z, c, v;
    uint32_t read[3];
    int second;
} Ctx;
static const uint64_t g_module_base = 0;
#define HOS_HEAP_BASE 0x1000ULL
static uint8_t mem[0x4000];
static uint8_t *guest(uint64_t a) {
    if (a == 0x84c9e890ULL) return mem + 0x100;   /* (the pointer to the game's time object) */
    return a < sizeof mem ? mem + a : mem;
}
#define GP(a) ((void *)guest(a))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
static void *hos_guest_range(uint64_t a, uint64_t n) { return a + n <= sizeof mem ? mem + a : NULL; }
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
#define AcquireSRWLockExclusive(l) ((void)(l))
#define ReleaseSRWLockExclusive(l) ((void)(l))
#define TryAcquireSRWLockExclusive(l) 1
typedef long long LONG64;
#define InterlockedIncrement64(p) (++*(p))
static void hos_log(const char *f, ...) { (void)f; }
volatile uint32_t hos_frame_seconds_bits;
static void a64_vset64(uint64_t *v, uint64_t bits) { v[0] = bits; v[1] = 0; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) { uint64_t v; (void)c; memcpy(&v, guest(a), 8); return v; }
static uint64_t recomp_load32(Ctx *c, uint64_t a) { uint32_t v; (void)c; memcpy(&v, guest(a), 4); return v; }
static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { uint32_t w = (uint32_t)v; (void)c; memcpy(guest(a), &w, 4); }
static void recomp_ldp32(Ctx *c, uint64_t a, uint64_t *p0, uint64_t *p1) { *p0 = recomp_load32(c, a); *p1 = recomp_load32(c, a + 4); }
'''


def compile_and_run(cc, directory, source, env=None):
    src, binary = Path(directory) / "fixture.c", Path(directory) / "fixture.exe"
    src.write_text(source, encoding="utf-8")
    build = subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-const-variable",
                    "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-I", str(ROOT / "host"),
                    str(src), "-o", str(binary), "-lm"], capture_output=True, text=True)
    if build.returncode:
        raise SystemExit("FAIL: the fixture did not compile: " + build.stderr[:3000])
    run_env = dict(os.environ)
    run_env.pop("HOS_FPS_LEDGE", None), run_env.pop("HOS_FPS_SONG", None), run_env.pop("HOS_FPS_LOOP", None)
    run_env.pop("HOS_FPS_WATER", None), run_env.pop("HOS_FPS_WHOLE", None)
    run_env.update(env or {})
    return subprocess.run([str(binary)], text=True, capture_output=True, timeout=60, env=run_env)


# ---- Link's driven body (the pull-up's lift played update by update, as upstream's test does)
DRIVE_FIXTURE = r'''
@HOST@
@UPDATE@
static int failures, checks;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}
static float getf(uint64_t a) { float f; memcpy(&f, guest(a), 4); return f; }
static void setf(uint64_t a, float f) { memcpy(guest(a), &f, 4); }
/* (the pull-up's lift: 1.92 m up and 0.08 m in over 0.4 s, eased in and out) */
static float lift(float t, float total) { if (t > 0.4f) t = 0.4f; return total * (1.0f - cosf(3.14159265f * t / 0.4f)) * 0.5f; }
/* (the physics' share of a driven body's move, as measured in the game: all at 60 fps and slower) */
static float share(float step) { return step >= 1.0f ? 1.0f : 1.0f - 0.126f * (1.0f / step - 1.0f); }
static uint64_t body = 0x2000;              /* (the controller's body) */
static int is_link = 1;                     /* (Link's components point at it) */
static uint64_t flags = 0x200180;
static int second;                          /* (the copy of the path the update takes) */
static int held_from = -1, held_to = -1;    /* (updates the physics holds the body back) */
static int stale_from = -1, stale_to = -1;  /* (updates the game does not renew the target) */
static float worst_extra;                   /* (the most the target read went past the game's, over the frame's move) */
static int changed, stale_grew;
static int twice_at = -1, twice_same;
static int flat_from = -1, flat_to = -1;    /* (updates Link's own update does not run before the body update) */
static int standing = 5;                    /* (updates the path is not reached before a climb) */
static void place(float x, float y, float z) { setf(body + 0x8c, x); setf(body + 0x9c, y); setf(body + 0xac, z); }
static void setup(float step, int timed_now) {
    hos_frame_seconds_bits = timed_now ? 1u : 0u;
    const uint32_t s = oot_float_bits(step);
    G64(0x84c9e890ULL) = 0x2800;
    G64(0x2800) = 0x2800;
    memcpy(mem + 0x2810, &s, 4);
    G64(0x1000 + 0xf0) = body;
    G64(0x3000 + 0x50) = 0x3100; G64(0x3100 + 0x20) = 0x3200; G64(0x3200 + 0x10) = 0x3300; G64(0x3300 + 8) = 0x3400;
    G64(0x3400 + 0x20) = is_link ? body : 0x2c00;
}
static int link_runs = 1;
static void update(uint64_t f, float t[3]) {
    if (link_runs) hos_oot_note_link(0x3000);
    G64(0x1000 + 0x100) = f;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[20] = 0x1000;
    c.second = second;
    for (int i = 0; i < 3; i++) memcpy(&c.read[i], GP(0x1000 + 0x3c + 4 * i), 4);
    f_804f5030(&c);
    for (int i = 0; i < 3; i++) memcpy(&t[i], &c.read[i], 4);
}
static void game_target(float x, float y, float z) { setf(0x1000 + 0x3c, x); setf(0x1000 + 0x40, y); setf(0x1000 + 0x44, z); }
static float climb(float step, int timed_now, float *in) {
    setup(step, timed_now);
    float t[3];
    for (int i = 0; i < standing; i++) update(0x200000, t);
    const float x0 = getf(body + 0x8c), y0 = getf(body + 0x9c);
    const float dt = step / 60.0f;
    float tm = 0.0f;
    worst_extra = 0.0f;
    changed = stale_grew = 0;
    for (int n = 0; tm < 0.6f; n++, tm += dt) {
        const float bx = getf(body + 0x8c), by = getf(body + 0x9c), bz = getf(body + 0xac);
        const float mx = -(lift(tm + dt, 0.08f) - lift(tm, 0.08f)), my = lift(tm + dt, 1.92f) - lift(tm, 1.92f);
        const int stale = n >= stale_from && n <= stale_to;
        if (!stale) game_target(bx + mx, by + my, bz);
        const float gx = getf(0x1000 + 0x3c), gy = getf(0x1000 + 0x40), gz = getf(0x1000 + 0x44);
        link_runs = !(n >= flat_from && n <= flat_to);
        update(flags, t);
        link_runs = 1;
        if (n == twice_at) {   /* (again in this update: the game sets the same target, no update of Link's between) */
            game_target(gx, gy, gz);
            Ctx c;
            memset(&c, 0, sizeof c);
            c.x[20] = 0x1000;
            c.second = second;
            f_804f5030(&c);
            float a[3];
            for (int i = 0; i < 3; i++) memcpy(&a[i], &c.read[i], 4);
            twice_same = a[0] == t[0] && a[1] == t[1] && a[2] == t[2] && (t[0] != gx || t[1] != gy);
        }
        if (stale && (t[0] != gx || t[1] != gy || t[2] != gz)) stale_grew++;
        if (t[0] != gx || t[1] != gy || t[2] != gz) changed++;
        const float move = sqrtf(mx * mx + my * my);
        const float extra = sqrtf((t[0] - bx - mx) * (t[0] - bx - mx) + (t[1] - by - my) * (t[1] - by - my) + (t[2] - bz) * (t[2] - bz));
        if (!stale && move > 1e-6f && extra / move > worst_extra) worst_extra = extra / move;
        const float k = n >= held_from && n <= held_to ? 0.0f : share(step);   /* (the physics: a share of the move) */
        place(bx + k * (t[0] - bx), by + k * (t[1] - by), bz + k * (t[2] - bz));
    }
    if (in) *in = x0 - getf(body + 0x8c);
    return getf(body + 0x9c) - y0;
}
int main(void) {
    char what[200];
    float in, t[3];
    place(-10.5f, 100.0f, 3.8f);
    check(fabsf(climb(1.0f, 0, NULL) - 1.92f) < 0.002f, "a cap of 60: the body follows the lift");
    place(-10.5f, 100.0f, 3.8f);
    held_from = 5, held_to = 9;
    climb(1.0f, 0, NULL);
    check(changed == 0, "a cap of 60, the body held back: the game's targets (nothing carried)");
    place(-10.5f, 100.0f, 3.8f);
    climb(0.5f, 0, NULL);
    check(changed == 0, "a cap of 60 with time slowed (a step of 0.5), the body held back: the game's targets");
    held_from = held_to = -1;
    const float steps[] = {0.5f, 60.0f / 144.0f, 60.0f / 170.0f, 0.25f};
    for (second = 0; second < 2; second++)
        for (int i = 0; i < 4; i++) {
            place(-10.5f, 100.0f, 3.8f);
            const float risen = climb(steps[i], 1, &in);
            snprintf(what, sizeof what, "%.0f fps%s: the body rose %.3f m of the lift's 1.920 and moved in %.4f m of 0.0800",
                     60.0f / steps[i], second ? " (the path's second copy)" : "", risen, in);
            check(fabsf(risen - 1.92f) < 0.0096f && fabsf(in - 0.08f) < 0.001f, what);
        }
    second = 0;
    const float game120 = 1.92f * share(0.5f);   /* (each target the game's: the body rises the share of the lift) */
    place(-10.5f, 100.0f, 3.8f);
    is_link = 0;
    check(fabsf(climb(0.5f, 1, NULL) - game120) < 0.002f, "not Link's body at 120 fps: the game's targets");
    is_link = 1;
    place(-10.5f, 100.0f, 3.8f);
    flags = 0x200080;
    check(fabsf(climb(0.5f, 1, NULL) - game120) < 0.002f, "on the path but not handing a velocity (flags 0x80) at 120 fps: the game's targets");
    flags = 0x200180;
    place(-10.5f, 100.0f, 3.8f);
    check(fabsf(climb(1.25f, 1, NULL) - 1.92f) < 0.002f, "a frame slower than 60 fps: the body follows");
    place(-10.5f, 100.0f, 3.8f);
    held_from = 5, held_to = 9;
    climb(1.25f, 1, NULL);
    check(changed == 0, "a frame slower than 60 fps, the body held back: the game's targets (nothing carried)");
    held_from = held_to = -1;
    place(-10.5f, 100.0f, 3.8f);
    climb(0.5f, 1, NULL);
    game_target(NAN, 101.0f, 3.8f);
    update(flags, t);
    check(t[0] != t[0] && t[1] == 101.0f, "a NaN target: read as the game wrote it");
    game_target(INFINITY, 101.0f, 3.8f);
    update(flags, t);
    place(-3.0f, 50.0f, 1.0f);
    game_target(-3.0f, 50.03f, 1.0f);
    update(flags, t);
    check(t[0] == -3.0f && t[1] == 50.03f, "after a NaN and an infinite target: the next target is the game's (nothing carried)");
    place(-10.5f, 100.0f, 3.8f);
    climb(0.5f, 1, NULL);
    setf(body + 0x9c, INFINITY);
    game_target(-10.5f, 101.9f, 3.8f);
    update(flags, t);
    check(t[0] == -10.5f && t[1] == 101.9f && t[2] == 3.8f, "an infinite body position: the game's target");
    place(-10.5f, 100.0f, 3.8f);
    climb(0.5f, 1, NULL);
    for (int i = 0; i < 5; i++) update(0x200000, t);
    place(20.0f, 10.0f, -4.0f);
    game_target(20.0f, 10.02f, -4.0f);
    update(flags, t);
    check(t[0] == 20.0f && t[1] == 10.02f && t[2] == -4.0f, "a climb after updates the path was not reached: the game's own target");
    place(-10.5f, 100.0f, 3.8f);
    climb(0.5f, 1, NULL);
    body = 0x2400;
    setup(0.5f, 1);
    place(5.0f, 30.0f, 2.0f);
    game_target(5.0f, 30.02f, 2.0f);
    update(flags, t);
    check(t[0] == 5.0f && t[1] == 30.02f && t[2] == 2.0f, "Link's body changed: the new body starts from the game's own target");
    body = 0x2000;
    setup(0.5f, 1);
    place(-10.5f, 100.0f, 3.8f);
    stale_from = 20, stale_to = 25;
    climb(0.25f, 1, NULL);
    stale_from = stale_to = -1;
    snprintf(what, sizeof what, "a target not renewed for 6 updates: read as it is (%d of 6 read otherwise)", stale_grew);
    check(stale_grew == 0, what);
    place(-10.5f, 100.0f, 3.8f);
    twice_at = 30;
    climb(0.5f, 1, NULL);
    twice_at = -1;
    check(twice_same == 1, "a second call in the same update, mid-climb: the same target as the first");
    place(-10.5f, 100.0f, 3.8f);
    flat_from = 20, flat_to = 29;
    {
        const float risen = climb(0.5f, 1, NULL);
        snprintf(what, sizeof what, "Link's update not run before 10 body updates: the body rose %.3f m of the lift's 1.920", risen);
        check(fabsf(risen - 1.92f) < 0.0096f, what);
    }
    flat_from = flat_to = -1;
    place(-10.5f, 100.0f, 3.8f);
    climb(0.5f, 1, NULL);
    update(0x200000, t);
    place(-30.0f, 60.0f, 9.0f);
    game_target(-30.0f, 60.02f, 9.0f);
    update(flags, t);
    check(t[0] == -30.0f && t[1] == 60.02f && t[2] == 9.0f, "after one update the path was not reached: the game's own target");
    place(-10.5f, 100.0f, 3.8f);
    held_from = 20, held_to = 31;
    climb(0.5f, 1, NULL);
    held_from = held_to = -1;
    snprintf(what, sizeof what, "a body held back: the target went %.2f frame moves past the game's (at most 3)", worst_extra);
    check(worst_extra <= 3.001f && worst_extra > 1.5f, what);
    if (!failures) printf("PASS: Link's driven body's target (%d checks)\n", checks);
    return failures != 0;
}
'''


def drive_test(cc, directory):
    host = host_file("hl_high_fps.c")
    update = hooked(UPDATE, DRIVE, "drive")
    unrelated = UNRELATED_UPDATE
    source = STANDINS + DRIVE_FIXTURE.replace("@HOST@", host).replace("@UPDATE@", update + unrelated)
    result = compile_and_run(cc, directory, source)
    print(result.stdout, end="")
    check(result.returncode == 0, "Link's driven body's target above 60 fps (hooked)")
    # the off switch: the game's targets, so the 120 fps climb falls short
    off = compile_and_run(cc, directory, source, {"HOS_FPS_LEDGE": "0"})
    check(off.returncode != 0 and "FAIL: 120 fps" in off.stdout, "HOS_FPS_LEDGE=0 did not leave the game's targets")
    print("PASS: HOS_FPS_LEDGE=0 leaves the game's targets (%s)" % off.stdout.split("FAIL: ")[1].split("\n")[0])
    # the unhooked update, with Link's update stubbed out: the bug, shown without the fix
    stub = STANDINS + DRIVE_FIXTURE.replace("@HOST@", host).replace("@UPDATE@", UPDATE)
    bare = compile_and_run(cc, directory, stub)
    check(bare.returncode != 0 and "FAIL: 120 fps" in bare.stdout, "the unhooked update did not fail the 120 fps check")
    print("PASS: the unhooked update fails the 120 fps check")


# ---- the frozen Zoras
LOOP_FIXTURE = r'''
uint64_t a64_fpx_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z);
uint64_t a64_fpx_hot_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z);
@HOST@
static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float real(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
uint64_t a64_fpx_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) { (void)c; (void)z; return bits(real((uint32_t)a) * real((uint32_t)b)); }
uint64_t a64_fpx_hot_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) { (void)c; (void)z; return bits(real((uint32_t)a) * real((uint32_t)b)); }
static void clock_advance(Ctx *c) {   /* (the game's own wrap of a clock that passes its end) */
    float time = real((uint32_t)recomp_load32(c, c->x[20] + 4)) + real((uint32_t)c->vreg[2][0]);
    const float start = real((uint32_t)recomp_load32(c, c->x[20] + 16)), end = real((uint32_t)recomp_load32(c, c->x[20] + 20));
    if (time >= end) time = start + fmodf(time - start, end - start);
    recomp_store32(c, c->x[20] + 4, bits(time));
}
@UPDATES@
static int failures;
static const char *failed[64];
static int failed_count[64], failed_kinds;
static void fail(const char *what) {
    failures++;
    for (int i = 0; i < failed_kinds; i++) if (failed[i] == what) { failed_count[i]++; return; }
    if (failed_kinds < 64) failed[failed_kinds] = what, failed_count[failed_kinds++] = 1;
}
#define CHECK(test, what) do { if (!(test)) fail(what); } while (0)
static void set_timed(int t) { hos_frame_seconds_bits = t ? bits(1.0f / 165) : 0; }
enum { A = 0x100, B = 0x200, C = 0x300, STATE = 0x400 };
static void entry(uint64_t at, float time, float start, float end) {
    Ctx c = {0};
    recomp_store32(&c, at + 0, 0x403);
    recomp_store32(&c, at + 4, bits(time));
    recomp_store32(&c, at + 16, bits(start));
    recomp_store32(&c, at + 20, bits(end));
}
static float update(uint64_t at, float step, int mode) {
    Ctx c = {0};
    c.x[20] = at;
    c.x[10] = (uint64_t)mode;
    c.vreg[8][0] = bits(step);
    c.vreg[1][0] = c.vreg[0][0] = bits(1.0f);
    f_802cfca0(&c);
    return real((uint32_t)recomp_load32(&c, at + 4));
}
static float phase(float t) { return t - floorf(t); }
static float pose(float incoming, float min, float max) {
    Ctx c = {0};
    c.x[22] = STATE;
    recomp_store32(&c, STATE + 0, bits(-1.0f));
    recomp_store32(&c, STATE + 4, bits(min));
    recomp_store32(&c, STATE + 8, bits(max));
    c.vreg[0][0] = bits(incoming);
    f_80944aa0(&c);
    return real((uint32_t)recomp_load32(&c, STATE));
}
int main(void) {
    const float frame = 60.0f / 165.0f;   /* (the step at 165 fps) */
    for (int mode = 0; mode < 2; mode++) {
        set_timed(1);
        entry(A, 0.0057f, 0.0f, 1.0f);
        float lo = 2.0f, hi = -1.0f;
        for (int i = 0; i < 165; i++) {
            const float p = phase(update(A, frame, mode));
            if (p < lo) lo = p;
            if (p > hi) hi = p;
        }
        CHECK(hi - lo < 0.01f, "a loop of one frame must stay on one phase at 165 fps");
        entry(B, 0.25f, 0.0f, 1.0f); entry(C, 0.75f, 0.0f, 1.0f);
        for (int i = 0; i < 165; i++) {
            const float pb = phase(update(B, frame, mode)), pc = phase(update(C, frame, mode));
            CHECK(fabsf(pb - 0.25f) < 0.01f && fabsf(pc - 0.75f) < 0.01f, "each loop of one frame keeps its own phase");
        }
        entry(A, 2.5f, 2.0f, 3.0f);
        update(A, frame, mode); update(A, frame, mode); update(A, frame, mode);
        { Ctx c = {0}; CHECK(fabsf(phase(real((uint32_t)recomp_load32(&c, A + 4))) - 0.5f) < 0.01f, "the loop from 2 to 3 is a pose too"); }
        entry(A, 0.0f, 0.0f, 1.5f);
        update(A, frame, mode);
        { Ctx c = {0}; CHECK(fabsf(real((uint32_t)recomp_load32(&c, A + 4)) - frame) < 1e-4f, "a loop longer than a frame keeps the fractional step"); }
        entry(A, 0.0f, 0.0f, 1000.0f);
        float t = 0;
        for (int i = 0; i < 165; i++) t = update(A, frame, mode);
        CHECK(fabsf(t - 60.0f) < 0.05f, "a long loop must keep moving by the frames' own time");
        set_timed(0);
        entry(A, 0.0057f, 0.0f, 1.0f);
        CHECK(fabsf(update(A, 0.4f, mode) - 0.4057f) < 1e-4f, "at a cap of 60 a loop of one frame moves by the game's step");
        entry(A, 0.0057f, 0.0f, 1.0f);
        CHECK(fabsf(phase(update(A, 1.0f, mode)) - 0.0057f) < 1e-4f, "at a cap of 60 the step of 1 stays on its phase");
        set_timed(1);
        entry(A, 0.3f, 0.0f, 1.0f);
        CHECK(fabsf(update(A, 0.0f, mode) - 0.3f) < 1e-6f, "a paused clock does not move");
    }
    {
        float sum = 0;
        set_timed(1);
        for (int i = 0; i < 1650; i++) sum += real(hos_loop_step(0x778, bits(frame), bits(0.0f), bits(1.0f)));
        CHECK(fabsf(sum - 600.0f) < 1.0f, "ten seconds at 165 fps are 600 whole frames");
    }
    set_timed(1);
    CHECK(pose(0.7f, 0.0f, 1.0f) == 0.0f, "a loop of one frame is drawn at its start");
    CHECK(pose(0.7f, 0.25f, 1.0f) == 0.25f, "a loop of one frame is drawn at its own start");
    CHECK(pose(0.7f, 0.0f, 100.0f) == 0.7f, "a long loop is drawn at its time");
    CHECK(pose(0.7f, 0.0f, 1.5f) == 0.7f, "a loop of a frame and a half is drawn at its time");
    CHECK(pose(0.7f, 1.0f, 0.0f) == 0.7f, "a range that is the wrong way round is left alone");
    set_timed(0);
    CHECK(pose(0.7f, 0.0f, 1.0f) == 0.7f, "at a cap of 60 the time is the game's");
    for (int i = 0; i < failed_kinds; i++) printf("FAIL: %s (%d times)\n", failed[i], failed_count[i]);
    if (!failures) puts("PASS: loop of one frame");
    return failures != 0;
}
'''


def loop_test(cc, directory):
    host = host_file("hl_step.c")
    host = block(host, "static int loop_timed(void) {") + "\n" + block(host, "uint32_t hos_loop_step(") + "\n" + \
        block(host, "uint32_t hos_loop_pose(")
    clock, pose = hooked(CLOCK, LOOP, "loop"), hooked(POSE_TEXT, POSE, "pose")
    check(clock.count("hos_loop_step(c->x[20],") == 2 and pose.count("hos_loop_pose(_t,") == 1, "the loop hooks are not twice and once")
    source = STANDINS + LOOP_FIXTURE.replace("@HOST@", '#include "loop_step.h"\n' + host).replace("@UPDATES@", clock + pose + UNRELATED_LOOP)
    result = compile_and_run(cc, directory, source)
    print(result.stdout, end="")
    check(result.returncode == 0, "the frozen Zoras: a loop of one frame stands still above 60 fps (hooked)")
    off = compile_and_run(cc, directory, source, {"HOS_FPS_LOOP": "0"})
    check(off.returncode != 0 and "must stay on one phase" in off.stdout, "HOS_FPS_LOOP=0 did not leave the game's clock")
    print("PASS: HOS_FPS_LOOP=0 leaves the game's clock and time")
    # the unhooked updates (the game's text as authored): the same checks fail
    bare = compile_and_run(cc, directory, STANDINS + LOOP_FIXTURE.replace("@HOST@", '#include "loop_step.h"\n' + host).replace(
        "@UPDATES@", CLOCK + POSE_TEXT))
    check(bare.returncode != 0 and "must stay on one phase" in bare.stdout, "the unhooked updates did not fail the one-phase check")
    print("PASS: the unhooked updates fail the same checks")


# ---- the water's ripples
WATER_FIXTURE = r'''
static uint32_t ran;                 /* (the substeps the frame runs: what the function stores at sp+52) */
static void recomp_store32_sp(Ctx *c, uint64_t a, uint64_t v) { (void)c; (void)a; ran = (uint32_t)v; }
#include "frame_whole.h"
@HOST@
@UPDATE@
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static void frames(int fps, uint32_t per, int carry, uint32_t *least, uint32_t *most, uint32_t *sum) {
    static uint64_t water = 0x7100000;
    Ctx c; memset(&c, 0, sizeof c);
    c.x[21] = water += 0x400;   /* (a new object each run: its own remainder) */
    c.x[19] = 0x1000;
    memcpy(mem + 0x1000 + 76, &per, 4);   /* (the water object's +76: the substeps) */
    const float step = 60.0f / (float)fps;
    hos_frame_seconds_bits = carry ? bits(1.0f / (float)fps) : 0;
    *least = 0xffffffffu, *most = 0, *sum = 0;
    for (int f = 0; f < fps; f++) {
        c.vreg[0][0] = bits(step);
        f_800672e0(&c);
        if (ran < *least) *least = ran;
        if (ran > *most) *most = ran;
        *sum += ran;
    }
}
int main(int argc, char **argv) {
    (void)argv;
    const int off = argc > 1;   /* (HOS_FPS_WATER=0: the whole frames carried, then multiplied) */
    uint32_t lo, hi, n;
    frames(108, 4, 1, &lo, &hi, &n);   /* (the fountain at a 240 cap, about 108 fps) */
    printf("108 fps, 4 substeps: %u to %u a frame, %u a second\n", lo, hi, n);
    if (off) { CHECK(lo == 0 && hi == 4); return fails != 0; }
    CHECK(lo >= 2 && hi <= 3);         /* (every frame steps: no frame stands still, none runs all four at once) */
    CHECK(n >= 239 && n <= 240);       /* (the 240 a second of 60 fps) */
    frames(144, 4, 1, &lo, &hi, &n);
    CHECK(lo >= 1 && hi <= 2);
    CHECK(n >= 239 && n <= 240);
    frames(144, 1, 1, &lo, &hi, &n);   /* (one substep a frame: whole substeps, as before, at the game's rate) */
    CHECK(hi <= 1 && n >= 59 && n <= 60);
    frames(60, 4, 0, &lo, &hi, &n);    /* (a cap of 60: the game's own step, cut as it cuts it) */
    CHECK(lo == 4 && hi == 4 && n == 240);
    frames(30, 4, 0, &lo, &hi, &n);    /* (30 fps: the step is 2, two frames' substeps) */
    CHECK(lo == 8 && hi == 8);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
'''


def water_test(cc, directory):
    host = host_file("hl_step.c")
    host = block(host, "uint32_t hos_frame_whole_times(")
    fixture = WATER_FIXTURE.replace("@HOST@", host)
    # (the stand-ins' recomp_load32 is the water's: the hooks' own loads are not used here)
    update = hooked(WATER_TEXT, WATER, "whole", WATER_ARGS)
    check(update.count("hos_frame_whole_times(c->x[21], (uint32_t)c->vreg[0][0], (uint32_t)c->x[8]); c->x[8] = 1;") == 1,
          "the water hook is not the whole substeps")
    prelude = STANDINS + "static uint64_t recomp_fp_to_int(uint64_t bits, int a, int b, int s, int r, uint32_t fpcr, uint32_t *fpsr) {\n" \
               "    (void)a; (void)b; (void)s; (void)r; (void)fpcr; (void)fpsr;\n" \
               "    float f; uint32_t u = (uint32_t)bits; memcpy(&f, &u, 4);\n    return f > 0.0f ? (uint64_t)(uint32_t)f : 0;\n}\n"
    prelude = prelude.replace("typedef struct {\n    uint64_t x[32], vreg[32][2];", "typedef struct {\n    uint32_t fpcr, fpsr;\n    uint64_t x[32], vreg[32][2];")
    source = prelude + fixture.replace("@UPDATE@", update)
    result = compile_and_run(cc, directory, source)
    print(result.stdout, end="")
    check(result.returncode == 0, "the water's ripples step every frame above 60 fps (hooked)")
    src = Path(directory) / "fixture.exe"
    off = subprocess.run([str(src), "off"], text=True, capture_output=True, timeout=30, env=dict(os.environ, HOS_FPS_WATER="0"))
    check(off.returncode == 0, "HOS_FPS_WATER=0 did not leave the whole frames carried and multiplied: " + off.stdout)
    print("PASS: HOS_FPS_WATER=0 keeps the whole frames carried and multiplied (all substeps or none)")
    gone = subprocess.run([str(src)], text=True, capture_output=True, timeout=30, env=dict(os.environ, HOS_FPS_WHOLE="0"))
    check(gone.returncode != 0, "HOS_FPS_WHOLE=0 did not leave the game's cut")
    print("PASS: HOS_FPS_WHOLE=0 leaves the game's cut")


# ---- the ocarina's notes (upstream's fixture: the sound job's phases through the host's gate, then both playbacks)
SONG_FIXTURE = r'''
static int unrelated;
static void next_note(int playback);
static uint32_t flag;   /* (the note flag: the sound engine's word) */
static void f_832b7b50(Ctx *c) { c->x[0] = flag != 0; }
@HOST@
@UPDATES@
#define NOTES 6
static const int lullaby[NOTES] = {10, 60, 85, 165, 218, 246};   /* (the sound engine's runs at which the melody reaches a note) */
static const int close_notes[NOTES] = {10, 60, 61, 165, 218, 246};
static const int *melody = lullaby;
static int runs, note[2], waiting[2], went[2][16];
static Ctx ctx;
static void next_note(int playback) {
    if (waiting[playback]) {
        if (note[playback] < 16) went[playback][note[playback]] = runs;
        note[playback]++;
    }
    waiting[playback] = 1;
}
static void sound_engine(void) {
    runs++;
    flag = 0;
    for (int i = 0; i < NOTES; i++)
        if (melody[i] == runs) flag = 0x100000;
}
static void frame(float step, int timed_now, int reads) {
    const float seconds = step / 60.0f;
    hos_frame_seconds_bits = timed_now ? oot_float_bits(seconds) : 0;
    memcpy(mem + 0x2010, &step, 4);
    for (uint32_t phase = 0; phase < 3; phase++)
        if (hos_oot_sound_gate(&ctx, phase, 0x84c9e890ULL) && phase == 0) sound_engine();
    for (int r = 0; r < reads; r++) {
        f_832b2af0(&ctx);
        f_832b2dc0(&ctx);
    }
}
static int failures, checks;
static void run(const char *what, const int *notes, float step, float jitter, int timed_now, int reads, int per_note) {
    int expected[16], n = 0;
    for (int i = 0; i < NOTES; i++)
        for (int k = 0; k < per_note; k++)
            if (i || k) expected[n++] = notes[i];
    melody = notes;
    memset(note, 0, sizeof note);
    memset(waiting, 0, sizeof waiting);
    memset(went, 0, sizeof went);
    runs = 0;
    flag = 0;
    hos_frame_seconds_bits = 0;
    for (uint32_t phase = 0; phase < 3; phase++) hos_oot_sound_gate(&ctx, phase, 0x84c9e890ULL);   /* (the gate starts afresh) */
    runs = 0;
    uint32_t seed = 1;
    for (int i = 0; i < 100000 && runs < 300; i++) {
        seed = seed * 1664525u + 1013904223u;
        const float s = step + jitter * ((float)(seed >> 8) / 8388608.0f - 1.0f);
        frame(s, timed_now, reads);
    }
    for (int p = 0; p < 2; p++) {
        checks++;
        int ok = note[p] == n;
        for (int i = 0; ok && i < n; i++) ok = went[p][i] == expected[i];
        if (!ok) {
            printf("FAIL: %s: playback %d went on %d notes, on the sound engine's runs", what, p, note[p]);
            for (int i = 0; i < note[p] && i < 16; i++) printf(" %d", went[p][i]);
            printf(" (expected %d:", n);
            for (int i = 0; i < n; i++) printf(" %d", expected[i]);
            printf(")\n");
            failures++;
        }
    }
}
int main(void) {
    const uint64_t time_at = 0x1000, time_obj = 0x2000;
    memcpy(mem + 0x100, &time_at, 8);
    memcpy(mem + 0x1000, &time_obj, 8);
    run("120 fps", lullaby, 0.5f, 0.0f, 1, 1, 1);
    run("144 fps", lullaby, 60.0f / 144.0f, 0.0f, 1, 1, 1);
    run("165 fps", lullaby, 60.0f / 165.0f, 0.0f, 1, 1, 1);
    run("240 fps", lullaby, 0.25f, 0.0f, 1, 1, 1);
    run("90 fps", lullaby, 60.0f / 90.0f, 0.0f, 1, 1, 1);
    run("uncapped, each frame's time varying (100 to 300 fps)", lullaby, 0.4f, 0.2f, 1, 1, 1);
    run("60 fps under a cap above 60", lullaby, 1.0f, 0.0f, 1, 1, 1);
    run("a frame slower than 60 fps", lullaby, 1.25f, 0.0f, 1, 1, 1);
    run("a cap of 60", lullaby, 1.0f, 0.0f, 0, 1, 1);
    run("120 fps, two notes on consecutive runs of the sound engine", close_notes, 0.5f, 0.0f, 1, 1, 1);
    run("a frame slower than 60 fps, two notes on consecutive runs", close_notes, 1.25f, 0.0f, 1, 1, 1);
    run("a cap of 60, two notes on consecutive runs", close_notes, 1.0f, 0.0f, 0, 1, 1);
    run("a cap of 60, the playbacks read twice a frame (the game's own: two notes at each)", lullaby, 1.0f, 0.0f, 0, 2, 2);
    run("about 60 fps under a cap above 60 (steps 0.98 to 1.02)", lullaby, 1.0f, 0.02f, 1, 1, 1);
    static const float rates[] = {75.0f, 90.0f, 144.0f, 165.0f};
    for (int k = 0; k < 4; k++) {
        const int pair[NOTES] = {10, 60 + k, 61 + k, 165, 218, 246};
        char what[160];
        for (int r = 0; r < 4; r++) {
            snprintf(what, sizeof what, "%g fps, two notes on consecutive runs (%d and %d)", rates[r], 60 + k, 61 + k);
            run(what, pair, 60.0f / rates[r], 0.0f, 1, 1, 1);
        }
        snprintf(what, sizeof what, "uncapped, each frame's time varying, two notes on consecutive runs (%d and %d)", 60 + k, 61 + k);
        run(what, pair, 0.4f, 0.2f, 1, 1, 1);
        snprintf(what, sizeof what, "about 60 fps (steps 0.98 to 1.02), two notes on consecutive runs (%d and %d)", 60 + k, 61 + k);
        run(what, pair, 1.0f, 0.02f, 1, 1, 1);
    }
    if (unrelated) {
        printf("FAIL: the unrelated function went through the host\n");
        failures++;
    }
    if (!failures) printf("PASS: a song's playback goes on one note at each of its melody's notes (%d checks)\n", checks);
    return failures != 0;
}
'''


def song_test(cc, directory):
    host = host_file("hl_high_fps.c")
    played, other = hooked(PLAYED, SONGS[0], "song"), hooked(OTHER, SONGS[1], "song")
    check(played.count("hos_oot_song_cue(0, c->x[0])") == 1 and other.count("hos_oot_song_cue(1, c->x[0])") == 1,
          "the song hooks are not once each, with their sites")
    source = STANDINS + SONG_FIXTURE.replace("@HOST@", host).replace("@UPDATES@", played + other + UNRELATED_SONG)
    result = compile_and_run(cc, directory, source)
    print(result.stdout, end="")
    check(result.returncode == 0, "a song's playback above 60 fps (hooked)")
    off = compile_and_run(cc, directory, source, {"HOS_FPS_SONG": "0"})
    check(off.returncode != 0 and "FAIL: 120 fps:" in off.stdout, "HOS_FPS_SONG=0 did not leave the game's flag")
    print("PASS: HOS_FPS_SONG=0 leaves the game's flag (two notes at each of the melody's)")
    bare = compile_and_run(cc, directory, STANDINS + SONG_FIXTURE.replace("@HOST@", host).replace(
        "@UPDATES@", PLAYED + OTHER + UNRELATED_SONG))
    check(bare.returncode != 0 and "FAIL: 120 fps:" in bare.stdout, "the unhooked updates did not fail the 120 fps check")
    print("PASS: the unhooked updates fail the 120 fps check")


# ---- the lift hooks as hl_gates puts them in
def gate_text():
    """The hooks in a made-up lift: once each, after their anchors, hashes unchanged, idempotent; another text, a function an exeFS mod
    patches and a tree that does not ask leave the lift as it was; an older tree's stale-gate rule takes them out."""
    lifted = "".join((UPDATE, CLOCK, POSE_TEXT, PLAYED, OTHER, WATER_TEXT, UNRELATED_UPDATE, UNRELATED_LOOP, UNRELATED_SONG))
    addrs = (DRIVE, LOOP, POSE) + SONGS + (WATER,)
    hashes = {a: hl_gates.text_hash(lifted, a) for a in addrs}
    asks = ('HL_DRIVE_HOOK(%s, "%s");\nHL_LOOP_HOOK(%s, "%s");\nHL_POSE_HOOK(%s, "%s");\n' %
            (DRIVE, hashes[DRIVE], LOOP, hashes[LOOP], POSE, hashes[POSE]) +
            "".join('HL_SONG_HOOK(%s, "%s");\n' % (a, hashes[a]) for a in SONGS) +
            'HL_WHOLE_HOOK(%s, "%s", 9, "c->x[21]", "t8");\n' % (WATER, hashes[WATER]))
    with tempfile.TemporaryDirectory(prefix="above60-ledge-") as tmp:
        gen, host = Path(tmp) / "gen", Path(tmp) / "host"
        gen.mkdir()
        host.mkdir()
        code = gen / "code_000.c"
        code.write_text(lifted, encoding="utf-8", newline="")
        (host / "hl_f.c").write_text(asks, encoding="utf-8")
        hl_gates.apply(str(gen), str(host), quiet=True)
        s = code.read_text(encoding="utf-8")
        drive = hl_gates.lift_lines("drive", DRIVE, ())
        for reg, line in zip(hl_gates.DRIVE_REGS, drive):
            first, rest = (READ % reg).split("\n", 1)
            check(s.count(first + "\n" + line + rest) == 1, "the driven-body hook is not between the load of x%d and the target's reads" % reg)
        check(s.count("hos_oot_drive_target(") == 2 + 2, "the drive hook is not twice in f_804f5030 (and no other function)")   # (+ 2 externs)
        loop = hl_gates.lift_lines("loop", LOOP, ())
        check(s.count(loop[0] + hl_gates.LOOP_MUL % 1) == 1 and s.count(loop[2] + hl_gates.LOOP_MUL % 0) == 1,
              "the clock's hook is not right before each multiply")
        check(s.count(loop[1]) == 1 and s.count(loop[3]) == 1, "the clock's step is not given back once after each multiply")
        pose = hl_gates.lift_lines("pose", POSE, ())[0]
        check(s.count(hl_gates.POSE_STORE + pose) == 1, "the pose hook is not right after the store")
        for addr, call in hl_gates.SONG_CALLS.items():
            line = hl_gates.lift_lines("song", addr, ())[0]
            check(s.count(call + line) == 1, "the song hook is not right after the playback's call (%s)" % addr)
        water = hl_gates.lift_lines("whole", WATER, WATER_ARGS)[0]
        check(s.count(WATER_LOAD + WATER_CUT + water) == 1, "the water hook is not right after the cut, the load of the substeps before it")
        check(s.count("hos_frame_whole_times(") == 2 and "hl gate 9" not in s, "the unrelated functions were hooked")
        check(all(hl_gates.text_hash(s, a) == h for a, h in hashes.items()), "a hook changed its function's hash")
        old = re.compile(r"^.*/\* hl gate ([0-9a-f]{8}) \*/\n", re.M).sub("", s)
        check(old == lifted, "an older tree's gates script does not restore the lift")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == s, "a second run changed a hooked file")
        # (another text: the anchor changed, or the hash is not the recorded one: left as it is)
        altered = lifted.replace("c->x[20] + 240", "c->x[20] + 248", 1)
        check(hl_gates.apply_lift(altered, DRIVE, "drive", ()) is None, "a changed driven-body anchor was hooked")
        check(hl_gates.apply_lift(lifted.replace("goto L_802d07f4", "goto L_802d07f8", 1), LOOP, "loop", ()) is None,
              "a changed clock anchor was hooked")
        check(hl_gates.apply_lift(lifted.replace("0x832b2c84ULL", "0x832b2c88ULL", 1), SONGS[0], "song", ()) is None,
              "a changed song call was hooked")
        check(hl_gates.apply_lift(lifted.replace(WATER_LOAD, WATER_LOAD.replace("+ 76", "+ 80"), 1), WATER, "whole", WATER_ARGS) is None,
              "a changed substeps' load was hooked")
        code.write_text(altered, encoding="utf-8", newline="")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check("hos_oot_drive_target(" not in code.read_text(encoding="utf-8"), "a text that hashes otherwise was hooked")
        # (a function an exeFS mod patches: no hook, and one already there is taken out)
        code.write_text(lifted, encoding="utf-8", newline="")
        (gen / "hl_gates.state").unlink(missing_ok=True)
        hl_gates.apply(str(gen), str(host), quiet=True, skip={DRIVE: "mod"})
        t = code.read_text(encoding="utf-8")
        check("hos_oot_drive_target(" not in t and "hos_loop_step(" in t, "an exeFS mod's function was hooked (or the others were not)")
        # (the tree asks for none: they are taken out as stale gates)
        (host / "hl_f.c").write_text("", encoding="utf-8")
        hl_gates.apply(str(gen), str(host), quiet=True)
        check(code.read_text(encoding="utf-8") == lifted, "a tree that asks for no hook left them in")
    print("PASS: the hooks go in once each after their anchors, hashes unchanged, idempotent; other text, an exeFS mod's function and a "
          "tree that does not ask leave the lift alone")


def asks_and_wiring():
    asked = hl_gates.wanted_lifts()
    for addr, kind in ((DRIVE, "drive"), (LOOP, "loop"), (POSE, "pose"), (SONGS[0], "song"), (SONGS[1], "song")):
        check(asked.get(addr, (None, None))[1] == kind, "the tree does not ask for the %s hook of f_%s" % (kind, addr))
    check(asked.get(WATER, (None, None, ()))[2][2:] == ("t8",), "the water hook is not asked for with its substeps (t8)")
    main_c = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    lift_player = block(main_c, "void hos_oot_lift_player(Ctx *c, uint64_t actor) {")
    call = lift_player.find("\n    hos_oot_note_link(components);\n")   # (a statement of its own, not under a condition)
    check(0 < call < lift_player.find("TryAcquireSRWLockExclusive(&cheats_lock)"),
          "host/hos_main.c's hos_oot_lift_player does not call hos_oot_note_link(components) on every update of Link's, before the "
          "cheats' lock")
    print("PASS: the tree asks for the five hooks and the water's substeps; Link's update gives his body and counts his updates")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    asks_and_wiring()
    gate_text()
    with tempfile.TemporaryDirectory(prefix="above60-ledge-run-") as directory:
        for test in (drive_test, loop_test, water_test, song_test):
            test(args.cc, directory)


if __name__ == "__main__":
    main()
