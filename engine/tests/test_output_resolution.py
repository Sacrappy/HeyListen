"""Exercise the host and menu's docked output sizes without a game dump."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    host = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

using DWORD = unsigned long;
constexpr int MAX_PATH = 260;
DWORD hos_game_path_a(char *path, int size) { return snprintf(path, size, "./hookshot.exe"); }
void hos_log(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }   // (the case's output: the warning)
volatile uint64_t hos_wide_out = 0;   // (host/hos_main.c: the widescreen picture, 0 = none; nn_applet.c reads it)
struct { int handheld, output_res; } opts;
namespace mkwvk {
int up_now = 0;   // (the Upscaler shown: 0 the game's own, 1 DLSS, 2 DLAA)
int upscaler() { return up_now; }
}
#include "gpu/vk/upscaler_rows.h"
using namespace mkwvk;
namespace ur = upscaler_rows;
''' + host[host.index("int hos_ini_option(const char *key, int otherwise);"):host.index("HLE(_ZN2nn2oe16GetOperationModeEv)")] + menu[
        menu.index("struct Out {"):menu.index("/// The Render resolution: the value chosen")] + menu[
        menu.index("const char* tx_output_res("):menu.index("const char* tx_speed(")] + r'''
int main(int argc, char **argv) {
    assert(argc == 5);
    const int mode = atoi(argv[1]);
    const unsigned w = atoi(argv[2]), h = atoi(argv[3]);
    assert(output_mode() == mode);
    assert(hos_output_size(0) == w && hos_output_size(1) == h);
    assert(hos_rt_pool_size(305ull << 20) == ((mode == 2 ? 687ull : mode == 4 ? 610ull : 512ull) << 20));   // (512 MB at least: nn_applet.c)
    assert(kLastOutput == 4);
    assert(output_choice(-1) == 0 && output_choice(4) == 4 && output_choice(5) == 0);
    // (issue 14: the sizes written out, as the menu reads the options file; any other size the game's)
    assert(output_choice_text("1920x1080\n") == 3 && output_choice_text(" 5120x1440") == 4 && output_choice_text("2") == 2);
    assert(output_choice_text("1280x800") == 0 && output_choice_text("1280") == 0 && output_choice_text("9") == 0);
    assert(step_output_choice(3, -1) == 3);
    assert(step_output_choice(3, 1) == 0 && step_output_choice(0, -1) == 3);
    assert(step_output_choice(0, 1) == 1 && step_output_choice(1, -1) == 0);
    assert(step_output_choice(1, 1) == 2 && step_output_choice(2, -1) == 1);
    assert(step_output_choice(2, 1) == 4);
    assert(step_output_choice(4, -1) == 2 && step_output_choice(4, 1) == 4);
    opts.output_res = mode;
    opts.handheld = handheld();
    assert(strcmp(tx_output_res(mode), argv[4]) == 0);
    unsigned scene_w, scene_h;
    game_size(1.0f, scene_w, scene_h);
    assert(scene_w == (opts.handheld ? 1536 : mode ? w : 2048));
    assert(scene_h == (opts.handheld ? 864 : mode ? h : 1152));
    if (mode == 3) assert(strcmp(tx_resolution(2), "75% (1440x810)") == 0);
    if (mode == 3) {   // (DLSS at 1080p: its modes' render sizes, each a twelfth of the output, exact; DLAA the full size)
        up_now = ur::DLSS;
        assert(strcmp(tx_resolution(3), "Quality (67%)") == 0 && strcmp(tx_resolution(6), "Ultra Performance (33%)") == 0);
        const unsigned want[][3] = {{3, 1280, 720}, {5, 1120, 630}, {4, 960, 540}, {6, 640, 360}};
        for (const auto& m : want) {
            game_size(ur::percent(m[0]) / 100.0f, scene_w, scene_h);
            assert(scene_w == m[1] && scene_h == m[2] && scene_w * 1080 == scene_h * 1920);
        }
        // (DLAA and FSR 4's native AA are the Render resolution's native choice of DLSS and FSR 4 now: the full size)
        assert(strcmp(tx_resolution(ur::kNativeAa), "DLAA (native)") == 0 && strcmp(tx_resolution(1), "Quality (67%)") == 0);
        up_now = ur::FSR4;
        assert(strcmp(tx_resolution(ur::kNativeAa), "Native AA (FSR 4)") == 0);
        up_now = ur::GAME;
        assert(strcmp(tx_resolution(ur::kNativeAa), "100% (1920x1080)") == 0);   // (the game's own has no native choice: its 100%)
    }
}
'''
    cases = (
        (0, 0, None, 0, 2560, 1440, "2048x1152 (game's)"),
        (1, 0, None, 1, 2560, 1440, "2560x1440"),
        (2, 0, None, 2, 3840, 2160, "3840x2160"),
        (3, 0, None, 3, 1920, 1080, "1920x1080"),
        (1, 0, "3", 3, 1920, 1080, "1920x1080"),
        (3, 1, "3", 0, 2560, 1440, "2048x1152 (game's)"),
        (4, 1, None, 0, 2560, 1440, "2048x1152 (game's)"),
        (5, 0, None, 0, 2560, 1440, "2048x1152 (game's)"),
        (4, 0, None, 4, 5120, 1440, "5120x1440"),
        ("1920x1080", 0, None, 3, 1920, 1080, "1920x1080"),   # (issue 14: a size written out)
        (0, 0, "3840x2160", 2, 3840, 2160, "3840x2160"),
        ("1280x800", 0, None, 0, 2560, 1440, "2048x1152 (game's)"),   # (not one of them: the game's, and said so)
        (0, 0, "1280x800", 0, 2560, 1440, "2048x1152 (game's)"),
    )
    with tempfile.TemporaryDirectory(prefix="output-resolution-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-I", str(ROOT), str(source), "-o", str(exe)], check=True)
        for saved, handheld, override, mode, w, h, label in cases:
            (work / "hookshot_options.ini").write_text(f"output_res={saved}\nhandheld_profile={handheld}\n", encoding="ascii")
            env = os.environ.copy()
            for key in ("HOS_OUTPUT_RES", "HOS_HANDHELD", "HOS_RT_POOL_MB"):
                env.pop(key, None)
            if override is not None:
                env["HOS_OUTPUT_RES"] = override
            out = subprocess.run([str(exe), str(mode), str(w), str(h), label], cwd=work, env=env, check=True,
                                 capture_output=True, text=True).stdout
            unknown = str(saved if override is None else override) in ("5", "1280x800")   # (the values read that are none of them)
            assert ("is not one of the output resolutions" in out) == unknown, (saved, override, out)
            if unknown:
                assert (f"HOS_OUTPUT_RES={override}" if override else f"output_res={saved}") in out, out
    print("PASS: output sizes, menu order, 1080p override, handheld, render-target budgets and DLSS sizes at 1080p, 5120x1440, "
          "and sizes written out (an unknown one logged)")


if __name__ == "__main__":
    main()
