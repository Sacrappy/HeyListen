# Copyright (C) 2026 Sacrappy

"""Compile and run the Texture resolution Low's rule (gpu/vk/tex_low.h) and check where the renderer asks it (no game, no GPU)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The option's row, its key, the texture setup and the switches are still wired"""
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "tex_low.h"', "tex_low::levels_dropped(x.width, x.height, x.levels, 0, texture_low_min_dim())",
                   "static const bool on = opts.tex_low != 0;", 'getenv("RX_VK_TEX_LOW_DIM")',
                   "const bool bc_candidate = astc && !(x.skip > x.low_skip && x.layers == 1) &&"):
        if needle not in tex:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    for needle in ('{TAB_GRAPHICS, "Texture resolution", &opts.tex_low, 0, 1, 1, tx_tex_res,', "hn_tex_res, true, true},",
                   'getenv("RX_VK_TEX_LOW"); e && *e) opts.tex_low = atoi(e) != 0;', "void menu_first_run_textures() {",
                   'getenv("RX_VK_TEX_LOW_FIRST_RUN")'):
        if needle not in menu:
            raise SystemExit("vk_menu.cpp no longer has: " + needle)
    # the first-run proposal is off unless asked for, and never touches an existing options file
    first = menu[menu.index("void menu_first_run_textures() {"):]
    first = first[:first.index("\n}\n")]
    for needle in ("!on || options_file_found", "vram > (6ull << 30) + (512ull << 20)"):
        if needle not in first:
            raise SystemExit("menu_first_run_textures lost: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="tex-low-") as tmp:
        exe = Path(tmp) / "tex-low"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_tex_low.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
