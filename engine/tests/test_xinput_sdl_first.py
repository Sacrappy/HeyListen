# Copyright (C) 2026 Sacrappy
"""HL_XINPUT_SDL_FIRST: XInput's pads left out while SDL has a controller open (host/nn_hid.c pads_read; after Hookshot
731898a2, #1480 #1583).

Steam Input shows a pad SDL already reads as an Xbox pad on XInput too, and the two merged gave doubled or swapped
buttons. With the switch on, XInput's reading is not merged while SDL has a pad; with it off (the default), or with no
SDL pad, both are merged as before. Production's pads_read is compiled with tests/test_pads_after_menu.py's fakes; the
XInput reader is a fake that reports the top face button.

python -I -B tests/test_xinput_sdl_first.py [--cc gcc]
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
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves this folder off the path)
import test_pads_after_menu as pam   # noqa: E402

STUB = "static void xinput_read(int64_t now, uint64_t *raw, int32_t s[4]) { (void)now; (void)raw; (void)s; }\n"
FAKE = ("static int xinput_reads;\n"
        "static void xinput_read(int64_t now, uint64_t *raw, int32_t s[4]) { (void)now; (void)s; xinput_reads++;"
        " *raw |= 1ULL << HOS_PAD_NORTH; }\n")

MAIN = r"""
static uint64_t game(int32_t stick[4]) {
    now_ns += 16000000;   /* (past pads_read's 2 ms cache) */
    uint64_t buttons = 0;
    memset(stick, 0, 4 * sizeof *stick);
    pads_read(&buttons, stick);
    return buttons;
}
#define PAD(p) (1ULL << HOS_PAD_##p)
#define SW(b) (1ULL << HOS_SW_##b)
int main(int argc, char **argv) {
    const int on = argc > 1 && !strcmp(argv[1], "on");
    for (int i = 0; i < HOS_SW_N; i++) pad_bind[i] = pad_default[i];
    int32_t s[4];
    pads_down = 0;
    sdl_count = 0;   /* no SDL pad: XInput's counts either way */
    CHECK(game(s) == SW(X) && xinput_reads == 1);
    sdl_count = 1; pads_down = PAD(SOUTH);   /* an SDL pad open: */
    const uint64_t got = game(s);
    if (on) CHECK(got == SW(B) && xinput_reads == 1);   /* XInput's left out */
    else CHECK(got == (SW(B) | SW(X)) && xinput_reads == 2);   /* both merged, as before */
    printf("PASS: HL_XINPUT_SDL_FIRST %s: %s\n", on ? "on" : "off",
           on ? "XInput's pads left out while SDL has one open" : "XInput's and SDL's pads merged as before");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    if 'getenv("HL_XINPUT_SDL_FIRST")' not in hid or "if (!(sdl_first && sdl_pads)) xinput_read(now, &raw, s);" not in hid:
        raise SystemExit("FAIL: pads_read no longer gates XInput on HL_XINPUT_SDL_FIRST and SDL's pads")
    prelude = pam.PRELUDE
    if STUB not in prelude or "return 2; }" not in prelude:
        raise SystemExit("FAIL: test_pads_after_menu.py's fakes changed: update this test")
    prelude = prelude.replace(STUB, FAKE).replace("static uint64_t pads_down;", "static int sdl_count;\nstatic uint64_t pads_down;")
    prelude = prelude.replace("return 2; }", "return sdl_count; }")
    state = hid[hid.index("static volatile LONG pad_raw_now;"):hid.index("uint32_t hos_pad_raw(")]
    code = "\n".join((
        pam.block(api, "enum { HOS_SW_A") + ";", pam.block(api, "enum { HOS_PAD_SOUTH") + ";",
        re.search(r"^enum \{ HOS_PAD_TOUCHPAD = HOS_PAD_N \};", api, re.M).group(0),
        prelude,
        pam.block(hid, "static const signed char pad_default[HOS_SW_N] =") + ";",
        pam.block(hid, "static void stick_max("), pam.block(hid, "static uint64_t bound_pad_buttons("), state,
        pam.block(hid, "int32_t hos_stick_axis("),
        pam.block(hid, "static void pads_read("),
        "static void use_menu_hold(void) { (void)pad_after_menu; (void)pad_raw_game; (void)menu_open; }",
        MAIN))
    with tempfile.TemporaryDirectory(prefix="hl-xinput-sdl-first-") as tmp:
        src = Path(tmp) / "t.c"
        src.write_text(code, encoding="utf-8")
        exe = Path(tmp) / "t.exe"
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-o", str(exe), str(src)], check=True)
        for arm, value in (("off", None), ("on", "1")):
            env = {k: v for k, v in os.environ.items() if k != "HL_XINPUT_SDL_FIRST"}
            if value:
                env["HL_XINPUT_SDL_FIRST"] = value
            r = subprocess.run([str(exe), arm], capture_output=True, text=True, env=env)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            if r.returncode:
                raise SystemExit(1)


if __name__ == "__main__":
    main()
