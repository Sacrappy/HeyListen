"""The frame rows: Frame generation by and its multiplier right under it, the multiplier never past what the mode supports, and the
Frame rate limit as the rate PRESENTED, with Unlimited (gpu/vk/framegen_rows.h, vk_menu.cpp, vk_framegen.cpp, host/nn_vi.c).

- framegen_rows.h's rules, compiled and run (tests/test_framegen_rows.cpp): each mode's top (NVIDIA DLSS FG its runtime's most + 1,
  AMD FSR 3.1 2x, the renderer's own 8x), Auto only for the renderer's own, the arrows, the switch of mode clamping the saved
  multiplier, the limit's values (Unlimited is a new 6: the saved 0 to 5 keep their meaning) and the multiplier that leaves the game
  its 20 frames a second.
- The menu: the rows' order (Frame generation by, its multiplier directly under it), the Frame rate limit row's range and label, the
  options saved as before.
- The limit's arithmetic in the host (nn_vi.c fps_plan and the functions after it, compiled and run with stand-ins): the game runs at
  the limit over the multiplier that runs, its period exact, and Unlimited holds nothing.
- menu_fps_limit (vk_menu.cpp, compiled and run): the row, HOS_FPS_LIMIT over it, Per frame none.

No game or GPU required.

python -I -B tests/test_framegen_rows.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

HOST = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HL_OPT_FPS_LIMIT 90
#define HL_OPT_FRAMEGEN_MULT 91
#define HL_OPT_VRR_CAP 93
static int option_limit = 60, option_mult = 1, option_realtime = 1, option_vrr_cap = 0;
int rx_option(int id, int otherwise) {
    if (id == 0) return option_realtime;
    if (id == HL_OPT_FPS_LIMIT) return option_limit;
    if (id == HL_OPT_FRAMEGEN_MULT) return option_mult;
    if (id == HL_OPT_VRR_CAP) return option_vrr_cap;
    return otherwise;
}
FPS_PLAN
int main(int argc, char **argv) {
    if (argc != 5) return 90;
    option_limit = atoi(argv[1]), option_mult = atoi(argv[2]), option_realtime = atoi(argv[3]);
    option_vrr_cap = argv[4][0] == '-' ? 0 : atoi(argv[4]);
    printf("%d %d %lld\n", fps_limit(), fps_unlimited(), (long long)fps_period_ns());
    return 0;
}
"""

MENU = r"""
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct { int fps_limit = 0, realtime = 1; } opts;
FRAMEGEN_ROWS
namespace fgr = mkwvk::framegen_rows;
MENU_FPS_LIMIT
int main(int argc, char **argv) {
    if (argc != 3) return 90;
    opts.fps_limit = atoi(argv[1]), opts.realtime = atoi(argv[2]);
    std::printf("%d\n", menu_fps_limit());
    return 0;
}
"""


def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True, **kwargs).stdout


