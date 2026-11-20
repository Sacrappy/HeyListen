"""The performance summary (gpu/vk/perf_report.h, perf_report.cpp), ported from Longshot's #983, with no game and no GPU.

- tests/test_perf_report.cpp: the buckets, the interval, what a hitch is and what it waited on, the pacing misses, the JSON
  (every key of Longshot's field tables, the limits where they cut a field, the parts' average and the hitch detail's keys),
  the release flag, and what a frame costs.
- The wiring in the source: the summary is gated by the menu's row AND an address in the build (nothing is measured or written
  without both), or HOS_PERF_REPORT=1 for a local summary the sender cannot send; the row defaults to off and is saved as
  send_performance_stats, turning it off deletes perf_pending and perf_sent, the host's perf_info is in the API (version 14),
  and the frame hook is in the shown-frame count.
- The measurement's wiring (v0.4.3): the frame's parts are timed on the presenting thread (perf_parts.h's scopes in
  vk_replay.cpp's submit/present paths and present_image's swapchain waits), the read-backs' waits are counted where they were
  already timed (invalidate), the pacing sleep is measured host-side (nn_vi.c) and pushed at each present (nvn_native.c), the
  loading flag is pushed when it changes (hos_main.c), and the ids agree on both sides of the renderer's boundary.

    python -I -B tests/test_perf_report.py [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)
    print("PASS:", what)


def wiring():
    cpp = (ROOT / "gpu/vk/perf_report.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    menu_h = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    check(re.search(r"int send_perf = 0;", menu_h) and re.search(r"int send_crash = 0;", menu_h),
          "both rows default to off (0) in the options")
    check('{TAB_GAMEPLAY, "Send performance statistics", &opts.send_perf, 0, 1, 1,' in menu and
          '{TAB_GAMEPLAY, "Send crash reports automatically", &opts.send_crash, 0, 1, 1,' in menu,
          "the two rows are on the Gameplay tab, Off or On")
    check('return "send_performance_stats";' in menu and 'return "send_crash_reports";' in menu,
          "they are saved with the other options under the keys host/hos_telemetry.h reads")
    check("Not available in this build" in menu and "!telemetry_available()" in menu,
          "without an address the rows say so and are fixed (no arrows)")
    check(re.search(r"return \(available \|\| perf_local\(\)\) && opts\.send_perf == 1;", cpp) and
          'getenv("HOS_PERF_REPORT")' in cpp,
          "measuring needs the row on, and an address in the build or HOS_PERF_REPORT (a local summary: no address, never sent)")
    check("hos_telemetry_base(" in cpp and "HOS_TELEMETRY_ENV" in cpp, "the address is the build's or HL_TELEMETRY_URL's, by the host's rule")
    switch_off = cpp[cpp.index("void switch_off() {"):]
    switch_off = switch_off[:switch_off.index("\n}\n")]
    check('"perf_pending", "perf_sent"' in switch_off and "perf_measure.store(false" in switch_off,
          "turning the row off stops the measuring and deletes perf_pending and perf_sent")
    check("perf_frame_shown();" in menu, "the frame hook is where the shown frames are counted")
    check(re.search(r"#define MKWVK_API_VERSION 1[4-9]\b", api) and "perf_info" in api, "the host's perf_info is in the API, version 14 or later")
    check("hos_perf_info" in (ROOT / "host/rx_backend.c").read_text(encoding="utf-8"), "rx_backend.c fills perf_info")
    check("perf_report_final(-1);" in (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8"),
          "the window's close writes the last summary")
    check("parts_ms" in (ROOT / "gpu/vk/perf_report.h").read_text(encoding="utf-8"), "the payload keeps Longshot's parts_ms")
    # frame generation, the DLSS model and the render scale ride with the upscaler: read with the other settings, from what runs
    read_settings = cpp[cpp.index("void read_settings() {"):]
    read_settings = read_settings[:read_settings.index("\n}\n")]
    check("machine.upscaler = upscaler_name(upscaler_rows::mode_for(opts.dlss, opts.resolution));" in read_settings and "read_effective_settings();" in read_settings,
          "the settings are read together, the upscaler as before")
    effective = cpp[cpp.index("void read_effective_settings() {"):]
    effective = effective[:effective.index("\n}\n")]
    for what in ("framegen_engine(&mult, &automatic)", "pr::framegen_name(", "machine.framegen_x = mult", "machine.framegen_auto = automatic",
                 "fsr3_wanted() ? (int)upscaler_rows::FSR3 : temporal_mode()", 'getenv("RX_VK_DLSS_PRESET")',
                 "upscaler_rows::dlss_preset(opts.dlss_model,", "upscaler_rows::is_dlss(up)", "machine.render_scale = ",
                 "upscaler_rows::full_scale(up) ? 100 : upscaler_rows::percent(upscaler_rows::snap(up, opts.resolution))"):
        check(what in effective, "the effective settings: " + what)
    vk_dlss = (ROOT / "gpu/vk/vk_dlss.cpp").read_text(encoding="utf-8")
    check('getenv("RX_VK_DLSS_PRESET")' in vk_dlss and "upscaler_rows::dlss_preset(opts.dlss_model, env && *env ? env : nullptr)" in vk_dlss,
          "the DLSS model is the preset DLSS is asked for (vk_dlss.cpp wanted_preset's rule)")
    rows = (ROOT / "gpu/vk/upscaler_rows.h").read_text(encoding="utf-8")
    names = re.search(r'static const char\* const t\[\] = \{("Game network".*?)\};', cpp[cpp.index("const char* upscaler_name"):], re.S)
    count = int(re.search(r"kUpscalers = (\d+)", rows).group(1))
    check(names and len(re.findall(r'"[^"]*"', names.group(1))) == count, "upscaler_name has a name for each of upscaler_rows.h's %d upscalers" % count)
    docs = (ROOT / "../docs/performance-statistics.md").read_text(encoding="utf-8")
    check(all("| `%s` |" % key in docs for key in ("framegen", "framegen_x", "framegen_auto", "dlss_model", "render_scale")),
          "docs/performance-statistics.md lists the new fields")


FRAMEGEN_STUBS = r"""
#include <cstdio>
#include <cstring>
struct Options { int framegen = 0; } opts;
static bool g_on = true, g_fsr = false;
static int g_by = 0;   // (framegen_rows::BY_OWN; the Frame generation by row's choice)
static bool framegen_on() { return g_on; }
static int multiplier() { return opts.framegen == 1 ? 3 : opts.framegen; }   // (Auto: the display's cap, here 3)
static int framegen_by_now() { return g_by; }
namespace framegen_rows { enum { BY_OWN, BY_DLSS, BY_FSR3 }; }
namespace fsr3fi { static unsigned frames_wanted() { return g_fsr ? 1u : 0u; } }
%s
#define CHECK(name, cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %%s\n", name); return 1; } } while (0)
int main() {
    int m = 0; bool a = true;
    opts.framegen = 0; g_on = false;
    CHECK("off: none, 1x", framegen_engine(&m, &a) == 0 && m == 1 && !a);
    opts.framegen = 4; g_on = false;
    CHECK("row on, interpolation not running (RX_VK_FRAMEGEN=0, offscreen, images not made): none, 1x", framegen_engine(&m, &a) == 0 && m == 1 && !a);
    g_on = true;
    CHECK("4x: the renderer's own", framegen_engine(&m, &a) == 1 && m == 4 && !a);
    opts.framegen = 2; g_fsr = false;
    CHECK("2x without FSR 3.1: the renderer's own", framegen_engine(&m, &a) == 1 && m == 2 && !a);
    g_fsr = true;
    CHECK("2x where FSR 3.1 runs: FSR 3.1", framegen_engine(&m, &a) == 2 && m == 2 && !a);
    opts.framegen = 3;
    CHECK("3x: FSR 3.1 makes the midpoint only, the renderer's own", framegen_engine(&m, &a) == 1 && m == 3 && !a);
    opts.framegen = 1;
    CHECK("Auto: the renderer's own, its limit, marked", framegen_engine(&m, &a) == 1 && m == 3 && a);
    g_by = framegen_rows::BY_DLSS;
    CHECK("Auto with the by row on DLSS: DLSS FG, its limit, marked", framegen_engine(&m, &a) == 3 && m == 3 && a);
    opts.framegen = 4;
    CHECK("DLSS FG at 4x: its engine", framegen_engine(&m, &a) == 3 && m == 4 && !a);
    opts.framegen = 2; g_fsr = true;
    CHECK("DLSS FG chosen over FSR 3.1: its engine", framegen_engine(&m, &a) == 3 && m == 2 && !a);
    g_by = framegen_rows::BY_OWN;
    CHECK("2x where FSR 3.1 runs, the by row elsewhere: FSR 3.1", framegen_engine(&m, &a) == 2 && m == 2 && !a);
    std::puts("ok");
    return 0;
}
"""


def framegen_engine(cxx):
    source = (ROOT / "gpu/vk/vk_framegen.cpp").read_text(encoding="utf-8")
    start = source.index("int framegen_engine(int* mult, bool* automatic) {")
    body = source[start:source.index("\n}\n", start) + 3]
    with tempfile.TemporaryDirectory(prefix="perf-framegen-") as t:
        src, exe = Path(t) / "fg.cpp", Path(t) / "fg.exe"
        src.write_text(FRAMEGEN_STUBS % body, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-O1", str(src), "-o", str(exe)], check=True, timeout=300)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        check(out.returncode == 0, "framegen_engine: off, refused, builtin, FSR 3.1 at 2x only, Auto's limit, DLSS FG where it is chosen" + (": " + out.stderr.strip() if out.returncode else ""))


def parts_wiring():
    """The frame's parts are measured on the presenting thread, and the causes that were always 0 are fed."""
    parts = (ROOT / "gpu/vk/perf_parts.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    report = (ROOT / "gpu/vk/perf_report.cpp").read_text(encoding="utf-8")
    vi = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    nvn = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    main = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    check("kOptPace = 90" in parts and "kOptLoading = 91" in parts,
          "the host's pushes have their ids (perf_parts.h)")
    check("perf_parts::Scope perf_entry(perf_parts::t_render);" in replay and replay.count("perf_parts::t_render") >= 4,
          "the renderer's entries are timed: submit_q, present and the replay thread's jobs")
    check(replay.count("perf_parts::Scope perf_wait(perf_parts::t_wait)") == 1 and
          "recording_wait(r);" in replay,
          "a recording slot's wait is timed once, at begin_recording")
    check(replay.count("perf_parts::Scope perf_wait(perf_parts::t_present)") == 4,
          "the swapchain's waits are timed: the blit's fence, the image acquire, its fence and the present call")
    check("st_rb_wait_ns" in replay and "readback_wait_ns()" in report and "int64_t readback_wait_ns();" in
          (ROOT / "gpu/vk/perf_report.h").read_text(encoding="utf-8"),
          "the read-backs' waits are counted (invalidate) and read per frame")
    check("pace_since_present_us" in vi and "hos_vi_pace_us" in vi and "hos_sleep_ns(until - now);" in vi,
          "the pacing sleep is measured host-side (nn_vi.c)")
    check("rx_option_set(90 /* HOS_OPT_PERF_PACE" in nvn and "rx_nvn_present(win_tex(o, i), o->crop);" in nvn and
          nvn.index("rx_option_set(90") < nvn.index("rx_nvn_present(win_tex(o, i), o->crop);"),
          "the frame's pacing sleep is pushed before the present (nvn_native.c)")
    check("rx_option_set(91 /* HOS_OPT_PERF_LOADING" in main and "0xef2b60c2u" in main,
          "the loading flag (Sequence_IsLoadingBanc) is pushed when it changes (hos_main.c)")
    check("perf_parts::kOptPace" in menu and "perf_parts::kOptLoading" in menu and "note_pace(value)" in menu,
          "the menu's option_set takes the two pushes to perf_parts")
    check("perf_parts::pace_us.exchange(0" in report and "perf_parts::loading.load(" in report and
          "thread_cpu_ns()" in report,
          "the summary reads the pacing sleep, the loading flag and the presenting thread's CPU time per frame")
    check("const bool loading = load_now || load_last || map_switched;" in report and "load_last = load_now, map_switched = false;" in report,
          "a load's stall frame is the one presented once it has ended, or the frame its map changed on: the loading flag is kept for it")
    check("if (current) map_switched = true;" in report,
          "a map change is the area change's load (the game thread is inside the load, so its tick never sees the flag)")
    check("perf_parts::on.store(true" in report and "perf_parts::on.store(false" in report,
          "the parts' scopes are turned on and off with the row")


def unit(cxx):
    with tempfile.TemporaryDirectory(prefix="perf-report-") as t:
        exe = Path(t) / "unit.exe"
        subprocess.run([cxx, "-std=c++20", "-O2", "-I", str(ROOT / "gpu/vk"), str(ROOT / "tests/test_perf_report.cpp"), "-o", str(exe)],
                       check=True, timeout=300)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        print(out.stdout.strip())
        check(out.returncode == 0, "the summary's buckets, hitches, JSON, release flag and cost" + (": " + out.stderr.strip() if out.returncode else ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    wiring()
    framegen_engine(args.cxx)
    parts_wiring()
    unit(args.cxx)


if __name__ == "__main__":
    main()
