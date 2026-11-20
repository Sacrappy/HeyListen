"""(HeyListen, from Longshot 83b161e3) NVIDIA DLAA before the game's bloom and tone map (gpu/vk/pre_bloom.h, vk_temporal.cpp temporal_pre_bloom): where in a
frame it runs. The place is learned from the frame before: the first draw that reads the scene's colour after the frame's last
draw into it, by its program and its place among the frame's readers. The C++ part plays the Market frame's passes (a frame
dump of main, DLAA at 5120x2160: the game's copies of the scene, more draws into it, the bloom's first draw, the scene mapping
draw) and the cases around them: never at a copy the scene is drawn into after, a late draw teaching nothing, another area's
passes, a frame without bloom, once a frame, the list's end. The Python part checks that the renderer uses it as described:
vk_draw.cpp gives every draw to temporal_pre_bloom after its descriptors and before its pass, the chain learns at the gameplay
upscale and copies there after DLAA ran before the bloom, DLSS makes its feature for HDR colour then, and the row is in the menu. No game, GPU or NVIDIA SDK required.

python -I -B tests/test_dlss_pre_bloom.py [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import between, block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def check_sources():
    vk = ROOT / "gpu" / "vk"
    chain = (vk / "vk_temporal.cpp").read_text(encoding="utf-8")
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    dlss = (vk / "vk_dlss.cpp").read_text(encoding="utf-8")

    # (every draw, after its descriptors listed what it samples and before its pass: the hook ends the open pass itself)
    hook = "const bool pre_recorded = temporal_pre_bloom(cmd, p.key, gfx_sampled(), at.color, at.n);"
    assert draw.count(hook) == 1, "vk_draw.cpp: draw() gives each draw to temporal_pre_bloom once"
    assert draw.index("if (!same_desc) gfx_sampled() = draw_sampled();") < draw.index(hook) < \
        draw.index("writes = draw_hazards(cmd, p, s, at"), "draw(): the hook after the descriptors, before the pass"
    # (#1672: DLAA's passes and its actor motion's draws bind a pipeline, an upright viewport and descriptors of their own:
    # the draw forgets what it thought bound and binds all of its own again, as after the network's stand-in)
    after = draw[draw.index(hook):]
    # (HeyListen: its bound-state flags, as after temporal_stand_in)
    reset = "last_dyn_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;"
    assert after.index("if (pre_recorded) {") < after.index(reset) < after.index("writes = draw_hazards(cmd, p, s, at"), \
        "draw(): the bound state reset after DLAA's commands, before the draw sets its own"
    assert "vertex_set.on = false, last_ib.on = false;" in after[:after.index("writes = draw_hazards(")], \
        "draw(): its vertex streams and index buffer bound again after DLAA's commands"
    assert "if (!same_desc || pre_recorded) {" in after, "draw(): its descriptors pushed again after DLAA's commands"

    pre = block(chain, "bool temporal_pre_bloom(VkCommandBuffer cmd")
    assert "s.pre.scene_draw()" in pre and "s.pre.sampler(key, s.pre_trigger)" in pre, "temporal_pre_bloom: the frame's list"
    assert pre.index("s.pre.sampler(") < pre.index("end_pass(cmd);") < pre.index("draw_engine(cmd, s, mode, cam, key, true,"), \
        "temporal_pre_bloom: the pass ended before DLAA's compute"
    assert "full_barrier(cmd);" in pre[pre.index("draw_engine("):], "temporal_pre_bloom: the draw waits for DLAA's picture"
    assert "s.pre.ran = false" in pre, "temporal_pre_bloom: a failed run leaves the frame to the network"
    assert "return s.pre.ran = false, true;" in pre and "    return true;\n" in pre, \
        "temporal_pre_bloom: true once it recorded commands, DLAA drawn or not"
    # (the scene before its tone map is linear: taken as it is, written back as it is, by a feature made for HDR colour)
    engine = block(chain, "bool draw_engine(VkCommandBuffer cmd")
    assert "pre ? s.prep_lin" in engine, "draw_engine: the scene's linear colour taken as it is"
    assert "if (pre && (!engine.e->hdr || !engine.e->hdr()))" in engine, "draw_engine: only a feature made for HDR colour"
    assert "s.pre_last != pre" in engine, "draw_engine: a change of place starts a new history"
    assert "prep_lin_data{16384.0f, VK_FALSE}" in chain, "make_pipelines: prep_lin neither squares nor clamps at 1"

    up = block(chain, "bool temporal_upscale(VkCommandBuffer cmd")
    assert "pre_learn(s);" in up, "temporal_upscale: the frame teaches the next"
    assert up.index("pre_learn(s);") < up.index("if (s.pre_drawn == now)") < up.index("pre_copy(cmd, in_view, out,") \
        < up.index("return draw_engine(cmd, s, mode, cam, key, false,"), \
        "temporal_upscale: after DLAA ran before the bloom the network's input is copied, else the engine runs there"

    note = block(chain, "void temporal_note_scene_draw(const Texture*")
    assert "pre_bloom_wanted(temporal_mode())" in note and "s.out_w.load(" in note, \
        "temporal_note_scene_draw: only DLAA, the scene at the output's size"

    prepare = between(dlss, "int prepare(VkCommandBuffer cmd", "bool start()")
    assert "hdr_allowed && (present_native_pq.load(std::memory_order_relaxed) || temporal_hdr_input())" in prepare, \
        "vk_dlss.cpp prepare: an HDR feature for the scene's linear colour"

    wanted = block(chain, "bool pre_bloom_wanted(int mode)")
    assert "opts.dlss_pre_bloom != 0" in wanted, "pre_bloom_wanted: the DLAA before the bloom row (off by default)"
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    assert '{TAB_GRAPHICS, "DLAA before the bloom", &opts.dlss_pre_bloom, 0, 1, 1, tx_onoff,' in menu, "vk_menu.cpp: the row"
    assert menu.index('"DLSS model", &opts.dlss_model') < menu.index('"DLAA before the bloom", &opts.dlss_pre_bloom') < \
        menu.index('"Frame generation by", &opts.framegen_by'), "the row under DLSS model, with the upscaler rows"
    assert "ur::pre_bloom_shown(dlss_why() == DLSS_OK, upscaler_mode())" in block(menu, "bool row_shown(int i)"), "row_shown"
    assert "ur::pre_bloom_fixed(upscaler_mode(), pre_bloom_env(), pre_bloom_parallel())" in block(menu, "bool row_fixed(const int* value)"), "row_fixed"
    assert 'if (r.value == &opts.dlss_pre_bloom) return "dlss_pre_bloom";' in menu, "ini_key: dlss_pre_bloom"
    assert 'getenv("RX_VK_DLSS_PRE_BLOOM"); e && *e) opts.dlss_pre_bloom = atoi(e) != 0;' in menu, \
        "RX_VK_DLSS_PRE_BLOOM over the row for a run"
    assert 'env_switch("RX_VK_PARALLEL") >= 2' in wanted, "pre_bloom_wanted: off with the parallel replay"
    assert "mode == ur::DLAA" in wanted, "pre_bloom_wanted: DLAA only"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="dlss-pre-bloom-") as tmp:
        exe = Path(tmp) / "dlss-pre-bloom"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_dlss_pre_bloom.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