def check_rows(menu):
    # the rows' order: the Upscaler pair, then Frame generation by, its multiplier directly under it, then Frame pacing
    by = menu.index('{TAB_GRAPHICS, "Frame generation by", &opts.framegen_by')
    gen = menu.index('{TAB_GRAPHICS, "Frame generation", &opts.framegen')
    pace = menu.index('{TAB_GRAPHICS, "Frame pacing"')
    assert by < gen < pace, "the multiplier row is not directly under Frame generation by, before Frame pacing"
    assert menu[by:gen].count("{TAB_") == 1, "a row sits between Frame generation by and the Frame generation multiplier"
    assert menu[gen:pace].count("{TAB_") == 1, "a row sits between the multiplier and Frame pacing"
    # the saved keys are the rows' own (frame_generation_by, frame_generation, frame_rate_limit): no special case
    ini_key = block(menu, "const char* ini_key(const Row& r) {", where="gpu/vk/vk_menu.cpp")
    for name in ("framegen", "fps_limit", "framegen_by"):
        assert "&opts.%s)" % name not in ini_key, "%s has a key of its own: an options file from before would lose it" % name
    # the limit row: seven values, the last Unlimited; the multiplier row 0 to 8; the mode row three
    limit = menu[menu.index('{TAB_GRAPHICS, "Frame rate limit"'):]
    limit = limit[:limit.index("nullptr, false, true},")]
    assert "&opts.fps_limit, 0, fgr::kLimits - 1, 1, tx_fps_limit" in limit, "the Frame rate limit row is not 0 to Unlimited"
    assert re.search(r'"Frame generation", &opts\.framegen, 0, 8, 1, tx_framegen,', menu), "the multiplier row is not Off, Auto, 2x to 8x"
    assert '"Frame generation by", &opts.framegen_by, 0, fgr::kBy - 1, 1, tx_framegen_by,' in menu, "the mode row does not have three modes"
    assert "Unlimited" in block(menu, "const char* tx_fps_limit(int v) {", where="gpu/vk/vk_menu.cpp"), "no Unlimited label"
    names = block(menu, "const char* tx_framegen_by(int v) {", where="gpu/vk/vk_menu.cpp")
    for name in ("Built-in / AMD FSR 3.1", "NVIDIA DLSS FG", "AMD FSR 3.1 FG"):
        assert '"%s"' % name in names, name
    assert names.index("Built-in") < names.index("NVIDIA DLSS FG") < names.index("AMD FSR 3.1 FG"), \
        "the saved modes 0 and 1 are not the first two names (their meaning would change)"
    shown = block(menu, "const char* tx_framegen(int v) {", where="gpu/vk/vk_menu.cpp")
    assert "fgr::clamp(framegen_by_now(), dlss_fg_most()," in shown, "the multiplier row shows a value past what the mode makes"
    # set_row: the arrows step what the mode offers; a switch of mode clamps the multiplier (never raising it)
    row = block(menu, "void set_row(Row& r, int v, int dir, bool keep = true) {", where="gpu/vk/vk_menu.cpp")
    assert "fgr::step(framegen_by_now(), dlss_fg_most(), *r.value, dir)" in row, "set_row: Frame generation's arrows"
    assert "fgr::on_mode_change(framegen_by_now(), dlss_fg_most(), opts.framegen)" in row, "set_row: a mode switch keeps the multiplier in range"
    assert row.index("fgr::on_mode_change(") > row.index("*r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;"), \
        "set_row clamps the multiplier before the new mode is set"
    # the row is shown in a build with DLSS, or where AMD FSR 3.1's frame interpolation runs
    shown_rows = block(menu, "bool row_shown(int i) {", where="gpu/vk/vk_menu.cpp")
    assert "&opts.framegen_by || dlss_built() || fsr3fi::capable()" in shown_rows, "row_shown: Frame generation by"


