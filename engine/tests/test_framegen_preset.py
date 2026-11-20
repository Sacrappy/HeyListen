"""Quality presets preserve the player's frame-generation choice, and the Frame generation row's arrows and the Frame
generation by row's switch keep the multiplier within what the mode that makes the frames supports.

Compile the menu's actual preset and row handlers with small stand-ins. No game
or GPU is needed.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, start):
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated block at %d" % start)


def definition(text, name):
    match = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not match:
        raise SystemExit("missing definition: " + name)
    return block(text, match.start())


FIXTURE = r"""
#include <algorithm>
#include <cstdio>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
int saves = 0;
void save() { ++saves; }
void apply_pad_layout(int) {}
void sync_preset_budgets(int) {}
bool row_fixed(const int*) { return false; }
int upscaler() { return 0; }
unsigned upscalers_available() { return 1; }
namespace ur {
int snap(int, int value) { return value; }
int step(int, int value, int dir) { return value + dir; }
int step_upscaler(int, int dir, unsigned) { return dir; }
}
namespace mr { constexpr int kMax = 8; }
void restart_values(int*) {}
struct { void note(const int*) {} } restart_state;
bool can_mic() { return false; }
/* FRAMEGEN_ROWS */
namespace fgr = mkwvk::framegen_rows;
/* (the mode that makes the frames and DLSS-G's runtime: the test sets them; the row is in a build with DLSS unless it says not) */
int g_most = 0;
bool g_dlss_built = true;
int framegen_by_now() { return opts.framegen_by; }
int dlss_fg_most() { return g_most; }
bool dlss_built() { return g_dlss_built; }
const char* tx_framegen_by(int) { return ""; }
const char* hn_framegen_by(int, int) { return ""; }
void mic_chosen() {}
int step_output_choice(int id, int dir) { return id + dir; }
const char* tx_framegen(int) { return ""; }
const char* hn_framegen(int, int) { return ""; }
const char* tx_preset(int) { return ""; }
const char* hn_preset(int, int) { return ""; }
/* STOPS */
/* PRESET */
/* SET_ROW */

