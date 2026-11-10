"""The overlays (the start-up screen, the menu, the performance overlay) over the window's image: menu_record draws them with
Dear ImGui in one frame and one recording: the overlay's font first, the start-up screen under the menu, then the performance
overlay (overlay_draw: the FPS box or the full panel, F10; HeyListen replaced upstream's draw_fps with vk_overlay.cpp's) and the
menu over it; nothing at all (no ImGui frame, no render pass, only ui::idle letting old textures go) while none shows (but one
frame that bakes the glyphs after a resize), and no "0 FPS" over the start-up screen.

python -I -B tests/test_fps_overlay_order.py [--cxx g++] [--source path/to/vk_menu.cpp]
Executes the real menu_record with recording stubs; no GPU or game data.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_menu.cpp")
    a = p.parse_args()
    fn = block(a.source.read_text(encoding="utf-8"), "void menu_record(")
    fixture = r'''#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkCommandBuffer = int;
struct { int show_fps = 0; } opts;   // (0 off, 1 FPS, 2 Full)
std::atomic<bool> open_now{false}, startup_on{false};
std::string calls;
bool can_draw = true;
float ease_step = 0;
int64_t prof_now() { return 1000; }
namespace ui {
struct Ctx {};
bool begin(uint32_t, uint32_t) { calls += "b"; return can_draw; }
void idle() { calls += "i"; }
void record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) { CHECK(cmd == 1 && index == 2 && w == 640 && h == 360); calls += "r"; }
}
ui::Ctx ui_ctx(uint32_t, uint32_t) { return {}; }
void prebake(const ui::Ctx&) { calls += "p"; }
bool resized = false;
bool prebake_needed(uint32_t, uint32_t) { return resized; }
void draw_startup(ui::Ctx&) { calls += "s"; }
void draw_menu(ui::Ctx&) { calls += "m"; }
void overlay_prepare() { calls += "o"; }
void overlay_draw(uint32_t w, uint32_t h, int mode, bool menu_open) {
    CHECK(w == 640 && h == 360 && menu_open == open_now.load());
    calls += mode == 2 ? "F" : mode == 1 ? "f" : "-";   // (-: called with nothing to show; it draws nothing then)
}
''' + fn + r'''
int main() {
    for (bool startup : {false, true}) for (int fps : {0, 1, 2}) for (bool menu : {false, true}) for (bool can : {true, false}) {
        opts.show_fps = fps;
        open_now = menu;
        startup_on = startup;
        can_draw = can;
        calls.clear();
        menu_record(1, 2, 640, 360);
        if (!startup && !fps && !menu) {   // (nothing shows: no ImGui frame at all, only the reaping; after a resize, one bake)
            CHECK(calls == "i");
            calls.clear(), resized = true;
            menu_record(1, 2, 640, 360);
            CHECK(calls == (can ? "bpr" : "bi"));
            resized = false;
            continue;
        }
        if (!can) { CHECK(calls == "ob"); continue; }                          // (no ImGui here: nothing recorded)
        std::string want = "obp";
        if (startup) want += "s";
        want += !fps || startup ? "-" : fps == 2 ? "F" : "f";   // (no "0 FPS" over the start-up screen: it gets mode 0)
        want += "r";
        if (menu) want.insert(want.size() - 1, "m");
        CHECK(calls == want);
    }
    std::printf("PASS: one ImGui frame and recording for whatever shows, in order, nothing while nothing shows\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="fps-overlay-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
