"""The Insert menu's dependent rows (gpu/vk/menu_rows.h, vk_menu.cpp row_shown / row_fixed): the rows that only matter with another
setting on (the HDR rows with HDR on where the display has it, PsychoV35's with it, Frame generation by and Frame pacing with
frame generation on, V-Sync mode with V-Sync On, the DLSS model with DLSS or DLAA, DLAA before the bloom with DLSS) are hidden
while it is off; the cursor stays on its row when they hide or show; the HDR row says plainly that it needs an HDR screen. No game or GPU required.

python -I -B tests/test_menu_rows.py [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def need(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def check_sources():
    vk = ROOT / "gpu" / "vk"
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    main = (vk / "vk_main.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    shown = block(menu, "bool row_shown(int i)")
    for call in ("mrow::framegen_sub_shown(opts.framegen)", "mrow::vsync_mode_shown(opts.vsync, vsync_env_decides())",
                 "mrow::hdr_levels_shown(hdr_now())", "mrow::tonemap_shown(hdr_now())",
                 "mrow::contrast_shown(hdr_now(), opts.hdr_tonemap)", "mrow::psychov35_shown(hdr_now(), opts.hdr_tonemap)",
                 "ur::model_shown(dlss_built(), upscaler())", "ur::pre_bloom_shown(dlss_why() == DLSS_OK, upscaler_mode())"):
        need(call in shown, "row_shown does not use " + call)
    need("&opts.framegen_by && v != &opts.pace" in shown, "row_shown: Frame generation by and Frame pacing")
    need("&opts.hdr_white && v != &opts.hdr_peak" in shown, "row_shown: HDR paper white and peak")
    need("mrow::vsync_mode_fixed(opts.vsync, vsync_env_decides())" in block(menu, "bool row_inactive(const int* value)"),
         "row_inactive: V-Sync mode")
    # the HDR row: the display's answer, found at each swapchain whatever the row says
    need("dev.hdr_listed = true" in main, "vk_main.cpp: the display's HDR10 surface is looked for with the HDR row Off too")
    need("mrow::hdr_effective(opts.hdr, dev.hdr_listed || dev.hdr, hdr_forced())" in menu, "vk_menu.cpp: hdr_now")
    need("Needs an HDR screen with Windows HDR on" in block(menu, "const char* hn_hdr(int v, int variant)"), "the HDR row's help")
    need("hn_hdr, false, true}" in menu, "the HDR row has its help")
    # the cursor stays on its row
    change = block(menu, "void change_row(Row& r, int v, int dir, bool keep = true)")
    need("set_row(r, v, dir, keep)" in change and "reselect(cur_tab, cursor_row)" in change, "change_row: the cursor stays on its row")
    need(menu.count("change_row(") == 5 and "set_row(r, *r.value" not in menu, "the menu's input changes rows through change_row")
    # DLAA before the bloom asks the mode the chain runs (the Upscaler row's choice is DLSS for DLAA too)
    need("int upscaler_mode()" in menu and "temporal_mode()" in block(menu, "int upscaler_mode()"), "upscaler_mode")
    need("upscaler_mode() != ur::DLAA" in block(menu, "const char* hn_dlss_pre_bloom(int v, int variant)"), "its help follows the mode")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="menu-rows-") as tmp:
        exe = Path(tmp) / "menu-rows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_menu_rows.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the menu's dependent rows")


if __name__ == "__main__":
    main()
