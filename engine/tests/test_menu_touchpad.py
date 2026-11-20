# Copyright (C) 2026 Sacrappy

"""A DualSense / DualShock 4 touchpad click opens and closes the Insert menu, as Home does.

SDL reports the click as its gamepad touchpad button (index 20 in SDL2's and SDL3's enums). host/pad_sdl.c turns it into
HOS_PAD_TOUCHPAD (vk_api.h), one past HOS_PAD_N: a position the menu reads (host->pad_raw) and the game's controls cannot
reach, because host/nn_hid.c only maps positions below HOS_PAD_N to the Switch's buttons (the defaults have no touchpad
click, and hos_bind and hookshot_options.ini refuse the code). So the click never reaches the game, while the menu is open,
or when it opens. gpu/vk/vk_menu.cpp's menu_tick toggles on it with Home and Select + Start, and the Controls page's
"press a button" wait ignores it as it ignores Home. The docs say so.

This reads the sources (no controller on the test PCs); the position arithmetic is checked on the parsed enums.

    python -I -B tests/test_menu_touchpad.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT.parent / "docs"


def read(p):
    return p.read_text(encoding="utf-8")


def enum_values(text, first):
    """The values of the enum that starts with `first`, by position (explicit `= n` honoured)."""
    m = re.search(r"enum\s*\{\s*" + re.escape(first) + r"(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("FAIL: no enum starting with " + first)
    body = re.sub(r"/\*.*?\*/", "", first + m.group(1), flags=re.S)
    out, n = {}, 0
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        name, _, val = (x.strip() for x in item.partition("="))
        if val:
            n = out[val] if val in out else int(val)
        out[name] = n
        n += 1
    return out


def need(ok, what):
    if not ok:
        print("FAIL: " + what)
        sys.exit(1)


def main():
    api = read(ROOT / "gpu/vk/vk_api.h")
    pad = read(ROOT / "host/pad_sdl.c")
    hid = read(ROOT / "host/nn_hid.c")
    menu = read(ROOT / "gpu/vk/vk_menu.cpp")

    pos = enum_values(api, "HOS_PAD_SOUTH")
    need(pos["HOS_PAD_N"] == 17, "HOS_PAD_N moved (the bindings and the prompts' tables use it)")
    need(re.search(r"enum\s*\{\s*HOS_PAD_TOUCHPAD\s*=\s*HOS_PAD_N\s*\}", api), "HOS_PAD_TOUCHPAD is not one past HOS_PAD_N")
    touch = pos["HOS_PAD_N"]
    need(touch < 32, "the touchpad bit does not fit hos_pad_raw's 32 bits")

    # (the SDL side: button 20, SDL2's SDL_CONTROLLER_BUTTON_TOUCHPAD and SDL3's SDL_GAMEPAD_BUTTON_TOUCHPAD)
    btn = enum_values(pad, "BTN_A")
    need(btn["BTN_TOUCHPAD"] == 20 and btn["BTN_N"] == 21, "BTN_TOUCHPAD is not SDL's button 20")
    need(btn["BTN_RIGHT"] == 14 and btn["BTN_MISC1"] == 15, "the SDL button order moved")
    need(re.search(r"hos_sdl_pad_button\(gc, BTN_TOUCHPAD\)\)\s*b \|= 1ULL << HOS_PAD_TOUCHPAD;", pad),
         "pad_sdl.c does not turn the touchpad click into HOS_PAD_TOUCHPAD")
    # (the loop over the other buttons stops at the D-pad's right: the touchpad is not a position below HOS_PAD_N)
    need(len(re.findall(r"for \(int k = BTN_A; k <= BTN_RIGHT; k\+\+\)", pad)) == 2, "the button loops changed")

    # (the game's side: nothing binds a position at or above HOS_PAD_N, and no default is the touchpad)
    need("HOS_PAD_TOUCHPAD" not in hid, "nn_hid.c names the touchpad: it must stay out of the game's controls")
    need("pad_bind[i] = v >= 0 && v < HOS_PAD_N ? v : -1;" in hid, "the ini may bind a position at or above HOS_PAD_N")
    need("code < -1 || code >= HOS_PAD_N) return;" in hid, "hos_bind may bind a position at or above HOS_PAD_N")
    block = hid[hid.index("pad_default[HOS_SW_N]"):hid.index("static const unsigned char key_default")]
    names = re.findall(r"HOS_PAD_[A-Z]+", block)
    need(len(names) == 16 and "HOS_PAD_GUIDE" not in names and all(pos[n] < pos["HOS_PAD_N"] for n in names),
         "the default bindings changed: check none is the touchpad's click")
    need("pads_read" in hid and "uint32_t hos_pad_raw(void)" in hid, "nn_hid.c no longer hands the menu the raw pads")
    # (while the menu is open the game gets no pad input; after it closes each input the menu had waits until let go: c45be62f)
    need(re.search(r"if \(menu \|\| menu_was\) pad_after_menu \|= \(uint32_t\)pad_raw_now;\s*else pad_after_menu &= \(uint32_t\)pad_raw_now;"
                   r"\s*menu_was = menu;\s*if \(!menu\) \{", hid),
         "the pads are no longer the menu's alone while it is open")

    # (the menu: toggles on it, does not bind it)
    tick = menu[menu.index("void menu_tick() {"):]
    need(re.search(r"if \(pad\(HOS_PAD_GUIDE\) \|\| \(opts\.touchpad_menu && pad\(HOS_PAD_TOUCHPAD\)\) \|\| chord_now\) press\[0\] = true;", tick),
         "the touchpad click does not toggle the menu like Home (unless the Touchpad opens menu row is off: tests/test_input_options.py)")
    need(re.search(r"menu_only = 1u << HOS_PAD_GUIDE \| 1u << HOS_PAD_TOUCHPAD;", tick), "the binding wait takes the touchpad click")
    need("(raw & ~menu_only) == 0" in tick and "pad_press & ~menu_only" in tick, "the binding wait does not skip menu_only")
    need("touchpad click" in menu, "the Menu shortcut row does not mention the touchpad")

    # (the Controls page's Open menu: one gamepad input or two together, saved as pad_menu=<a>,<b>, beside the fixed ways)
    need(re.search(r"enum\s*\{\s*HOS_BTN_OPEN_MENU\s*=\s*100\s*\}", api), "HOS_BTN_OPEN_MENU is missing")
    need("static volatile LONG menu_bind[2] = {-1, -1};" in hid, "the Open menu is not unset by default")
    need("InterlockedExchange(&menu_bind[0], -1);" in hid and "InterlockedExchange(&menu_bind[1], -1);" in hid,
         "Reset to defaults does not clear the Open menu")
    need('!strcmp(line + 4, "menu")' in hid and "a < HOS_PAD_N" in hid, "the ini's pad_menu line is not read (positions below HOS_PAD_N)")
    need(re.search(r"if \(set == HOS_BIND_PAD && button == HOS_BTN_OPEN_MENU\) return slot >= 0", hid), "hos_binding does not give the Open menu")
    need(re.search(r"button == HOS_BTN_OPEN_MENU\) \{.*?code >= HOS_PAD_N\) return;", hid, re.S), "hos_bind takes a code the game cannot get")
    # (a combination held opens the menu and is taken out of what the game gets: after pad_raw_now is set, before the bindings)
    strip = hid.index("if (mm && (raw & mm) == mm) raw &= ~mm;")
    need(hid.index("InterlockedExchange(&pad_raw_now, (LONG)raw);") < strip < hid.index("pad_raw_game = raw;") < hid.index("bound_pad_buttons(raw)", strip),
         "the Open menu's inputs are not taken out of the game's input after the menu's copy")
    need(re.search(r"HOS_BIND_PAD, HOS_BTN_OPEN_MENU, k\); b >= 0 && b < HOS_PAD_N\) combo \|= 1u << b;", tick), "the menu does not read the Open menu")
    need(re.search(r"\(combo && listening != 1 && \(raw & combo\) == combo && \(pad_held & combo\) != combo\)", tick),
         "the combination does not toggle the menu on its press (and not while a binding waits)")
    need(menu.index("(combo && listening != 1") < menu.index("pad_held = raw;"), "pad_held moves before the combination is tested")
    need('{HOS_BTN_OPEN_MENU, "Open menu"}' in menu, "the Controls page has no Open menu row")
    need("bool menu_row(int i)" in menu and "controls[i].button != HOS_BTN_OPEN_MENU" in menu, "the Open menu row is a keys-only one")
    need('put("pad_menu=%d,%d\\n"' in menu, "the menu does not save pad_menu")
    need("host->bind(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 0, first);" in tick and "host->bind(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 1, second);" in tick,
         "a bound combination is not stored in both slots")
    need("700000000LL" in tick, "the second input of a combination has no time limit")
    # (a log line when a pad opens: its SDL mapping and which inputs open the menu; the first press of each, once)
    need("pad_log_open(gc);" in pad and "hos_sdl_pad_mapping(gc, map, sizeof map)" in pad, "pad_sdl.c does not log an opened pad's mapping")
    need("pad_log_presses(i, gc);" in pad and "the Insert menu opens with Home, the touchpad click" in pad, "pad_sdl.c does not log the menu inputs")

    # (the docs)
    for p in (ROOT / "readme.md", DOCS / "USER_GUIDE.md"):
        need("touchpad" in read(p), p.name + " does not mention the touchpad")
        need("Open menu" in read(p), p.name + " does not mention the Open menu control")
    need("pad_menu" in read(ROOT / "readme.md"), "readme.md does not describe pad_menu")
    print("PASS: the touchpad click (position %d) opens and closes the menu, binds to nothing and never reaches the game" % touch)


if __name__ == "__main__":
    main()
