"""The Insert menu's scaling rows: the Upscaler (opts.dlss: the game's own, NVIDIA DLSS, AMD FSR 3.1, AMD FSR 4, Off) and the
Render resolution's steps for it, DLAA and FSR 4's Native AA being the native choice of DLSS's and FSR 4's (gpu/vk/upscaler_rows.h),
their order on the Graphics tab (together, just above the frame generation rows), options files from before still loading (a
saved DLAA or FSR 4 Native AA Upscaler is DLSS or FSR 4 with the native Render resolution), and their help (why DLSS or FSR 4 is
not offered, the Sharpness DLSS wants). No game or GPU required.

python -I -B tests/test_upscaler_rows.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, start):
    """A function's body from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    # the Upscaler and the Render resolution together, just above the frame generation rows (Frame generation by, its multiplier
    # under it, Frame pacing); the output's size and the window's filter after Reflex
    order = ['"Upscaler", &opts.dlss', '"Render resolution", &opts.resolution', '"DLSS model", &opts.dlss_model',
             '"Frame generation by", &opts.framegen_by', '"Frame generation", &opts.framegen', '"Frame pacing", &opts.pace',
             '"NVIDIA Reflex", &opts.reflex', '"Output resolution", &opts.output_res',
             '"Window scaling filter", &opts.upscale', '"Sharpness", &opts.sharpness']
    at = [menu.find(row) for row in order]
    if -1 in at:
        raise SystemExit("vk_menu.cpp: a scaling row is missing: " + order[at.index(-1)])
    if at != sorted(at):
        raise SystemExit("vk_menu.cpp: the scaling rows are not in the picture's order")
    # (the pair together: no other row between the Upscaler and the Render resolution; only the DLSS model, which is theirs, may sit
    # between them and the frame rows)
    between = menu[at[0]:at[1]]
    if between.count('{TAB_') != 1:
        raise SystemExit("vk_menu.cpp: a row sits between the Upscaler and the Render resolution")
    between = menu[at[1]:at[3]]
    if between.count('{TAB_') != 3:   # (Render resolution, DLSS model, DLAA before the bloom)
        raise SystemExit("vk_menu.cpp: rows other than the DLSS model and DLAA before the bloom sit between the Render resolution and Frame generation by")
    if '"NVIDIA DLSS", &opts.' in menu or '"AMD FSR 4", &opts.' in menu:
        raise SystemExit("vk_menu.cpp: DLSS or FSR has a row of its own beside the Upscaler")
    # saved options: the renamed rows keep their keys (data, so read from the source)
    key = function(menu, "const char* ini_key(const Row& r) {")
    for value, name in (("opts.resolution", "resolution"), ("opts.upscale", "upscaling"), ("opts.dlss", "dlss")):
        if not re.search(r"&%s\)\s*return\s*\"%s\"" % (re.escape(value), name), key):
            raise SystemExit("ini_key: %s is not saved as %s (an options file from before would lose it)" % (value, name))
    # the menu's rules are upscaler_rows.h's, checked by behaviour in test_upscaler_rows.cpp: here only that the menu uses them
    uses = {
        "row_fixed": ("reflex_available()", "ur::model_fixed("),
        "row_shown": ("ur::model_shown(dlss_built(), upscaler())",),
        "set_row": ("ur::step_upscaler(", "ur::step(", "ur::snap(", "row_fixed("),
    }
    for name, calls in uses.items():
        body = function(menu, "%s %s(" % ("void" if name == "set_row" else "bool", name))
        for call in calls:
            if call not in body:
                raise SystemExit("%s does not use %s (upscaler_rows.h)" % (name, call))
    # the help: a row's own, then a line for the value chosen (Row::hint). The Upscaler's says why DLSS is not offered, and
    # DLSS's values ask for more Sharpness (it looks softer than the game's own)
    upscaler = function(menu, "const char* hn_upscaler(int v, int variant) {")
    for why in ("not in this build", "RX_VK_DLSS=0", "NVIDIA RTX GPU", "newer NVIDIA driver", "runtime was not found",
                "(int)dlss_why()", "RX_VK_FSR4=0", "AMD's files were not found", "no FSR 4 for this GPU", "(int)fsr4_why()"):
        if why not in upscaler:
            raise SystemExit("hn_upscaler: the help no longer says why DLSS or FSR is not offered (%s)" % why)
    if upscaler.count("Sharpness to") < 2:
        raise SystemExit("hn_upscaler: NVIDIA DLSS and DLAA do not say to set Sharpness to around 80%")
    resolution = function(menu, "const char* hn_resolution(int v, int variant) {")
    if resolution.count("Sharpness to around 80%") < 5 or "upscaler()" not in resolution:
        raise SystemExit("hn_resolution: DLSS's modes do not say to set Sharpness to around 80%, or not for the Upscaler shown")
    measure = function(menu, "float measure_help(ui::Ctx& c, float width) {")
    if "r.hint(v, k)" not in measure:
        raise SystemExit("measure_help leaves the values' lines out: the help's room would jump as a value changes")
    for row, hint in (('"Upscaler", &opts.dlss', "hn_upscaler"), ('"DLSS model", &opts.dlss_model', "hn_dlss_model"), ('"Render resolution", &opts.resolution', "hn_resolution"),
                      ('"NVIDIA Reflex", &opts.reflex', "hn_reflex"), ('"Window scaling filter", &opts.upscale', "hn_filter")):
        entry = menu[menu.index(row):]
        if hint not in entry[:entry.index("},")]:
            raise SystemExit("vk_menu.cpp: %s has no %s" % (row.split(",")[0], hint))
    # the DLSS model row: not the Quality preset's (preset_exempt), no restart, and its preset is what DLSS is made with
    model = menu[menu.index('"DLSS model", &opts.dlss_model'):]
    if not model[:model.index("},")].rstrip().endswith("hn_dlss_model, false, true"):
        raise SystemExit("vk_menu.cpp: the DLSS model row takes a restart or turns the Quality preset to Custom")
    # the Window scaling filter's sizes: only from the game's frames. The start-up screen is drawn at the window's size
    # (present_image with no source): taken as the picture's, the row read "Not needed" while the output was another size
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    present = function(replay, "void present_image(")
    note = present.index("menu_note_present_sizes(")
    if "if (src) {" not in present[:note] or present.rindex("if (src) {", 0, note) < present.rindex("\n    }", 0, note):
        raise SystemExit("present_image: the start-up screen's frames (no src) count as the game's picture for the filter row")
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    feature = function(dlss, "int prepare(")
    if "wanted_preset()" not in feature or "s.feature_preset == preset" not in feature:
        raise SystemExit("vk_dlss.cpp: DLSS's feature is not made again for another DLSS model")
    if "upscaler_rows::dlss_preset(opts.dlss_model" not in function(dlss, "unsigned wanted_preset() {"):
        raise SystemExit("vk_dlss.cpp: the DLSS model row (and RX_VK_DLSS_PRESET over it) is not the preset asked for")
    # (the chosen model is for every mode and GPU: wanted_preset asks nothing of the card; the first run's default is written to
    # the ini at once, and only when the ini has no model)
    if "device_" in function(dlss, "unsigned wanted_preset() {"):
        raise SystemExit("vk_dlss.cpp wanted_preset: the card overrides the DLSS model row")
    if "ur::step_model(" in menu or "MODEL_AUTO" in menu:
        raise SystemExit("vk_menu.cpp: the DLSS model row has an Auto again")
    if "int dlss_model = 0;" not in (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8"):
        raise SystemExit("vk_menu.h: the DLSS model row's default is not K (0)")
    first = function(menu, "void menu_first_run_model(uint32_t vendor_id, uint32_t device_id) {")
    if "dlss_model_saved" not in first or "ur::default_model(vendor_id, device_id, device_vram_bytes())" not in first or "save();" not in first:
        raise SystemExit("menu_first_run_model: does not give an ini with no model the GPU's default and save it at once")
    init = function(menu, "void menu_init() {")
    if "dlss_model_saved = true" not in init:
        raise SystemExit("menu_init: a model in the ini is not remembered as the player's")
    if "menu_first_run_model(dev.props.vendorID, dev.props.deviceID)" not in (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8"):
        raise SystemExit("vk_main.cpp: the first run's model is not set from the GPU once it is chosen")
    if "if (keep) save();" not in function(menu, "void set_row(Row& r, int v, int dir, bool keep = true) {"):
        raise SystemExit("set_row: a row's change is not saved to the ini at once")
    temporal = (ROOT / "gpu" / "vk" / "vk_temporal.cpp").read_text(encoding="utf-8")
    mode = function(temporal, "int temporal_mode() {")
    if "dlss_why() != DLSS_OK" not in mode or "fsr4_why() != FSR_OK" not in mode:
        raise SystemExit("temporal_mode asks for an engine where it cannot run (DLAA or FSR 4's native AA would hold the 3D at "
                         "full scale for nothing)")
    if "std::clamp(" in mode:
        raise SystemExit("temporal_mode clamps the row's value: FSR 3.1 (3) would run on the chain")
    if "ur::mode_for(v, opts.resolution)" not in mode:
        raise SystemExit("temporal_mode: DLSS and FSR 4 at the Render resolution's native choice do not run as DLAA and native AA")
    if "fsr3_wanted()" not in mode:
        raise SystemExit("temporal_mode runs DLSS or FSR 4 while FSR 3.1 upscales (two upscalers, two jitters)")
    fsr3 = (ROOT / "gpu" / "vk" / "vk_fsr3.cpp").read_text(encoding="utf-8")
    if "!temporal_env_chosen()" not in function(fsr3, "bool fsr3_chosen() {"):
        raise SystemExit("fsr3_chosen: a saved FSR 3.1 drops RX_VK_DLSS / RX_VK_FSR4, which go over the row")
    # FSR 3.1 is an engine of the chain (the skinned draws' motion), unless RX_VK_FSR3_CHAIN=0 keeps its own inputs: the chain
    # takes it before its fallback to the game's own, fsr3_wanted (its own inputs) is then off, and the chain has its slot
    if "return !fsr3_on_chain() && fsr3_chosen();" not in function(fsr3, "bool fsr3_wanted() {"):
        raise SystemExit("fsr3_wanted: FSR 3.1 would upscale with its own inputs while it draws on the chain (two upscales)")
    on_chain = "if (fsr3_on_chain() && fsr3_chosen()) return ur::FSR3;"
    if on_chain not in mode or mode.index(on_chain) > mode.index("if (!ur::is_dlss(v) && !ur::is_fsr4(v)) return ur::GAME;"):
        raise SystemExit("temporal_mode does not run the row's FSR 3.1 on the chain before falling back to the game's own")
    if "fsr3_engine()" not in function(temporal, "Engine& engine_of("):
        raise SystemExit("engine_of has no slot for FSR 3.1's engine")
    # The chain's depth is reversed for FSR 3.1 (the prep's bit 3, from fsr_flip): the frame says so, for whatever else reads it
    upscale = function(temporal, "bool draw_engine(")   # (the engine's evaluation, after Longshot a7621bd7)
    if "(fsr_flip ? 8 : 0)" not in upscale or "f.depth_reversed = fsr_flip;" not in upscale:
        raise SystemExit("draw_engine reverses the depth without saying so in the frame (TemporalFrame::depth_reversed)")
    # (HeyListen has no DLSS Frame Generation: upstream's check of its depth convention is not here.) FSR 3.1 goes on the chain only
    # while PsychoV35 is on, unless RX_VK_FSR3_CHAIN says otherwise
    if "return env > 0 || (env != 0 && scene_tonemap_on());" not in function(fsr3, "bool fsr3_on_chain() {"):
        raise SystemExit("fsr3_on_chain: FSR 3.1 is not on the chain exactly while PsychoV35 is on (RX_VK_FSR3_CHAIN over it)")
    chosen = function(temporal, "bool temporal_env_chosen() {")
    if "dlss_why() == DLSS_OK" not in chosen or "fsr4_why() == FSR_OK" not in chosen:
        raise SystemExit("temporal_env_chosen: a switch for an engine that cannot run here leaves the game's own, not the saved FSR 3.1")
    # FSR 3.1 and FSR 4 are offered where they run, and the row steps over what does not run here
    available = function(menu, "unsigned upscalers_available() {")
    if "fsr3::supported()" not in available:
        raise SystemExit("upscalers_available does not ask vk_fsr3.cpp whether FSR 3.1 runs here")
    if "fsr4_why() == FSR_OK" not in available:
        raise SystemExit("upscalers_available does not ask vk_fsr4.cpp whether FSR 4 runs here")
    if "upscalers_available()" not in function(menu, "void set_row("):
        raise SystemExit("the Upscaler row does not step over what runs here")
    # the saved Render resolution is the player's: another Upscaler does not overwrite it with its nearest step (Balanced 58%
    # came back from the game's own as Performance 50%); what draws and what the row shows is that nearest step
    if "opts.resolution = ur::snap(" in function(menu, "void set_row("):
        raise SystemExit("set_row saves the nearest step over the Render resolution when the Upscaler changes")
    for name, text in (("int resolution_scale() {", "upscaler_rows::snap(upscaler(), opts.resolution)"),
                       ("bool resolution_auto() {", "upscaler_rows::snap(upscaler(), opts.resolution)"),
                       ("const char* tx_resolution(int v) {", "v = ur::snap(up, v)")):
        if text not in function(menu, name):
            raise SystemExit("%s does not read the saved Render resolution through the Upscaler's steps" % name)
    if "ur::step(upscaler(), ur::snap(upscaler(), *r.value), dir)" not in function(menu, "void set_row("):
        raise SystemExit("set_row steps the Render resolution from the saved value, not from the step the row shows")
    # an options file from before: a saved DLAA or FSR 4 Native AA Upscaler is DLSS or FSR 4 with the native Render resolution, in
    # the load path, after the file's rows (and the oldest file's dynamic resolution) are read
    init = function(menu, "void menu_init() {")
    if "ur::migrate(opts.dlss, opts.resolution)" not in init or init.index("ur::migrate(") < init.index("old_dynres ? 0 : 1"):
        raise SystemExit("menu_init: a saved DLAA or FSR 4 Native AA is not made DLSS or FSR 4 with the native choice after the load")
    if "ur::migrate(" in function(menu, "void set_row(Row& r, int v, int dir, bool keep = true) {"):
        raise SystemExit("set_row migrates: the saved values are for the load path")
    # DLAA and the native AA are no stop of the Upscaler row, and the row shows DLSS or FSR 4 while they run
    if "ur::engine(upscaler_mode())" not in function(menu, "int upscaler() {") or "temporal_mode()" not in function(menu, "int upscaler_mode() {"):
        raise SystemExit("upscaler(): the Upscaler row shows DLAA and FSR 4 Native AA, which are Render resolution choices now")
    resolution_text = function(menu, "const char* tx_resolution(int v) {")
    if '"DLAA (native)"' not in resolution_text or '"Native AA (FSR 4)"' not in resolution_text or "ur::kNativeAa" not in resolution_text:
        raise SystemExit("tx_resolution: the native choice is not named DLAA (native) with DLSS and Native AA with FSR 4")
    if '"NVIDIA DLAA"' in menu or '"AMD FSR 4 Native AA"' in menu:
        raise SystemExit("vk_menu.cpp: DLAA or FSR 4 Native AA is named as an Upscaler choice")
    with tempfile.TemporaryDirectory(prefix="upscaler-rows-") as tmp:
        exe = Path(tmp) / "upscaler-rows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_upscaler_rows.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
