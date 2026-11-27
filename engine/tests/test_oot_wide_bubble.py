"""The speech bubble over a talking NPC (BalloonMessage_00) stays over its speaker at every picture shape.

The game anchors the bubble at the speaker's place on the screen (-1..1) times the UI canvas's half size: x * 1920 in s0,
y * 1080 in s1 (main+0x20c2b0..0x20c2c8, then stp s0, s1 to the bubble). Its screen draws through the UI projection that
nx2/oot_wide.py's HUD site scales across by hos_ui_qx and up by hos_ui_qy (0: the game's own, a 16:9 picture), so the
anchor has to be divided by them, or the bubble is pulled toward the middle of a wider screen. This runs those seven
instructions in C with whatever C nx2/oot_wide.py adds after them, as the lifter would, and draws the anchor through the
scaled projection. No game or image is used.

python -I -B tests/test_oot_wide_bubble.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_wide  # noqa: E402

# (main offset, the game's instruction word there, what it does as C on ctx)
STEPS = (
    (0x20c2b0, 0x52a89e08, "ctx.x[8] = 0x44f00000u;"),        # mov w8, #1920.0
    (0x20c2b4, 0x1e270100, "s_bits(0, (uint32_t)ctx.x[8]);"), # fmov s0, w8
    (0x20c2b8, 0x52a890e8, "ctx.x[8] = 0x44870000u;"),        # mov w8, #1080.0
    (0x20c2bc, 0x1e200820, "s_put(0, s_get(1) * s_get(0));"), # fmul s0, s1, s0
    (0x20c2c0, 0x1e270101, "s_bits(1, (uint32_t)ctx.x[8]);"), # fmov s1, w8
    (0x20c2c4, 0xf9401fe8, "ctx.x[8] = 0x1000;"),             # ldr x8, [sp, #0x38]: the bubble
    (0x20c2c8, 0x1e210841, "s_put(1, s_get(2) * s_get(1));"), # fmul s1, s2, s1
)

FIXTURE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
volatile float hos_ui_qx, hos_ui_qy;
static Ctx ctx;
static float s_get(int n) { float f; memcpy(&f, ctx.vreg[n], 4); return f; }
static void s_put(int n, float f) { ctx.vreg[n][0] = ctx.vreg[n][1] = 0; memcpy(ctx.vreg[n], &f, 4); }
static void s_bits(int n, uint32_t u) { float f; memcpy(&f, &u, 4); s_put(n, f); }
AFTER_C
/* the anchor the game stores for a speaker at (nx, ny) on the screen */
static void anchor(float nx, float ny, float *x, float *y) {
    memset(&ctx, 0, sizeof ctx);
    s_put(1, nx), s_put(2, ny);   /* ldp s1, s2, [sp, #0xf0] */
STEPS_C
    *x = s_get(0), *y = s_get(1);
}
/* drawn through its screen's projection (the 3840 x 2160 canvas, across times qx, up times qy), it lands on the speaker */
static void over(const char *shape, float qx, float qy) {
    static const float at[][2] = {{0.0f, 0.0f}, {0.5f, -0.25f}, {-0.9f, 0.6f}, {1.0f, -1.0f}};
    hos_ui_qx = qx, hos_ui_qy = qy;
    for (int i = 0; i < 4; i++) {
        float x, y;
        anchor(at[i][0], at[i][1], &x, &y);
        const float px = x / 1920.0f * (qx > 0.0f ? qx : 1.0f), py = y / 1080.0f * (qy > 0.0f ? qy : 1.0f);
        const float dx = px - at[i][0], dy = py - at[i][1];
        if (dx > 1e-4f || dx < -1e-4f || dy > 1e-4f || dy < -1e-4f) {
            fprintf(stderr, "FAIL: %s: a speaker at (%g, %g) gets its bubble at (%g, %g)\n", shape, at[i][0], at[i][1], px, py);
            exit(1);
        }
    }
}
int main(void) {
    const float game = 16.0f / 9.0f;
    over("16:9", 0.0f, 0.0f);
    over("21:9", game / (3440.0f / 1440.0f), 1.0f);
    over("32:9", game / (5120.0f / 1440.0f), 1.0f);
    over("16:10", game / 1.6f, 1.0f);
    over("4:3, the HUD at 90%", 0.9f * game / (4.0f / 3.0f), 0.9f);
    puts("PASS: the speech bubble's anchor lands on its speaker at 16:9, 21:9, 32:9, 16:10 and 4:3");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    words = {off: word for off, word, _ in STEPS}
    sites = {off: (word, text) for off, word, text in oot_wide.SITES if off in words}
    for off, (word, _) in sites.items():
        assert word == words[off], "main+%#x: oot_wide.py expects %08x, the game has %08x" % (off, word, words[off])
    after = "".join("static void after_%x(Ctx *c) {\n%s\n}\n" % (off, text) for off, (_, text) in sites.items())
    steps = "".join("    %s%s\n" % (code, " after_%x(&ctx);" % off if off in sites else "") for off, _, code in STEPS)
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="oot-wide-bubble-") as temporary:
        folder = Path(temporary)
        source = folder / "fixture.c"
        source.write_text(FIXTURE.replace("AFTER_C", after).replace("STEPS_C", steps), encoding="utf-8")
        executable = folder / "fixture.exe"
        for command in ([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)],
                        [str(executable)]):
            result = subprocess.run(command, creationflags=flags, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=30)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
