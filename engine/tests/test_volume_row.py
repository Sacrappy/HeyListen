"""The Insert menu's Volume row (Gameplay tab) reaches the game's mix (host/nn_audio.c), after Hookshot 2c87e357.

The row is a slider from 0 to 100 in steps of 5, kept in hookshot_options.ini as volume=. Its runtime option id is
HL_OPT_VOLUME (gpu/vk/vk_opt_ids.h, 94: Hookshot's 44 is the Skip title screen's here), asked by host/nn_audio.c as each frame
is mixed and answered by gpu/vk/vk_replay.cpp. The gain is the percentage squared, ramped over the frame, after the limiter;
the production lines are compiled and run here. The id is in vk_opt_ids.h and not vk_api.h, which is part of the shader stamp.

python -I -B tests/test_volume_row.py [--cc gcc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    ids = read("gpu", "vk", "vk_opt_ids.h")
    values = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define\s+(HL_OPT_\w+)\s+(\d+)", ids)}
    check(values.get("HL_OPT_VOLUME") == 94, "HL_OPT_VOLUME is not 94")
    check(list(values.values()).count(94) == 1, "the id is not unique in vk_opt_ids.h")
    api = read("gpu", "vk", "vk_api.h")
    check("HL_OPT_VOLUME" not in api and not re.search(r"=\s*94\b", api), "vk_api.h (the shader stamp) has the id")

    menu = read("gpu", "vk", "vk_menu.cpp")
    check('{TAB_GAMEPLAY, "Volume", &opts.volume, 0, 100, 5, tx_percent,' in menu, "the Gameplay tab has no Volume row 0..100 by 5")
    check("r.value == &opts.volume" in menu[menu.index("bool is_slider("):menu.index("bool stops_at_ends(")], "the row is not a slider")
    check("r.value == &opts.volume" in menu[menu.index("bool stops_at_ends("):][:600], "the row does not stop at its ends")
    check("opts.volume = opts.volume / 5 * 5;" in menu, "a hand-edited value is not put on the steps")
    check("int volume = 100;" in read("gpu", "vk", "vk_menu.h"), "the default is not 100%")
    check("id == HL_OPT_VOLUME" in read("gpu", "vk", "vk_replay.cpp"), "the renderer does not answer the option")
    check("Volume | 0% to 100%" in read("readme.md"), "the readme's table has no Volume row")

    audio = read("host", "nn_audio.c")
    start = audio.index("static float volume_gain = 1.0f;")
    code = audio[start:audio.index("            if (a != a) a = 0;", start)]
    check("rx_option(HL_OPT_VOLUME, 100)" in code, "host/nn_audio.c does not ask for the option")
    gain = audio[start:audio.index("    for (int i = 0; i < FRAME; i++)", start)]
    prog = r"""#include <stdio.h>
#include <stdlib.h>
#define FRAME 240
#define HL_OPT_VOLUME 94
static int percent_now = 100;
int rx_option(int id, int otherwise) { return id == HL_OPT_VOLUME ? percent_now : otherwise; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static float frame(int pct, float *first, float *last) {
    percent_now = pct;
    float out = 1.0f;
""" + "    " + gain.replace("\n", "\n    ") + r"""
    *first = volume_from + volume_step * 1.0f;
    *last = volume_from + volume_step * (float)FRAME;
    return volume_gain;
}
int main(void) {
    float f, l;
    frame(100, &f, &l); CHECK(f == 1.0f && l == 1.0f);       /* the default changes nothing */
    frame(50, &f, &l); CHECK(l > 0.2499f && l < 0.2501f && f < 1.0f && f > l);   /* a quarter of the power, ramped down */
    frame(50, &f, &l); CHECK(f == l);                         /* then steady */
    frame(0, &f, &l); CHECK(l == 0.0f);                       /* muted */
    frame(-5, &f, &l); CHECK(l == 0.0f);                      /* clamped */
    frame(400, &f, &l); CHECK(l == 1.0f);
    puts("PASS");
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="hl-volume-") as tmp:
        src = Path(tmp) / "t.c"
        src.write_text(prog, encoding="utf-8")
        exe = Path(tmp) / "t.exe"
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-o", str(exe), str(src)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout + r.stderr, end="")
        if r.returncode:
            raise SystemExit(1)
    print("PASS: the Volume row, its option id and the mix's gain")


if __name__ == "__main__":
    main()