int main() {
    Row framegen = /* FRAMEGEN_ROW */;
    Row by = /* FRAMEGEN_BY_ROW */;
    Row quality = /* QUALITY_ROW */;
    CHECK(opts.framegen == 0);
    for (int previous = 0; previous <= 8; ++previous) {
        for (int preset = 0; preset <= 5; ++preset) {
            opts = Options{};
            opts.framegen = previous;
            set_row(quality, preset, 1);
            CHECK(opts.preset == preset);
            CHECK(opts.framegen == previous);
        }
    }
    CHECK(framegen.preset_exempt && !framegen.restart);
    for (int preset = 1; preset <= 5; ++preset) {
        opts = Options{};
        opts.preset = preset;
        set_row(framegen, 2, 1);
        CHECK(opts.framegen == 2 && opts.preset == preset);
        set_row(framegen, 0, -1);
        CHECK(opts.framegen == 0 && opts.preset == preset);
    }
    // the arrows: Right and Left from a value (set_row's v is the arrow's step, as the menu presses it), as each mode offers them
    const auto right = [&](int from) { opts.framegen = from; set_row(framegen, from + 1, 1); return opts.framegen; };
    const auto left = [&](int from) { opts.framegen = from; set_row(framegen, from - 1, -1); return opts.framegen; };
    opts = Options{};
    opts.framegen_by = 0;   // (the renderer's own: Off, Auto, 2x to 8x, then round)
    CHECK(right(0) == 1 && right(1) == 2 && right(7) == 8 && right(8) == 0 && left(0) == 8 && left(2) == 1 && left(1) == 0);
    g_most = 5;
    opts.framegen_by = 1;   // (NVIDIA DLSS FG on an RTX 50's runtime: Off, 2x to 6x, no Auto)
    CHECK(right(0) == 2 && right(2) == 3 && right(5) == 6 && right(6) == 0 && left(0) == 6 && left(2) == 0 && left(3) == 2);
    CHECK(right(8) == 0 && left(8) == 5 && right(7) == 0);   // (a saved value past the top: the arrow steps from the top shown)
    CHECK(right(1) == 3 && left(1) == 0);   // (a saved Auto shows 2x: Right goes to 3x, Left to Off)
    g_most = 1;   // (an RTX 40's runtime: Off and 2x)
    CHECK(right(0) == 2 && right(2) == 0 && left(0) == 2 && left(2) == 0 && right(8) == 0 && left(8) == 0);
    g_most = 0;   // (not known, or DLSS-G does not make the frames: up to 8x, as the renderer's own)
    CHECK(right(7) == 8 && right(8) == 0 && left(0) == 8);
    opts.framegen_by = 2;   // (AMD FSR 3.1's frame interpolation: Off and 2x)
    CHECK(right(0) == 2 && right(2) == 0 && left(0) == 2 && left(2) == 0 && right(5) == 0);
    opts.framegen_by = 0;
    // a value set as it is (the options file's, a control bridge's) is kept as the mode makes it, not stepped
    g_most = 5;
    opts.framegen_by = 1;
    opts.framegen = 0;
    set_row(framegen, 8, 1);
    CHECK(opts.framegen == 6);
    set_row(framegen, 4, 1);
    CHECK(opts.framegen == 4);
    // Frame generation by: a switch clamps the saved multiplier to the new mode, never raising it
    const auto switch_to = [&](int from_by, int to_by, int framegen_value) {
        opts.framegen_by = from_by, opts.framegen = framegen_value;
        set_row(by, to_by, to_by > from_by ? 1 : -1);
        return opts.framegen;
    };
    CHECK(switch_to(0, 1, 8) == 6);   // (DLSS FG on an RTX 50: 6x at most)
    CHECK(switch_to(0, 1, 5) == 5 && switch_to(0, 1, 2) == 2 && switch_to(0, 1, 0) == 0);
    CHECK(switch_to(0, 1, 1) == 2);   // (Auto is the renderer's own: DLSS FG's lowest is 2x)
    CHECK(switch_to(0, 2, 8) == 2 && switch_to(0, 2, 3) == 2 && switch_to(0, 2, 1) == 2 && switch_to(0, 2, 0) == 0);
    CHECK(switch_to(1, 2, 6) == 2 && opts.framegen_by == 2);
    CHECK(switch_to(2, 0, 2) == 2 && opts.framegen_by == 0 && switch_to(2, 1, 2) == 2);   // (never raised by a switch back)
    g_most = 1;
    CHECK(switch_to(0, 1, 8) == 2 && switch_to(0, 1, 4) == 2);   // (an RTX 40's runtime)
    g_most = 5;
    // the row's own modes: the renderer's own, DLSS FG, AMD FSR 3.1; DLSS FG is no stop in a build without it
    opts.framegen_by = 0;
    set_row(by, 1, 1);
    CHECK(opts.framegen_by == 1);
    set_row(by, 2, 1);
    CHECK(opts.framegen_by == 2);
    set_row(by, 3, 1);
    CHECK(opts.framegen_by == 0);
    set_row(by, -1, -1);
    CHECK(opts.framegen_by == 2);
    g_dlss_built = false;
    opts.framegen_by = 0;
    set_row(by, 1, 1);
    CHECK(opts.framegen_by == 2);   // (Right skips DLSS FG)
    set_row(by, 3, 1);
    CHECK(opts.framegen_by == 0);
    opts.framegen_by = 2;
    set_row(by, 1, -1);
    CHECK(opts.framegen_by == 0);   // (Left skips it too)
    g_dlss_built = true;
    CHECK(by.hi == 2 && by.lo == 0 && by.preset_exempt && !by.restart);
    CHECK(std::strstr(quality.help, "Frame generation") != nullptr);
    CHECK(std::strstr(quality.help, "stay yours") != nullptr);
    CHECK(saves > 0);
    std::puts("PASS: quality presets preserve frame generation and its row preserves the preset");
}
"""


def fixture(source):
    menu = (source / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (source / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    options = re.search(r"struct Options \{.*?\n\};", header, re.S)
    tabs = re.search(r"enum MenuTab \{.*?\};", menu, re.S)
    row = re.search(r"struct Row \{.*?\n\};", menu, re.S)
    if not options or not tabs or not row:
        raise SystemExit("the menu's options, tabs, or row definition is missing")
    rows_header = (source / "gpu" / "vk" / "framegen_rows.h").read_text(encoding="utf-8").replace("#pragma once", "")
    replacements = {
        "/* OPTIONS */": options.group(0),
        "/* TABS */": tabs.group(0),
        "/* ROW */": row.group(0),
        "/* STOPS */": definition(menu, "bool stops_at_ends"),
        "/* PRESET */": definition(menu, "void apply_preset"),
        "/* SET_ROW */": definition(menu, "void set_row"),
        "/* FRAMEGEN_ROWS */": rows_header,
        "/* FRAMEGEN_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Frame generation", ')),
        "/* FRAMEGEN_BY_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Frame generation by"')),
        "/* QUALITY_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Quality preset"')),
    }
    result = FIXTURE
    for marker, value in replacements.items():
        result = result.replace(marker, value)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="framegen-preset-") as tmp:
        src = Path(tmp) / "framegen_preset.cpp"
        exe = Path(tmp) / ("framegen_preset.exe" if os.name == "nt" else "framegen_preset")
        src.write_text(fixture(args.source), encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", str(src),
                       "/Fo" + str(Path(tmp) / "framegen_preset.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                       str(src), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
