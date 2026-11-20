"""After the Insert menu the pads wait input by input, not all at once (host/nn_hid.c, pads_merge).

The press that closes the menu, and every pad input still down from it, must not reach the game until let go. Each
waits on its own: one that stays down (a worn trigger, a Guide button, something resting on a second pad) used to keep
every button and both sticks of every pad from the game. Production's pads_read, bound_pad_buttons and pads_merge
are compiled with fakes for SDL, XInput and the clock; no device is read.

python -I -B tests/test_pads_after_menu.py [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PRELUDE = r"""#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef long LONG;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef void *HMODULE;
#define WINAPI
#define ERROR_SUCCESS 0
#define XUSER_MAX_COUNT 4
enum { XINPUT_GAMEPAD_DPAD_UP = 1, XINPUT_GAMEPAD_DPAD_DOWN = 2, XINPUT_GAMEPAD_DPAD_LEFT = 4, XINPUT_GAMEPAD_DPAD_RIGHT = 8,
       XINPUT_GAMEPAD_START = 16, XINPUT_GAMEPAD_BACK = 32, XINPUT_GAMEPAD_LEFT_THUMB = 64, XINPUT_GAMEPAD_RIGHT_THUMB = 128,
       XINPUT_GAMEPAD_LEFT_SHOULDER = 256, XINPUT_GAMEPAD_RIGHT_SHOULDER = 512, XINPUT_GAMEPAD_A = 4096,
       XINPUT_GAMEPAD_B = 8192, XINPUT_GAMEPAD_X = 16384, XINPUT_GAMEPAD_Y = 32768 };
typedef struct { WORD wButtons; uint8_t bLeftTrigger, bRightTrigger; int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
static HMODULE LoadLibraryA(const char *name) { (void)name; return NULL; }
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0
static HMODULE LoadLibraryExA(const char *name, void *file, DWORD flags) { (void)name; (void)file; (void)flags; return NULL; }
static void *GetProcAddress(HMODULE m, const char *name) { (void)m; (void)name; return NULL; }
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
static void hos_log(const char *format, ...) { (void)format; }
static int64_t now_ns = 1000000000LL;
static int64_t hos_now_ns(void) { return now_ns; }
static int menu_open;
int rx_option(int id, int otherwise) { return id == 1 ? menu_open : id == 11 || id == 16 || id == 17 ? 0 : otherwise; }
static uint64_t pads_down;      /* (by position, 1 << HOS_PAD_*: every pad SDL has, merged as pad_sdl.c merges them) */
static int32_t pads_stick[4];
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) { *buttons |= pads_down; memcpy(stick, pads_stick, sizeof pads_stick); return 2; }
static void binds_load(void) {}
/* (main's XInput is read on a thread of its own, after Longshot #2124: here no XInput pad, SDL has them) */
static void xinput_read(int64_t now, uint64_t *raw, int32_t s[4]) { (void)now; (void)raw; (void)s; }
static volatile int64_t input_pad_ns;   /* (Auto's device in use: written by pads_read, not looked at here) */
static volatile LONG pad_bind[HOS_SW_N];
static volatile LONG menu_bind[2] = {-1, -1};
"""

MAIN = r"""
/* one read of the game's: what the pads give it */
static uint64_t game(int32_t stick[4]) {
    now_ns += 16000000;   /* (past pads_read's 2 ms cache) */
    uint64_t buttons = 0;
    memset(stick, 0, 4 * sizeof *stick);
    pads_merge(&buttons, stick);
    return buttons;
}
#define PAD(p) (1ULL << HOS_PAD_##p)
#define SW(b) (1ULL << HOS_SW_##b)

int main(void) {
    for (int i = 0; i < HOS_SW_N; i++) pad_bind[i] = pad_default[i];
    int32_t s[4];
    CHECK(game(s) == 0);
    pads_down = PAD(SOUTH); CHECK(game(s) == SW(B));   /* (by position: the bottom face button is the Switch's B) */
    pads_down = 0; CHECK(game(s) == 0);

    /* A trigger that stays down through the menu (worn, or past its rest), the menu closed with East (A) */
    menu_open = 1;
    pads_down = PAD(LTRIGGER); CHECK(game(s) == 0);
    pads_stick[0] = 30000; pads_down |= PAD(EAST); CHECK(game(s) == 0 && s[0] == 0);   /* (the menu's own, sticks too) */
    menu_open = 0;
    CHECK(game(s) == 0);                    /* the press that closed it and the trigger wait, */
    CHECK(s[0] == 30000);                   /* the sticks count at once, */
    pads_down = PAD(LTRIGGER); pads_stick[0] = 0;
    CHECK(game(s) == 0);
    pads_down |= PAD(SOUTH); CHECK(game(s) == SW(B));   /* and another button counts with the trigger still down, */
    pads_down = PAD(LTRIGGER) | PAD(EAST); CHECK(game(s) == SW(A));   /* also the one that closed it, pressed again */
    pads_down = 0; CHECK(game(s) == 0);
    pads_down = PAD(LTRIGGER); CHECK(game(s) == SW(ZL));   /* the trigger once it was let go */
    pads_down = 0; CHECK(game(s) == 0);

    /* The menu closed by a press no read saw while it was open: it waits too */
    menu_open = 1; CHECK(game(s) == 0);
    menu_open = 0; pads_down = PAD(EAST); CHECK(game(s) == 0);
    CHECK(game(s) == 0);
    pads_down = 0; CHECK(game(s) == 0);
    pads_down = PAD(EAST); CHECK(game(s) == SW(A));
    pads_down = 0; CHECK(game(s) == 0);

    /* Something resting on a second pad: its button waits, the first pad's count */
    menu_open = 1; pads_down = PAD(NORTH); CHECK(game(s) == 0);
    menu_open = 0; CHECK(game(s) == 0);
    pads_down |= PAD(WEST) | PAD(DOWN); CHECK(game(s) == (SW(Y) | SW(DOWN)));
    pads_down = 0; CHECK(game(s) == 0);
    puts("PASS: after the Insert menu each pad input waits until let go; the others and the sticks count at once");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    state = hid[hid.index("static volatile LONG pad_raw_now;"):hid.index("uint32_t hos_pad_raw(")]
    code = "\n".join((
        block(api, "enum { HOS_SW_A") + ";", block(api, "enum { HOS_PAD_SOUTH") + ";",
        re.search(r"^enum \{ HOS_PAD_TOUCHPAD = HOS_PAD_N \};", api, re.M).group(0),
        PRELUDE,
        block(hid, "static const signed char pad_default[HOS_SW_N] =") + ";",
        block(hid, "static void stick_max("), block(hid, "static uint64_t bound_pad_buttons("), state,
        block(hid, "int32_t hos_stick_axis("),
        block(hid, "static void pads_read("),
        block(hid, "static void pads_merge("),
        MAIN))
    with tempfile.TemporaryDirectory(prefix="hl-pads-after-menu-") as tmp:
        src = Path(tmp) / "t.c"
        src.write_text(code, encoding="utf-8")
        exe = Path(tmp) / "t.exe"
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-o", str(exe), str(src)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
