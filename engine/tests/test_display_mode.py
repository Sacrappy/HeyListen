"""The Insert/F1 menu's Display mode row (Windowed / Borderless / Fullscreen, saved as fullscreen=0/1/2 in hookshot_options.ini):
checks source contracts across vk_menu.cpp, host/display.c, host/posix/display_sdl.c, host/hos_sdl.c, and recomplauncher.json,
and compiles and runs tx_display_mode, hn_display_mode, and fullscreen_tick against mock window state.

    python -I -B tests/test_display_mode.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import between, block  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def need(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def check_sources():
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    need('{TAB_GRAPHICS, "Display mode", &opts.fullscreen, 0, 2, 1, tx_display_mode,' in menu,
         "vk_menu.cpp: Display mode row (0..2) missing on Graphics tab")
    need('{TAB_GRAPHICS, "Screen", &opts.screen, 0, kMaxScreens, 1, tx_screen,' in menu,
         "vk_menu.cpp: Screen row missing on Graphics tab")
    ini_key = block(menu, "const char* ini_key(const Row& r) {")
    need('r.value == &opts.fullscreen) return "fullscreen";' in ini_key,
         "vk_menu.cpp: ini_key must map &opts.fullscreen to fullscreen")

    disp = (ROOT / "host" / "display.c").read_text(encoding="utf-8").replace("\r\n", "\n")
    style = block(disp, "static void fullscreen_style(HWND w) {")
    need("fullscreen == 2" in style and "popup_wanted" in style and "monitor_info_for" in style,
         "display.c: fullscreen_style must enable WS_POPUP for Fullscreen (2) or VRR and target selected screen")
    fs = block(disp, "static void set_fullscreen(HWND w, int on) {")
    need("if (on > 2) on = 2;" in fs and "if (!fullscreen)" in fs,
         "display.c: set_fullscreen must clamp 0..2 and preserve windowed_place across Borderless <-> Fullscreen")
    scr = block(disp, "static void set_screen(HWND w, int screen) {")
    need("monitor_info_for(w, screen, &mi)" in scr and "WM_APP + 11" in disp,
         "display.c: set_screen and WM_APP + 11 must target selected monitor")

    sdl_disp = (ROOT / "host" / "posix" / "display_sdl.c").read_text(encoding="utf-8").replace("\r\n", "\n")
    sdl_fs = block(sdl_disp, "static void set_fullscreen(int on) {")
    need("if (on > 2) on = 2;" in sdl_fs, "display_sdl.c: set_fullscreen must clamp 0..2")
    need("(int)(wp > 2 ? 2 : wp)" in sdl_disp, "display_sdl.c: WM_APP + 7 must preserve modes 0..2")
    need("WM_APP + 11" in sdl_disp and "hos_posix_display_count" in sdl_disp and "hos_posix_display_name" in sdl_disp,
         "display_sdl.c: Linux display enumeration and WM_APP + 11 screen selection missing")

    hos_sdl = (ROOT / "host" / "hos_sdl.c").read_text(encoding="utf-8").replace("\r\n", "\n")
    need("on == 2 ? 0x0001u : on ? 0x1001u" in hos_sdl,
         "hos_sdl.c: SDL2 must distinguish Borderless (0x1001) and Fullscreen (0x0001)")


FIXTURE = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using HWND = void*;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
constexpr unsigned WM_APP = 0x8000;
constexpr int GWL_STYLE = -16;
constexpr long WS_CAPTION = 0x00C00000L;

#define MENU_HINTS(fn, lo, ...)                                                                                        \
    const char* fn(int v, int variant) {                                                                               \
        static const char* const t[] = {__VA_ARGS__};                                                                  \
        return variant <= 0 && v - (lo) >= 0 && v - (lo) < (int)(sizeof t / sizeof t[0]) ? t[v - (lo)] : nullptr;      \
    }

static struct { int fullscreen = 0; int screen = 0; } opts;
static std::vector<std::string> screen_names;
static bool menu_refresh = false;
static HWND fake_window = (HWND)(uintptr_t)1;
static int posted_count = 0;
static int posted_mode = -1;
static int posted_screen_count = 0;
static int posted_screen = -1;
static int save_calls = 0;
static long window_style = WS_CAPTION;

static HWND app_window() { return fake_window; }
static long GetWindowLongA(HWND, int idx) { return idx == GWL_STYLE ? window_style : 0; }
static int PostMessageA(HWND, unsigned msg, WPARAM wp, LPARAM) {
    if (msg == WM_APP + 7) {
        posted_count++;
        posted_mode = (int)wp;
        window_style = posted_mode ? 0 : WS_CAPTION;
    } else if (msg == WM_APP + 11) {
        posted_screen_count++;
        posted_screen = (int)wp;
    } else {
        assert(false);
    }
    return 1;
}
static void save() { save_calls++; }

@TX_DISPLAY_MODE@
@HN_DISPLAY_MODE@
@TX_SCREEN@
@HN_SCREEN@
@SCREEN_TICK@
@FULLSCREEN_TICK@

int main() {
    assert(std::strcmp(tx_display_mode(0), "Windowed") == 0);
    assert(std::strcmp(tx_display_mode(1), "Borderless") == 0);
    assert(std::strcmp(tx_display_mode(2), "Fullscreen") == 0);
    assert(hn_display_mode(0, 0) && hn_display_mode(1, 0) && hn_display_mode(2, 0));

    screen_names = {"Monitor A (2560x1440, primary)", "Monitor B (1920x1080)"};
    assert(std::strcmp(tx_screen(0), "Auto") == 0);
    assert(std::strcmp(tx_screen(1), "1: Monitor A (2560x1440, primary)") == 0);
    assert(std::strcmp(tx_screen(2), "2: Monitor B (1920x1080)") == 0);
    assert(std::strstr(tx_screen(3), "not connected") != nullptr);
    assert(hn_screen(0, 0) && hn_screen(2, 0) && hn_screen(3, 0));

    opts.screen = 2;
    screen_tick();
    screen_tick();
    assert(posted_screen_count == 1 && posted_screen == 2);
    opts.screen = 0;
    screen_tick();
    assert(posted_screen_count == 2 && posted_screen == 0);

    // Selecting Borderless (1) and Fullscreen (2) posts WM_APP + 7 and settles without overwriting mode 2 back to 1.
    opts.fullscreen = 2;
    fullscreen_tick();
    assert(posted_count == 1 && posted_mode == 2);
    for (int i = 0; i < 65; i++) fullscreen_tick();
    assert(opts.fullscreen == 2 && save_calls == 0);

    // Switching Fullscreen (2) -> Borderless (1) posts mode 1 and stays 1.
    opts.fullscreen = 1;
    fullscreen_tick();
    assert(posted_count == 2 && posted_mode == 1);
    for (int i = 0; i < 65; i++) fullscreen_tick();
    assert(opts.fullscreen == 1 && save_calls == 0);

    // Pressing F11 externally restores WS_CAPTION -> syncs opts.fullscreen back to 0 and saves.
    window_style = WS_CAPTION;
    fullscreen_tick();
    assert(opts.fullscreen == 0 && save_calls == 1 && menu_refresh);

    // Pressing F11 externally clears WS_CAPTION -> syncs opts.fullscreen to 1 (Borderless) and saves.
    menu_refresh = false;
    window_style = 0;
    fullscreen_tick();
    assert(opts.fullscreen == 1 && save_calls == 2 && menu_refresh);

    std::puts("test_display_mode: ok");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_sources()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    hn = between(menu, "MENU_HINTS(hn_display_mode,", ")\n") + ")"
    src = (FIXTURE
           .replace("@TX_DISPLAY_MODE@", block(menu, "const char* tx_display_mode(int v) {"))
           .replace("@HN_DISPLAY_MODE@", hn)
           .replace("@TX_SCREEN@", block(menu, "const char* tx_screen(int v) {"))
           .replace("@HN_SCREEN@", block(menu, "const char* hn_screen(int v, int variant) {"))
           .replace("@SCREEN_TICK@", block(menu, "void screen_tick() {"))
           .replace("@FULLSCREEN_TICK@", block(menu, "void fullscreen_tick() {")))
    with tempfile.TemporaryDirectory(prefix="display-mode-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture.exe"
        cpp.write_text(src, encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe"):
            flags = ["/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS", f"/Fe:{exe}"]
        else:
            flags = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-o", str(exe)]
        subprocess.run([args.cxx, *flags, str(cpp)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