def check_runtime():
    framegen = (VK / "vk_framegen.cpp").read_text(encoding="utf-8")
    now = block(framegen, "int framegen_now() {", where="gpu/vk/vk_framegen.cpp")
    assert "framegen_rows::limited(menu_fps_limit(), framegen_rows::clamp(framegen_by_now(), dlss_fg_most(), opts.framegen))" in now, \
        "framegen_now: the mode's clamp, then the limit's lowering"
    by = block(framegen, "int framegen_by_now() {", where="gpu/vk/vk_framegen.cpp")
    assert "dlss_fg_wanted() && temporal_wanted()" in by and "BY_FSR3" in by, "framegen_by_now"
    record = block(framegen, "bool framegen_record(", where="gpu/vk/vk_framegen.cpp")
    assert "menu_fps_limit()" in record and "limit > 0 ? (int64_t)(1000000000LL / limit)" in record, \
        "Auto's refresh period is not held to the Frame rate limit's"
    fsr = (VK / "vk_fsr3fi.cpp").read_text(encoding="utf-8")
    assert "return framegen_now() == 2 ? 1u : 0u;" in block(fsr, "uint32_t asked() {", where="gpu/vk/vk_fsr3fi.cpp"), \
        "AMD FSR 3.1's frame is asked for at the multiplier as it runs"
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    table = block(replay, "t.option = [](int id) -> int {", where="gpu/vk/vk_replay.cpp")
    assert "id == HL_OPT_FPS_LIMIT ? menu_fps_limit()" in table, "the renderer does not answer the limit as the menu's"
    assert "id == HL_OPT_FRAMEGEN_MULT ? (framegen_on() && framegen_now() >= 2 ? framegen_now() : 1)" in table, \
        "the renderer does not answer the multiplier that runs (1 when frame generation does not run)"
    ids = (VK / "vk_opt_ids.h").read_text(encoding="utf-8")
    assert re.search(r"#define HL_OPT_FRAMEGEN_MULT 91\b", ids), "HL_OPT_FRAMEGEN_MULT is not 91"
    assert re.search(r"#define HL_OPT_VRR_CAP 93\b", ids), "HL_OPT_VRR_CAP is not 93"
    assert not re.search(r"#define HL_OPT_\w+ 93\b", ids.replace("#define HL_OPT_VRR_CAP 93", "", 1)), \
        "another option id is 93 (HL_OPT_VRR_CAP's)"
    host = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    assert "rx_option(HL_OPT_FRAMEGEN_MULT, 1)" in host, "the host does not ask the multiplier"
    assert "rx_option(HL_OPT_VRR_CAP, 0)" in host, "the host does not ask the VRR row's cap"
    # the host paces at the exact period of the game's rate, and Unlimited out of the way of the game's own limiter
    presented = block(host, "void hos_vi_native_presented(void) {", where="host/nn_vi.c")
    assert "fps_period_ns()" in presented, "hos_vi_native_presented: the exact period of the game's rate"
    assert "fps_unlimited() ? 1000 : cap" in host, "Unlimited does not take the game's own limiter out of the way"
    overlay = (VK / "vk_overlay.cpp").read_text(encoding="utf-8")
    assert "menu_fps_limit()" in overlay and "Unlimited" in overlay and "game %d x %d" in overlay, "the FPS counter's Frame limit line"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    check_rows(menu)
    check_runtime()
    host = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    plan = "\n".join(block(host, head, where="host/nn_vi.c") for head in (
        "static void fps_plan(int *limit, int *mult) {", "static int fps_limit(void) {", "static int fps_unlimited(void) {",
        "static int64_t fps_period_ns(void) {"))
    rows = (VK / "framegen_rows.h").read_text(encoding="utf-8").replace("#pragma once", "")
    fps_limit_fn = block(menu, "int menu_fps_limit() {", where="gpu/vk/vk_menu.cpp")
    with tempfile.TemporaryDirectory(prefix="framegen-rows-") as tmp:
        work = Path(tmp)
        pure = work / "pure.exe"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_framegen_rows.cpp"), "-o", str(pure)],
                       check=True)
        subprocess.run([str(pure)], check=True)
        # the host: the game runs at the limit over the multiplier that runs; its period is exact; Unlimited has none
        host_cpp, host_exe = work / "host.c", work / "host_c.exe"
        host_cpp.write_text(HOST.replace("FPS_PLAN", plan), encoding="utf-8")
        cc = [args.cxx.replace("clang++", "clang").replace("g++", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(host_cpp), "-o", str(host_exe)]
        subprocess.run(cc, check=True)
        env = {k: v for k, v in os.environ.items() if k not in ("HOS_FPS_LIMIT", "HOS_FPS_MAX")}
        cases = (   # (limit, multiplier, Real time) -> (the game's rate, unlimited, its period in ns)
            (60, 1, 1, 60, 0, 1000000000 // 60 * 1),
            (60, 3, 1, 20, 0, 1000000000 * 3 // 60),
            (120, 1, 1, 120, 0, 1000000000 // 120),
            (120, 2, 1, 60, 0, 1000000000 * 2 // 120),
            (120, 3, 1, 40, 0, 1000000000 * 3 // 120),
            (120, 6, 1, 20, 0, 1000000000 * 6 // 120),
            (144, 4, 1, 36, 0, 1000000000 * 4 // 144),
            (165, 4, 1, 41, 0, 1000000000 * 4 // 165),      # (41.25: the rate rounds, the period is exact)
            (240, 8, 1, 30, 0, 1000000000 * 8 // 240),
            (240, 1, 1, 240, 0, 1000000000 // 240),
            (0, 1, 1, 360, 1, 0),                           # (Unlimited: nothing to hold)
            (0, 4, 1, 360, 1, 0),
            (120, 4, 0, 60, 0, 1000000000 // 60),           # (Per frame: the limit does not apply, 60 and the frames made on top)
            (0, 3, 0, 60, 0, 1000000000 // 60),
        )
        for limit, mult, realtime, want_fps, want_unlimited, want_period in cases:
            out = run([str(host_exe), str(limit), str(mult), str(realtime), "-"], env=env).split()
            got = (int(out[0]), int(out[1]), int(out[2]))
            assert got == (want_fps, want_unlimited, want_period), \
                "limit %d x%d realtime %d: %s, wanted %s" % (limit, mult, realtime, got, (want_fps, want_unlimited, want_period))
        # (the environment's HOS_FPS_LIMIT over the row, clamped to 30 to 360, as the renderer's menu_fps_limit reads it)
        env_limit = dict(env, HOS_FPS_LIMIT="144")
        out = run([str(host_exe), "60", "2", "1", "-"], env=env_limit).split()
        assert (int(out[0]), int(out[2])) == (72, 1000000000 * 2 // 144), out
        # the VRR row's cap (feat/vsync-vrr): a limit above 60 is held under the refresh; below 60, above the limit or
        # Per-frame it changes nothing, and Unlimited has none of it either
        for limit, mult, realtime, cap, want_fps, want_unlimited, want_period in (
            (165, 1, 1, 162, 162, 0, 1000000000 // 162),
            (240, 4, 1, 162, 41, 0, 1000000000 * 4 // 162),
            (240, 4, 1, 58, 60, 0, 1000000000 * 4 // 240),      # (a cap under 60 would hold the game under its console rate)
            (90, 1, 1, 200, 90, 0, 1000000000 // 90),          # (a cap over the limit leaves it alone)
            (0, 4, 1, 162, 360, 1, 0),                           # (Unlimited)
            (100, 1, 0, 162, 60, 0, 1000000000 // 60),           # (Per frame: the cap does not apply)
        ):
            out = run([str(host_exe), str(limit), str(mult), str(realtime), str(cap)], env=env).split()
            got = (int(out[0]), int(out[1]), int(out[2]))
            assert got == (want_fps, want_unlimited, want_period), \
                "limit %d x%d realtime %d cap %d: %s, wanted %s" % (limit, mult, realtime, cap, got,
                                                                    (want_fps, want_unlimited, want_period))
        out = run([str(host_exe), "60", "2", "1", "117"], env=env_limit).split()
        assert (int(out[0]), int(out[2])) == (59, 1000000000 * 2 // 117), out   # (the cap over HOS_FPS_LIMIT too)
        # menu_fps_limit
        menu_cpp, menu_exe = work / "menu.cpp", work / "menu.exe"
        menu_cpp.write_text(MENU.replace("FRAMEGEN_ROWS", rows).replace("MENU_FPS_LIMIT", fps_limit_fn), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(menu_cpp), "-o", str(menu_exe)], check=True)
        want = {(0, 1): 60, (1, 1): 90, (2, 1): 120, (3, 1): 144, (4, 1): 165, (5, 1): 240, (6, 1): 0, (9, 1): 60, (2, 0): -1, (6, 0): -1}
        for (row, realtime), expected in want.items():
            assert int(run([str(menu_exe), str(row), str(realtime)], env=env)) == expected, (row, realtime, expected)
        assert int(run([str(menu_exe), "6", "1"], env=dict(env, HOS_FPS_LIMIT="100"))) == 100
        assert int(run([str(menu_exe), "6", "1"], env=dict(env, HOS_FPS_MAX="500"))) == 360
        assert int(run([str(menu_exe), "2", "0"], env=dict(env, HOS_FPS_LIMIT="100"))) == -1
    print("PASS: the frame rows: each mode's multiplier, the limit as the rate presented, Unlimited")


if __name__ == "__main__":
    main()
