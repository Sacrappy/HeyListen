"""The Upscaler row's Off (opts.dlss == 6, gpu/vk/upscaler_rows.h OFF): no upscaling network runs, the gameplay networks' dispatch
draws the plain upscale of the 3D input instead. Checked: the value is new (the saved 0 to 5 keep their meaning) and is saved and
loaded under the row's own key; the row steps through it, on every GPU; the menu shows it and says what it is; Off is no engine
of the temporal chain (DLSS's and FSR 4's state is let go as for the game's own); the dispatch takes the stand-in after FSR 3.1's
inputs and before the TAAU stand-in; the stand-in itself (the real function cut out of vk_draw.cpp, GPU boundaries as spies)
draws with no edge smoothing, only the gameplay networks and only the scene's formats; the stub's fallback draws without FXAA;
the launcher and the docs list it. No game or GPU required.

python -I -B tests/test_upscaler_off.py [--cxx clang++]
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def function(source, start):
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def need(ok, message):
    if not ok:
        raise SystemExit(message)


def block(source, marker):
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


PREFIX = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
using VkCommandBuffer = int;
using VkImageView = int;
using VkFormat = int;
constexpr int VK_IMAGE_VIEW_TYPE_2D = 2, VK_FORMAT_UNDEFINED = 0, VK_FORMAT_B10G11R11_UFLOAT_PACK32 = 7, VK_FORMAT_R8G8B8A8_UNORM = 37, VK_FORMAT_R16G16B16A16_SFLOAT = 97;
enum class ViewUse { Storage, Sample };
struct Desc { unsigned count = 1; int format = 23; };
struct Info { std::vector<Desc> texture_descriptors, image_descriptors; bool uses_cooperative_matrix = false; };
struct Stage { Info info; };
struct NvnState {};
struct Program { uint64_t key; std::vector<Stage> stages; };
struct Texture { int format = VK_FORMAT_B10G11R11_UFLOAT_PACK32; uint32_t width = 0, height = 0; };
static Texture in_tex, out_tex;
static struct { bool push_descriptor = true; } dev;
static bool vkCmdPushDescriptorSetKHR = true;
struct ImageInfo { VkImageView imageView; };
static struct DrawWork { std::vector<ImageInfo> imgs; } draw_work;
static DrawWork& dw() { return draw_work; }
static bool off_now = true, upscale_result = true;
static bool upscaler_off() { return off_now; }
static bool tone = false;   // (the PsychoV35 tone mapper: its RGBA16F scene, HeyListen after Longshot b80a68f1)
static bool scene_tonemap_on() { return tone; }
static int upscale_calls, logs;
static VkCommandBuffer forwarded_cmd;
static VkImageView forwarded_input, forwarded_output;
static Texture* forwarded_texture;
static uint32_t forwarded_level;
static uint64_t forwarded_key;
static std::string forwarded_text;
static bool forwarded_fxaa;
static uint32_t handle_of(const NvnState&, const Stage&, const Desc&, uint32_t index) { return index == 0 ? 5 : 0; }
static int image_format(int f) { return f + 100; }
static VkImageView texture_view_of(uint32_t handle, int type, ViewUse use, int format, Texture** out = nullptr, uint32_t* level = nullptr) {
    if (handle != 5 || type != VK_IMAGE_VIEW_TYPE_2D) return 0;
    if (format == VK_FORMAT_R16G16B16A16_SFLOAT) return use == ViewUse::Sample ? 51 : 52;   // (the RGBA16F views)
    if (use == ViewUse::Storage) { if (format != 123) return 0; *out = &out_tex; *level = 1; return 99; }
    *out = &in_tex;
    return 98;
}
static bool typed_upscale(VkCommandBuffer cmd, VkImageView input, VkImageView output, Texture* out, uint32_t level, uint64_t key,
                          const char* text, bool fxaa) {
    ++upscale_calls; forwarded_cmd = cmd; forwarded_input = input; forwarded_output = output; forwarded_texture = out;
    forwarded_level = level; forwarded_key = key; forwarded_text = text; forwarded_fxaa = fxaa;
    return upscale_result;
}
#define logf(...) (++logs)
'''

SUFFIX = r'''
static unsigned failures = 0;
static void check(bool ok, const char* what) { if (!ok) { ++failures; fprintf(stderr, "FAIL: %s\n", what); } }
int main() {
    NvnState state{};
    const uint64_t taau = 0xfd029a317a9a4b94ull, same = 0x9dc98871eb7fc101ull, fixed_share = 0x6676619641320ba7ull;
    Stage stage;
    stage.info.texture_descriptors = {{1, 23}};
    stage.info.image_descriptors = {{1, 23}};
    stage.info.uses_cooperative_matrix = true;
    dw().imgs = {{11}, {22}};
    in_tex.width = 1366, out_tex.width = 2048 << 1;   // (the output's width is shifted by the level read: 2048)
    const auto run = [&](uint64_t key, const Stage& st) {
        Program p{key, {st}};
        return off_stand_in(19, state, p);
    };
    // (a gameplay network by its key: the picture is drawn, plain: no FXAA (the typed upscale's last argument), nothing else touched)
    check(run(taau, stage) && upscale_calls == 1, "the 4/3 network is drawn as the plain upscale");
    check(forwarded_cmd == 19 && forwarded_input == 11 && forwarded_output == 22 && forwarded_texture == &out_tex &&
              forwarded_level == 1 && forwarded_key == taau,
          "the network's own views, output and level are the stand-in's");
    check(!forwarded_fxaa, "Upscaler Off draws without edge smoothing");
    check(forwarded_text == "Upscaler Off", "the helper is told why");
    check(run(same, stage) && forwarded_key == same && !forwarded_fxaa, "the same-size pass (Render resolution 100%) is drawn plain too");
    // (the key alone is enough: a known network need not have the shape)
    Stage plain = stage;
    plain.info.uses_cooperative_matrix = false;
    in_tex.format = VK_FORMAT_R8G8B8A8_UNORM;
    check(run(taau, plain), "a gameplay network by its key does not need the shape or the input's format");
    in_tex.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    // (another network of the upscale's shape, the fixed shares: only the scene's formats, and only an upscale)
    int before = upscale_calls;
    check(run(fixed_share, stage) && upscale_calls == before + 1, "a fixed share's network of the shape is drawn");
    before = upscale_calls;
    check(!run(fixed_share, plain) && upscale_calls == before, "no cooperative matrices: not a network of the shape");
    in_tex.format = VK_FORMAT_R8G8B8A8_UNORM;
    check(!run(fixed_share, stage) && upscale_calls == before, "a movie network (RGBA8 in) is left to run");
    in_tex.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    in_tex.width = 2048 << 1;
    check(!run(fixed_share, stage) && upscale_calls == before, "a network of the shape that does not enlarge is left to run");
    in_tex.width = 1366;
    Stage two = stage;
    two.info.texture_descriptors = {{1, 23}, {1, 23}};
    check(!run(fixed_share, two) && upscale_calls == before, "a network with two inputs is left to run");
    // (the output must be the scene's colour format; a view that cannot be had leaves the network to run)
    out_tex.format = VK_FORMAT_R8G8B8A8_UNORM;
    check(!run(taau, stage) && upscale_calls == before, "an output that is not B10G11R11 is left to run");
    out_tex.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    dw().imgs = {{11}};
    check(!run(taau, stage) && upscale_calls == before, "descriptors too few: left to run");
    dw().imgs = {{11}, {22}};
    dev.push_descriptor = false;
    check(!run(taau, stage) && upscale_calls == before, "no push descriptors: left to run");
    dev.push_descriptor = true;
    // (a failed helper: the network runs)
    upscale_result = false;
    check(!run(taau, stage), "a typed upscale that fails is not taken for drawn");
    upscale_result = true;
    // (the row not on Off, or a switch over it: nothing is touched at all)
    // (PsychoV35: an RGBA16F pair through the RGBA16F views, plain; never mixed with the packed format, never without it)
    in_tex.format = out_tex.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    before = upscale_calls;
    check(!run(taau, stage) && upscale_calls == before, "RGBA16F without the PsychoV35 tone mapper is left to run");
    tone = true;
    check(run(taau, stage) && upscale_calls == before + 1 && forwarded_input == 51 && forwarded_output == 52 && !forwarded_fxaa,
          "PsychoV35's RGBA16F pair is drawn through its RGBA16F views");
    check(run(fixed_share, stage) && upscale_calls == before + 2, "a fixed share's RGBA16F network of the shape is drawn");
    in_tex.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    check(!run(taau, stage) && upscale_calls == before + 2, "a packed input into an RGBA16F output is left to run");
    in_tex.format = VK_FORMAT_R16G16B16A16_SFLOAT, out_tex.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    check(run(taau, stage) && forwarded_input == 11, "a known network's packed output keeps its own views, whatever its input");
    in_tex.format = out_tex.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    tone = false;
    off_now = false;
    before = upscale_calls;
    check(!run(taau, stage) && !run(same, stage) && !run(fixed_share, stage) && upscale_calls == before, "not Off: no network is taken");
    printf("failures=%u calls=%d\n", failures, upscale_calls);
    return failures ? 1 : 0;
}
'''


def stand_in_fixture(draw):
    gameplay = re.search(r"^bool gameplay_network\(uint64_t key\) \{.*\}$", draw, re.M)
    need(gameplay, "vk_draw.cpp: gameplay_network is gone")
    return PREFIX + gameplay.group(0) + "\n" + block(draw, "static bool off_stand_in(") + "\n" + SUFFIX


def check_source():
    rows = read("gpu/vk/upscaler_rows.h")
    need(re.search(r"enum \{ GAME = 0, DLSS = 1, DLAA = 2, FSR3 = 3, FSR4 = 4, FSR4_AA = 5, OFF = 6, kUpscalers = 7 \};", rows),
         "upscaler_rows.h: Off is not the value 6 after the saved ones (GAME..FSR4_AA keep 0 to 5)")
    step = function(rows, "inline int step_upscaler(int v, int dir, unsigned available) {")
    need("bit(OFF)" in step, "step_upscaler: Off is not always in the row")
    menu = read("gpu/vk/vk_menu.cpp")
    need(re.search(r'\{TAB_GRAPHICS, "Upscaler", &opts\.dlss, 0, ur::kUpscalers - 1, 1,', menu),
         "vk_menu.cpp: the Upscaler row does not run over all of the values (0 to kUpscalers - 1)")
    key = function(menu, "const char* ini_key(const Row& r) {")
    need(re.search(r'&opts\.dlss\)\s*return\s*"dlss"', key), "ini_key: Off would not be saved as dlss")
    need("std::min(std::max(v, r.lo), r.hi)" in menu, "load: a row's value is no longer kept to its range")
    available = function(menu, "unsigned upscalers_available() {")
    need("ur::bit(ur::OFF)" in available, "upscalers_available: Off is not available on every GPU")
    need("upscaler_off()" in function(menu, "int upscaler_mode() {") and "ur::OFF" in function(menu, "int upscaler_mode() {") and
         "upscaler_mode()" in function(menu, "int upscaler() {"),
         "upscaler(): the row does not show Off while it draws")
    need('"Off"}' in function(menu, "const char* tx_upscaler(int) {") or '"Off"};' in function(menu, "const char* tx_upscaler(int) {"),
         "tx_upscaler: no name for Off")
    help_text = function(menu, "const char* hn_upscaler(int v, int variant) {")
    for words in ("No upscaling network", "Render resolution", "no anti-aliasing", "glitches"):
        need(words in help_text, "hn_upscaler: Off's help does not say %r" % words)
    shown = function(menu, "bool row_shown(int i) {")
    need(not re.search(r"&opts\.dlss\b", shown), "row_shown hides the Upscaler row: Off is for every GPU")
    fixed = function(menu, "bool row_fixed(const int* value) {")
    need(not re.search(r"&opts\.dlss\b", fixed), "row_fixed makes the Upscaler row fixed: it always has the game's own and Off")
    # saved choices and the other rows
    need("snap(upscaler()" in function(menu, "int resolution_scale() {"),
         "resolution_scale: Render resolution is not read through the Upscaler shown")
    need("opts.dlss" not in function(menu, "void apply_preset(int p) {"), "apply_preset: a preset sets the Upscaler (the player's choice, Off too)")
    temporal = read("gpu/vk/vk_temporal.cpp")
    mode = function(temporal, "int temporal_mode() {")
    need("!ur::is_dlss(v) && !ur::is_fsr4(v)) return ur::GAME" in mode,
         "temporal_mode: Off (6) would be an engine of the chain: DLSS's and FSR 4's state is not let go")
    off = function(temporal, "bool upscaler_off() {")
    for part in ("opts.dlss == ur::OFF", "!temporal_env_chosen()", "!fsr3_wanted()"):
        need(part in off, "upscaler_off: %s is missing (a switch for an engine goes over the row)" % part)
    idle = function(temporal, "void temporal_idle() {")
    need("temporal_wanted()" in idle and "s.drawing->release()" in idle, "temporal_idle: DLSS and FSR 4 are not let go off the engines")
    perf = read("gpu/vk/perf_report.cpp")
    need('"FSR 4 AA", "Off"}' in perf, "perf_report: no name for Off")


def check_dispatch():
    draw = read("gpu/vk/vk_draw.cpp")
    d = function(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    order = [d.find(c) for c in ("fsr3_stand_in(cmd, s, p)", "temporal_stand_in(cmd, s, p)", "off_stand_in(cmd, s, p)",
                                 "taau_stand_in(cmd, s, p)")]
    need(-1 not in order and order == sorted(order),
         "dispatch: the Off stand-in is not after FSR 3.1's (frame generation's inputs) and DLSS's, and before the TAAU stand-in")
    need("return void(st_dispatches++)" in d[order[2]:order[3]], "dispatch: the Off stand-in's dispatch is not counted and left out")
    stand = block(draw, "static bool off_stand_in(")
    need("upscaler_off()" in stand and "typed_upscale(" in stand and ", false)" in stand, "off_stand_in does not draw the plain upscale")
    need("temporal_upscale(" not in stand and "fsr3_upscale(" not in stand, "off_stand_in runs an engine")
    stub = function(draw, "bool upscale_stub(VkCommandBuffer cmd, const NvnState& s, uint64_t key) {")
    need(re.search(r"gameplay_network\(key\) && !upscaler_off\(\)\)", stub),
         "upscale_stub: a network left out on this card is still drawn with FXAA while the row says Off")
    # no other call turns the edge smoothing on: test_taau_fxaa.py holds the bits; here that Off's calls never set the flag
    need(not re.search(r'typed_upscale\([^;]*"Upscaler Off", true\)', draw), "Off asks for edge smoothing")


def check_docs():
    launcher = (REPO / "recomplauncher.json").read_text(encoding="utf-8")
    need(re.search(r'"label": "Off",\s*"set": \{\s*"dlss": "6"', launcher), "recomplauncher.json: no Off in the Upscaler choices")
    for path, words in (("docs/USER_GUIDE.md", ("**Off**", "dlss=6")), ("docs/recomplauncher.md", ("6 Off",)),
                        ("engine/readme.md", ("Off (every build and GPU, `dlss=6`",)), ("engine/docs/development.md", ("dlss=6",))):
        text = (REPO / path).read_text(encoding="utf-8")
        for w in words:
            need(w in text, "%s does not say %r" % (path, w))


def check_behaviour(cxx):
    cpp = stand_in_fixture(read("gpu/vk/vk_draw.cpp"))
    with tempfile.TemporaryDirectory(prefix="upscaler-off-") as temp:
        folder = Path(temp)
        source, exe = folder / "fixture.cpp", folder / "fixture.exe"
        source.write_text(cpp, encoding="utf-8")
        subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)], cwd=folder, check=True)
        subprocess.run([str(exe)], cwd=folder, check=True)
        # the row's values: the saved 0 to 5 as before, 6 Off, a value past it kept to the row's range as the load does
        rows = folder / "rows.cpp"
        rows.write_text(r'''
#include "%s"
#include <algorithm>
#include <cstdio>
namespace ur = mkwvk::upscaler_rows;
int main() {
    const int lo = 0, hi = ur::kUpscalers - 1;
    for (int saved = 0; saved <= 6; saved++) if (std::min(std::max(saved, lo), hi) != saved) return 1;
    if (std::min(std::max(7, lo), hi) != ur::OFF || std::min(std::max(-1, lo), hi) != ur::GAME) return 2;
    // (a build with nothing but the game's own and Off: the row has the two, one Right away from each other)
    if (ur::step_upscaler(ur::GAME, 1, ur::bit(ur::GAME)) != ur::OFF || ur::step_upscaler(ur::OFF, 1, ur::bit(ur::GAME)) != ur::GAME) return 3;
    std::puts("rows ok");
}
''' % (ROOT / "gpu" / "vk" / "upscaler_rows.h").as_posix(), encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(rows), "-o", str(folder / "rows.exe")], cwd=folder, check=True)
        subprocess.run([str(folder / "rows.exe")], cwd=folder, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    check_source()
    check_dispatch()
    check_docs()
    check_behaviour(args.cxx)
    print("PASS: the Upscaler row's Off: a new value 6, always available, no network run, plain upscale drawn")


if __name__ == "__main__":
    main()
