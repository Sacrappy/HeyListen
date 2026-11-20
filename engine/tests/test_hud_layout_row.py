"""The Insert menu's HUD layout row: the HUD at the screen's edges, or in the middle 16:9 (#1133), game-free.

On a screen that is not 16:9 the HUD's corners go to the screen's own edges (host/hos_wide_ui.c, #95). Until now only the
switch HOS_WIDE_UI=0 kept the HUD in the middle 16:9 as the first versions had it, and no row of the menu moved it: an
ultrawide player who wanted the hearts and the minimap near the middle found the Ultrawide menus row moving only the
menus. The Graphics tab's HUD layout row chooses: Screen's edges (0, the default, as before) or Middle 16:9 (1, as
HOS_WIDE_UI=0). It changes at once. HOS_WIDE_UI, when set, still decides, and the row says so.

Checked: host/hos_wide_ui.c compiled whole (tests/test_wide_ui.py's stand-ins, tests/test_hud_layout.py's HUD) places the
HUD's panes at 21:9 and at 4:3 with the row at Middle 16:9 as HOS_WIDE_UI=0 places them, and with the row at Screen's
edges as before; HOS_WIDE_UI over the row either way; the row changed during play moves the HUD at once. And the wiring (the row fixed while
HOS_WIDE_UI holds it):
the row on the Graphics tab, saved as hud_layout, 0 by default; option 71 (HOS_OPT_HUD_LAYOUT, gpu/vk/vk_api.h) answers
it; the README and docs/development.md say what it does.

python -I -B tests/test_hud_layout_row.py [--cc gcc]
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
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "tests"))
import test_hud_layout  # noqa: E402
from test_wide_ui import FIXTURE_HEAD  # noqa: E402

# (test_hud_layout's panes and their placing, up to its main; the HUD's place printed for each run here)
HELPERS = test_hud_layout.FIXTURE_MAIN[:test_hud_layout.FIXTURE_MAIN.index("int main(void) {")]
MAIN = r'''
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    const float ratio = (float)atof(argv[1]);
    layout_named(STATUS, "PlayerStatus_00"), layout_named(PLUS, "Test_Menu_00"), layout_named(FADE, "LoadingFade_00");
    sroot = pane_at(STATUS, 0, "RootPane", 0, 0, 0);
    hearts = pane_at(STATUS, 1, "P_Hearts_00", 500, 120, 1);
    rupees = pane_at(STATUS, 2, "P_Rupee_00", 300, 120, 1);
    buttons = pane_at(STATUS, 3, "P_Buttons_00", 400, 400, 1);
    minimap = pane_at(STATUS, 4, "P_Map_00", 600, 500, 1);
    banner = pane_at(STATUS, 5, "P_Band_00", 3840, 300, 1);
    mroot = pane_at(PLUS, 6, "RootPane", 0, 0, 0);
    mleft = pane_at(PLUS, 7, "P_Column_00", 400, 1500, 1);
    froot = pane_at(FADE, 8, "RootPane", 0, 0, 0);
    fpane = pane_at(FADE, 9, "P_Fade_00", 3840, 2160, 1);
    ficon = pane_at(FADE, 10, "P_Icon_00", 160, 160, 1);
    layout_named(MAP, "MapMenu_00"), layout_named(DIALOG, "MessageDialog_00"), layout_named(GUIDE, "MessageLog_01");
    maproot = pane_at(MAP, 11, "RootPane", 0, 0, 0);
    mapicons = pane_at(MAP, 12, "N_MapIcon_00", 60, 60, 0);
    mapframe = pane_at(MAP, 13, "P_Frame_00", 400, 1500, 1);
    droot = pane_at(DIALOG, 14, "RootPane", 0, 0, 0);
    dbox = pane_at(DIALOG, 19, "P_MessageBase_00", 2200, 600, 1);
    dcol = pane_at(DIALOG, 18, "N_BtnNum_00", 90, 120, 0);
    dchoice = pane_at(DIALOG, 15, "Pa_Btn_00", 940, 226, 1);
    lroot = pane_at(GUIDE, 16, "RootPane", 0, 0, 0);
    lguide = pane_at(GUIDE, 17, "A_GuideAll_00", 300, 80, 1);
    /* the row at argv[2] from the start, then at argv[3] (a change in play: -1, none) */
    fixture_hud_layout = atoi(argv[2]);
    at(ratio);
    const int then = atoi(argv[3]);
    if (then >= 0) fixture_hud_layout = then, at(ratio);
    printf("hearts %.1f %.1f rupees %.1f %.1f buttons %.1f minimap %.1f %.1f menu %.1f scale %.4f %.4f\n",
           hx, hy, rx_, ry, bx, mx, my, menux, hos_ui_qx, hos_ui_qy);
    return 0;
}
'''

R21, R43 = 2560.0 / 1080.0, 4.0 / 3.0


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def placing_checks(cc):
    host = (ROOT / "host" / "hos_wide_ui.c").read_text(encoding="utf-8")
    body = "".join(line for line in host.splitlines(keepends=True) if not line.startswith("#include"))
    decl = "extern volatile float hos_wide_ratio;\n"
    check(decl in body, "the host file no longer declares " + decl.strip())
    body = body.replace(decl, "", 1)
    with tempfile.TemporaryDirectory(prefix="hud-layout-row-") as tmp:
        c, exe = Path(tmp) / "row.c", Path(tmp) / ("row.exe" if os.name == "nt" else "row")
        c.write_text(FIXTURE_HEAD + body + HELPERS + MAIN, encoding="utf-8")
        built = subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wno-unused-function", "-o", str(exe), str(c), "-lm"],
                               capture_output=True, text=True)
        check(built.returncode == 0, "host/hos_wide_ui.c did not compile with the stand-ins:\n" + built.stdout + built.stderr)

        def run(ratio, row, then=-1, env=None):
            e = {k: v for k, v in os.environ.items() if k != "HOS_WIDE_UI"}
            if env is not None:
                e["HOS_WIDE_UI"] = env
            r = subprocess.run([str(exe), "%.6f" % ratio, str(row), str(then)], capture_output=True, text=True, env=e)
            check(r.returncode == 0, "the placing run failed:\n" + r.stdout + r.stderr)
            return r.stdout.strip()

        failed = []
        for name, ratio in (("21:9", R21), ("4:3", R43)):
            edges, middle = run(ratio, 0), run(ratio, 0, env="0")
            check(edges != middle, "at %s the switch HOS_WIDE_UI=0 places the HUD as the default does: the check checks nothing"
                  % name)
            for what, got, want in (
                    ("the row at Middle 16:9 places the HUD as HOS_WIDE_UI=0", run(ratio, 1), middle),
                    ("the row at Screen's edges places the HUD as before", run(ratio, 0), edges),
                    ("HOS_WIDE_UI=1 places the HUD at the edges over the row's Middle 16:9", run(ratio, 1, env="1"), edges),
                    ("HOS_WIDE_UI=0 keeps the HUD in the middle over the row's Screen's edges", run(ratio, 0, env="0"), middle),
                    ("the row set to Middle 16:9 in play moves the HUD at once", run(ratio, 0, then=1), middle),
                    ("the row set back to Screen's edges in play moves the HUD back at once", run(ratio, 1, then=0), edges)):
                ok = got == want
                print("%s %s: %s" % ("ok  " if ok else "FAIL", name, what))
                if not ok:
                    print("     got  %s\n     want %s" % (got, want))
                    failed.append("%s: %s" % (name, what))
        print("16:9 unchanged by the row:", run(16.0 / 9.0, 1) == run(16.0 / 9.0, 0))
        check(run(16.0 / 9.0, 1) == run(16.0 / 9.0, 0), "at 16:9 the row moved the HUD")
        return failed


def wiring_checks():
    failed = []
    opt_ids = (ROOT / "gpu" / "vk" / "vk_opt_ids.h").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    menu_h = (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    host = (ROOT / "host" / "hos_wide_ui.c").read_text(encoding="utf-8")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    dev = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    for ok, what in (
            (re.search(r"#define HL_OPT_HUD_LAYOUT 71\b", opt_ids), "gpu/vk/vk_opt_ids.h names option 71 HL_OPT_HUD_LAYOUT"),
            (re.search(r'\{TAB_GRAPHICS, "HUD layout", &opts\.hud_layout, 0, 1, 1,', menu),
             "the Graphics tab has the HUD layout row, 0 to 1"),
            (re.search(r"int hud_layout = 0;", menu_h), "opts.hud_layout is 0 (Screen's edges) by default"),
            (re.search(r'value == &opts\.hud_layout && getenv\("HOS_WIDE_UI"\)', menu[menu.index("bool row_fixed("):]),
             "the row is fixed while HOS_WIDE_UI holds it (row_fixed), as rows other switches hold"),
            (re.search(r"if \(id == HL_OPT_HUD_LAYOUT\) return opts\.hud_layout;", replay), "option 71 answers the row"),
            (re.search(r"rx_option\(HL_OPT_HUD_LAYOUT, 0\)", host), "host/hos_wide_ui.c asks the row (rx_option 71)"),
            ("| HUD layout |" in readme, "the readme's options table has the row"),
            ("HUD layout" in dev and "hud_layout" in dev, "docs/development.md's options file has hud_layout"),
            ("keeps the HUD and movies in the middle 16:9" not in readme,
             "the readme still says the HUD stays in the middle 16:9 by default")):
        print("%s %s" % ("ok  " if ok else "FAIL", what))
        if not ok:
            failed.append(what)
    return failed


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    failed = placing_checks(a.cc) + wiring_checks()
    if failed:
        raise SystemExit("FAIL: %d checks:\n  %s" % (len(failed), "\n  ".join(failed)))
    print("hud layout row: ok")


if __name__ == "__main__":
    main()
