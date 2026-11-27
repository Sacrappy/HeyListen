// Copyright (C) 2026 Sacrappy

// The Insert menu. Drawn with Dear ImGui over the window's image at the last step of a present (menu_record; vk_imgui.h, in the
// game's style: vk_ui.h), so it costs nothing while closed and it never touches the game's picture, nor what frame interpolation
// sees. The FPS counter and the start-up screen are drawn the same way. Insert or F1 opens and closes it by default.
// Up and Down choose, Left and Right change, Esc closes; the game gets no keyboard input while it is open.
// (hookshot) A gamepad's Home button, its touchpad's click (DualSense, DualShock 4), or Select and Start together, opens it too; the D-pad chooses and changes, the button
// the Switch's A is bound to changes too (and opens a page), the one B is bound to goes back (vk_menu_keys.h). The game gets no gamepad input while it is open either.
// Its Controls page binds the Switch's buttons to gamepad inputs and keys.
// (hookshot) The mouse is free while it is open (the window lets it go: WM_APP + 8, host/display.c and
// host/posix/display_sdl.c): the row under the cursor is selected, a click on a tab shows it, on a value's left or right
// half changes it down or up, on a toggle flips it, on a slider's track sets it (or drag it); the wheel scrolls the rows
// (never a value, wherever the cursor is); a right click is Esc. On the Cheats page and its lists a click is Enter (on a
// choice's or an amount's left or right half, Left or Right) and a right click goes back. The drawing records each control's
// place and the mouse is tested against that (vk_menu_hit.h).
// (hookshot) The rows that take a restart (Row::restart) are tagged "After restart" while their value is not the one the game
// started with, and the Graphics tab's help lists them. Closing the menu then asks whether to restart now (menu_restart.h):
// Later keeps the saved settings for the next start and is not asked again until they change; Restart now saves and
// closes the window as the player would, and the game starts again (WM_APP + 9: host/hos_relaunch.c).
#include <atomic>
#include <chrono>
#include <cmath>
#include <utility>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "vk_paths.h"
#include "vk_opt_ids.h"   // (HL_OPT_PROMPTS_LIVE, HL_OPT_PROMPTS_MISSING)
#include "../../host/hos_atomic_file.h"   // (the options file is written whole or not at all)
#include "vk_menu.h"
#include "vk_perf.h"
#include "perf_report.h"   // (telemetry_available, perf_frame_shown: the two telemetry rows)
#include "perf_parts.h"   // (the host's pushes at each present: the frame's pacing sleep, a map loading)
#include "vk_menu_hit.h"
#include "vk_draw.h"   // (pipeline_cache_save: Restart now keeps what was built)
#include "oot_cheat_items.h"
#include "vk_progress.h"   // (StartupView, version_text: the start-up screen)
#include "vk_imgui.h"   // (the renderer's side of the overlay: begin, fonts, logo, record)
#include "vk_ui.h"
#include "vk_menu_keys.h"   // (which gamepad buttons confirm and go back: those A and B are bound to)         // (how it looks: Dear ImGui, vk_imgui.h)
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)
#include "vk_dlss.h"
#include "vk_framegen.h"   // (framegen_on: the V-Sync mode row's hint when frame generation needs more images)
#include "shader_recompiler/ir_opt/psychov35.h"   // (PsychoV35's grading bank: psychov35_bank)
#include "vk_fsr3.h"      // (the Upscaler row: whether AMD FSR 3.1 runs on this GPU)
#include "vk_fsr3fi.h"    // (the Frame generation row: whether FSR 3.1 makes the frame)
#include "vk_framegen.h"  // (the Frame generation row: the multiplier as it runs, who makes the frames)
#include "framegen_rows.h"   // (what each Frame generation by offers; the Frame rate limit's share)
#include "vk_fsr4.h"
#include "vk_fsr4fg.h"
#include "vk_temporal.h"
#include "vk_reflex.h"
#include "upscaler_rows.h"
#include "menu_rows.h"   // (which dependent rows show: row_shown)
#include "menu_restart.h"
#include "present_mode.h"
#include "lowend_rule.h"
#include "lowend_measure.h"   // (the measure that decides whether an integrated GPU is too weak; the Volumetric fog row and the first-start rule)
#ifndef HOOKSHOT_VERSION   // (nx2/build.py gives it to this file: nx2/version.py)
#define HOOKSHOT_VERSION "unknown"
#endif

namespace mkwvk {
HWND app_window();

Options opts;

namespace {

enum MenuTab {
    TAB_GRAPHICS = 0,
    TAB_GAMEPLAY = 1,
    TAB_CONTROLS = 2,
    TAB_DEBUG = 3,   // (the OoT debug tools of #785, a tab of their own)
    TAB_COUNT = 4,
    TAB_MOTION = TAB_COUNT    // (hookshot: not a tab of the strip, the Motion page that the Controls tab's Motion row
                              // opens: one past the strip's tabs, so a tab added before TAB_COUNT moves it along)
};

namespace fgr = framegen_rows;   // (the Frame generation rows' modes, and the Frame rate limit's values)
struct Row {
    MenuTab tab;
    const char* name;
    int* value;
    int lo, hi, step;
    const char* (*text)(int v);
    const char* help;   // (shown under the list while the row is selected: what the row is, the same whatever its value)
    // (a line about the value chosen, under the help, or nullptr: hint(v, -1) is the one to show now for v, hint(v, 0),
    // hint(v, 1), ... every one v can have (those that depend on the other rows or on the GPU), until nullptr, so the
    // menu keeps the height of the tallest: measure_help)
    const char* (*hint)(int v, int variant) = nullptr;
    bool restart = false;   // (read once, at the start: a change applies after a restart)
    bool preset_exempt = false;   // (a change here leaves the Quality preset alone: the controls, the window, the counter,
                                  // frame generation)
    signed char cheat = -1;   // (a debug tool, on the Debug tab with the tools on: its index in the cheat_ tables, -1 none)
    bool heading = false;     // (a section's title on its tab, not a row: the selection steps over it)
};
/// A row's hints from a table, the value lo first (nullptr: none for that value)
const char* hint_in(const char* const* t, int n, int lo, int v, int variant) {
    return variant <= 0 && v - lo >= 0 && v - lo < n ? t[v - lo] : nullptr;
}
#define MENU_HINTS(fn, lo, ...)                                                                                              \
    const char* fn(int v, int variant) {                                                                                     \
        static const char* const t[] = {__VA_ARGS__};                                                                        \
        return hint_in(t, (int)(sizeof t / sizeof t[0]), lo, v, variant);                                                    \
    }
/// Frame generation: the value as the mode that makes the frames supports it (fgr::clamp): a saved 8x with NVIDIA DLSS FG on an
/// RTX 50 shows 6x, and a saved Auto with AMD FSR 3.1 shows 2x. The Frame rate limit's lowering of it is in the row's line
const char* tx_framegen(int v) {
    static const char* t[] = {"Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
    return t[std::clamp(fgr::clamp(framegen_by_now(), dlss_fg_most(), std::clamp(v, 0, 8)), 0, 8)];
}
const char* tx_onoff(int v) { return v ? "On" : "Off"; }
const char* tx_sprint(int v) { return v == 1 ? "Hold" : v == 2 ? "Toggle" : "Off"; }   // (Sprint with L3: 0 Off, 1 Hold, 2 Toggle)
const char* tx_framegen_by(int v) {
    static const char* t[fgr::kBy] = {"Built-in / AMD FSR 3.1", "NVIDIA DLSS FG", "AMD FSR 3.1 FG", "AMD FSR 4 FG"};
    return t[std::clamp(v, 0, fgr::kBy - 1)];
}
const char* tx_intel_compiles(int v) { return v ? "Side by side" : "One at a time"; }
MENU_HINTS(hn_intel_compiles, 0,
           "The safe choice, and the best on a CPU with 4 cores or fewer: a shader Intel's compiler faults on is left out instead of "
           "closing the game.",
           "Shaders are built faster on a CPU with many cores, but Intel's driver can crash the game while it builds them.")
const char* tx_fps_limit(int v) {
    static const char* t[fgr::kLimits] = {"60", "90", "120", "144", "165", "240", "Unlimited"};
    return t[v < 0 || v >= fgr::kLimits ? 0 : v];
}
const char* tx_fps_base(int v) {
    static const char* t[fgr::kBases] = {"Off", "30", "40", "45", "48", "50", "60", "72", "75", "90", "120"};
    return t[v < 0 || v >= fgr::kBases ? 0 : v];
}
const char* tx_perf(int v) { return v == 2 ? "Full" : v ? "FPS" : "Off"; }
const char* tx_menu_shortcut(int v) {
    static const char* const names[] = {"Insert or F1", "Insert only", "F1 only"};
    return names[v];
}
namespace mrow = menu_rows;
/// The HDR row as it works on this PC (menu_rows.h hdr_effective): what the HDR rows under it follow
int hdr_now() { return mrow::hdr_effective(opts.hdr, dev.hdr_listed || dev.hdr, hdr_forced()); }
const char* tx_hdr(int v) {
    const char* const name = v == 1 ? "Expanded" : v == 2 ? "Native (the game's)" : "Off";
    if (v == 1) return hdr_now() ? name : "Expanded (no HDR display)";
    if (v == 2) return hdr_now() ? name : "Native (no HDR display)";
    return name;
}
/// HDR: what the choice is, and when it cannot run
const char* hn_hdr(int v, int variant) {
    static const char* const t[] = {
        "Off: the game's SDR picture.",
        "Expanded: the game's SDR picture, its white at the paper white and its highlights up to the peak. Needs an HDR screen with Windows HDR on (Use HDR in the display settings).",
        "Native: the game's own HDR, told the display's peak and paper white. Needs an HDR screen with Windows HDR on (Use HDR in the display settings). Its HDR tone mapper row, with PsychoV35, appears then.",
    };
    static const char* const none = "Needs an HDR screen with Windows HDR on (Use HDR in the display settings): this display lists no HDR10, so the picture stays SDR "
                                    "and the HDR rows (paper white, peak, tone mapper, PsychoV35) are hidden. They appear once HDR is on and this is set to Expanded or Native.";
    const int i = std::clamp(v, 0, 2);
    if (variant < 0) return i && !hdr_now() ? none : t[i];
    return variant == 0 ? t[i] : variant == 1 && i && !hdr_now() ? none : nullptr;
}
// (HeyListen, after Longshot ddb156df and 79a34381: the HDR tone mapper and PsychoV35's rows)
const char* tx_tonemap(int v) { return v == 1 ? "PsychoV35" : "Vanilla"; }
const char* tx_midgray(int v) { return v == 1 ? "Vanilla" : "Render"; }
const char* tx_color_space(int v) { return v == 2 ? "DCI-P3" : v == 1 ? "BT.709" : "Vanilla"; }
const char* tx_grade(int v) {   // (PsychoV35's grading rows: RenoDX's 0 to 100)
    static char b[8];
    snprintf(b, sizeof b, "%d", v);
    return b;
}
const char* tx_nits(int v) {
    static char b[24];
    snprintf(b, sizeof b, "%d nits", v);
    return b;
}
// (hookshot) Motion controls: the choice, and whether a connected controller has a gyroscope (host/pad_sdl.c). Either can be
// chosen whatever is connected: a pad plugged in later is read when it comes
const char* tx_gyro(int v) {
    const int status = host && host->gyro_status ? host->gyro_status() : -1;
    if (status == 2) return v ? "On (gyro found)" : "Off (gyro found)";
    if (status == 1) return v ? "On (no gyro on this pad)" : "Off (no gyro on this pad)";
    if (status == 0) return v ? "On (no controller)" : "Off (no controller)";
    return v ? "On" : "Off";
}
const char* tx_mousecam(int v) { return v ? "Native" : "As the stick"; }
const char* tx_sens(int v) {
    static char b[12];
    snprintf(b, sizeof b, "%.2f", v / 100.0);
    return b;
}
// (the game's picture and the window it is shown in, as the last present had them: menu_note_present_sizes)
std::atomic<uint64_t> present_sizes{0};
bool filter_needed() {
    const uint64_t v = present_sizes.load(std::memory_order_relaxed);
    return upscaler_rows::filter_needed((unsigned)(v >> 48), (unsigned)(v >> 32) & 0xffff, (unsigned)(v >> 16) & 0xffff,
                                        (unsigned)v & 0xffff);
}
const char* tx_upscale(int v) {
    static const char* t[] = {"Off", "Catmull-Rom", "Lanczos"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];   // (a setting past the row: the label, not the next bytes)
    return t[v];
}
/// The Window scaling filter row's value: its filter, or that none is needed (the saved value stays, for another window size)
const char* tx_filter(int v) { return filter_needed() ? tx_upscale(v) : "Not needed (window = output)"; }
const char* tx_reflex(int v) {
    static const char* t[] = {"Off", "On", "On + Boost"};
    return t[reflex_available() ? std::clamp(v, 0, 2) : 0];
}
/// Reflex: on the value, or that it is not available here (the row shows Off)
const char* hn_reflex(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver paces the frames for less latency.",
                                    "As On, and the GPU's clocks are kept up as well."};
    static const char* const none = "Not available here: it needs an NVIDIA GPU whose driver has VK_NV_low_latency2.";
    if (variant < 0) return reflex_available() ? t[std::clamp(v, 0, 2)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 2)] : variant == 1 ? none : nullptr;
}
// The Upscaler (opts.dlss): what scales the 3D up to the output. DLSS and DLAA only where dlss_why() lets them run, FSR 3.1
// only where vk_fsr3.cpp says it does, FSR 4 and its native AA only where fsr4_why() does; the menu shows (and steps from)
// what runs, and keeps the saved choice for a PC where it can
namespace ur = upscaler_rows;
/// One bit per upscaler that runs here (upscaler_rows.h bit). Each backend caches its own answer, so this costs nothing
unsigned upscalers_available() {
    return ur::bit(ur::GAME) | ur::bit(ur::OFF) | ur::dlss_bits(dlss_why() == DLSS_OK) |
           (fsr3::supported() ? ur::bit(ur::FSR3) : 0u) | ur::fsr4_bits(fsr4_why() == FSR_OK);
}
/// The Upscaler row's choice that runs (FSR 3.1 has inputs of its own; DLSS and FSR 4 run on the frame chain they share,
/// temporal_mode; Off draws no network, vk_draw.cpp off_stand_in). DLAA and FSR 4's native AA are those engines at the Render
/// resolution's native choice, so the row shows DLSS or FSR 4 for them (ur::engine); native_aa_now says which
/// What runs, as the chain's mode: DLAA and FSR 4's native AA stay themselves here (temporal_mode)
int upscaler_mode() { return upscaler_off() ? (int)ur::OFF : fsr3_wanted() ? (int)ur::FSR3 : temporal_mode(); }
int upscaler() { return ur::engine(upscaler_mode()); }
/// Whether the chain runs DLAA or FSR 4's native AA now: the Render resolution's native choice, with DLSS or FSR 4 running
bool native_aa_now() { return !upscaler_off() && !fsr3_wanted() && ur::full_scale(temporal_mode()); }
const char* tx_upscaler(int) {
    // (DLAA and FSR 4's native AA are not choices: the Render resolution's native choice, with DLSS or FSR 4)
    static const char* t[ur::kUpscalers] = {"Game's own", "NVIDIA DLSS", "NVIDIA DLSS", "AMD FSR 3.1", "AMD FSR 4",
                                            "AMD FSR 4", "Off"};
    return t[std::clamp(upscaler(), 0, ur::kUpscalers - 1)];
}
/// Whether a file (a path with /) is in the game folder: NVIDIA's DLSS runtime and AMD's FSR 4 loader, which a game built without
/// them can have beside it (Windows' file names: the Linux runtimes carry a version in theirs)
bool file_beside_game(const char* relative) {
#ifdef _WIN32
    std::error_code ec;
    return std::filesystem::is_regular_file(game_dir_path() / relative, ec);
#else
    (void)relative;
    return false;
#endif
}
/// The Upscaler: the one shown (what runs, upscaler()), and with the game's own, why DLSS and FSR are not offered. Variants
/// (every one): each value, then the game's own with each of DLSS's reasons, then with each of FSR's
const char* hn_upscaler(int v, int variant) {
    static const char* const t[ur::kUpscalers] = {
        "The game's own network.",
        "NVIDIA's, from a smaller 3D, or at full size with its edges smoothed (DLAA, in Render resolution). Softer than the "
        "game's own: set Sharpness to around 80%. Things that move by themselves can trail for now.",
        "NVIDIA's, the 3D at full size with its edges smoothed (DLAA: choose DLSS, then DLAA in Render resolution). Softer than "
        "the game's own: set Sharpness to around 80%. Things that move by themselves can trail for now.",
        "AMD's, from a smaller 3D, on any GPU: built from AMD's own shaders. Things that move by themselves can trail for "
        "now.",
        "AMD's machine-learned upscaler, from a smaller 3D, or at full size with its edges smoothed (Native AA, in Render "
        "resolution), with AMD's files beside the game. Things that move by themselves can trail for now.",
        "AMD's machine-learned upscaler, the 3D at full size with its edges smoothed (Native AA: choose FSR 4, then Native AA "
        "in Render resolution).",
        "No upscaling network: the 3D drawn at the Render resolution and stretched to the output, no anti-aliasing. For GPUs "
        "where the game's own network shows glitches.",
    };
    static const char* const dlss_no[DLSS_WHY_COUNT] = {
        nullptr,
        "DLSS and DLAA: not in this build (built without NVIDIA's DLSS SDK, see the readme).",
        "DLSS and DLAA: off for this run (RX_VK_DLSS=0).",
        "DLSS and DLAA: need an NVIDIA RTX GPU.",
        "DLSS and DLAA: need a newer NVIDIA driver.",
        "DLSS and DLAA: DLSS's runtime was not found (the game folder's ngx folder, see the readme).",
        "DLSS and DLAA: NVIDIA's NGX on Linux loads DLSS's runtime only from a folder named in ASCII (see the readme).",
    };
    static const char* const fsr_no[FSR_WHY_COUNT] = {
        nullptr,
        "AMD FSR 4: not in this build (see the readme).",
        "AMD FSR 4: off for this run (RX_VK_FSR4=0).",
        "AMD FSR 4: AMD's files were not found beside the game (see the readme).",
        "AMD FSR 4: AMD's files have no FSR 4 for this GPU.",
    };
    // (now: the value shown and what runs here, with both engines' reasons; every one: as said above)
    int d = 0, f = 0;
    if (variant < 0) {
        v = std::clamp(upscaler(), 0, ur::kUpscalers - 1), d = (int)dlss_why(), f = (int)fsr4_why();
    } else if (v == ur::GAME) {
        if (variant < DLSS_WHY_COUNT) d = variant;
        else if (variant < DLSS_WHY_COUNT + FSR_WHY_COUNT - 1) f = variant - DLSS_WHY_COUNT + 1;
        else return nullptr;
    } else if (variant > 0) {
        return nullptr;
    }
    v = std::clamp(v, 0, ur::kUpscalers - 1);
    // (the reasons are said only where the engine is in the build: one without FSR does not tell an NVIDIA user why)
    const char* dn = dlss_built() || variant >= 0 ? dlss_no[d] : nullptr;
    const char* fn = fsr4_built() || variant >= 0 ? fsr_no[f] : nullptr;
    // (a build without DLSS or FSR 4 says nothing of it, except where its files are in the game folder on a card that would use
    // them: then the way to get it. Each answer is kept: the folder and the GPU do not change while the game runs)
    if (variant < 0 && v == ur::GAME) {
        static const bool dlss_hint = ur::dlss_rebuild_hint_wanted(dlss_built(), ur::nvidia_rtx(dev.props.vendorID, dev.props.deviceName),
                                                                    file_beside_game("ngx/nvngx_dlss.dll"));
        static const bool fsr_hint = ur::fsr4_rebuild_hint_wanted(fsr4_built(), ur::amd_gpu(dev.props.vendorID),
                                                                   file_beside_game("amd_fidelityfx_loader_vk.dll"));
        if (dlss_hint) dn = ur::dlss_rebuild_hint();
        if (fsr_hint) fn = ur::fsr4_rebuild_hint();
    }
    if (v != ur::GAME || (!dn && !fn)) return t[v];
    static char b[DLSS_WHY_COUNT][FSR_WHY_COUNT][520];
    snprintf(b[d][f], sizeof b[d][f], "%s%s%s%s%s", t[0], dn ? " " : "", dn ? dn : "", fn ? " " : "", fn ? fn : "");
    return b[d][f];
}
/// The DLSS model row (opts.dlss_model): NVIDIA's network by its preset, or RX_VK_DLSS_PRESET's for this run
const char* dlss_preset_env() {
    static const char* const e = getenv("RX_VK_DLSS_PRESET");
    return ur::preset_env(e) ? e : nullptr;
}
const char* tx_dlss_model(int v) {
    static const char* const t[] = {"DLSS 4 (Transformer, preset K)", "DLSS 4.5 (Transformer 2, preset M)",
                                    "DLSS 4.5 (Transformer 2, preset L)", "NVIDIA default", "DLSS 3 (CNN, preset E)"};
    if (const char* e = dlss_preset_env()) {
        static char b[48];
        const unsigned p = ur::dlss_preset(v, e);
        if (p) snprintf(b, sizeof b, "Preset %c (RX_VK_DLSS_PRESET)", ur::preset_letter(p));
        else snprintf(b, sizeof b, "NVIDIA default (RX_VK_DLSS_PRESET)");
        return b;
    }
    return t[std::clamp(v, 0, ur::kModels - 1)];
}
/// The DLSS model: the value chosen; variant 1: the game's own upscales (the row waits for DLSS or DLAA); variant 2: set for
/// this run by RX_VK_DLSS_PRESET
const char* hn_dlss_model(int v, int variant) {
    static const char* const t[] = {
        "DLSS 4: NVIDIA's first transformer model, the lighter one. First-run default on RTX 20 and 30. Applies to every mode.",
        "DLSS 4.5: NVIDIA's second-generation transformer, sharper. First-run default on RTX 40 and 50; on RTX 20 and 30 (no FP8) "
        "it costs more GPU time, but you can use it.",
        "DLSS 4.5, preset L: NVIDIA tuned it for Ultra Performance, but it works in every mode. Costs the most GPU time, more on RTX 20 and 30 (no FP8).",
        "Lets DLSS's runtime pick for each mode: what it picks can change with a runtime update.",
        "DLSS 3: NVIDIA's older CNN model, lighter on video memory and GPU time. First-run default under 7 GB of video memory.",
    };
    static const char* const unused = "Used by NVIDIA DLSS and DLAA: choose DLSS in the Upscaler row.";
    static const char* const env = "Set for this run by RX_VK_DLSS_PRESET.";
    if (variant < 0) variant = dlss_preset_env() ? 2 : upscaler() == ur::GAME ? 1 : 0;
    if (variant == 1) return unused;
    if (variant == 2) return env;
    return variant == 0 ? t[std::clamp(v, 0, ur::kModels - 1)] : nullptr;
}
/// (HeyListen, after Longshot 83b161e3) The DLAA before the bloom row (opts.dlss_pre_bloom; vk_temporal.cpp pre_bloom_wanted):
/// RX_VK_DLSS_PRE_BLOOM set for this run wins over it
bool pre_bloom_env() {
    static const char* const e = getenv("RX_VK_DLSS_PRE_BLOOM");
    return e && *e;
}
/// RX_VK_PARALLEL=2 or more (vk_temporal.cpp pre_bloom_wanted): DLAA stays after the bloom
bool pre_bloom_parallel() {
    static const bool on = getenv("RX_VK_PARALLEL") && atoi(getenv("RX_VK_PARALLEL")) >= 2;
    return on;
}
/// Its line: the value chosen; variant 1: DLSS at a Render resolution below Native (the row waits for DLAA); variant 2: set
/// for this run by RX_VK_DLSS_PRE_BLOOM
const char* hn_dlss_pre_bloom(int v, int variant) {
    static const char* const t[] = {
        "Off: DLAA smooths the finished picture, after the game's bloom and tone map.",
        "On: DLAA smooths the 3D before the game's bloom and tone map, so the glow is made from a smooth picture.",
    };
    static const char* const not_dlaa = "Needs DLAA: set the Render resolution to Native (DLSS at the full output size). DLSS's other modes draw the 3D "
                                        "smaller than the picture, which this cannot go back into.";
    static const char* const env = "Set for this run by RX_VK_DLSS_PRE_BLOOM.";
    static const char* const parallel = "Not with the parallel replay (RX_VK_PARALLEL=2 or more): DLAA stays after the game's bloom and tone map.";
    if (variant < 0) variant = pre_bloom_env() ? 2 : upscaler_mode() != ur::DLAA ? 1 : pre_bloom_parallel() ? 3 : 0;
    if (variant == 3) return parallel;
    if (variant == 1) return not_dlaa;
    if (variant == 2) return env;
    return variant == 0 ? t[std::clamp(v, 0, 1)] : nullptr;
}
const char* tx_percent(int v) {
    static char b[8];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
// The output the menu's sizes are shown for: the Output resolution row's (applied at the next start), so the rows below follow a change
// at once; the handheld profile runs the game's own (1536x864)
struct Out {
    uint32_t w, h;
};
constexpr Out kOutputs[] = {{2048, 1152}, {2560, 1440}, {3840, 2160}, {1920, 1080}, {5120, 1440}};
constexpr int kLastOutput = std::size(kOutputs) - 1;
int output_choice(int v) { return v >= 0 && v <= kLastOutput ? v : 0; }
// (issue 14) output_res= and HOS_OUTPUT_RES as host/nn_applet.c output_res_value reads them: 0-4, or one of the sizes written
// out (1920x1080); anything else the game's
int output_choice_text(const char* s) {
    unsigned w = 0, h = 0;
    if (sscanf(s, " %ux%u", &w, &h) == 2) {
        for (int i = 0; i <= kLastOutput; i++)
            if (kOutputs[i].w == w && kOutputs[i].h == h) return i;
        return 0;
    }
    return output_choice(atoi(s));
}
// Display the sizes from smallest to largest while keeping the IDs used by saved settings.
constexpr int kOutputOrder[] = {3, 0, 1, 2, 4};
static_assert(std::size(kOutputOrder) == std::size(kOutputs));
int step_output_choice(int id, int dir) {
    const int* pos = std::find(std::begin(kOutputOrder), std::end(kOutputOrder), output_choice(id));
    const int next = std::clamp((int)(pos - std::begin(kOutputOrder)) + dir, 0, kLastOutput);
    return kOutputOrder[next];
}
Out shown_output() {
    return opts.handheld ? Out{1536, 864} : kOutputs[output_choice(opts.output_res)];
}
// The size the game draws the 3D at for a fixed scale of its output, a twelfth of it: every twelfth asked came out exact at the three
// outputs (2048x1152: 0.58 drew 1195x672, 0.6667 1366x768). So the scale the game is given is the twelfth too: 0.67 drew 2615x1471 at
// 3840x2160, not the 2560x1440 of 8/12.
int twelfths(float scale) { return std::min(std::max((int)(scale * 12.0f + 0.5f), 1), 12); }
void game_size(float scale, uint32_t& w, uint32_t& h) {
    const Out o = shown_output();
    const int k = twelfths(scale);
    w = (o.w * k + 11) / 12, h = o.h * k / 12;
}
const char* tx_resolution(int v) {
    static char b[48];
    const int up = upscaler();
    v = ur::snap(up, v);   // (the saved choice, as the Upscaler shown draws it: ur::snap)
    if (v == ur::kNativeAa) return up == ur::FSR4 ? "Native AA (FSR 4)" : "DLAA (native)";
    if (!ur::percent(v)) return "Auto (dynamic)";
    if (const char* mode = ur::scales_by_mode(up) ? ur::dlss_mode_name(v) : nullptr) {
        snprintf(b, sizeof b, "%s (%d%%)", mode, ur::percent(v));
        return b;
    }
    uint32_t w, h;
    game_size(ur::percent(v) / 100.0f, w, h);
    snprintf(b, sizeof b, "%d%% (%ux%u)", ur::percent(v), w, h);
    return b;
}
/// The Render resolution: the value chosen, for the Upscaler shown (every one: each value with each Upscaler)
const char* hn_resolution(int v, int variant) {
    const int up = variant < 0 ? std::clamp(upscaler(), 0, ur::kUpscalers - 1) : variant;
    if (up >= ur::kUpscalers) return nullptr;
    if (variant < 0) v = ur::snap(up, v);
    const int p = ur::percent(v);
    if (v == ur::kNativeAa && up == ur::DLSS)
        return "DLAA: the 3D at full size with DLSS's anti-aliasing, the sharpest and the slowest. Set Sharpness to around 80%.";
    if (v == ur::kNativeAa && up == ur::FSR4) return "Native AA: the 3D at full size with FSR 4's anti-aliasing, the slowest.";
    if (up == ur::DLSS) {
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and DLSS follows. Set Sharpness to around 80%.";
        case 67: return "DLSS's sharpest mode. Set Sharpness to around 80%.";
        case 58: return "Between Quality and Performance. Set Sharpness to around 80%.";
        case 50: return "Faster, and softer. Set Sharpness to around 80%.";
        case 33: return "DLSS's fastest mode, and its softest. Set Sharpness to around 80%.";
        default: return nullptr;   // (not one of DLSS's steps: the row moves to one)
        }
    }
    if (up == ur::FSR3 || up == ur::FSR4) {   // (the same modes, AMD's ratios)
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and FSR follows.";
        case 67: return "FSR's Quality mode, 1.5x: its sharpest.";
        case 58: return "FSR's Balanced mode, 1.7x.";
        case 50: return "FSR's Performance mode, 2x: faster, and softer.";
        case 33: return "FSR's Ultra Performance mode, 3x: its fastest, and its softest.";
        default: return nullptr;   // (not one of FSR's steps: the row moves to one)
        }
    }
    if (!p) return "The game lowers it when the GPU is busy.";
    if (p == 100) return "The 3D at the output's full size: the sharpest, and the slowest.";
    return "A steady size: lower is faster, and softer.";
}
/// (the V-Sync mode row with V-Sync Off: greyed, with a hint saying so, and Left and Right leave it alone)
bool vsync_env_decides() { return getenv("RX_VK_FIFO") || getenv("RX_VK_IMMEDIATE"); }
bool row_inactive(const int* value) {
    return value == &opts.vsync_mode && mrow::vsync_mode_fixed(opts.vsync, vsync_env_decides());
}
/// A row with nothing else to choose here: shown without its arrows, Left and Right leave it (and the saved value) alone
bool row_fixed(const int* value) {
    return row_inactive(value) || ((value == &opts.send_perf || value == &opts.send_crash) && !telemetry_available()) || (value == &opts.reflex && !reflex_available()) ||
           (value == &opts.upscale && !filter_needed()) ||
           (value == &opts.hud_layout && getenv("HOS_WIDE_UI") && *getenv("HOS_WIDE_UI")) ||   // (host/hos_wide_ui.c's, first)
           (value == &opts.dlss_model && ur::model_fixed(upscaler(), dlss_preset_env() != nullptr)) ||
           (value == &opts.dlss_pre_bloom && ur::pre_bloom_fixed(upscaler_mode(), pre_bloom_env(), pre_bloom_parallel()));
}
/// (heylisten) The two telemetry rows: Off or On, and "Not available in this build" when the build has no address to send to
/// (nx2/addresses.ini's telemetry= is empty), where nothing can be sent whatever the row says
const char* tx_telemetry(int v) {
    if (!telemetry_available()) return "Not available in this build";
    return v ? "On" : "Off";
}
const char* hn_telemetry(int v, int variant) {
    (void)v;
    if (variant > 0) return nullptr;
    return telemetry_available() ? nullptr : "This build has no address to send to, so nothing is sent.";
}
const char* tx_output_res(int v) {
    static char b[32];
    v = output_choice(v);
    const Out o = kOutputs[v];
    snprintf(b, sizeof b, "%ux%u%s", o.w, o.h, v ? "" : " (game's)");
    return b;
}
const char* tx_speed(int v) { return v ? "Real time" : "Per frame"; }
const char* tx_text_speed(int v) {
    static const char* t[] = {"Normal", "2x", "4x", "Instant"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
const char* tx_ultrawide(int v) { return v ? "Monitor's shape" : "16:9"; }
/// (the HUD layout row, HL_OPT_HUD_LAYOUT: host/hos_wide_ui.c places the HUD by it at each present, #1133)
const char* tx_hud_layout(int v) { return v ? "Middle 16:9" : "Screen's edges"; }
const char* hn_hud_layout(int v, int variant) {
    static const char* const t[] = {"The hearts, the buttons and the minimap at the screen's own edges and corners.",
                                    "The HUD where it is at 16:9, in the middle (fitted to the width on a narrower screen)."};
    static const char* const held = "Set by HOS_WIDE_UI (hookshot_env.txt or the environment): remove that line to choose here.";
    const char* env = getenv("HOS_WIDE_UI");
    if (variant < 0) return env && *env ? held : t[v ? 1 : 0];
    return variant == 0 ? t[v ? 1 : 0] : variant == 1 && env && *env ? held : nullptr;
}
const char* tx_prompts(int v) {
    static const char* t[HOS_PROMPT_N + 1] = {"Auto", "Switch", "Xbox", "PlayStation 4", "PlayStation 5", "Steam Deck",
                                              "Keyboard"};
    return t[std::min(std::max(v, 0), (int)HOS_PROMPT_N)];
}
const char* tx_gpu(int v) {
    static char buf[300];
    const int idx = v - 1;
    if (v <= 0) snprintf(buf, sizeof buf, "Auto");
    else if (idx < (int)gpu_device_names.size()) snprintf(buf, sizeof buf, "%d: %s", idx, gpu_device_names[idx].c_str());
    else snprintf(buf, sizeof buf, "%d: Device %d", idx, idx);
    // (the whole name shows: the selected row slides through it, see marquee_pos)
    return buf;
}
/// V-Sync: Off is the game's own way (mailbox, else immediate: no waiting for the refresh), On is FIFO with the mode row's images
const char* tx_vsync(int v) { return v ? "On" : "Off"; }
const char* hn_vsync(int v, int variant) {
    static const char* const off = "No waiting for the screen's refresh: the latest frame is shown at once, for the least delay. The picture can tear where the display does not match.";
    static const char* const on = "Frames wait for the screen's refresh: no tearing, a little more delay, and the frame rate cannot pass the refresh rate. The V-Sync mode row below chooses how many images it keeps.";
    static const char* const env = "RX_VK_FIFO or RX_VK_IMMEDIATE in the environment decide the mode: this row does nothing.";
    if (variant < 0) return getenv("RX_VK_FIFO") || getenv("RX_VK_IMMEDIATE") ? env : v ? on : off;
    return variant == 0 ? (v ? on : off) : variant == 1 ? env : nullptr;
}
/// The V-Sync mode row: FIFO double (2 images, the least delay; a missed refresh waits a whole one), FIFO triple (the default,
/// today's On), Mailbox (3+ images, the newest frame shown; only where the surface lists it)
const char* tx_vsync_mode(int v) {
    static const char* const t[] = {"FIFO (double buffering)", "FIFO (triple buffering)", "Mailbox (triple buffering)"};
    const int i = std::clamp(v, 0, 2);
    if (i == present_mode::kMailbox && dev.present_modes_known && !present_mode::buffering_offered(i, dev.present_modes_offered))
        return "Mailbox (not offered)";
    if (i == present_mode::kFifoDouble && dev.present_modes_known && dev.swap_min_images >= 3) return "FIFO (double: not possible)";
    return t[i];
}
const char* hn_vsync_mode(int v, int variant) {
    static const char* const t[] = {
        "Two images: the least delay with V-Sync. A missed refresh waits a whole one, so below 60 fps at 60 Hz the frame rate falls to 30.",
        "Three images, the default: one is kept free, so a slow frame costs a refresh rather than half the frame rate.",
        "No tearing, and the newest frame is shown: the game can render past the refresh instead of waiting for it. Triple buffered by nature (it needs a spare image), so no double version."};
    static const char* const off = "V-Sync is Off: this row does nothing. Turn V-Sync On to choose how many images it keeps.";
    static const char* const no_mailbox = "This display or driver offers no Mailbox (AMD's Windows driver has none): FIFO (triple buffering) is used.";
    static const char* const no_double = "This surface asks for at least 3 images: FIFO (double buffering) is not possible here.";
    static const char* const fg = "Frame generation needs three images: the row cannot go lower, so FIFO (triple buffering) is used.";
    static const char* const fg_mailbox = "Frame generation needs a spare image: four are used with Mailbox.";
    static const char* const vrr = "VRR is on: FIFO is used while it is on.";
    static const char* const env = "RX_VK_FIFO or RX_VK_IMMEDIATE in the environment decide the mode: this row does nothing.";
    const int i = std::clamp(v, 0, 2);
    const bool known = dev.present_modes_known;
    const bool no_mb = known && !present_mode::buffering_offered(i, dev.present_modes_offered);
    const bool min3 = known && dev.swap_min_images >= 3;
    const bool frame_gen = framegen_on();
    const bool forced = getenv("RX_VK_FIFO") || getenv("RX_VK_IMMEDIATE");
    if (variant < 0) {
        if (forced) return env;
        if (opts.vsync == 0) return off;
        return i == present_mode::kMailbox && no_mb ? no_mailbox : t[i];
    }
    if (variant == 0) return forced ? env : opts.vsync == 0 ? off : t[i];
    if (variant == 1) return i == present_mode::kMailbox && no_mb ? no_mailbox : nullptr;
    if (variant == 2) return i == present_mode::kFifoDouble && frame_gen ? fg : nullptr;
    if (variant == 3) return i == present_mode::kFifoDouble && min3 ? no_double : nullptr;
    if (variant == 4) return i == present_mode::kMailbox && frame_gen ? fg_mailbox : nullptr;
    if (variant == 5) return i == present_mode::kMailbox && dev.vrr_active ? vrr : nullptr;
    return nullptr;
}
const char* tx_vrr(int v) {
    static const char* const t[] = {"Off", "On", "Auto"};
    return t[std::clamp(v, 0, 2)];
}
MENU_HINTS(hn_vrr, 0,
           "Nothing changes: the game presents as the V-Sync row says.",
           "For a G-Sync or FreeSync screen: V-Sync On, the fullscreen window made so Windows can show it directly, and a frame rate "
           "limit above 60 held 3 fps under the refresh. New: tell us on the issue tracker whether it works for you.",
           "On in fullscreen when the display lists a variable refresh range, off otherwise. New: tell us on the issue tracker how it goes.")
const char* tx_pad_layout(int v);
// (hookshot) the Microphone row: Off, the system's default, or one of the host's microphones (host/nn_audio_in.c: the
// ocarina's input streams), told to the host by its name (host->mic_select) whenever it changes. The chosen one is kept by
// its name, mic_want, also while it is unplugged: the row then has one more entry for it, "<name> (not connected)", the
// host lets the default stand in and opens it again once it is back, and the name stays in the ini. In the ini the name is
// cut to kMicNameKept bytes (at a character's start), so the line fits the 80-byte readers of the file (host/nn_hid.c,
// nn_applet.c, hos_main.c), and a name is matched by that much of it
constexpr int kMaxMics = 16;
constexpr size_t kMicNameKept = 60;
std::string mic_want;      // (the chosen microphone's name, cut to kMicNameKept; empty: Off or the default chosen)
bool mic_missing = false;  // (mic_want is not plugged in: the row's last entry, 2 + mic_listed, stands for it)
int mic_listed = 0;        // (the microphones as the host last listed them)
bool can_mic() { return host && host->mic_count && host->mic_name && host->mic_select; }
size_t text_char_start(const std::string& text, size_t at) {
    while (at > 0 && at < text.size() && ((unsigned char)text[at] & 0xc0) == 0x80) at--;
    return at;
}
std::string mic_key(const char* name) {
    std::string k(name ? name : "");
    if (k.size() <= kMicNameKept) return k;
    return k.substr(0, text_char_start(k, kMicNameKept));
}
/// buf cut back to whole UTF-8 characters (snprintf may have cut one)
void utf8_whole(char* buf) {
    size_t n = strlen(buf), i = n;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) i--;   // (the last character's continuation bytes)
    if (i == 0) return;
    const unsigned char lead = (unsigned char)buf[i - 1];
    const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < want) buf[i - 1] = 0;
}
const char* tx_mic(int v) {
    static char buf[160];
    if (v <= 0) return "Off";
    if (v == 1) return "Default";
    if (mic_missing && v == 2 + mic_listed) {
        const int room = (int)sizeof buf - (int)sizeof " (not connected)";
        snprintf(buf, (size_t)room + 1, "%s", mic_want.c_str());
        utf8_whole(buf);
        strcat(buf, " (not connected)");
        return buf;
    }
    const char* n = can_mic() ? host->mic_name(v - 2) : "";
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "Microphone");
    utf8_whole(buf);
    return buf;
}
/// The Microphone row: Off, the default, a microphone of the list, or the chosen one while it is unplugged (variant 1)
const char* hn_mic(int v, int variant) {
    if (v <= 0) return variant <= 0 ? "The game hears silence." : nullptr;
    if (v == 1)
        return variant <= 0 ? "The system's default recording device. On Windows, desktop apps must be allowed to use the "
                              "microphone (Settings, Privacy & security)."
                            : nullptr;
    const bool missing = variant < 0 ? mic_missing && v == 2 + mic_listed : variant == 1;
    if (variant > 1) return nullptr;
    return missing ? "Not plugged in: Default stands in until it is back." : "This microphone, by its name. A change applies at once.";
}
// (heylisten) the Output device row: Default (the system's default output, followed as it changes) or one of the host's outputs
// (host/nn_audio.c), by its friendly name, told to the host by its id (host->out_select) whenever it changes; it applies at
// once. The chosen one is kept by its id (output_device_id=, cut to kMicNameKept as the microphone's name is) and its name
// (output_device_name=, for display), also while it is unplugged: the row then has one more entry, "<name> (not connected)",
// the host lets Default stand in (with a log line) and tries it again when the menu opens. Where the host cannot list them (Linux:
// the sound goes to SDL's default device) the row has Default only
constexpr int kMaxOuts = 16;
std::string out_want_id;     // (the chosen output's id, cut; empty: Default chosen)
std::string out_want_name;   // (its name when it was chosen, cut: shown while it is not connected)
bool out_missing = false;    // (out_want_id is not listed: the row's last entry, 1 + out_listed, stands for it)
int out_listed = 0;          // (the outputs as the host last listed them)
bool can_out() { return host && host->out_count && host->out_name && host->out_id && host->out_select; }
const char* tx_out(int v) {
    static char buf[160];
    if (v <= 0) return "Default";
    if (out_missing && v == 1 + out_listed) {
        const int room = (int)sizeof buf - (int)sizeof " (not connected)";
        snprintf(buf, (size_t)room + 1, "%s", out_want_name.empty() ? "Output device" : out_want_name.c_str());
        utf8_whole(buf);
        strcat(buf, " (not connected)");
        return buf;
    }
    const char* n = can_out() ? host->out_name(v - 1) : "";
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "Output device");
    utf8_whole(buf);
    return buf;
}
/// The Output device row: Default, an output of the list, or the chosen one while it is unplugged (variant 1)
const char* hn_out(int v, int variant) {
    if (v <= 0)
        return variant <= 0 ? "The system's default output device, followed as it changes. The list of other outputs is on "
                              "Windows only; on Linux the system's sound settings choose the device."
                            : nullptr;
    const bool missing = variant < 0 ? out_missing && v == 1 + out_listed : variant == 1;
    if (variant > 1) return nullptr;
    return missing ? "Not plugged in: Default stands in until it is back (open this menu again to try it)."
                   : "This output device. A change applies at once, no restart.";
}
const char* tx_rumble(int v) { return v ? tx_percent(v) : "Off"; }
// (hookshot) Menu size: how large the overlays are drawn (vk_ui.h unit); Auto is Large in fullscreen at 1080 lines and up while
// a controller is the menu's input, else Medium
bool pad_last = false;   // (a controller was the menu's last input: its buttons in the prompts)
// (Auto's choice: the controller or not, decided as the menu opens by the input that opened it. A controller's input makes it
// Large at any time; the keys and the mouse never make it smaller while the menu is open, so it never changes size under the
// cursor)
bool auto_pad = false;
int pad_confirm = 0, pad_back = 1;   // (the face buttons that confirm and go back, HOS_PAD_*: menu_keys::confirm_back, each tick)
bool auto_large(uint32_t h) { return opts.fullscreen && h >= 1080 && auto_pad; }
const char* tx_menu_size(int v) {
    static const char* t[] = {"Auto", "Small", "Medium", "Large"};
    if (v == 0) return auto_large(dev.swap_extent.height) ? "Auto (Large)" : "Auto (Medium)";
    return (unsigned)v < 4 ? t[v] : t[0];
}
const char* tx_stick_dead_zone(int v) {
    static const char* t[] = {"Default (24%)", "5%", "10%", "15%", "20%", "30%"};
    return (unsigned)v < 6 ? t[v] : t[0];
}
// (hookshot) Audio output: the speakers the game mixes for, read once at its start (host/nn_audio.c au_menu_output)
const char* tx_audio_output(int v) {
    static const char* t[] = {"Auto", "Stereo", "5.1"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
// (hookshot) the motion's deadzone: the console's three zero-drift modes, the rate under which the pad counts as still
const char* tx_deadzone(int v) {
    static const char* t[] = {"Low", "Standard", "High"};
    return t[v < 0 ? 0 : v > 2 ? 2 : v];
}
const char* tx_robust(int v) {
    static const char* t[] = {"Auto", "Off", "On"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
const char* tx_tex_res(int v) { return v ? "Low" : "High"; }
MENU_HINTS(hn_tex_res, 0,
           "Every level of the game's textures: the sharpest picture, and the most video memory.",
           "The largest textures lose their top level, a quarter of their size: about 30-40% less video memory in Hyrule Field and "
           "the Market, for cards of 6 GB and less that run out of it (missing icons and text, noise). Rendered images are not touched.")
const char* tx_preset(int v) {
    static const char* t[] = {"Custom", "Lowest (integrated GPU)", "Minimum (4GB)", "Low (6GB)", "Balanced (8GB)", "High (12GB)", "Ultra (16GB+)"};
    return t[std::min(std::max(v, 0), 6)];
}
void sync_preset_budgets(int p) {
    switch (p) {
    case 1: opts.vram_mb = 2200;  opts.mirror_mb = 256;  break;   // (Lowest: Minimum's, the smallest there is)
    case 2: opts.vram_mb = 2200;  opts.mirror_mb = 256;  break;
    case 3: opts.vram_mb = 3600;  opts.mirror_mb = 512;  break;
    case 4: opts.vram_mb = 4800;  opts.mirror_mb = 1024; break;
    case 5: opts.vram_mb = 7500;  opts.mirror_mb = 2048; break;
    case 6: opts.vram_mb = 11000; opts.mirror_mb = 3072; break;
    default: opts.vram_mb = 0;    opts.mirror_mb = 0;    break;
    }
}
// (Sharpness: tuned for the window pass's neighbours one picture pixel apart, each about the old setting's sharpening on a
// 3840 x 2160 window at that preset's picture, 2048 x 1152 or the handheld profile's 1536 x 864: same frame, mean |Laplacian|
// of the window and of the picture, on the slider's steps)
void apply_preset(int p) {
    switch (p) {
    case 1: // Lowest (a weak integrated GPU): Minimum's rows with the lowest of each (Render resolution 33%) and the Volumetric fog Off
        // (the rule's rows: lowend_rule.h kResolution, kHandheld, kFog); the fog and the Handheld profile apply from the next start
        opts.resolution = lowend_rule::kResolution; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = lowend_rule::kHandheld; opts.fog = lowend_rule::kFog;
        break;
    case 2: // Minimum (4GB VRAM / iGPU)
        opts.resolution = 4; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1; opts.fog = 1;
        break;
    case 3: // Low (6GB VRAM)
        opts.resolution = 2; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1; opts.fog = 1;
        break;
    case 4: // Balanced (8GB VRAM)
        opts.resolution = 1; opts.upscale = 1; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0; opts.fog = 1;
        break;
    case 5: // High (12GB VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 40;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0; opts.fog = 1;
        break;
    case 6: // Ultra (16GB+ VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 50;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0; opts.fog = 1;
        break;
    default:
        break;
    }
    // (every preset sets the Volumetric fog too: On, as the game's default, but Lowest; the fog and the Handheld profile apply from the next start)
    // (the Upscaler and Frame generation are the player's: a preset never turns DLSS or made frames on or off. With DLSS
    // its resolution is DLSS's nearest mode)
    if (p > 0) opts.resolution = ur::snap(upscaler(), opts.resolution);
    sync_preset_budgets(p);
}

MENU_HINTS(hn_preset, 0, "Each row as you set it. Changing most rows yourself comes back to Custom.",
           "Resolution 33% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile, Volumetric fog Off. The Handheld profile and "
           "the fog apply after a restart.",
           "Resolution 50% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile. Volumetric fog On.",
           "Resolution 75% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile. Volumetric fog On.",
           "Resolution 100% (with DLSS its nearest mode), Catmull-Rom, Sharpness 30%. Volumetric fog On.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 40%. Volumetric fog On.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 50%. Volumetric fog On.")
MENU_HINTS(hn_gpu, 0, "The best dedicated GPU.")
/// The line about the Frame rate limit under a fixed multiplier (v: the value shown): what the game runs at and what is shown,
/// or that the multiplier runs lower to keep the game's 20 frames a second. worst: the longest it can be (for the help's height)
const char* framegen_limit_note(int v, bool worst) {
    static char b[200];
    const int limit = worst ? fgr::limit_fps(fgr::kLimit60) : menu_fps_limit();
    if (limit < 0) return "";
    if (!worst && menu_base_fps() > 0) {   // (the Base frame rate limit on: the shown rate is the game's times the multiplier)
        snprintf(b, sizeof b, " The Base frame rate limit holds the game at %d frames a second, so %d are shown.", menu_base_fps(), menu_base_fps() * v);
        return b;
    }
    if (limit == 0) return " Unlimited: the game is not held back, and every frame of it gets its made frames.";
    const int run = worst ? v : fgr::limited(limit, v);
    if (run < v || worst)
        snprintf(b, sizeof b, " At the Frame rate limit of %d it runs as %dx: the game keeps at least 20 frames a second, and %d are shown.",
                 limit, run, limit);
    else
        snprintf(b, sizeof b, " At the Frame rate limit of %d the game runs at %d frames a second and %d are shown.", limit, limit / v, limit);
    return b;
}
/// Frame generation: how many frames are made for each of the game's, by the mode that makes them (the value shown is as that
/// mode supports it, tx_framegen). Variants (every one): the made frames' text; 2x by AMD FSR 3.1; the longest, with the limit's line
const char* hn_framegen(int v, int variant) {
    static char b[9][420];
    int shown = v;
    bool fsr = false, worst = false, fsr4 = false;
    if (variant < 0) {
        shown = fgr::clamp(framegen_by_now(), dlss_fg_most(), std::clamp(v, 0, 8));
        fsr4 = shown == 2 && framegen_by_now() == fgr::BY_FSR4;   // (AMD FSR 4 frame generation makes it, where AMD's runtime runs here)
        // (2x: by AMD FSR 3.1 where it runs, from the scene's depth and motion: vk_framegen.cpp)
        fsr = shown == 2 && (framegen_by_now() == fgr::BY_FSR3 || fsr3fi::frames_wanted() != 0);
    } else if (v >= 2) {
        // (every one: the text, then at 2x the same by AMD FSR 3.1, then the longest, with the limit's lowering)
        if (variant > (v == 2 ? 2 : 1)) return nullptr;
        fsr = v == 2 && variant >= 1, worst = variant == (v == 2 ? 2 : 1);
    } else if (variant > 0) {
        return nullptr;
    }
    if (shown <= 0) return "Only the game's own frames: no added delay.";
    if (shown == 1)
        return "A frame for each of the display's refreshes, up to the Frame rate limit: none made when the game keeps up. Frame "
               "pacing does not apply.";
    const char* made = shown == 2 ? "One made frame" : shown == 3 ? "Two made frames" : shown == 4 ? "Three made frames" : shown == 5 ? "Four made frames"
                       : shown == 6 ? "Five made frames" : shown == 7 ? "Six made frames" : "Seven made frames";
    char* out = b[std::clamp(shown, 2, 8)];
    snprintf(out, sizeof b[0], "%s between each two of the game's%s.%s", made, fsr4 ? ", by AMD FSR 4 from the scene's depth and motion" : fsr ? ", by AMD FSR 3.1 from the scene's depth and motion" : "",
             variant < 0 || worst ? framegen_limit_note(shown, worst) : "");
    return out;
}
/// Frame generation by: who makes the frames at 2x and up (Auto stays the renderer's own), and what the Frame generation row then
/// offers. Variants of DLSS FG's value: it runs (0), no NVIDIA runtime (1), NGX said no (2), not in this build (3), not on this GPU
/// (4), no temporal Upscaler (5)
const char* hn_framegen_by(int v, int variant) {
    static char b[260];
    if (v == fgr::BY_FSR4) {   // (AMD FSR 4 frame generation: variants 0 it runs, 1 not offered here (the reason: its why text) )
        static char c[420];
        const bool offered = variant < 0 ? fsr4fg_offered() : variant == 0;
        if (variant > 1) return nullptr;
        if (offered)
            return "AMD FSR 4 frame generation (a Radeon RX 9000 and AMD's runtime), from the scene's depth and motion: one made frame between two of "
                   "the game's, 2x only. NOT TESTED on real hardware yet. Where AMD's runtime refuses a frame, the renderer's own makes it.";
        snprintf(c, sizeof c, "Not available: %s The renderer's own (or AMD FSR 3.1) makes the frames.",
                 *fsr4fg_why_text(fsr4fg_why()) ? fsr4fg_why_text(fsr4fg_why()) : "Not asked yet.");
        return c;
    }
    if (v == fgr::BY_FSR3)
        return variant > 0 ? nullptr
                           : "AMD FSR 3.1's frame interpolation, from the scene's depth and motion: one made frame between two of the "
                             "game's, 2x only. Where it cannot run, the renderer's own makes that frame.";
    if (v <= 0)
        return variant > 0 ? nullptr
                           : "The renderer's own: AMD FSR 3.1 at 2x where it runs, else its own block matching. Auto and 2x to 8x.";
    if (variant > 5) return nullptr;
    int k = variant;
    if (k < 0) {
        const DlssFgWhy why = dlss_fg_why();
        k = why == FG_OK ? (temporal_wanted() ? 0 : 5) : why == FG_NO_RUNTIME ? 1 : why == FG_REFUSED ? 2 : why == FG_NOT_BUILT ? 3 : 4;
    }
    switch (k) {
    case 1: return "Not available: DLSS is not there (no NVIDIA runtime found: scripts/ngx_sdk.py fetches nvngx_dlss.dll and nvngx_dlssg.dll). The renderer's own makes the frames.";
    case 2: return "NVIDIA's runtime said no here (it needs an RTX 40 or newer and a current driver; the log has its answer). The renderer's own makes the frames.";
    case 3: return "Not in this build (it needs NVIDIA's SDK at build time). The renderer's own makes the frames.";
    case 4: return "Not available on this GPU. The renderer's own makes the frames.";
    case 5: return "NVIDIA DLSS Frame Generation needs a temporal Upscaler (NVIDIA DLSS or AMD FSR 4 on the Upscaler row) for the scene's depth and motion. Until then the renderer's own makes the frames, at any multiplier.";
    default: break;
    }
    const int most = dlss_fg_most();
    if (most > 0)
        snprintf(b, sizeof b, "NVIDIA DLSS Frame Generation makes the frames, from the scene's depth and motion: 2x up to %dx on this GPU, no Auto.", most + 1);
    else
        snprintf(b, sizeof b, "NVIDIA DLSS Frame Generation makes the frames, from the scene's depth and motion (RTX 40 and newer): 2x and up, no Auto.");
    return b;
}
/// The Base frame rate limit: what the value makes at the multiplier that runs now. Variants (every one): none made; a multiplier
/// (the longest); Game speed on Per frame
const char* hn_fps_base(int v, int variant) {
    static char b[240];
    if (v <= 0) return variant > 0 ? nullptr : "The Frame rate limit holds the rate alone, as before: the rate shown, the game's frames and the made ones.";
    if (variant > 2) return nullptr;
    const int base = fgr::base_fps(v);
    const bool per_frame = variant < 0 ? !opts.realtime : variant == 2;
    if (per_frame) return "Game speed is on Per frame: the game runs at its own 60 and this holds nothing. Set Game speed to Real time.";
    const int mult = variant < 0 ? (framegen_on() && framegen_now() >= 2 ? framegen_now() : 1) : variant == 0 ? 1 : 3;
    if (mult < 2)
        snprintf(b, sizeof b, "The game runs at %d frames a second%s.", base,
                 variant < 0 && framegen_on() && framegen_now() == 1 ? ", and Auto frame generation fills up to the display's refresh from them"
                                                                       : " (with Frame generation Off, the frames shown are those)");
    else
        snprintf(b, sizeof b, "At %dx the game runs at %d and %d frames a second are shown (the Frame rate limit holds nothing back; "
                              "the VRR row's cap still does).", mult, base, base * mult);
    return b;
}
MENU_HINTS(hn_pace, 0, "As soon as it is ready.", "At an even interval: less stutter, at up to one frame of delay.")
MENU_HINTS(hn_output_res, 0, "The game's own size.",
           "The sharpest on a 2560x1440 screen: nothing to scale to the window.",
           "The sharpest on a 3840x2160 screen. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 1920x1080 screen: the HUD and text pixel for pixel, nothing to scale to the window.",
           "The sharpest on a 5120x1440 ultrawide screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.")
/// The Window scaling filter: its filter, or that none is needed at this window's size
const char* hn_filter(int v, int variant) {
    static const char* const t[] = {"The plain copy.", "Sharper.", "Sharper still; can ring on edges."};
    static const char* const none = "The window is the Output resolution's size: nothing to scale. The choice is kept for "
                                    "another size.";
    if (variant < 0) return filter_needed() ? t[std::clamp(v, 0, 2)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 2)] : variant == 1 ? none : nullptr;
}
/// Sharpness: with DLSS or DLAA, the amount that suits them
const char* hn_sharpness(int v, int variant) {
    static const char* const dlss = "DLSS and DLAA look softer than the game's own upscaler: around 80% suits them.";
    const bool with_dlss = variant < 0 ? upscaler() == ur::DLSS : variant == 1;
    if (variant > 1) return nullptr;
    return with_dlss ? dlss : v == 0 ? "Off: the picture as it is." : nullptr;
}
// (HeyListen) the Lowest quality preset's first-start measure (lowend_rule.h): whether the question was asked once (the options file's
// lowend_rule=1, kept by save(), read by menu_init)
static bool lowend_marked;
static bool lowend_touched;     // the player set the Quality preset or one of the rows it sets in this run
static bool lowend_old_row;     // the options file has lowend_settings=1 (a build of 25 Nov with the row): menu_init reads it as the preset
MENU_HINTS(hn_handheld, 0, "Docked: the Output resolution's size.", "Lower resolutions, lighter on the GPU.")
MENU_HINTS(hn_ultrawide, 0, "The game's own shape.",
           "The camera takes the screen's shape, wider or narrower. The HUD keeps its 16:9 size, its corners at the "
           "screen's edges (HUD layout).")
MENU_HINTS(hn_robust, 0, "On, on every card.", "Can be faster on some NVIDIA cards.", "Always on.")
MENU_HINTS(hn_fps, 0, nullptr, "With frame generation on, the game's own rate too.")
MENU_HINTS(hn_menu_size, 0, "Large in fullscreen on a screen of 1080 lines or more while a controller is in use, else Medium.",
           "Smaller, for a desk.", "The usual size.", "Larger, for a TV across the room.")
MENU_HINTS(hn_speed, 0, "A step each frame: the game slows down whenever it runs below 60 fps.",
           "Normal speed down to 15 fps, by taking longer steps.")
MENU_HINTS(hn_text_speed, 0, "The game's own pace, with its pauses.", "Twice as fast.", "Four times as fast.",
           "Each page at once, as pressing A does.")
MENU_HINTS(hn_audio_output, 0, "5.1 when the default output device has 5.1 or more speakers, else stereo.",
           "Stereo, whatever the device has.", "5.1, whatever the device has. Mixed down on stereo speakers.")
MENU_HINTS(hn_background, 0, "Only while the game's window has the focus.",
           "Also behind other windows, for streaming or other apps. The keyboard and mouse still need the game's window.")
MENU_HINTS(hn_mousecam, 0, "The mouse stands in for the right stick, with its curve and top speed.",
           "The mouse turns the camera itself, one to one, as in a PC game.")
MENU_HINTS(hn_mouse_invert, 0, nullptr, "Moving the mouse forward tilts the camera down.")
MENU_HINTS(hn_gyro, 0, "The sticks alone aim.",
           "Switch Pro controllers and pads in Switch mode, DualSense and DualShock 4. The value says whether the "
           "connected pad has a gyroscope.")
MENU_HINTS(hn_pad_layout, 0, "The Switch's order: A on the right, B at the bottom, X at the top, Y on the left.",
           "A at the bottom (action and talk), B on the right, X on the left, Y at the top.",
           "A at the bottom, B on the right; X and Y as on the Switch.",
           "Bindings of your own, from the Controls page.")
std::atomic<int> prompts_missing{0};   // (HL_OPT_PROMPTS_MISSING, pushed by host/prompts.c: the game folder has no prompts/ folder)
const char* hn_prompts_text(int v, int variant);
const char* hn_prompts(int v, int variant) {
    // (no prompts/ folder: the row can change nothing, and the hint says why)
    if (prompts_missing.load(std::memory_order_relaxed) && variant <= 0)
        return "Changes nothing here: this game folder has no prompts folder (the Builder could not make it: it needs fontTools and "
               "Pillow), so the game shows its own icons. Run the Builder again with them installed.";
    return hn_prompts_text(v, variant);
}
MENU_HINTS(hn_prompts_text, 0,
           "Follows what you play with: a controller's (from its name, SDL's), or the keyboard's once you use its keys. "
           "At the start, the first controller's; with none, the keyboard's (an Xbox's with XInput alone).",
           "The Switch's buttons.", "An Xbox controller's.", "A PlayStation 4 controller's.", "A PlayStation 5 controller's.",
           "A Steam Deck's.", "The keys and mouse buttons your controls put each action on.")

Row rows[] = {
    // Graphics (the Upscaler in every build: it always has the game's own and Off)
    {TAB_GRAPHICS, "Quality preset", &opts.preset, 0, 6, 1, tx_preset,
     "Sets the scaling rows and memory limits for a GPU with that much memory; Lowest is for a weak integrated GPU. The Upscaler and Frame "
     "generation stay yours. On an integrated GPU the game picks Lowest once by itself if it runs under 20 fps (measured after 15 seconds of "
     "play); HL_LOWEND=0 in the environment stops that, HL_LOWEND=1 chooses it for one run.",
     hn_preset},
    {TAB_GRAPHICS, "Fullscreen", &opts.fullscreen, 0, 1, 1, tx_onoff,
     "Fills the whole screen with the game, without the window's border. The same as F11.", nullptr, false, true},
    {TAB_GRAPHICS, "GPU device", &opts.gpu_device, 0, 8, 1, tx_gpu,
     "Which graphics card the game runs on. Applies after a restart.", hn_gpu, true, true},
    {TAB_GRAPHICS, "V-Sync", &opts.vsync, 0, 1, 1, tx_vsync,
     "Whether frames wait for the screen's refresh. Applies at once, when the swapchain is made again; RX_VK_FIFO and RX_VK_IMMEDIATE in the "
     "environment win over it. The V-Sync mode row chooses how many images it keeps. The overlay's Presentation line shows the mode in use.",
     hn_vsync, false, true},
    {TAB_GRAPHICS, "V-Sync mode", &opts.vsync_mode, 0, 2, 1, tx_vsync_mode,   // (opts.vsync_mode, present_mode.h; off while V-Sync is Off)
     "With V-Sync On: FIFO double buffering, FIFO triple buffering or Mailbox. Applies at once, when the swapchain is made again.",
     hn_vsync_mode, false, true},
    {TAB_GRAPHICS, "VRR (G-Sync / FreeSync)", &opts.vrr, 0, 2, 1, tx_vrr,
     "For a variable refresh rate screen, in fullscreen: the game is shown without tearing and without V-Sync's wait. "
     "Needs G-Sync or FreeSync on in the driver. Linux: the compositor's job.", hn_vrr, false, true},
    // (the picture's scaling, then who makes the frames: the Upscaler and its Render resolution (DLSS's model with them), Frame generation by
    // and its multiplier right under it, Frame pacing)
    {TAB_GRAPHICS, "Upscaler", &opts.dlss, 0, ur::kUpscalers - 1, 1, tx_upscaler, "What scales the 3D up to the Output resolution. The Render resolution below it is the share of the output the 3D is drawn at.",
     hn_upscaler},
    {TAB_GRAPHICS, "Render resolution", &opts.resolution, 0, ur::kValues - 1, 1, tx_resolution,
     "The share of the output the 3D is drawn at; the Upscaler fills in the rest.", hn_resolution},
    {TAB_GRAPHICS, "DLSS model", &opts.dlss_model, 0, ur::kModels - 1, 1, tx_dlss_model,
     "Which of NVIDIA's DLSS networks draws the picture.", hn_dlss_model, false, true},
    {TAB_GRAPHICS, "DLAA before the bloom", &opts.dlss_pre_bloom, 0, 1, 1, tx_onoff,
     "Where NVIDIA DLAA smooths the picture: after the game's bloom and tone map, or before them. Changes at once.",
     hn_dlss_pre_bloom, false, true},
    {TAB_GRAPHICS, "Frame generation by", &opts.framegen_by, 0, fgr::kBy - 1, 1, tx_framegen_by,
     "Who makes the made frames, and so how many the Frame generation row offers. NVIDIA DLSS Frame Generation (RTX 40 and newer, "
     "with a temporal Upscaler) is off unless chosen here. AMD FSR 4 FG (a Radeon RX 9000, 2x) is a choice only where AMD's own runtime "
     "says it runs on this GPU (the Builder's AMD FSR 4 box fetches it: if it is missing, run the Builder again with the box ticked), and is not tested on real hardware yet. Changing it keeps the multiplier within what the new mode makes.",
     hn_framegen_by, false, true},
    {TAB_GRAPHICS, "Frame generation", &opts.framegen, 0, 8, 1, tx_framegen,
     "Makes frames between the game's own, for smoother motion, at a little input delay. Fast motion or camera cuts can "
     "show artifacts. What the mode in Frame generation by makes: NVIDIA DLSS FG up to what NVIDIA's runtime makes on this GPU "
     "(6x on an RTX 50, 2x on an RTX 40), AMD FSR 3.1 2x, the built-in one Auto and 2x to 8x. The Frame rate limit is the rate shown: "
     "the game runs at the limit over the multiplier.", hn_framegen, false, true},
    {TAB_GRAPHICS, "Frame pacing", &opts.pace, 0, 1, 1, tx_onoff, "When each frame is shown.", hn_pace},
    {TAB_GRAPHICS, "HDR", &opts.hdr, 0, 2, 1, tx_hdr,
     "HDR10 out on an HDR display (Windows: Use HDR on in the display settings). Expanded: the game's SDR picture, its white at "
     "the paper white and its highlights up to the peak. Native: the game's own HDR, told the display's peak and paper white. "
     "With Native, the HDR tone mapper row below chooses the game's own curve or PsychoV35. Needs an HDR screen with Windows HDR on.",
     hn_hdr, false, true},
    {TAB_GRAPHICS, "HDR paper white", &opts.hdr_white, 80, 400, 10, tx_nits,
     "How bright the picture's white is with HDR on. Around 200 for a normal room; higher in a bright one.", nullptr, false, true},
    {TAB_GRAPHICS, "HDR peak brightness", &opts.hdr_peak, 400, 4000, 100, tx_nits,
     "The brightest highlights with HDR on: set it to your display's peak brightness.", nullptr, false, true},
    {TAB_GRAPHICS, "HDR contrast", &opts.hdr_contrast, 50, 200, 5, tx_percent,
     "The HDR picture's contrast around mid grey, as the RenoDX HDR mods grade it: above 100% deeper shadows and brighter "
     "highlights, which ease into the peak brightness; below it flatter. With Native the game's own HDR picture is graded too, "
     "under the peak brightness the game is told. Hidden with PsychoV35, which has its own Contrast.",
     nullptr, false, true},
    // (HeyListen, after Longshot ddb156df: shown with HDR set to Native)
    {TAB_GRAPHICS, "HDR tone mapper", &opts.hdr_tonemap, 0, 1, 1, tx_tonemap,
     "With Native, how the game's scene meets the peak brightness. Vanilla: the game's own curve. PsychoV35: RenoDX's "
     "tone mapper, the same mid grey with highlights that roll off into the peak and keep their colour. Every Upscaler "
     "takes its scene: DLSS, DLAA, FSR, TAAU and Off.",
     nullptr, false, true},
    // (after Longshot 79a34381: PsychoV35's grading, RenoDX's zelda-oot-oz settings: shown with it, read by the scene each draw,
    // psychov35_bank)
    {TAB_GRAPHICS, "PsychoV35 UI brightness", &opts.pv35_ui_white, 80, 500, 1, tx_nits,
     "PsychoV35: how bright the menus and the HUD's white is. The scene keeps the paper white.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 color space", &opts.pv35_color_space, 0, 2, 1, tx_color_space,
     "PsychoV35: the picture's colours on the display. BT.709: as made, RenoDX's default; Vanilla: through the game's own "
     "matrix; DCI-P3: read as P3, more vivid.",
     nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 midgray", &opts.pv35_midgray, 0, 1, 1, tx_midgray,
     "PsychoV35: Vanilla keeps mid grey where the game's own curve puts it; Render keeps the scene's own.", nullptr, false,
     true},
    {TAB_GRAPHICS, "PsychoV35 match tone slope", &opts.pv35_match_slope, 0, 1, 1, tx_onoff,
     "PsychoV35: On follows the game's own curve's slope at mid grey; Off a neutral one.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 tone slope", &opts.pv35_tone_slope, 0, 100, 1, tx_grade,
     "PsychoV35: how steeply the tones rise through mid grey. 50 is the game's.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 hue clip emulation", &opts.pv35_hue_clip, 0, 100, 1, tx_grade,
     "PsychoV35: how much of the game's own colour shift in its brightest parts is kept. 50 is RenoDX's.", nullptr, false,
     true},
    {TAB_GRAPHICS, "PsychoV35 exposure", &opts.pv35_exposure, 0, 200, 5, tx_percent,
     "PsychoV35: the scene's brightness at mid grey. 100% is the game's.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 highlights", &opts.pv35_highlights, 0, 100, 1, tx_grade,
     "PsychoV35: above 50 brighter highlights, below it softer ones. 50 leaves them as they are.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 shadows", &opts.pv35_shadows, 0, 100, 1, tx_grade,
     "PsychoV35: above 50 deeper shadows, below it lifted ones. 50 leaves them as they are.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 contrast", &opts.pv35_contrast, 0, 100, 1, tx_grade,
     "PsychoV35: the contrast around mid grey. 50 is the game's.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 saturation", &opts.pv35_saturation, 0, 100, 1, tx_grade,
     "PsychoV35: the colours' strength. 50 is the game's.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 bloom", &opts.pv35_bloom, 0, 100, 1, tx_grade,
     "PsychoV35: the glow around bright things. 50 is the game's; 0 none.", nullptr, false, true},
    {TAB_GRAPHICS, "PsychoV35 reset all", nullptr, 0, 0, 0, nullptr,
     "PsychoV35: every row above back to RenoDX's defaults.", nullptr, false, true},
    {TAB_GRAPHICS, "NVIDIA Reflex", &opts.reflex, 0, 2, 1, tx_reflex,
     "NVIDIA GPUs: each frame starts as late as it can, for less input delay. Not used with frame generation.", hn_reflex},
    {TAB_GRAPHICS, "Frame rate limit", &opts.fps_limit, 0, fgr::kLimits - 1, 1, tx_fps_limit,   // (HeyListen: HL_OPT_FPS_LIMIT, vk_opt_ids.h, host/nn_vi.c)
     "The most frames a second shown: the game's own and, with frame generation on, the frames made between them. With a fixed "
     "Frame generation multiplier the game runs at the limit over it (120 at 3x: the game at 40). Unlimited holds nothing back. "
     "Above 60 it needs Game speed on Real time (the game's time follows the clock, so it does not run faster), and your display "
     "must run that fast to show every frame.", nullptr, false, true},
    {TAB_GRAPHICS, "Base frame rate limit", &opts.fps_base, 0, fgr::kBases - 1, 1, tx_fps_base,   // (HeyListen: HL_OPT_BASE_LIMIT, vk_opt_ids.h, host/nn_vi.c)
     "The game's OWN frame rate, before frame generation makes more: the Frame rate limit above holds what is shown, this holds what "
     "the game draws. With it on, the game runs at this rate, the frames shown are this rate times the Frame generation multiplier "
     "(60 at 2x is 120, 48 at 3x is 144), and Auto fills up to the display's refresh from it; the Frame rate limit then holds nothing "
     "back. Every frame generator then gets an even rate. With Frame generation Off it is the limit itself. Off: the Frame rate limit "
     "alone, as before. Needs Game speed on Real time.", hn_fps_base, false, true},
    // (the picture's way, top to bottom: the output's size, what fills it from the 3D, the 3D's size, then the window)
    {TAB_GRAPHICS, "Output resolution", &opts.output_res, 0, kLastOutput, 1, tx_output_res,
     "The game's whole frame, HUD included, before it is scaled to the window. Docked only. Applies after a restart.",
     hn_output_res, true},
    {TAB_GRAPHICS, "Window scaling filter", &opts.upscale, 0, 2, 1, tx_filter,
     "How the finished picture is scaled to the window, when its size differs from the Output resolution.", hn_filter},
    {TAB_GRAPHICS, "Sharpness", &opts.sharpness, 0, 100, 10, tx_percent,
     "Sharpens the picture as it goes to the window, with any filter. Too much shows halos around edges.", hn_sharpness},
    {TAB_GRAPHICS, "Handheld profile", &opts.handheld, 0, 1, 1, tx_onoff,
     "Runs the game as if the console were undocked: the game is told, as a console tells it, and changes at once. The size of its "
     "picture follows after a restart.", hn_handheld},
    {TAB_GRAPHICS, "Volumetric fog", &opts.fog, 0, 1, 1, tx_onoff,
     "The haze in the distance and the mist of the forests. Off is lighter on the GPU (about an eighth of the frame on integrated "
     "graphics, a few percent on a graphics card) and shows the distance crisper. Applies at once.", nullptr},
    {TAB_GRAPHICS, "Ultrawide", &opts.ultrawide, 0, 1, 1, tx_ultrawide,
     "The camera's shape on a screen wider or narrower than 16:9 (4:3, 16:10). Applies after a restart.", hn_ultrawide, true},
    {TAB_GRAPHICS, "HUD layout", &opts.hud_layout, 0, 1, 1, tx_hud_layout,
     "On a screen that is not 16:9: where the hearts, buttons and minimap sit. At 16:9 both are the same. Changes at once.",
     hn_hud_layout, false, true},
    {TAB_GRAPHICS, "Robust buffers", &opts.robust, 0, 2, 1, tx_robust,
     "Bounds-checked buffer reads: a read past a buffer's end is dropped instead of stopping the GPU. Applies after a "
     "restart.", hn_robust, true},
    {TAB_GRAPHICS, "Texture resolution", &opts.tex_low, 0, 1, 1, tx_tex_res,
     "How sharp the game's large textures are. Low keeps cards with little video memory inside it. Applies after a restart.",
     hn_tex_res, true, true},

    // Gameplay
    {TAB_GAMEPLAY, "FPS counter", &opts.show_fps, 0, 2, 1, tx_perf,
     "FPS: frames per second and the frame time in the top right corner. Full: a panel with the GPU, VRAM, CPU and RAM load "
     "and a frame-time graph. F10 cycles Off, FPS and Full.", hn_fps, false, true},
    {TAB_GAMEPLAY, "Menu size", &opts.menu_size, 0, 3, 1, tx_menu_size,
     "How large this menu and the start-up screen are drawn.", hn_menu_size, false, true},
    {TAB_GAMEPLAY, "Game speed", &opts.realtime, 0, 1, 1, tx_speed, "How the game keeps time when it runs below 60 fps.",
     hn_speed},
    {TAB_GAMEPLAY, "Text speed", &opts.text_speed, 0, 3, 1, tx_text_speed,
     "How fast the letters of dialogue and other messages appear.", hn_text_speed},
    {TAB_GAMEPLAY, "Volume", &opts.volume, 0, 100, 5, tx_percent,   // (after Hookshot 2c87e357: host/nn_audio.c, HL_OPT_VOLUME)
     "The game's sound: music, effects and voices together. 0% mutes it.", nullptr, false, true},
    {TAB_GAMEPLAY, "Audio output", &opts.audio_output, 0, 2, 1, tx_audio_output,
     "The speakers the game mixes its sound for. Applies after a restart.", hn_audio_output, true, true},
    {TAB_GAMEPLAY, "Microphone", &opts.mic, 0, 2 + kMaxMics, 1, tx_mic,
     "What the ocarina hears when the game's microphone option is on (playing an instrument, or humming).", hn_mic, false,
     true},
    {TAB_GAMEPLAY, "Output device", &opts.out_dev, 0, 1 + kMaxOuts, 1, tx_out,
     "The device the game's sound plays on. Default follows the system's default output. A change applies at once.", hn_out,
     false, true},
    {TAB_GAMEPLAY, "Skip title screen", &opts.skip_title, 0, 1, 1, tx_onoff,   // (HL_OPT_SKIP_TITLE, vk_opt_ids.h, host/oot_autoload.c)
     "Starts a launch with the most recent saved game: presses through the title screen and its file list. Does nothing "
     "until the game has a save. Applies after a restart.", nullptr, true, true},
    {TAB_GAMEPLAY, "First person", &opts.first_person, 0, 1, 1, tx_onoff,   // (HL_OPT_FIRST_PERSON, vk_opt_ids.h, host/hos_main.c)
     "Plays from Link's eyes: the right stick looks around, Link walks the way you look, and his head is hidden. The free "
     "camera, while on, takes its place.", nullptr, false, true},
    {TAB_GAMEPLAY, "Send performance statistics", &opts.send_perf, 0, 1, 1, tx_telemetry,
     "Off by default. On: the game counts how its frames go (frame times, stutters and what they waited on, per map) and sends "
     "a summary of the counts, without your name or any ID; what is kept of it is totals for all players. Every 5 minutes of play "
     "and at exit it writes a summary to perf_pending in the game folder, and sends it at the next start; perf_sent keeps a copy "
     "of what was sent. At exit it also queues the graphics pipeline states your game made that the shipped list lacks (hashes "
      "and render state only, nothing of the game's code or data; pipelines_pending, pipelines_sent), unless HL_TELEMETRY_PIPELINES=0. "
      "It also notes, per map, which shader programs and pipelines your game's draws used, by hash alone (zones_pending, "
      "zones_sent), unless HL_TELEMETRY_ZONES=0. Each summary also carries a short memory record (your RAM size as a class, the least "
      "free RAM, the commit charge, working-set trims and page-fault counts, and how long the game held its memory trimming back) and, "
      "for each stutter, which kind of wait it was (a lock, a sleep, a file, a page fault...), all as numbers, with no path or name "
      "(HL_TEL_MEMORY=0 and HL_TEL_WAITS=0 leave them out). Turning it off deletes those folders. See docs/performance-statistics.md.",
     hn_telemetry, false, true},
    {TAB_GAMEPLAY, "Send crash reports automatically", &opts.send_crash, 0, 1, 1, tx_telemetry,
     "Off by default: after a crash a window asks what to do. On: the crash report (the error, your PC's hardware, the end of the "
     "log, the settings, with your home folder, user name, every folder path and device names taken out) is sent at once, without "
     "the window; if it cannot be sent the window shows as usual. Your report is published on the project's GitHub (issue #55) and "
     "Discord, without your user name or folders. It holds the crash's place and the registers' values (any th"
     "at look like text are masked), the map and the time played, the PC's GPU, driver, CPU, RAM and memory use, the log's last lines, the settings and the switches of hookshot_env.txt; never saves, game files or memory.", hn_telemetry, false, true},
    {TAB_GAMEPLAY, "Discord status", &opts.discord, 0, 1, 1, tx_onoff,
     "Shows \"Playing Hookshot\" and the time played in your Discord status, while Discord runs on this PC.", nullptr, false,
     true},
    {TAB_GAMEPLAY, "Check for updates", &opts.update_check, 0, 1, 1, tx_onoff,   // (host/hos_update.c reads update_check= from the options file at the start)
     "On (the default): at the start of a game you started yourself, a short look for a newer HeyListen release, and a window "
     "that offers to update, keep your version or skip that version. Nothing is asked when you are offline, and when the launcher "
     "starts the game it looks for updates itself. Applies after a restart.", nullptr, true, true},

    // Controls (15 rows; Motion opens the 6 TAB_MOTION rows, Controls its page, which shows this tab's Layout preset and Button prompts rows too)
    // (hookshot) the input rows first, together so they are easy to find; the Controls page shows the same two above its grid
    {TAB_CONTROLS, "Layout preset", &opts.pad_layout, 0, 3, 1, tx_pad_layout,
     "Which face button is which. Binding one yourself on the Controls page makes it Custom.", hn_pad_layout},
    {TAB_CONTROLS, "Button prompts", &opts.prompts, 0, HOS_PROMPT_N, 1, tx_prompts,
     "The button icons the game shows, in its texts, menus and the ocarina's notes: those of this controller or the keyboard, for "
     "the buttons your controls put each action on. Applies at once, no restart; a mod's own icon files keep theirs.", hn_prompts, false, true},
    {TAB_CONTROLS, "Controller in background", &opts.pad_background, 0, 1, 1, tx_onoff,
     "Whether the controller plays the game while another window has the focus.", hn_background},
    {TAB_CONTROLS, "Sprint with L3", &opts.sprint, 0, 2, 1, tx_sprint,   // (HL_OPT_SPRINT, vk_opt_ids.h, host/oot_sprint.c; saved as sprint_with_l3)
     "Off (the default): the left stick's press is the game's own LS. Hold: while it is pressed Link runs with the game's own fast "
     "run, the one that follows a roll while A is held, without the roll, until he stops, rolls or targets. Toggle: one click "
     "starts that run, which goes on without holding and ends when the left stick is back at rest, on another click, or when "
     "he targets, a menu or a cutscene opens, or he does something else. Any input bound to the "
     "game's LS counts, the keyboard's too. The game no longer gets that press as LS, except one that begins in a conversation or "
     "a cutscene, which still hides the text box. A is untouched: tapping it still rolls. Ported from Longshot (llun, kendowashere).",
     nullptr, false, true},
    {TAB_CONTROLS, "Touchpad opens menu", &opts.touchpad_menu, 0, 1, 1, tx_onoff,   // (issue 120; saved as touchpad_opens_menu)
     "On (the default): clicking the DualSense or DualShock 4 touchpad opens and closes this menu. Off: the click does nothing, "
     "so it cannot open the menu by accident. The menu still opens with Insert or F1, Home, Select and Start together, and the "
     "Open menu control of the Controls page.", nullptr, false, true},
    {TAB_CONTROLS, "Mouse camera", &opts.mouse_native, 0, 1, 1, tx_mousecam, "How the captured mouse turns the camera.",
     hn_mousecam, false, true},
    {TAB_CONTROLS, "Mouse sensitivity", &opts.mouse_sens, 10, 500, 10, tx_sens,
     "How far the camera turns for a mouse movement, with either Mouse camera: at 1.00 native look turns 0.1 degree a "
     "count. The - and = keys change it in game too.", nullptr, false, true},
    {TAB_CONTROLS, "Invert mouse vertical", &opts.mouse_invert, 0, 1, 1, tx_onoff, "Swaps up and down for the mouse.",
     hn_mouse_invert, false, true},
    {TAB_CONTROLS, "Invert camera X", &opts.invert_cam_x, 0, 1, 1, tx_onoff,
     "Inverts horizontal look (left/right) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Invert camera Y", &opts.invert_cam_y, 0, 1, 1, tx_onoff,
     "Inverts vertical look (up/down) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Stick dead zone", &opts.stick_dead_zone, 0, 5, 1, tx_stick_dead_zone,
     "How far a stick moves before the game sees it, on each axis. Lower for a precise stick, higher for one that drifts. "
     "The game's own dead zone still follows.", nullptr, false, true},
    {TAB_CONTROLS, "Rumble strength", &opts.rumble, 0, 150, 25, tx_rumble,
     "How strongly the controller rumbles, in percent of the game's rumble. Off turns it off.", nullptr, false, true},
    {TAB_MOTION, "Motion controls", &opts.gyro, 0, 1, 1, tx_gyro,
     "Aims and looks with the controller's gyroscope where the game uses motion, as on the Switch. Works with Switch Pro "
     "controllers and pads in Switch mode, DualSense and DualShock 4. Shown beside it: whether the connected pad has one.", hn_gyro,
     false, true},
    {TAB_MOTION, "Motion sensitivity X", &opts.gyro_sens_x, 10, 600, 10, tx_percent,
     "How far the aim moves left and right for a given turn of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Motion sensitivity Y", &opts.gyro_sens_y, 10, 300, 10, tx_percent,
     "How far the aim moves up and down for a given tilt of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Invert motion X", &opts.gyro_inv_x, 0, 1, 1, tx_onoff, "Turns the motion aim's left and right around.",
     nullptr, false, true},
    {TAB_MOTION, "Invert motion Y", &opts.gyro_inv_y, 0, 1, 1, tx_onoff, "Turns the motion aim's up and down around.",
     nullptr, false, true},
    {TAB_MOTION, "Motion deadzone", &opts.gyro_deadzone, 0, 2, 1, tx_deadzone,
     "How slow a turn counts as none, the console's three settings: Standard is its own. High steadies a pad that jitters "
     "when held still; Low keeps the smallest movements.", nullptr, false, true},
    {TAB_CONTROLS, "Motion", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens the Motion page, the TAB_MOTION rows)
     "Motion controls: aiming with the controller's gyroscope, its sensitivity and its inverts."},
    {TAB_CONTROLS, "Controls", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens its page)
     "The layout preset, the button prompts, and which gamepad button and which keys press each of the Switch's "
     "buttons."},
    {TAB_CONTROLS, "Menu shortcut", &opts.menu_shortcut, 0, 2, 1, tx_menu_shortcut,
     "Which keyboard shortcut opens and closes this menu. Choose one key to leave the other free for an overlay. "
     "On a controller, Home, Select and Start together or, unless Touchpad opens menu is off, the touchpad click (DualSense, DualShock 4) open it.",
     nullptr, false, true},
    // Debug: the tools' switch, the tools themselves below it while they are on (cheat: menu_tick does what they do), then the
    // developer's rows
    {TAB_DEBUG, "Debug tools", &opts.debug_tools, 0, 1, 1, tx_onoff,
     "Testing tools: no-clip flight, a free camera, teleport, Link's age, the inventory, the clock and heart protection. Off "
     "puts flight, the free camera and heart protection away.", nullptr, false, true},
    {TAB_DEBUG, "Heart protection", nullptr, 0, 0, 0, nullptr,
     "Protects Link's hearts. Hits can still knock him back.", nullptr, false, true, 0},
    {TAB_DEBUG, "No-clip flight", nullptr, 0, 0, 0, nullptr,
     "Enable double-tap Jump for takeoff and landing. Hold Jump to climb or L to descend.", nullptr, false, true, 1},
    {TAB_DEBUG, "Mark position", nullptr, 0, 0, 0, nullptr,
     "Remember Link's position for this loaded scene.", nullptr, false, true, 2},
    {TAB_DEBUG, "Return to mark", nullptr, 0, 0, 0, nullptr,
     "Teleport Link to the marked position in the current scene.", nullptr, false, true, 3},
    {TAB_DEBUG, "Set time", nullptr, 0, 0, 0, nullptr,
     "Choose an hour with Left or Right. Enter sets the world clock.", nullptr, false, true, 4},
    {TAB_DEBUG, "Inventory item", nullptr, 0, 0, 0, nullptr,
     "Choose a consumable with Left or Right.", nullptr, false, true, 5},
    {TAB_DEBUG, "Change quantity", nullptr, 0, 0, 0, nullptr,
     "Choose how many to add or remove. Enter applies the change.", nullptr, false, true, 6},
    {TAB_DEBUG, "Free camera", nullptr, 0, 0, 0, nullptr,
     "Move and look with your usual controls. Page Up / Page Down or R / L changes height.", nullptr, false, true, 7},
    {TAB_DEBUG, "Teleport", nullptr, 0, 0, 0, nullptr,
     "Opens the map list. Link travels through the game's own map change, in his current age.", nullptr, false, true, 8},
    {TAB_DEBUG, "Switch age", nullptr, 0, 0, 0, nullptr,
     "Swaps Link between child and adult and reloads this map in the other age (the Market when this map has no match).", nullptr, false, true, 9},
    {TAB_DEBUG, "Inventory", nullptr, 0, 0, 0, nullptr,
     "Enter opens the inventory editor: unlock or relock equipment, items, upgrades and quest items, and change amounts.", nullptr, false, true, 10},
    {TAB_DEBUG, "Developer", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    {TAB_DEBUG, "Descriptor reuse", &opts.descriptor_reuse, 0, 1, 1, tx_onoff,
     "A draw keeps its descriptors while the texture slots it samples are unchanged, so streaming elsewhere in the game no longer "
     "rebuilds every set each frame. Applies after restart.", nullptr, true},
    {TAB_DEBUG, "Sampler cache", &opts.sampler_cache, 0, 1, 1, tx_onoff,
     "What a sampler holds is kept beside it on the replay thread, so resolving a sampler that was not registered anew takes no "
     "lock. Helps while the game streams textures. Applies after restart.", nullptr, true},
    // (longshot #252, Intel graphics on Windows only: row_shown) IntelDriverGate, vk_internal.h
    {TAB_DEBUG, "Intel shader compiles", &opts.intel_compiles, 0, 1, 1, tx_intel_compiles,
     "Intel graphics: how many shaders Intel's compiler is given at once. Applies after a restart.", hn_intel_compiles, true},
    // (Restart and Quit stay the table's last two: kRestartRow and kQuitRow count back from its end)
    {TAB_GAMEPLAY, "Restart", nullptr, 0, 0, 0, nullptr,
     "Starts the game again. Save your progress first. Press Enter or A twice to restart. Esc or B "
     "cancels."},
    {TAB_GAMEPLAY, "Quit", nullptr, 0, 0, 0, nullptr,
     "Save your progress first. Press Enter or A twice to quit. Esc or B cancels."},
};
constexpr int kQuitRow = sizeof rows / sizeof rows[0] - 1;
constexpr int kRestartRow = kQuitRow - 1;
const Row* pending = nullptr;   // (Restart or Quit: shows Confirm until its second press)
bool cheats_env = false;   // (HOS_OOT_CHEATS=1: on whatever the Debug tab says)
/// The debug tools: the Debug tab's row, or HOS_OOT_CHEATS=1
bool cheats_on() { return cheats_env || opts.debug_tools; }
std::atomic<int> protect_hearts{-1};
// -1 disables the shortcut, 0 enables it on the ground, and 1 means flying.
std::atomic<int> player_flight{-1};
std::atomic<int> free_camera{0};
std::atomic<int> cheat_action{0};
int clock_hour = 12;
int pouch_item = 0;
int pouch_delta = 1;
constexpr const char* pouch_items[] = {"Deku Seeds", "Deku Nuts", "Deku Sticks", "Bombs", "Arrows"};
constexpr int kPouchItems = sizeof pouch_items / sizeof pouch_items[0];
// (teleport list: generated by work/oot-teleport-tools/write_destinations.py)
// (the order of oot_destinations in host/hos_main.c; Enter sends 1000 + the index)
constexpr const char* teleport_names[] = {
    "Kokiri Forest",
    "Hyrule Field",
    "Kakariko Village",
    "Market",
    "Hyrule Castle",
    "Lost Woods",
    "Sacred Forest Meadow",
    "Lake Hylia",
    "Graveyard",
    "Death Mountain Trail",
    "Death Mountain Crater",
    "Goron City",
    "Zora's River",
    "Zora's Domain",
    "Zora's Fountain",
    "Gerudo Valley",
    "Gerudo Fortress",
    "Haunted Wasteland",
    "Desert Colossus",
    "Inside the Deku Tree",
    "Dodongo's Cavern",
    "Jabu-Jabu's Belly",
    "Forest Temple",
    "Fire Temple",
    "Water Temple",
    "Shadow Temple",
    "Spirit Temple",
    "Bottom of the Well",
    "Ice Cavern",
    "Gerudo Training Ground",
    "Ganon's Castle"};
constexpr const char* teleport_notes[] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "adult only", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
struct TeleportSection { const char* name; int first; };
constexpr TeleportSection teleport_sections[] = {{"Overworld", 0}, {"Dungeons", 19}};
constexpr int kTeleportSections = sizeof teleport_sections / sizeof teleport_sections[0];
// (end of the generated teleport list)
constexpr int kTeleports = sizeof teleport_names / sizeof teleport_names[0];
int teleport_choice = 0;
int teleport_scroll = 0;   // (the first list line shown on the Teleport page)
int list_lines = 20;   // (list lines the Teleport and Inventory pages show at once: as many as fit, 8 to 20; set by the drawing)
// the Inventory page (gpu/vk/oot_cheat_items.h): items and their section headings, counts from the game side
#define OOT_MENU_ITEM_LABEL(label, name, kind) label,
#define OOT_MENU_ITEM_KIND(label, name, kind) kind,
#define OOT_MENU_NO_SECTION(name)
constexpr const char* item_names[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_LABEL)};
constexpr unsigned char item_kinds[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_KIND)};
constexpr int kItems = sizeof item_names / sizeof item_names[0];
// (each section's first item: a counter that a section heading reads and an item bumps)
constexpr int item_section_count() {
    int n = 0;
#define OOT_MENU_COUNT_SECTION(title) n++;
#define OOT_MENU_SKIP_ITEM(label, name, kind)
    OOT_CHEAT_ITEMS(OOT_MENU_COUNT_SECTION, OOT_MENU_SKIP_ITEM)
    return n;
}
constexpr int kItemSections = item_section_count();
struct ItemSections { TeleportSection s[kItemSections]; };
constexpr ItemSections make_item_sections() {
    ItemSections r{};
    int section = 0, item = 0;
#define OOT_MENU_ADD_SECTION(title) r.s[section].name = title, r.s[section].first = item, section++;
#define OOT_MENU_ADD_ITEM(label, name, kind) item++;
    OOT_CHEAT_ITEMS(OOT_MENU_ADD_SECTION, OOT_MENU_ADD_ITEM)
    return r;
}
constexpr ItemSections item_sections = make_item_sections();
std::atomic<int> prompts_live{0};   // (HL_PROMPTS_LIVE_* bits, pushed by host/prompts.c: what a new face of the Button prompts reaches while the game runs)
std::atomic<int> item_counts[kItems];   // (-1 until the game side reports: OOT_CHEAT_ITEM_BASE + i)
int item_choice = 0, item_scroll = 0;
constexpr int kCheatRows = 11;   // (the debug tools' rows: Row::cheat 0 to 10)
/// A debug tool's value as its row shows it ("" an action: confirm does it)
std::string cheat_value(int i) {
    char t[16];
    switch (i) {
    case 0: return tx_onoff(protect_hearts.load() == 1);
    case 1: return tx_onoff(player_flight.load() >= 0);
    case 4: return snprintf(t, sizeof t, "%02d:00", clock_hour), t;
    case 5: return pouch_items[pouch_item];
    case 6: return snprintf(t, sizeof t, "%+d", pouch_delta), t;
    case 7: return tx_onoff(free_camera.load());
    default: return "";
    }
}
/// The tools that choose with Left and Right (arrows at their value's ends): Set time, Inventory item, Change quantity
bool cheat_chooses(int i) { return i == 4 || i == 5 || i == 6; }
/// What confirm does on a tool's row, for its prompt (nullptr: nothing)
const char* cheat_confirm(int i) {
    static const char* const says[kCheatRows] = {"Toggle", "Toggle", "Apply", "Apply", "Set", nullptr, "Apply", "Toggle", "Open list",
                                                 "Switch", "Open"};
    return i >= 0 && i < kCheatRows ? says[i] : nullptr;
}
constexpr int kRows = sizeof rows / sizeof rows[0];

int cur_tab = TAB_GRAPHICS;
int tab_selected[TAB_COUNT + 1] = {0, 0, 0, 0, 0};   // (and the Motion page's)
bool is_motion_row(const Row& r) { return r.tab == TAB_CONTROLS && !r.value && !strcmp(r.name, "Motion"); }
// (HeyListen, after Longshot 79a34381) the Graphics tab's PsychoV35 reset all: an action, not a page
bool is_psychov35_reset_row(const Row& r) { return !r.value && r.tab == TAB_GRAPHICS && !strcmp(r.name, "PsychoV35 reset all"); }
/// PsychoV35's grading back to RenoDX's defaults (the Options' own)
void psychov35_reset() {
    const Options defaults;
    opts.pv35_exposure = defaults.pv35_exposure, opts.pv35_highlights = defaults.pv35_highlights;
    opts.pv35_shadows = defaults.pv35_shadows, opts.pv35_contrast = defaults.pv35_contrast;
    opts.pv35_saturation = defaults.pv35_saturation, opts.pv35_tone_slope = defaults.pv35_tone_slope;
    opts.pv35_hue_clip = defaults.pv35_hue_clip, opts.pv35_midgray = defaults.pv35_midgray;
    opts.pv35_match_slope = defaults.pv35_match_slope, opts.pv35_bloom = defaults.pv35_bloom;
    opts.pv35_ui_white = defaults.pv35_ui_white, opts.pv35_color_space = defaults.pv35_color_space;
}

// ---- the rows that take a restart: their values at the start (menu_init, the GPU's at menu_set_gpu_count) ----
namespace mr = menu_restart;
mr::State restart_state;
int restart_slot[kRows];             // (a row's slot in restart_state, -1 none)
const char* restart_names[mr::kMax];
void restart_init() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < kRows; i++) {
        restart_slot[i] = rows[i].restart && rows[i].value ? restart_state.add(*rows[i].value) : -1;
        if (restart_slot[i] >= 0) restart_names[restart_slot[i]] = rows[i].name;
    }
}
int restart_slot_of(const Row& r) { return restart_slot[&r - rows]; }
/// A row that takes a restart, changed from what the game started with (a row past menu_restart.h's kMax has no slot: never)
bool restart_changed(const Row& r) {
    const int slot = restart_slot_of(r);
    return r.value && r.restart && slot >= 0 && *r.value != restart_state.start[slot];
}
void restart_values(int* now) {
    for (int i = 0; i < kRows; i++)
        if (restart_slot[i] >= 0) now[restart_slot[i]] = *rows[i].value;
}
/// The rows whose value is not the one the game runs with (a bit a slot)
unsigned restart_pending() {
    int now[mr::kMax];
    restart_values(now);
    return restart_state.pending(now);
}
/// Whether closing the menu now asks to restart: something pending, not declined with Later, and a window to close
bool restart_ask() {
    int now[mr::kMax];
    restart_values(now);
    return app_window() && restart_state.ask(now);
}
std::string restart_list(const char* last) {
    return mr::names(restart_names, restart_state.n, restart_pending(), ", ", last);
}
int ask_sel = 0;   // (the restart question's choice: 0 Later, 1 Restart now)

/// A row on its tab: the debug tools only with them on, the DLSS model only in a build with DLSS
bool row_shown(int i) {
    // (the Intel shader compiles row: Intel graphics on Windows, the only place IntelDriverGate holds the compiler)
    if (rows[i].value == &opts.intel_compiles) {
#ifdef _WIN32
        if (dev.props.vendorID != 0x8086) return false;
#else
        return false;
#endif
    }
    const int* v = rows[i].value;
    return (rows[i].cheat < 0 || cheats_on()) &&
           (rows[i].value != &opts.dlss_model || ur::model_shown(dlss_built(), upscaler())) &&
           (rows[i].value != &opts.framegen_by || dlss_built() || fsr3fi::capable() || fsr4fg_offered()) &&   // (a build with DLSS, or a GPU AMD FSR 3.1's frame interpolation runs on: the row has a choice)
           (rows[i].value != &opts.dlss_pre_bloom || ur::pre_bloom_shown(dlss_why() == DLSS_OK, upscaler_mode())) &&
           // (HeyListen: the rows that only matter with another setting on show only then. Frame generation's sub-rows with it on)
           ((v != &opts.framegen_by && v != &opts.pace) || mrow::framegen_sub_shown(opts.framegen)) &&
           // (V-Sync mode with V-Sync On; greyed, with the hint, where RX_VK_FIFO or RX_VK_IMMEDIATE decide)
           (v != &opts.vsync_mode || mrow::vsync_mode_shown(opts.vsync, vsync_env_decides())) &&
           // (HDR paper white and peak with HDR on, where the display has it: hdr_now)
           ((v != &opts.hdr_white && v != &opts.hdr_peak) || mrow::hdr_levels_shown(hdr_now())) &&
           // (HeyListen, after Longshot ddb156df / 79a34381: the tone mapper with Native HDR, PsychoV35's rows with it on PsychoV35)
           (v != &opts.hdr_tonemap || mrow::tonemap_shown(hdr_now())) &&
           // (HeyListen, after Longshot a0dfd28b / 79a34381: HDR contrast with HDR on, and not with PsychoV35, which has its own)
           (v != &opts.hdr_contrast || mrow::contrast_shown(hdr_now(), opts.hdr_tonemap)) &&
           (strncmp(rows[i].name, "PsychoV35 ", 10) || mrow::psychov35_shown(hdr_now(), opts.hdr_tonemap));
}
int tab_row_count(int t) {
    int c = 0;
    for (int i = 0; i < kRows; i++) if (rows[i].tab == (MenuTab)t && row_shown(i)) c++;
    return c;
}

int tab_row_to_global(int t, int local_idx) {
    int count = 0;
    for (int i = 0; i < kRows; i++) {
        if (rows[i].tab == (MenuTab)t && row_shown(i)) {
            if (count == local_idx) return i;
            count++;
        }
    }
    return 0;
}

/// The cursor on global row g of tab t after rows were hidden or shown above or below it (a row is shown only while the setting it
/// depends on is on): the same row stays selected. A hidden row leaves the cursor where it was (it is clamped when drawn)
void reselect(int t, int g) {
    const int at = mrow::local_index(kRows, g, [&](int i) { return rows[i].tab == (MenuTab)t && row_shown(i); });
    if (at >= 0) tab_selected[t] = at;
}

std::atomic<bool> open_now{false};
bool menu_refresh = false;
// (a value too long for its row, a GPU's name: the selected row's slides through it, a character a step, resting at
// each end; the other rows end it with "...". The drawing (fit) says how far it can slide, menu_tick moves it)
int marquee_pos = 0, marquee_max = 0;
int64_t marquee_next = 0;
constexpr int64_t kMarqueeStep = 150000000LL, kMarqueeRest = 1200000000LL;   // (ns)
// (hookshot) the mouse (menu_tick): the tab strip's part under the cursor (a tab 0 to TAB_COUNT - 1, TAB_COUNT the previous,
// TAB_COUNT + 1 the next; -1 none); the slider held with the left button (its row in the tab, -1 none)
int hover_tab = -1;
int drag_row = -1;
bool mouse_read = false;   // (the menu has read the mouse since it opened)
bool is_slider(const Row& r) { return (!strncmp(r.name, "PsychoV35 ", 10) && r.hi > 2) || r.value == &opts.sharpness || r.value == &opts.mouse_sens || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y || r.value == &opts.volume; }
/// (a scale: the arrows and a click stop at its ends; Rumble strength would go from Off to its strongest)
bool stops_at_ends(const Row& r) { return (!strncmp(r.name, "PsychoV35 ", 10) && r.hi > 2) || r.value == &opts.mouse_sens || r.value == &opts.rumble || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y || r.value == &opts.volume; }
/// (the ini's Rumble strength on the menu's steps: 0 to 150, the nearest 25)
int rumble_step(int v) { return (std::min(std::max(v, 0), 150) + 12) / 25 * 25; }
std::string ini_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "hookshot_options.ini";
}
const char* ini_key(const Row& r) {
    if (r.value == &opts.output_res) return "output_res";   // (host/nn_applet.c reads it at the start)
    // (rows renamed since their values were first saved keep their keys: an options file from before still loads)
    if (r.value == &opts.resolution) return "resolution";
    if (r.value == &opts.upscale) return "upscaling";
    if (r.value == &opts.dlss) return "dlss";   // (as opts.dlss and RX_VK_DLSS)
    if (r.value == &opts.dlss_pre_bloom) return "dlss_pre_bloom";   // (as RX_VK_DLSS_PRE_BLOOM)
    if (r.value == &opts.send_perf) return "send_performance_stats";   // (host/hos_telemetry.h reads both of these)
    if (r.value == &opts.send_crash) return "send_crash_reports";
    if (r.value == &opts.vsync) return "vsync";
    if (r.value == &opts.vsync_mode) return "vsync_mode";
    if (r.value == &opts.vrr) return "vrr";
    if (r.value == &opts.update_check) return "update_check";   // (host/hos_update.c reads it, and writes it for "Never ask again")
    static char k[32];
    size_t n = 0;
    for (const char* p = r.name; *p && n + 1 < sizeof k; p++) k[n++] = *p == ' ' ? '_' : (char)tolower(*p);
    k[n] = 0;
    return k;
}
// (hookshot) the controls, kept by the host (host/nn_hid.c reads them back from here): pad_<button>, key_<button>
const char* const bind_names[HOS_KEY_N] = {"a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus", "minus",
                                           "left", "up", "right", "down", "move_up", "move_down", "move_left",
                                           "move_right", "cam_up", "cam_down", "cam_left", "cam_right"};
bool can_bind() { return host && host->binding && host->bind && host->pad_raw; }
const char* tx_pad_layout(int v) {
    static const char* t[] = {"Nintendo (Def)", "Xbox / PC", "Swap Face", "Custom"};
    return t[std::min(std::max(v, 0), 3)];
}
void apply_pad_layout(int layout) {
    if (!can_bind()) return;
    if (layout == 0) { // Nintendo (Default: Bottom=B, Right=A, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    } else if (layout == 1) { // Xbox / PC Standard (Bottom=A, Right=B, Left=X, Top=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_WEST);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_NORTH);
    } else if (layout == 2) { // Swap Face Buttons (Bottom=A, Right=B, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    }
}
int detect_pad_layout() {
    if (!can_bind()) return opts.pad_layout;
    const int a = host->binding(HOS_BIND_PAD, HOS_SW_A, 0);
    const int b = host->binding(HOS_BIND_PAD, HOS_SW_B, 0);
    const int x = host->binding(HOS_BIND_PAD, HOS_SW_X, 0);
    const int y = host->binding(HOS_BIND_PAD, HOS_SW_Y, 0);
    if (a == HOS_PAD_EAST && b == HOS_PAD_SOUTH && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 0;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_WEST && y == HOS_PAD_NORTH) return 1;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 2;
    return 3;
}
// (hookshot) the row's choice to the host: off, the default, or a microphone by its name (and its place in the host's list,
// -1 while it is unplugged: of two with one name, the one chosen). Unplugged, the name is the kept one, cut: the host takes
// a name that starts with it
void mic_tell_host() {
    if (!can_mic()) return;
    if (opts.mic <= 1) host->mic_select(opts.mic <= 0 ? 0 : 1, "", -1);
    else if (mic_missing && opts.mic == 2 + mic_listed) host->mic_select(2, mic_want.c_str(), -1);   // (the kept name, cut)
    else host->mic_select(2, host->mic_name(opts.mic - 2), opts.mic - 2);   // (its whole name: the ini's cut is only the ini's)
}
void mic_row_ends() {
    for (Row& r : rows)
        if (r.value == &opts.mic) r.hi = 1 + mic_listed + (mic_missing ? 1 : 0);
}
// (hookshot) the microphones listed again (one plugged in since shows up), the row on mic_want's: the one at the row's place
// when it still has that name (two microphones of one name: the second stays chosen), else the first of that name, else
// its "(not connected)" entry while it is unplugged
void refresh_mics() {
    if (!can_mic()) {
        if (opts.mic >= 2) opts.mic = 1;
        return;
    }
    mic_listed = std::min(host->mic_count(), kMaxMics);
    int at = -1;
    if (!mic_want.empty()) {
        if (opts.mic >= 2 && opts.mic - 2 < mic_listed && mic_key(host->mic_name(opts.mic - 2)) == mic_want) at = opts.mic - 2;
        for (int i = 0; i < mic_listed && at < 0; i++)
            if (mic_key(host->mic_name(i)) == mic_want) at = i;
        opts.mic = at >= 0 ? at + 2 : 2 + mic_listed;
    } else if (opts.mic >= 2) {
        opts.mic = 1;
    }
    mic_missing = !mic_want.empty() && at < 0;
    mic_row_ends();
    mic_tell_host();
}
// (the row changed by the player: the choice is now this one, by its name; the "(not connected)" entry is kept only while
// it is the one chosen)
void mic_chosen() {
    if (opts.mic >= 2 && !(mic_missing && opts.mic == 2 + mic_listed)) {
        mic_want = mic_key(host->mic_name(opts.mic - 2)), mic_missing = false;
    } else if (opts.mic < 2) {
        mic_want.clear(), mic_missing = false;
    }
    mic_row_ends();
    mic_tell_host();
}
// (heylisten) the Output device row's choice to the host: Default, or the output by its id (the kept one while it is not listed)
void out_tell_host() {
    if (!can_out()) return;
    if (opts.out_dev <= 0) host->out_select(0, "", "");
    else if (out_missing && opts.out_dev == 1 + out_listed) host->out_select(1, out_want_id.c_str(), out_want_name.c_str());
    else host->out_select(1, host->out_id(opts.out_dev - 1), host->out_name(opts.out_dev - 1));
}
void out_row_ends() {
    for (Row& r : rows)
        if (r.value == &opts.out_dev) r.hi = out_listed + (out_missing ? 1 : 0);
}
// (heylisten) the outputs listed again (one plugged in since shows up), the row on out_want_id's: by its id, never by its place,
// else its "(not connected)" entry. The host is told again: a device that was given up for is tried again
void refresh_outs() {
    out_listed = 0, out_missing = false;
    if (!can_out()) {
        opts.out_dev = 0;
        out_row_ends();
        return;
    }
    const int found = host->out_count();
    if (found < 0) {   // (no list here: Default only; the kept choice stays in the ini, as another system's)
        opts.out_dev = 0;
        out_row_ends();
        return;
    }
    out_listed = std::min(found, kMaxOuts);
    int at = -1;
    if (!out_want_id.empty()) {
        for (int i = 0; i < out_listed && at < 0; i++)
            if (mic_key(host->out_id(i)) == out_want_id) at = i;
        opts.out_dev = at >= 0 ? at + 1 : 1 + out_listed;
        if (at >= 0) out_want_name = mic_key(host->out_name(at));   // (a renamed device shows, and is kept, under its new name)
    } else {
        opts.out_dev = 0;
    }
    out_missing = !out_want_id.empty() && at < 0;
    out_row_ends();
    out_tell_host();
}
// (the row changed by the player: the choice is this one, by its id)
void out_chosen() {
    if (opts.out_dev >= 1 && !(out_missing && opts.out_dev == 1 + out_listed)) {
        out_want_id = mic_key(host->out_id(opts.out_dev - 1)), out_want_name = mic_key(host->out_name(opts.out_dev - 1));
        out_missing = false;
    } else if (opts.out_dev < 1) {
        out_want_id.clear(), out_want_name.clear(), out_missing = false;
    }
    out_row_ends();
    out_tell_host();
}
// (HeyListen) the options file's lowend_rule line: the low-end rule was applied once (menu_first_run_lowend), never again: the
// player's choices stay theirs. Kept by save()
void save() {
    // (hookshot) the lines that are not the menu's are kept: the controls, pad_ and key_ (host/nn_hid.c)
    // (and every other line the menu does not write, such as discord_app_id=: they were dropped at the next save)
    std::string kept, others;
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4)) {
                kept += line;
                continue;
            }
            const char* eq = strchr(line, '=');
            if (!eq || eq == line) continue;
            const std::string key(line, (size_t)(eq - line));
            // (lowend_settings and lowend_prev_*: the Low-end settings row's lines of a build of 25 Nov, dropped here)
            bool mine = key == "microphone_device" || key == "output_device_id" || key == "output_device_name" || key == "options_version" || key == "lowend_rule" || key == "lowend_settings" ||
                        !key.compare(0, strlen(lowend_rule::kPrevPrefix), lowend_rule::kPrevPrefix);
            for (const Row& r : rows)
                if (!mine && r.value && key == ini_key(r)) mine = true;
            if (!mine) others += line, others += others.back() == '\n' ? "" : "\n";
        }
        fclose(f);
    }
    // (longshot #976: written whole or not at all, beside the file and moved over it: a crash or a kill in the write left the
    // options file empty or cut short, and the next start took it for the player's settings)
    std::string out;
    const auto put = [&out](const char* fmt, auto... a) {
        char b[600];
        const int n = snprintf(b, sizeof b, fmt, a...);
        if (n > 0) out.append(b, std::min<size_t>((size_t)n, sizeof b - 1));
    };
    {
        for (const Row& r : rows)
            if (r.value) put("%s=%d\n", ini_key(r), *r.value);
        if (!mic_want.empty()) put("microphone_device=%s\n", mic_want.c_str());   // (hookshot: also while it is unplugged)
        if (!out_want_id.empty())   // (heylisten: the same)
            put("output_device_id=%s\noutput_device_name=%s\n", out_want_id.c_str(), out_want_name.c_str());
        // (2: game_speed below 2 was only ever the old default, see menu_init; 3: the mouse's buttons are bindings, see
        // host/nn_hid.c binds_load)
        // (4: the Quality preset has Lowest at 1, so Minimum to Ultra are 2 to 6: menu_init moves a saved one)
        put("options_version=4\n");
        if (lowend_marked) put("lowend_rule=1\n");   // (menu_first_run_lowend: the first start did it once)
        out += others;
        if (!can_bind()) out += kept;
        for (int i = 0; i < HOS_KEY_N && can_bind(); i++) {   // (the keys-only controls, HOS_KEY_*, have no pad_ line)
            if (i < HOS_SW_N) put("pad_%s=%d\n", bind_names[i], host->binding(HOS_BIND_PAD, i, 0));
            put("key_%s=%d,%d\n", bind_names[i], host->binding(HOS_BIND_KEY, i, 0), host->binding(HOS_BIND_KEY, i, 1));
        }
        // (the Open menu control: one or two gamepad inputs, -1 none; host/nn_hid.c binds_load reads it)
        if (can_bind()) put("pad_menu=%d,%d\n", host->binding(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 0), host->binding(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 1));
    }
    hos_atomic_write(ini_path().c_str(), out.data(), out.size());
}

/// A row's new value v, as the arrows set it (dir: 1 up, -1 down, for the rows that skip or wrap), saved unless !keep
/// (a slider's drag: saved once it is let go)
void set_row(Row& r, int v, int dir, bool keep = true) {
    // (nothing else to choose: no change, no save. A saved choice that cannot run here is kept)
    if (row_fixed(r.value)) return;
    // (the scaling rows: only what can run here; the Render resolution row through the Upscaler's own steps)
    if (r.value == &opts.dlss) v = ur::step_upscaler(upscaler(), dir, upscalers_available());
    if (r.value == &opts.resolution) v = ur::step(upscaler(), ur::snap(upscaler(), *r.value), dir);   // (from the step the row shows)
    if (r.value == &opts.output_res) {   // (smallest to largest: kOutputOrder)
        v = step_output_choice(*r.value, dir);
        if (v == *r.value) return;
    }
    // (Frame generation: the arrows step over what the mode that makes the frames offers (framegen_rows.h): Off, Auto where it has
    // one, 2x to its top (NVIDIA DLSS FG: its runtime's most + 1, AMD FSR 3.1: 2x). A saved value past it shows as the top
    // (tx_framegen), so an arrow steps from the value shown, up past the top to Off and down from Off to the top. A value set as
    // it is (not an arrow's step from the saved one) is kept as the mode makes it)
    if (r.value == &opts.framegen)
        v = dir && v == *r.value + dir * r.step ? fgr::step(framegen_by_now(), dlss_fg_most(), *r.value, dir)
                                                : fgr::clamp(framegen_by_now(), dlss_fg_most(), v);
    // (Frame generation by: NVIDIA DLSS FG is not a stop where this build has none: the row is there for AMD FSR 3.1's)
    if (r.value == &opts.framegen_by) {
        v = (v % fgr::kBy + fgr::kBy) % fgr::kBy;
        // (a stop this PC cannot make is stepped over: NVIDIA DLSS FG without it in the build, AMD FSR 4 where AMD's runtime does not offer it)
        if ((v == fgr::BY_DLSS && !dlss_built()) || (v == fgr::BY_FSR4 && !fsr4fg_offered())) v = fgr::step_by(v, dir, dlss_built(), fsr4fg_offered());
    }
    if (stops_at_ends(r)) v = std::min(std::max(v, r.lo), r.hi);   // (a scale: it stops at its ends)
    if (r.value == &opts.pad_layout) {
        if (v > 2) v = dir > 0 ? 0 : 2;
        if (v < 0) v = 2;
        *r.value = v;
        apply_pad_layout(*r.value);
    } else if (r.value == &opts.preset) {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        apply_preset(*r.value);
    } else {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        // (another Upscaler: the saved Render resolution stays, and the nearest of the new Upscaler's steps is what draws and shows,
        // so Balanced 58% comes back on return from an Upscaler that has no such step: resolution_scale, tx_resolution)
        if (!r.preset_exempt) {   // (the controls, the window and the counter are not the preset's)
            opts.preset = 0; // Custom
            sync_preset_budgets(0);
        }
    }
    // (another mode makes the frames: the saved multiplier is kept within what it makes, never raised: DLSS FG at 8x on an RTX 50
    // becomes 6x, AMD FSR 3.1 2x, Auto with either 2x)
    if (r.value == &opts.framegen_by) opts.framegen = fgr::on_mode_change(framegen_by_now(), dlss_fg_most(), opts.framegen);
    if (r.value == &opts.mic && can_mic()) mic_chosen();   // (hookshot)
    if (r.value == &opts.preset || r.value == &opts.resolution || r.value == &opts.handheld || r.value == &opts.fog || r.value == &opts.upscale ||
        r.value == &opts.sharpness || r.value == &opts.pace || r.value == &opts.realtime)
        lowend_touched = true;   // (the measure leaves the player's choices alone)
    if (r.value == &opts.out_dev && can_out()) out_chosen();   // (heylisten)
    {   // (a row that takes a restart, or a preset that sets one: asked again at the next close)
        int now[mr::kMax];
        restart_values(now);
        restart_state.note(now);
    }
    if (keep) save();
}
/// set_row from the menu's input: the cursor stays on its row when the change hides or shows the rows above it (a row that depends
/// on the one changed: the tone mapper under HDR, Frame generation by under Frame generation)
void change_row(Row& r, int v, int dir, bool keep = true) {
    const int cursor_row = r.tab == cur_tab ? tab_row_to_global(cur_tab, tab_selected[cur_tab]) : -1;
    set_row(r, v, dir, keep);
    if (cursor_row >= 0) reselect(cur_tab, cursor_row);
}

// ---- (hookshot) the Controls page: what presses each of the Switch's buttons, a gamepad input and up to two keys ----
struct Control {
    int button;   // HOS_SW_*, or HOS_KEY_* (keys only)
    const char* name;
};
const Control controls[] = {{HOS_SW_A, "A"}, {HOS_SW_B, "B"}, {HOS_SW_X, "X"}, {HOS_SW_Y, "Y"}, {HOS_SW_L, "L"},
                            {HOS_SW_R, "R"}, {HOS_SW_ZL, "ZL"}, {HOS_SW_ZR, "ZR"}, {HOS_SW_PLUS, "Plus"},
                            {HOS_SW_MINUS, "Minus"}, {HOS_SW_LS, "Left stick press"}, {HOS_SW_RS, "Right stick press"},
                            {HOS_SW_UP, "D-pad up"}, {HOS_SW_DOWN, "D-pad down"}, {HOS_SW_LEFT, "D-pad left"},
                            {HOS_SW_RIGHT, "D-pad right"}, {HOS_KEY_MOVE_UP, "Move forward"},
                            {HOS_KEY_MOVE_DOWN, "Move back"}, {HOS_KEY_MOVE_LEFT, "Move left"},
                            {HOS_KEY_MOVE_RIGHT, "Move right"}, {HOS_KEY_CAM_UP, "Camera up"},
                            {HOS_KEY_CAM_DOWN, "Camera down"}, {HOS_KEY_CAM_LEFT, "Camera left"},
                            {HOS_KEY_CAM_RIGHT, "Camera right"},
                            {HOS_BTN_OPEN_MENU, "Open menu"}};   // (a gamepad input or two together, not a Switch button)
constexpr int kControls = sizeof controls / sizeof controls[0];   // (then "Reset to defaults")
bool menu_row(int i) { return i >= 0 && i < kControls && controls[i].button == HOS_BTN_OPEN_MENU; }   // (no keys for it)
bool keys_only(int i) {   // (no gamepad input for those)
    return i >= 0 && i < kControls && controls[i].button >= HOS_SW_N && controls[i].button != HOS_BTN_OPEN_MENU;
}
// the Open menu control's inputs by name ("Share + Options"), "Not set" while it has none (Home, Select and Start together and
// the touchpad's click open the menu in any case)
std::string menu_combo_text();
// a pad input's name on the controller of the button prompts (the Button prompts row's choice; Auto: the face in use),
// else by its place
const char* pad_name(int p) {
    static const char* t[HOS_PROMPT_KEYBOARD + 1][HOS_PAD_N] = {   // (by place, then HOS_PROMPT_*; the keyboard's: by place)
        {"Bottom button", "Right button", "Left button", "Top button", "Select / Back", "Home", "Start", "Left stick press",
         "Right stick press", "L1 / LB", "R1 / RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "L2 / LT", "R2 / RT"},
        {"B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "ZL", "ZR"},
        {"A", "B", "X", "Y", "View", "Xbox", "Menu", "Left stick press", "Right stick press", "LB", "RB", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "LT", "RT"},
        {"Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down", "D-pad left",
         "D-pad right", "L2", "R2"}};
    const int face = opts.prompts ? opts.prompts - 1 : host && host->prompt_face ? host->prompt_face() : -1;
    return p >= 0 && p < HOS_PAD_N ? t[face >= 0 && face < HOS_PROMPT_KEYBOARD ? face + 1 : 0][p] : "None";
}
std::string menu_combo_text() {
    const int a = host ? host->binding(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 0) : -1;
    const int b = host ? host->binding(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 1) : -1;
    if (a < 0) return "Not set";
    return b < 0 ? std::string(pad_name(a)) : std::string(pad_name(a)) + " + " + pad_name(b);
}
const char* key_name(int vk) {
    static char b[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return b[0] = (char)vk, b[1] = 0, b;
    if (vk >= VK_F1 && vk <= VK_F12) return snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1), b;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return snprintf(b, sizeof b, "Num %d", vk - VK_NUMPAD0), b;
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_CAPITAL: return "Caps Lock";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_MULTIPLY: return "Num *";
    case VK_DIVIDE: return "Num /";
    case VK_DECIMAL: return "Num .";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    case VK_LBUTTON: return "Left click";
    case VK_RBUTTON: return "Right click";
    case VK_MBUTTON: return "Middle click";
    case 0x05: return "Mouse 4";   // (VK_XBUTTON1)
    case 0x06: return "Mouse 5";
    }
    return snprintf(b, sizeof b, "Key %d", vk), b;
}
// the keys a binding can take (Esc cancels, Insert and F1 are the menu's, Delete clears)
const std::vector<int>& bindable_keys() {
    static std::vector<int> k;
    if (k.empty()) {
        for (int c = 'A'; c <= 'Z'; c++) k.push_back(c);
        for (int c = '0'; c <= '9'; c++) k.push_back(c);
        for (int c = VK_F2; c <= VK_F12; c++) k.push_back(c);
        for (int c = VK_NUMPAD0; c <= VK_NUMPAD0 + 9; c++) k.push_back(c);
        for (int c : {VK_SPACE, VK_SHIFT, VK_CONTROL, VK_MENU, VK_TAB, VK_RETURN, VK_BACK, VK_CAPITAL, VK_UP, VK_DOWN,
                      VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_ADD, VK_SUBTRACT, VK_MULTIPLY,
                      VK_DIVIDE, VK_DECIMAL, VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
                      VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                      0x05, 0x06})   // (the mouse's buttons too: they count while the mouse is captured)
            k.push_back(c);
    }
    return k;
}
int page = 0;          // 0 the options, 1 the controls, 3 the teleport list, 4 the inventory, 5 the restart question (2: was the
                       // debug tools' own page, now on the Debug tab)
int csel = 0, ccol = 0;   // the page's row (kTop first: its own rows, then the controls, then reset) and column (0 the
                          // gamepad, 1 the keyboard)
constexpr int kTop = 2;   // (Layout preset and Button prompts, rows of the Controls tab that the page shows above its grid too: a preset shows in the grid at once)
/// The global row of the page's own row j (0 the layout preset, 1 the button prompts)
int page_row_global(int j) {
    const int* want = j == 0 ? &opts.pad_layout : &opts.prompts;
    for (int i = 0; i < kRows; i++) if (rows[i].value == want) return i;
    return 0;
}
int listening = 0;     // waiting for the new input: 1 a gamepad's, 2 a key
bool listen_armed = false;   // (once what started it was let go)
int64_t listen_end = 0;
std::string keys_text(int button) {
    const int a = host->binding(HOS_BIND_KEY, button, 0), b = host->binding(HOS_BIND_KEY, button, 1);
    std::string t = a ? key_name(a) : "";
    if (b) t += std::string(a ? ", " : "") + key_name(b);
    return t.empty() ? "None" : t;
}

// ---- what is drawn: Dear ImGui over the window's image (vk_imgui.h) in the game's own style (vk_ui.h). Built anew at each
// present while something shows, from the menu's state (menu_tick and menu_record run on the presenting thread, one at a time:
// present_image's lock); nothing at all while nothing shows ----
std::mutex startup_mu;   // (the start-up screen's view, set from its own thread)
StartupView startup;
std::atomic<bool> startup_on{false};
std::atomic<int64_t> game_frames{0}, shown_frames{0};
char fps_text[48] = "";
char fps_sub[32] = "";
menu_hit::Map hits;      // (the menu's controls as last drawn: the mouse's, menu_tick)
float scroll_at[TAB_MOTION + 2];   // (the first row shown, eased toward the selection: the options' tabs and the Motion page, then
                                   // the Controls page: kControlsSlot)
constexpr int kControlsSlot = TAB_MOTION + 1;
int wheel_rows[TAB_MOTION + 2];   // (the mouse wheel's notches since the last drawing, a slot each: scroll_rows scrolls by them)
int scrolled_to[TAB_MOTION + 2] = {-1, -1, -1, -1, -1, -1};   // (the selection scroll_rows last brought into view)
float scroll_want[TAB_MOTION + 2];   // (where scroll_at is going: the wheel moves it, a new selection too)
float ease_step = 0;     // (this drawing's share of the way: the time since the last one)

ui::Ctx ui_ctx(uint32_t w, uint32_t h) {
    ui::Ctx c{ImGui::GetForegroundDrawList(), ui::fonts(), 1, (float)w, (float)h, 0, pad_last};
    c.confirm_dot = menu_keys::dot(pad_confirm), c.back_dot = menu_keys::dot(pad_back);
    c.s = ui::unit((float)h, opts.menu_size, opts.fullscreen != 0, auto_pad, &c.safe);
    return c;
}
/// The banner's height
float banner_h(const ui::Ctx& c) { return std::max(58 * c.s, c.safe + 40 * c.s); }

/// The tallest help a row of the options can have, in pixels at width: its help and the tallest line any of its values can
/// have: the help box keeps that height whichever row is selected and whatever its value
float measure_help(ui::Ctx& c, float width) {
    float most = 0;
    for (const Row& r : rows) {
        float hint = 0;
        for (int v = r.lo; r.value && r.hint && v <= r.hi; v++)
            for (int k = 0; const char* t = r.hint(v, k); k++)
                hint = std::max(hint, ui::para(c, c.f.body, 16 * c.s, ImVec2(), 0, t, width - 18 * c.s, false));
        if (r.help) most = std::max(most, ui::para(c, c.f.body, 18 * c.s, ImVec2(), 0, r.help, width, false) + (hint ? 14 * c.s + hint : 0));
    }
    return most;
}
/// label in room pixels: as it is when it fits; the selected row's slides through it (marquee_pos, marquee_max for menu_tick);
/// another's ends with "..."
/// (UTF-8: the byte offsets where its characters start, and the end; labels are cut and slid a character at a time, never
/// inside one: a microphone's name can be in any script)
std::vector<size_t> char_starts(const std::string& t) {
    std::vector<size_t> at;
    for (size_t i = 0; i < t.size(); i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) at.push_back(i);
    at.push_back(t.size());
    return at;
}
std::string fit(ui::Ctx& c, ImFont* f, float px, float room, const std::string& label, bool sel) {
    auto width = [&](const std::string& t) { return ui::size_of(f, px, t.c_str()).x; };
    if (width(label) <= room) return label;
    const std::vector<size_t> at = char_starts(label);
    const int chars = (int)at.size() - 1;
    if (sel) {   // (marquee_pos and marquee_max count characters)
        int start = chars;   // (the last start that still shows the label's end)
        while (start > 0 && width(label.substr(at[start - 1])) <= room) start--;
        marquee_max = start;
        const int from = std::min(marquee_pos, marquee_max);
        int end = chars;
        while (end > from + 1 && width(label.substr(at[from], at[end] - at[from])) > room) end--;
        return label.substr(at[from], at[end] - at[from]);
    }
    int end = chars;
    while (end > 1 && width(label.substr(0, at[end]) + "...") > room) end--;
    std::string t = label.substr(0, at[end]);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + "...";
}
/// A list of rows from top to bottom (pixels), n of them pitch apart, the selection sel kept in view: the first one shown,
/// eased toward its place (scroll_at[slot]); *shown how many fit
float scroll_rows(int slot, int n, int sel, float top, float bottom, float pitch, int* shown) {
    const int fit = std::max(1, (int)((bottom - top) / pitch));
    *shown = fit;
    float& at = scroll_at[slot];
    float& want = scroll_want[slot];
    const int wheel = std::exchange(wheel_rows[slot], 0);
    if (n <= fit) want = 0;   // (nothing to scroll: the wheel does nothing)
    else {
        want -= (float)wheel;   // (the wheel scrolls the rows and leaves the selection, and every value, as they are)
        static int fits[sizeof scroll_at / sizeof *scroll_at];   // (and after a resize: the rows that fit changed)
        if (sel != scrolled_to[slot] || fit != std::exchange(fits[slot], fit)) {   // (a new selection, by the keys or the pad: brought into view)
            const int margin = fit > 4 ? 1 : 0;   // (a row of context above and below the selection)
            if (sel - margin < want) want = (float)(sel - margin);
            if (sel + margin + 1 > want + fit) want = (float)(sel + margin + 1 - fit);
        }
        want = std::round(std::clamp(want, 0.0f, (float)(n - fit)));
    }
    scrolled_to[slot] = sel;
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.01f) at = want;
    return at;
}
/// The scroll bar at x, beside a list of n rows of which shown fit, the first at `first`
void scroll_bar(ui::Ctx& c, float x, float top, float bottom, int n, int shown, float first) {
    if (n <= shown) return;
    const float s = c.s, span = bottom - top, y0 = top + span * first / n, y1 = top + span * (first + shown) / n;
    c.d->AddRectFilled(ImVec2(x - 2 * s, top), ImVec2(x + 2 * s, bottom), ui::rgba(0, 0, 0, 140), 2 * s);
    c.d->AddRectFilled(ImVec2(x - 2 * s, y0), ImVec2(x + 2 * s, y1), ui::kAccent, 2 * s);
}
/// The banner with a page's title in the middle (the Controls, Cheats and list pages), its height
float title_banner(ui::Ctx& c, const char* title, const char* right) {
    const float hh = banner_h(c), s = c.s;
    ui::banner(c, hh);
    ui::text_mid_centre(c, c.f.heading, 24 * s, c.w * 0.5f, hh * 0.5f + s, ui::kBannerInk, title);
    if (right) ui::text_mid_right(c, c.f.body, 17 * s, c.w - c.safe, hh * 0.5f + s, ui::kBannerInk, right);
    return hh;
}
/// The help box from (x0, y0) to x1: a title, the help, a line set off by a bar, a gold line at its foot. min_h: its height at
/// least (the tallest help the page can have, so it does not jump)
void help_box(ui::Ctx& c, float x0, float x1, float y0, const char* title, const char* help, const char* hint, const char* gold_line,
              float min_h = 0, bool after_restart = false, float y_min = 0, float y_max = FLT_MAX) {
    // (y_max: the box ends above it, the prompt strip's top: moved up as far as y_min (under the banner) when it is taller than
    // the room below y0, its reserved height (min_h) given up first, its text cut at its foot only if it still does not fit)
    const float s = c.s, w = x1 - x0 - 48 * s;
    // (return early if the box bounds collapsed: avoids zero-width or zero-height scissor clip rects)
    if (x1 <= x0 + 4 * s || y_max <= y_min + 4 * s) return;
    const float th = help ? ui::para(c, c.f.body, 18 * s, ImVec2(), 0, help, w, false) : 0,
                hh = hint ? ui::para(c, c.f.body, 16 * s, ImVec2(), 0, hint, w - 18 * s, false) : 0;
    const float text_h = th + (hint ? 14 * s + hh : 0), note = after_restart ? 30 * s : 0;
    const float fixed = 22 * s + (title ? 32 * s : 0) + note + (gold_line ? 52 * s : 0) + 22 * s;
    float body = std::max(min_h, text_h);
    if (y0 + fixed + body > y_max) y0 = std::max(y_min, y_max - fixed - body);
    if (y0 + fixed + body > y_max) body = std::max(text_h, y_max - y0 - fixed);
    y0 = std::min(y0, y_max);
    const float y1 = std::max(y0, std::min(y0 + fixed + body, y_max));
    // (never push an inverted or collapsed clip rect: Min.y must be strictly less than Max.y for Vulkan scissors)
    if (y1 <= y0) return;
    ui::box(c, ImVec2(x0, y0), ImVec2(x1, y1));
    c.d->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    float y = y0 + 22 * s;
    if (title) ui::text(c, c.f.heading, 21 * s, ImVec2(x0 + 24 * s, y), ui::kInk, title), y += 32 * s;
    if (after_restart) {   // (this row's change applies at the next start: said here, not in the row, where it would cut the name)
        ui::diamond(c, ImVec2(x0 + 29 * s, y + 10 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, 16 * s, x0 + 42 * s, y + 10 * s, ui::kGoldHi, "Takes effect after a restart");
        y += note;
    }
    if (help) ui::para(c, c.f.body, 18 * s, ImVec2(x0 + 24 * s, y), ui::kInk, help, w);
    if (hint) {
        const float hy = y + th + 14 * s;
        c.d->AddRectFilled(ImVec2(x0 + 24 * s, hy + 2 * s), ImVec2(x0 + 26.5f * s, hy + hh - 2 * s), ui::kAccent, s);
        ui::para(c, c.f.body, 16 * s, ImVec2(x0 + 40 * s, hy), ui::kSoft, hint, w - 18 * s);
    }
    if (gold_line) {
        y += body + 20 * s;
        c.d->AddLine(ImVec2(x0 + 24 * s, y), ImVec2(x1 - 24 * s, y), ui::alpha(ui::kEdge, 0.8f), std::max(1.0f, s));
        ui::diamond(c, ImVec2(x0 + 29 * s, y + 20 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, 16 * s, x0 + 42 * s, y + 20 * s, ui::kGoldHi, gold_line);
    }
    c.d->PopClipRect();
}
/// The menu's column: from the safe margin to about 58% of the width (wide enough for a row at any unit); the help box beside it,
/// or, on a narrow window, over the column's foot (*narrow)
void columns(const ui::Ctx& c, float* x0, float* x1, float* hx0, float* hx1, bool* narrow) {
    const float s = c.s;
    *x0 = c.safe + 6 * s;
    *x1 = std::min(c.w - c.safe, std::max(c.w * 0.585f, *x0 + 560 * s));
    *hx0 = *x1 + 28 * s, *hx1 = c.w - c.safe;
    *narrow = *hx1 - *hx0 < 260 * s;
    if (*narrow) *hx0 = *x0 + 20 * s, *hx1 = *x1 - 20 * s;
}
void column(ui::Ctx& c, float x0, float x1, float top) {
    const float s = c.s;
    c.d->AddRectFilled(ImVec2(x0, top), ImVec2(x1, c.h), ui::kColumn);
    c.d->AddLine(ImVec2(x0, top), ImVec2(x0, c.h), ui::alpha(ui::kEdge, 1), 2 * s);
    c.d->AddLine(ImVec2(x1, top), ImVec2(x1, c.h), ui::alpha(ui::kEdge, 1), 2 * s);
}
/// A row's name cell, selected or not, its tag (After restart) at its right end
/// A row's name cell, selected or not; pending: a change that applies after a restart, a small gold diamond at its right end
/// (the help says it in words)
void name_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* name, bool sel, bool pending = false, float px = 18.5f, bool dim = false) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f;
    c.d->AddRectFilled(a, b, sel ? ui::kCellNameSel : ui::kCellName);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    if (sel) c.d->AddRectFilled(a, ImVec2(a.x + 4 * s, b.y), ui::kGoldHi);
    float room = b.x - a.x - 36 * s;
    if (pending) {
        ui::diamond(c, ImVec2(b.x - 12 * s, mid), 4 * s, ui::kGoldHi);
        room -= 14 * s;
    }
    ImFont* f = sel ? c.f.strong : c.f.body;
    const std::string shown = fit(c, f, px * s, room, name, false);
    ui::text_mid(c, f, px * s, a.x + 20 * s, mid, dim ? ui::kDim : sel ? ui::kInkBright : ui::kInk, shown.c_str());
}
/// A value cell: a choice with arrows at its ends (left, right: whether it can go that way), gold when selected
void choice_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const std::string& value, bool sel, bool left, bool right, bool fixed = false) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s;
    c.d->AddRectFilled(a, b, ui::kCellDark);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    if (sel && !fixed) ui::gold(c, a + ImVec2(in, in), b - ImVec2(in, in));
    const ImU32 ink = sel && !fixed ? ui::kGoldInk : fixed ? ui::kDim : ui::kInk, arrow = sel && !fixed ? ui::kGoldInk : ui::kDim;
    if (left) ui::triangle(c, ImVec2(a.x + in + 11 * s, mid), 9 * s, 0, arrow);
    if (right) ui::triangle(c, ImVec2(b.x - in - 11 * s, mid), 9 * s, 1, arrow);
    ImFont* f = sel ? c.f.strong : c.f.body;
    const std::string shown = fit(c, f, px, b.x - a.x - 2 * in - (left || right ? 48 : 16) * s, value, sel);   // (room for arrows)
    ui::text_mid_centre(c, f, px, (a.x + b.x) * 0.5f, mid, ink, shown.c_str());
    if (sel) ui::glow(c, a, b);
}
/// A value cell of two halves (a row of two values: On and Off), the chosen one filled
void halves_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* lo, const char* hi, bool high, bool sel, bool fixed) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s, xm = (a.x + b.x) * 0.5f;
    c.d->AddRectFilled(a, b, ui::kCellDark);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    const ImVec2 fa = high ? ImVec2(xm, a.y + in) : a + ImVec2(in, in), fb = high ? b - ImVec2(in, in) : ImVec2(xm, b.y - in);
    if (sel && !fixed) ui::gold(c, fa, fb);
    else c.d->AddRectFilled(fa, fb, ui::kBronze);
    const ImU32 on = sel && !fixed ? ui::kGoldInk : ui::kInk;
    ui::text_mid_centre(c, high ? c.f.body : c.f.strong, px, (a.x + xm) * 0.5f, mid, high ? ui::kDim : on, lo);
    ui::text_mid_centre(c, high ? c.f.strong : c.f.body, px, (xm + b.x) * 0.5f, mid, high ? on : ui::kDim, hi);
    if (sel) ui::glow(c, a, b);
}
/// A slider's value cell: the value, a track with its knob along the cell's foot; *t0, *t1 the track (for the mouse)
void slider_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* value, int permille, bool sel, float* t0, float* t1) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s;
    c.d->AddRectFilled(a, b, ui::kCellDark);
    c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
    if (sel) ui::gold(c, a + ImVec2(in, in), b - ImVec2(in, in));
    const float x0 = a.x + in + 24 * s, x1 = b.x - in - 24 * s, xv = x0 + (x1 - x0) * permille / 1000.0f, ty = b.y - in - 5.5f * s;
    const ImU32 track = sel ? ui::rgba(36, 24, 8, 80) : ui::rgba(255, 255, 255, 40), done = sel ? ui::kGoldInk : ui::kAccent;
    c.d->AddRectFilled(ImVec2(x0, ty - 1.5f * s), ImVec2(x1, ty + 1.5f * s), track, 1.5f * s);
    c.d->AddRectFilled(ImVec2(x0, ty - 1.5f * s), ImVec2(xv, ty + 1.5f * s), done, 1.5f * s);
    ui::diamond(c, ImVec2(xv, ty), 4.5f * s, done);
    ui::text_mid_centre(c, c.f.strong, px, (a.x + b.x) * 0.5f, mid - 3 * s, sel ? ui::kGoldInk : ui::kInk, value);
    const ImU32 arrow = sel ? ui::kGoldInk : ui::kDim;
    ui::triangle(c, ImVec2(a.x + in + 11 * s, mid), 9 * s, 0, arrow), ui::triangle(c, ImVec2(b.x - in - 11 * s, mid), 9 * s, 1, arrow);
    if (sel) ui::glow(c, a, b);
    *t0 = x0, *t1 = x1;
}
/// "Restart needed: " and the pending rows, on one line of width: the last ones as "and N more" when they do not fit
std::string restart_line(ui::Ctx& c, float px, float width) {
    const unsigned all = restart_pending();
    std::string line;
    for (int keep = restart_state.n; keep >= 0; keep--) {   // (the first `keep` pending rows by name)
        unsigned shown = 0;
        int named = 0, more = 0;
        for (int i = 0; i < restart_state.n; i++)
            if ((all >> i) & 1) (named < keep ? (shown |= 1u << i, named++) : more++);
        line = "Restart needed: " + mr::names(restart_names, restart_state.n, shown);
        if (more) line += (named ? " and " : "") + std::to_string(more) + (named ? " more" : more > 1 ? " settings" : " setting");
        if (ui::size_of(c.f.strong, px, line.c_str()).x <= width) break;
    }
    return line;
}

void draw_options(ui::Ctx& c) {
    const float s = c.s, hh = banner_h(c);
    ui::banner(c, hh);
    // the tabs in the banner: dark ink on the tan, the shown one light on a dark inset; the tab keys at their ends (a click there
    // is the previous or the next tab)
    const char* const tab_titles[TAB_COUNT] = {"Graphics", "Gameplay", "Controls", "Debug"};
    float tw[TAB_COUNT], total = 0;
    for (int i = 0; i < TAB_COUNT; i++) tw[i] = ui::size_of(c.f.heading, 23 * s, tab_titles[i]).x + 56 * s, total += tw[i];
    float x = (c.w - total) * 0.5f;
    const float ty = hh * 0.5f + s;
    const char* const keys[2] = {c.pad ? "L" : "Q", c.pad ? "R" : "E"};
    for (int i = 0; i < 2; i++) {
        const float cw = 26 * s, cx = i ? x + total + 22 * s : x - 22 * s - cw;
        const bool hover = hover_tab == TAB_COUNT + i;
        c.d->AddRectFilled(ImVec2(cx, ty - 13 * s), ImVec2(cx + cw, ty + 13 * s), hover ? ui::kTabInset : ui::kBannerInk, 3 * s);
        ui::text_mid_centre(c, c.f.strong, 14 * s, cx + cw * 0.5f, ty, ui::kInk, keys[i]);
        hits.add(i ? cx - 10 * s : 0, 0, i ? c.w : cx + cw + 10 * s, hh, i ? menu_hit::kTabNext : menu_hit::kTabPrev, -1);
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        const bool shown = i == (cur_tab == TAB_MOTION ? TAB_CONTROLS : cur_tab);   // (the Motion page: under Controls)
        if (shown || i == hover_tab) {
            c.d->AddRectFilled(ImVec2(x, 0), ImVec2(x + tw[i], hh - 3 * s), shown ? ui::kTabInset : ui::alpha(ui::kTabInset, 0.35f));
            if (shown) {
                c.d->AddLine(ImVec2(x + 1, 0), ImVec2(x + 1, hh - 3 * s), ui::rgba(52, 34, 16), std::max(1.0f, s));
                c.d->AddLine(ImVec2(x + tw[i] - 1, 0), ImVec2(x + tw[i] - 1, hh - 3 * s), ui::rgba(52, 34, 16), std::max(1.0f, s));
            }
        }
        ui::text_mid(c, c.f.heading, 23 * s, x + 28 * s, ty, shown ? ui::kInkBright : ui::kBannerInk, tab_titles[i]);
        hits.add(x, 0, x + tw[i], hh, menu_hit::kTab, i);
        x += tw[i];
    }
    {   // (which build, at the banner's left, so a report can say it without opening the log; the release alone when the whole
        // line would run into the tabs)
        std::string ver = std::string("HeyListen ") + HOOKSHOT_VERSION;
        const float room = (c.w - total) * 0.5f - 60 * s - c.safe;
        if (ui::size_of(c.f.body, 15 * s, ver.c_str()).x > room && ver.find(" (") != std::string::npos) ver.resize(ver.find(" ("));
        if (ui::size_of(c.f.body, 15 * s, ver.c_str()).x <= room) ui::text_mid(c, c.f.body, 15 * s, c.safe, ty, ui::kBannerInk, ver.c_str());
    }
    // the rows, in the column; the help beside it
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const int count = tab_row_count(cur_tab), sel = tab_selected[cur_tab];
    const Row& selr = rows[tab_row_to_global(cur_tab, sel)];
    const char* hint = selr.value && selr.hint ? selr.hint(*selr.value, -1) : nullptr;
    const std::string pending_line = (cur_tab == TAB_GRAPHICS || cur_tab == TAB_DEBUG) && restart_pending() ? restart_line(c, 16 * s, hx1 - hx0 - 66 * s) : "";
    static float help_tall = 0, help_tall_w = 0;
    if (help_tall_w != hx1 - hx0) help_tall_w = hx1 - hx0, help_tall = measure_help(c, hx1 - hx0 - 48 * s);
    // (a row that opens a page (Motion, Controls, Open debug tools): A or Enter opens it; Restart and Quit: A or Enter twice)
    const bool confirms = &selr == &rows[kRestartRow] || &selr == &rows[kQuitRow];
    ui::Prompt prompt_list[5] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"}, {ui::kTabs, "Tabs"}, {ui::kBack, "Close"}};
    int prompts_n = cur_tab == TAB_MOTION ? 3 : 4;
    if (!selr.value) prompt_list[1] = {ui::kConfirm, confirms ? (pending == &selr ? "Confirm" : selr.name) : "Open"};
    if (cur_tab == TAB_MOTION) prompt_list[2] = {ui::kBack, "Back"};   // (Esc: to Controls)
    if (selr.cheat >= 0) {   // (a debug tool: Change when Left and Right do something, then what confirm does)
        prompts_n = 1;
        if (selr.cheat <= 1 || selr.cheat == 7 || cheat_chooses(selr.cheat)) prompt_list[prompts_n++] = {ui::kLeftRight, "Change"};
        if (const char* says = cheat_confirm(selr.cheat)) prompt_list[prompts_n++] = {ui::kConfirm, says};
        prompt_list[prompts_n++] = {ui::kTabs, "Tabs"}, prompt_list[prompts_n++] = {ui::kBack, "Close"};
    }
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    float bottom = std::min(c.h - c.safe, bar_top) - 10 * s;   // (the rows end above the prompts)
    const bool after = restart_changed(selr);
    const float rh = 40 * s, pitch = 48 * s, top = hh + 24 * s, rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s, split = rx0 + (rx1 - rx0) * 0.55f;
    if (narrow) {   // (the help over the column's foot, the rows above it: at most half the room, the rows get the rest)
        // (bound help height so it leaves room for at least one row above it)
        const float max_help = std::max(0.0f, (bottom - top - rh - 16 * s) * 0.5f);
        const float help_h = std::min(22 * s + 32 * s + help_tall + (after ? 30 * s : 0) + (pending_line.empty() ? 0 : 52 * s) + 22 * s,
                                      max_help);
        bottom = std::min(bar_top, c.h - c.safe) - help_h - 16 * s;
        help_box(c, hx0, hx1, bottom + 8 * s, selr.name, selr.help, hint, pending_line.empty() ? nullptr : pending_line.c_str(), help_tall,
                 after, bottom + 8 * s, bar_top - 10 * s);
    } else {
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * 0.30f), selr.name, selr.help, hint,
                 pending_line.empty() ? nullptr : pending_line.c_str(), help_tall, after, hh + 18 * s, bar_top - 12 * s);
    }
    // (guarantee bottom stays at or below top + rh so scroll_rows and PushClipRect never receive an inverted range)
    bottom = std::max(bottom, top + rh);
    int shown = 0;
    const float first = scroll_rows(cur_tab, count, sel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), count, shown, first);
    marquee_max = 0;   // (set again below while the selected row's value is too long)
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int i = 0; i < count; i++) {
        const float y = top + (i - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const Row& r = rows[tab_row_to_global(cur_tab, i)];
        if (r.heading) {   // (a section's title: gold, a rule after it to the column's end; no cell, nothing to click)
            const float ty = y + rh * 0.62f, tw = ui::size_of(c.f.heading, 19 * s, r.name).x;
            ui::text_mid(c, c.f.heading, 19 * s, rx0 + 2 * s, ty, ui::kGoldHi, r.name);
            c.d->AddLine(ImVec2(rx0 + tw + 16 * s, ty), ImVec2(rx1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
            continue;
        }
        const bool selected = i == sel;
        name_cell(c, ImVec2(rx0, y), ImVec2(split, y + rh), r.name, selected, restart_changed(r), 18.5f, row_inactive(r.value));
        const ImVec2 va(split, y), vb(rx1, y + rh);
        float t0 = 0, t1 = 0;
        const std::string tool = r.cheat >= 0 ? cheat_value(r.cheat) : "";
        const bool tool_toggle = r.cheat == 0 || r.cheat == 1 || r.cheat == 7;   // (On and Off: halves, as the tabs' toggles)
        if (r.cheat >= 0 && tool_toggle) {
            halves_cell(c, va, vb, "Off", "On", tool == "On", selected, false);
        } else if (r.cheat >= 0 && !tool.empty()) {   // (a debug tool with a value: arrows when Left and Right choose it)
            choice_cell(c, va, vb, tool, selected, cheat_chooses(r.cheat), cheat_chooses(r.cheat));
        } else if (!r.value) {   // (a page, or Restart and Quit: Confirm until the second press)
            if (pending == &r) choice_cell(c, va, vb, "Press again to confirm", selected, false, false);
            else if (is_psychov35_reset_row(r)) choice_cell(c, va, vb, "Reset", selected, false, false);   // (an action, not a page)
            else {
                choice_cell(c, va, vb, "", selected, false, false);
                ui::triangle(c, ImVec2(vb.x - 15 * s, y + rh * 0.5f), 10 * s, 1, selected ? ui::kGoldInk : ui::kInk);
            }
        } else {
            const bool fixed = row_fixed(r.value);
            const std::string lo = r.text(r.lo), hi = r.text(r.hi);
            const float half = (vb.x - va.x) * 0.5f - 18 * s;
            if (is_slider(r)) {
                slider_cell(c, va, vb, r.text(*r.value), menu_hit::slider_permille(*r.value, r.lo, r.hi), selected, &t0, &t1);
            } else if (r.hi - r.lo == r.step && !fixed && ui::size_of(c.f.strong, 18 * s, lo.c_str()).x <= half &&
                       ui::size_of(c.f.strong, 18 * s, hi.c_str()).x <= half) {
                halves_cell(c, va, vb, lo.c_str(), hi.c_str(), *r.value == r.hi, selected, fixed);
            } else {
                const bool output_row = r.value == &opts.output_res;
                const bool left = !fixed && (!output_row || step_output_choice(*r.value, -1) != *r.value) && (!stops_at_ends(r) || *r.value > r.lo);
                const bool right = !fixed && (!output_row || step_output_choice(*r.value, 1) != *r.value) && (!stops_at_ends(r) || *r.value < r.hi);
                choice_cell(c, va, vb, r.text(*r.value), selected, left, right, fixed);
            }
        }
        hits.add(rx0, std::max(y, top - 8 * s), rx1, std::min(y + rh, bottom + 8 * s), menu_hit::kRow, i, va.x, vb.x, t0, t1);
    }
    c.d->PopClipRect();
    ui::prompt_bar(c, prompt_list, prompts_n);   // (the prompts, on top of the help and the rows)
}

void draw_controls(ui::Ctx& c) {
    // (the page's own rows first, kTop of them (the layout preset, the button prompts), then a row per control and Reset to
    // defaults: one list, csel its selection)
    const float s = c.s, hh = title_banner(c, "Controls", nullptr);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const int n = kTop + kControls + 1, sel_i = csel - kTop;   // (sel_i: the control's, kControls Reset; negative: a page row)
    const ui::Prompt idle[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Column"}, {ui::kConfirm, "Bind"}, {ui::kKey, "Clear", "Del"},
                               {ui::kBack, "Back"}};
    const ui::Prompt option[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"}, {ui::kBack, "Back"}};
    const ui::Prompt wait[] = {{ui::kBack, "Cancel"}};
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const Row* page_row = sel_i < 0 ? &rows[page_row_global(csel)] : nullptr;
    const char* title = page_row ? page_row->name : sel_i < kControls ? controls[sel_i].name : "Reset to defaults";
    const char* help = listening ? (listening == 1 ? "Press the gamepad button to use. Esc cancels." : "Press the key to use. Esc cancels.")
                       : page_row ? page_row->help
                       : menu_row(sel_i) ? "A gamepad button, or two pressed together (L3 + R3, Share + Options), that opens and closes "
                                           "this menu. Home, Select and Start together, the touchpad click, Insert and F1 work too. "
                                           "A combination held does not reach the game. Enter binds it, Delete clears it."
                                  : "Which gamepad button and which keys press each of the Switch's buttons. Enter binds the one "
                                    "selected; Delete clears it. The game's button icons follow at once, with no restart.";
    const char* hint = page_row ? (page_row->hint ? page_row->hint(*page_row->value, -1) : nullptr)
                                : "Gamepad: the D-pad moves, A binds, B goes back.";
    float bottom = std::min(c.h - c.safe, bar_top) - 10 * s;   // (the rows end above the prompts)
    if (narrow) {
        // (bound help height so it leaves room for at least one controls row above it: the rows start at hh + 48 * s, below
        // the column titles, and are 36 * s tall)
        const float max_help = std::max(0.0f, (bottom - (hh + 48 * s) - 36 * s - 16 * s) * 0.5f);
        const float help_h = std::min(200 * s, max_help);
        bottom = std::min(bar_top, c.h - c.safe) - help_h - 16 * s;
        help_box(c, hx0, hx1, bottom + 8 * s, nullptr, help, hint, nullptr, 0, page_row && restart_changed(*page_row), bottom + 8 * s,
                 bar_top - 10 * s);
    } else {
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * 0.30f), title, help, hint, nullptr, 0, page_row && restart_changed(*page_row),
                 hh + 18 * s, bar_top - 12 * s);
    }
    const float rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s, pad_x = rx0 + (rx1 - rx0) * 0.36f, key_x = rx0 + (rx1 - rx0) * 0.68f;
    float top = hh + 22 * s;
    ui::text_mid(c, c.f.body, 15 * s, rx0 + 20 * s, top + 10 * s, ui::kDim, "Button");
    ui::text_mid(c, c.f.body, 15 * s, pad_x + 14 * s, top + 10 * s, ui::kDim, "Gamepad");
    ui::text_mid(c, c.f.body, 15 * s, key_x + 14 * s, top + 10 * s, ui::kDim, "Keyboard");
    top += 26 * s;
    const float rh = 36 * s, pitch = 42 * s;
    // (guarantee bottom stays at or below top + rh so controls rows and clip rects never invert)
    bottom = std::max(bottom, top + rh);
    int shown = 0;
    const float first = scroll_rows(kControlsSlot, n, csel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), n, shown, first);
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int j = 0; j < n; j++) {
        const float y = top + (j - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const bool sel = j == csel;
        const int i = j - kTop;
        const float y0 = std::max(y, top - 8 * s), y1 = std::min(y + rh, bottom + 8 * s);
        if (i < 0) {   // (one of the page's own rows: a name and a choice, as on the tabs)
            const Row& r = rows[page_row_global(j)];
            // (their names wider than a control's, as wide as the tabs' rows': "Button prompts" whole at any size)
            const float opt_x = rx0 + (rx1 - rx0) * 0.5f;
            name_cell(c, ImVec2(rx0, y), ImVec2(opt_x, y + rh), r.name, sel, restart_changed(r), 17);
            choice_cell(c, ImVec2(opt_x, y), ImVec2(rx1, y + rh), r.text(*r.value), sel, *r.value > r.lo, *r.value < r.hi);
            hits.add(rx0, y0, rx1, y1, menu_hit::kCtlOption, j, opt_x, rx1);
            continue;
        }
        if (i == kControls) {
            ui::button(c, ImVec2(rx0, y), ImVec2(pad_x - 8 * s, y + rh), "Reset to defaults", sel);
            hits.add(rx0, y0, pad_x - 8 * s, y1, menu_hit::kCtlRow, i);
            continue;
        }
        const Control& ctl = controls[i];
        name_cell(c, ImVec2(rx0, y), ImVec2(pad_x, y + rh), ctl.name, sel, false, 17);
        hits.add(rx0, y0, pad_x, y1, menu_hit::kCtlRow, i);
        for (int col = 0; col < 2; col++) {   // (two value cells, a gap between them, each drawn as the tabs' values are)
            const float a = col ? key_x + 3 * s : pad_x, b = col ? rx1 : key_x - 3 * s;
            const bool dead = (col == 0 && keys_only(i)) || (col == 1 && menu_row(i));   // (no such input for this control)
            std::string v = col == 0 ? (keys_only(i) ? "-" : menu_row(i) ? menu_combo_text() : pad_name(host->binding(HOS_BIND_PAD, ctl.button, 0)))
                                     : menu_row(i) ? "Insert, F1" : keys_text(ctl.button);
            const bool here = sel && col == ccol;
            const ImVec2 va(a, y), vb(b, y + rh);
            if (here && listening) {   // (waiting for the new input: amber, the outline as a selected value's)
                const float in = 4 * s;
                c.d->AddRectFilled(va, vb, ui::kCellDark);
                c.d->AddRect(va, vb, ui::kEdge, 0, std::max(1.0f, s));
                c.d->AddRectFilled(va + ImVec2(in, in), vb - ImVec2(in, in), ui::kListen);
                ui::text_mid_centre(c, c.f.strong, 16.5f * s, (a + b) * 0.5f, y + rh * 0.5f, ui::kInkBright,
                                    col == 0 ? (menu_row(i) ? "Press a button, or two..." : "Press a button...") : "Press a key...");
                ui::glow(c, va, vb);
            } else {
                choice_cell(c, va, vb, v, here, false, false, dead);
            }
            if (!dead) hits.add(a, y0, b, y1, menu_hit::kCtlCell, i, 0, 0, 0, 0, col);
        }
    }
    c.d->PopClipRect();
    (void)(listening ? ui::prompt_bar(c, wait, 1) : sel_i < 0 ? ui::prompt_bar(c, option, 3) : ui::prompt_bar(c, idle, 5));   // (the prompts, on top of the help and the rows)
}

void draw_list(ui::Ctx& c, bool inv) {
    const float s = c.s;
    const int n = inv ? kItems : kTeleports, sections = inv ? kItemSections : kTeleportSections;
    const TeleportSection* sec = inv ? item_sections.s : teleport_sections;
    const int choice = inv ? item_choice : teleport_choice, scroll = inv ? item_scroll : teleport_scroll;   // (as menu_tick left them)
    char count[32];
    snprintf(count, sizeof count, "%d of %d", choice + 1, n);
    const float hh = title_banner(c, inv ? "Inventory" : "Teleport", count);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const ui::Prompt tele[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Section"}, {ui::kConfirm, "Travel"}, {ui::kBack, "Back"}};
    const ui::Prompt items[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Amount"}, {ui::kTabs, "Section"}, {ui::kConfirm, "Unlock"},
                                {ui::kBack, "Back"}};
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const char* help = inv ? "Enter unlocks or relocks the item. Left and Right change an amount. Page Up, Page Down, Home and End jump."
                           : "Link travels through the game's own map change, in his current age, and arrives at the map's own entrance. "
                             "Page Up, Page Down, Home and End jump.";
    const char* name = inv ? item_names[choice] : teleport_names[choice];
    if (!narrow)
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * 0.30f), name, help, nullptr, nullptr, 0, false, hh + 18 * s, bar_top - 12 * s);
    const float top = hh + 18 * s, bottom = std::min(bar_top, c.h - c.safe) - 8 * s;
    list_lines = std::clamp((int)((bottom - top) / (30 * s)), 8, 20);   // (menu_tick scrolls by it from the next frame)
    const float pitch = std::min(36 * s, (bottom - top) / list_lines), rh = pitch - 4 * s, px = std::min(18 * s, rh * 0.58f);
    const float rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s;
    for (int line = scroll; line < std::min(n + sections, scroll + list_lines); line++) {
        const float y = top + (line - scroll) * pitch;
        int section = 0;
        while (section + 1 < sections && line >= sec[section + 1].first + section + 1) section++;
        if (line == sec[section].first + section) {   // (a section's heading: its name over a fine line)
            const float tw = ui::size_of(c.f.heading, px, sec[section].name).x, mid = y + rh * 0.5f, xm = (rx0 + rx1) * 0.5f;
            c.d->AddLine(ImVec2(rx0 + 30 * s, mid), ImVec2(xm - tw * 0.5f - 16 * s, mid), ui::alpha(ui::kAccent, 0.5f), std::max(1.0f, s));
            c.d->AddLine(ImVec2(xm + tw * 0.5f + 16 * s, mid), ImVec2(rx1 - 30 * s, mid), ui::alpha(ui::kAccent, 0.5f), std::max(1.0f, s));
            ui::text_mid_centre(c, c.f.heading, px, xm, mid, ui::kAccent, sec[section].name);
            continue;
        }
        const int i = line - section - 1;
        const bool sel = i == choice;
        const ImVec2 a(rx0, y), b(rx1, y + rh);
        c.d->AddRectFilled(a, b, sel ? ui::kCellNameSel : ui::kCellName);
        c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
        ui::text_mid(c, sel ? c.f.strong : c.f.body, px, rx0 + 20 * s, y + rh * 0.5f, sel ? ui::kInkBright : ui::kInk,
                     inv ? item_names[i] : teleport_names[i]);
        char value[24] = "";
        const char* note = nullptr;
        bool bright = false;
        if (inv) {
            const int have = item_counts[i].load(std::memory_order_relaxed);
            if (have < 0) note = "?";
            else if (item_kinds[i]) snprintf(value, sizeof value, "%d", have), note = value, bright = true;
            else note = have > 0 ? "Have" : "-", bright = have > 0;
        } else {
            note = teleport_notes[i];
        }
        float v0 = b.x, v1 = b.x;
        if (note) {
            const float nw = ui::size_of(c.f.body, px, note).x;
            const bool amount = inv && item_kinds[i] && sel;
            v0 = b.x - nw - (amount ? 64 : 22) * s, v1 = b.x;
            if (sel) ui::gold(c, ImVec2(v0, y + 4 * s), ImVec2(b.x - 4 * s, b.y - 4 * s));
            const ImU32 ink = sel ? ui::kGoldInk : bright ? ui::kInk : ui::kDim;
            if (amount) {
                ui::triangle(c, ImVec2(v0 + 14 * s, y + rh * 0.5f), 8 * s, 0, ink), ui::triangle(c, ImVec2(b.x - 16 * s, y + rh * 0.5f), 8 * s, 1, ink);
                ui::text_mid_centre(c, c.f.strong, px, (v0 + b.x) * 0.5f, y + rh * 0.5f, ink, note);
            } else {
                ui::text_mid_right(c, sel ? c.f.strong : c.f.body, px, b.x - 12 * s, y + rh * 0.5f, ink, note);
            }
        }
        if (sel) {
            c.d->AddRectFilled(a, ImVec2(a.x + 4 * s, b.y), ui::kGoldHi);
            ui::glow(c, a, b);
        }
        hits.add(rx0, y, rx1, y + rh, menu_hit::kListLine, line - scroll, inv && item_kinds[i] ? v0 : 0, inv && item_kinds[i] ? v1 : 0);
    }
    (void)(inv ? ui::prompt_bar(c, items, 5) : ui::prompt_bar(c, tele, 4));   // (the prompts, on top of the help and the rows)
}

void draw_ask(ui::Ctx& c) {
    const float s = c.s, W = 560 * s, H = 232 * s;
    const ImVec2 p((c.w - W) * 0.5f, (c.h - H) * 0.5f), q = p + ImVec2(W, H);
    ui::shadow(c, p, q, 30 * s, 1.0f, 8 * s);
    c.d->AddRectFilled(p, q, ui::rgba(36, 28, 19, 250));
    c.d->AddRect(p, q, ui::rgba(122, 98, 64, 230), 0, 2 * s);
    const float hb = 50 * s;   // (a tan title strip, as the banner, its title in dark ink)
    c.d->AddRectFilledMultiColor(p, ImVec2(q.x, p.y + hb), ui::kBannerTop, ui::kBannerTop, ui::kBanner, ui::kBanner);
    c.d->AddRectFilled(ImVec2(p.x, p.y + hb - 3 * s), ImVec2(q.x, p.y + hb), ui::kBannerEdge);
    ui::text_mid_centre(c, c.f.heading, 23 * s, (p.x + q.x) * 0.5f, p.y + hb * 0.5f, ui::kBannerInk, "Restart Needed");
    const std::string ask = "Restart to apply " + restart_list(" and ") + "?";
    ui::para(c, c.f.body, 19 * s, ImVec2(p.x + 28 * s, p.y + 64 * s), ui::kInk, ask.c_str(), W - 56 * s);
    ui::text_mid(c, c.f.body, 16 * s, p.x + 28 * s, p.y + 124 * s, ui::kSoft, "Progress since your last save is lost.");
    const float bw = 210 * s, bh = 46 * s, gap = 24 * s, by = p.y + 160 * s, x0 = (p.x + q.x) * 0.5f - bw - gap * 0.5f;
    const char* labels[2] = {"Later", "Restart Now"};
    for (int i = 0; i < 2; i++) {
        const ImVec2 a(x0 + i * (bw + gap), by), b = a + ImVec2(bw, bh);
        ui::button(c, a, b, labels[i], i == ask_sel);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, i);
    }
    const ui::Prompt prompt_list[] = {{ui::kLeftRight, "Choose"}, {ui::kConfirm, "Confirm"}, {ui::kBack, "Later"}};
    ui::prompt_bar(c, prompt_list, 3);
}

void draw_menu(ui::Ctx& c);
/// The menu's text sizes at this window's unit (and, with Menu size Auto, at the other size Auto can pick), baked ahead by
/// vk_imgui.cpp a few a frame: the first open then draws with no glyphs to make (it took 2.6 ms at 1080p)
float prebaked[3] = {0, 0, 0};   // (the units and the width prebake last did)
/// The menu's units now (with Auto, both it can pick) for a window w x h: whether prebake has not done them yet
bool prebake_needed(uint32_t w, uint32_t h) {
    float m = 0;
    const float u0 = ui::unit((float)h, opts.menu_size, opts.fullscreen != 0, auto_pad, &m);
    const float u1 = opts.menu_size == 0 ? ui::unit((float)h, 0, opts.fullscreen != 0, !auto_pad, &m) : u0;
    return u0 != prebaked[0] || u1 != prebaked[1] || (float)w != prebaked[2];
}
void prebake(const ui::Ctx& c) {
    float units[2] = {c.s, c.s}, margins[2] = {c.safe, c.safe};
    if (opts.menu_size == 0) units[1] = ui::unit(c.h, 0, opts.fullscreen != 0, !auto_pad, &margins[1]);
    if (units[0] == prebaked[0] && units[1] == prebaked[1] && c.w == prebaked[2]) return;
    prebaked[0] = units[0], prebaked[1] = units[1], prebaked[2] = c.w;
    static const float points[] = {12, 12.5f, 13, 13.6f, 14, 14.28f, 14.5f, 15, 15.5f, 16, 16.5f, 17, 18, 18.5f, 19, 20, 21, 23, 24};
    constexpr int n = sizeof points / sizeof points[0];
    float px[2 * n];
    for (int u = 0; u < 2; u++)
        for (int i = 0; i < n; i++) px[u * n + i] = points[i] * units[u];
    ui::prebake(px, units[1] == units[0] ? n : 2 * n);
    if (open_now.load(std::memory_order_relaxed)) return;
    // (and the page itself drawn once into a list of its own, thrown away: what the first open would measure and bake (the
    // help's height over every row, the exact sizes) done now, the menu's state as it was)
    const menu_hit::Map saved_hits = hits;
    const int saved_marquee = marquee_max, saved_lines = list_lines;
    const float saved_ease = ease_step;
    float saved_scroll[sizeof scroll_at / sizeof *scroll_at], saved_want[sizeof scroll_at / sizeof *scroll_at];
    int saved_to[sizeof scroll_at / sizeof *scroll_at];   // (the scratch drawing's scroll, given back: the next is the menu's)
    memcpy(saved_scroll, scroll_at, sizeof scroll_at);
    memcpy(saved_want, scroll_want, sizeof scroll_want);
    memcpy(saved_to, scrolled_to, sizeof scrolled_to);
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch._ResetForNewFrame();
    scratch.PushTexture(ImGui::GetIO().Fonts->TexRef);
    scratch.PushClipRectFullScreen();
    ease_step = 0;
    for (int u = 0; u < (units[1] == units[0] ? 1 : 2); u++) {
        ui::Ctx dry = c;
        dry.d = &scratch;
        dry.s = units[u], dry.safe = margins[u];
        draw_menu(dry);
    }
    hits = saved_hits, marquee_max = saved_marquee, list_lines = saved_lines, ease_step = saved_ease;
    memcpy(scroll_at, saved_scroll, sizeof scroll_at);
    memcpy(scroll_want, saved_want, sizeof scroll_want);
    memcpy(scrolled_to, saved_to, sizeof scrolled_to);
}

float menu_ox = 0;   // (the menu's column's left edge in the window: more than 0 only on a window wider than 16:9)
/// The menu: the game dimmed across the whole window, the pages in a column as wide as a 16:9 screen of this height, in the
/// middle (a window of 16:9 or narrower: the whole of it). The pages draw from 0 as on 16:9; their vertices and clip
/// rectangles are moved over after, and menu_tick moves the mouse back by menu_ox before it tests the hits
void draw_menu(ui::Ctx& c) {
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, ui::kDimAlpha));   // (the game behind, dimmed)
    ui::Ctx k = c;
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    menu_ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size, cmd0 = c.d->CmdBuffer.Size;
    hits.clear(0, 0, k.w, k.h);
    if (page == 5) draw_ask(k);
    else if (page == 1) draw_controls(k);
    else if (page == 3 || page == 4) draw_list(k, page == 4);
    else draw_options(k);
    if (menu_ox <= 0) return;
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) c.d->VtxBuffer[i].pos.x += menu_ox;
    // (a clip rectangle of the column's: moved with it; the whole window's (the dim's, and the draw list's own) stays, as it
    // holds the column wherever it is)
    for (int i = std::max(0, cmd0 - 1); i < c.d->CmdBuffer.Size; i++) {
        ImVec4& r = c.d->CmdBuffer[i].ClipRect;
        if (r.x <= 0 && r.z >= c.w) continue;
        r.x += menu_ox, r.z += menu_ox;
    }
    // (the draw list's current header, for whatever is drawn next: the window's again after the pages' pops)
}

/// The FPS counter: a plate in the safe area's top right corner (under the banner while the menu is open). (HeyListen: not
/// drawn; the counter is the performance overlay's, vk_overlay.cpp, F10)
[[maybe_unused]] void draw_fps(ui::Ctx& c, bool under_banner) {
    const float s = c.s, px = 24 * s;
    const float nw = ui::size_of(c.f.heading, px, fps_text).x, sw = fps_sub[0] ? ui::size_of(c.f.body, 12.5f * s, fps_sub).x : 0;
    const float bw = std::max(nw + 54 * s, sw + 24 * s), bh = fps_sub[0] ? 48 * s : 34 * s;
    // (the window's top right corner, where it is in play; under the banner while the menu is open, unless the window is so
    // much wider than 16:9 that the plate fits beside the centred column's banner, in the corner)
    const bool beside = menu_ox >= bw + 2 * c.safe;
    const ImVec2 a(c.w - c.safe - bw, under_banner && page != 5 && !beside ? banner_h(c) + 10 * s : c.safe), b = a + ImVec2(bw, bh);
    c.d->AddRectFilled(a, b, ui::rgba(30, 23, 15, 236));
    c.d->AddRect(a, b, ui::kBoxEdge, 0, std::max(1.0f, s));
    ui::text_mid(c, c.f.heading, px, a.x + 12 * s, a.y + 17 * s, ui::rgba(246, 236, 214), fps_text);
    ui::text_mid(c, c.f.strong, 12 * s, a.x + 17 * s + nw, a.y + 17 * s, ui::kAccent, "FPS");
    if (fps_sub[0]) ui::text_mid(c, c.f.body, 12.5f * s, a.x + 12 * s, a.y + 37 * s, ui::kSoft, fps_sub);
}

/// The start-up screen: the logo, the wordmark, what the start is doing, a bar, on a warm dark
void draw_startup(ui::Ctx& c) {
    StartupView v;
    {
        std::lock_guard lk(startup_mu);
        v = startup;
    }
    const float s = c.s, w = c.w, h = c.h, cx = w * 0.5f;
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(14, 10, 6));
    for (int i = 40; i >= 1; i--) c.d->AddCircleFilled(ImVec2(cx, h * 0.42f), h * 0.021f * i, ui::rgba(46, 33, 18, 4), 128);
    float aspect = 1;
    ImTextureData* logo = ui::logo(&aspect);
    const float lh = 112 * s, top = h * 0.5f - 236 * s;
    if (logo) c.d->AddImage(logo->GetTexRef(), ImVec2(cx - lh * aspect * 0.5f, top), ImVec2(cx + lh * aspect * 0.5f, top + lh));
    float y = top + lh + 16 * s;
    ui::spaced(c, c.f.heading, 42 * s, ImVec2(cx, y), ui::rgba(246, 232, 204), "HEYLISTEN", 6 * s);
    ui::flourish(c, cx - 190 * s, cx + 190 * s, y + 66 * s, ui::kAccent);
    ui::text_centre(c, c.f.body, 14.5f * s, ImVec2(cx, y + 78 * s), ui::kDim, v.version.c_str());
    if (v.title.empty()) return;
    y += 120 * s;
    if (!v.step.empty()) {   // ("STEP 2 OF 2" as "Step 2 of 2")
        std::string step = v.step;
        for (size_t i = 1; i < step.size(); i++) step[i] = (char)tolower((unsigned char)step[i]);
        ui::text_centre(c, c.f.body, 15 * s, ImVec2(cx, y), ui::kAccent, step.c_str());
    }
    ui::text_centre(c, c.f.heading, 24 * s, ImVec2(cx, y + 20 * s), ui::kInk, v.title.c_str());
    const float x0 = cx - 270 * s, x1 = cx + 270 * s, by = y + 64 * s;
    float under = by;
    if (v.fraction >= 0) {   // (the bar as a value cell: a dark cell, gold inside, the percentage on it)
        const float bh = 26 * s, in = 4 * s, fx = x0 + in + (x1 - x0 - 2 * in) * (float)std::min(v.fraction, 1.0);
        c.d->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kCellDark);
        c.d->AddRect(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kEdge, 0, std::max(1.0f, s));
        if (fx > x0 + in) ui::gold(c, ImVec2(x0 + in, by + in), ImVec2(fx, by + bh - in));
        // (the percentage in the middle of the whole bar, drawn twice: dark ink where the gold is under it, light where the
        // empty bar is, each clipped to its part, so every letter keeps its contrast as the edge passes through it)
        const float split = std::max(fx, x0 + in);
        c.d->PushClipRect(ImVec2(x0, by), ImVec2(split, by + bh), true);
        ui::text_mid_centre(c, c.f.strong, 15 * s, cx, by + bh * 0.5f, ui::kGoldInk, v.pct.c_str());
        c.d->PopClipRect();
        c.d->PushClipRect(ImVec2(split, by), ImVec2(x1, by + bh), true);
        ui::text_mid_centre(c, c.f.strong, 15 * s, cx, by + bh * 0.5f, ui::kInk, v.pct.c_str());
        c.d->PopClipRect();
        under = by + bh;
    }
    ui::text(c, c.f.body, 16.5f * s, ImVec2(v.fraction >= 0 ? x0 : cx - ui::size_of(c.f.body, 16.5f * s, v.left.c_str()).x * 0.5f, under + 12 * s),
             ui::kSoft, v.left.c_str());
    if (!v.right.empty()) ui::text(c, c.f.body, 16.5f * s, ImVec2(x1 - ui::size_of(c.f.body, 16.5f * s, v.right.c_str()).x, under + 12 * s),
                                   ui::kSoft, v.right.c_str());
    if (!v.hint.empty()) ui::text_centre(c, c.f.body, 15.5f * s, ImVec2(cx, by + 92 * s), ui::kDim, v.hint.c_str());
}

} // namespace

void startup_screen(const StartupView* view) {
    if (!view) {
        startup_on.store(false, std::memory_order_release);
        return;
    }
    StartupView v = *view;
    if (v.version.empty()) v.version = version_text(HOOKSHOT_VERSION);   // ("v2.0.5"; "v2.0.5 dev" between releases)
    {
        std::lock_guard lk(startup_mu);
        startup = v;
    }
    startup_on.store(true, std::memory_order_release);
}

// (HeyListen) Low-end GPUs: the Volumetric fog row and the first-start rule (lowend_rule.h)
static int env_int(const char* name) {   // (-1: unset or empty)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
bool fog_runs() {   // (asked at each fog dispatch: the row applies at once; RX_VK_FOG, read once, stands over it)
    static const int env = env_int("RX_VK_FOG");
    const bool v = lowend_rule::fog_on(env, opts.fog);
    static std::atomic<int> last{-1};
    if (last.exchange(v) != (int)v) logf("vk: volumetric fog %s%s", v ? "on" : "off", env >= 0 ? " (RX_VK_FOG)" : " (the Volumetric fog row)");
    return v;
}
static std::atomic<bool> options_read{false};   // (set at the end of menu_init: the rows hold the options file's values)
int menu_handheld_now() { return options_read.load(std::memory_order_acquire) ? opts.handheld != 0 : -1; }
static bool lowend_mirror;
bool lowend_mirror_not_wait() { return lowend_mirror; }
/// The options file with only these keys' lines rewritten (lowend_rule.h ini_put_text: every other line and the file's line
/// endings kept), written whole or not at all as save() does. Read in binary: a text read would turn CRLF into LF
static void ini_put_only(const std::vector<std::pair<std::string, int>>& keys) {
    std::string in;
    if (FILE* f = fopen(ini_path().c_str(), "rb")) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) in.append(b, n);
        fclose(f);
    }
    const std::string out = lowend_rule::ini_put_text(in, keys);
    hos_atomic_write(ini_path().c_str(), out.data(), out.size());
}
static lowend_rule::Probe lowend_probe;
static std::atomic<bool> lowend_probing{false};
static double lowend_fake_fps = -1;   // (HL_LOWEND_TEST: a test hook)
/// The Lowest preset chosen, now (apply_preset: its rows and the memory limits), and the file written in place when save_it: only the lines
/// of the rows the preset sets, the preset's own and the rule's (every other line stays as it is)
static void lowend_turn_on(bool save_it) {
    opts.preset = 1;
    apply_preset(1);
    if (!save_it) return;
    lowend_marked = true;
    std::vector<std::pair<std::string, int>> keys;
    int* const set[] = {&opts.preset, &opts.resolution, &opts.upscale, &opts.sharpness, &opts.pace, &opts.realtime, &opts.handheld, &opts.fog};
    for (int* v : set)
        for (const Row& r : rows)
            if (r.value == v) keys.push_back({ini_key(r), *v});
    keys.push_back({"options_version", 4});   // (the preset's number is the one of version 4: menu_init)
    keys.push_back({"lowend_rule", 1});
    ini_put_only(keys);
}
void menu_first_run_lowend(uint32_t vendor_id, bool integrated, bool mesh_shader) {
    lowend_mirror = lowend_rule::mirror_not_wait(vendor_id, integrated, mesh_shader);
    const char* test = getenv("HL_LOWEND_TEST");   // (test hook: "fps[,skip s,window s]": the measure runs on any GPU, with this rate in place of the measured one)
    if (test && *test) {
        double skip = lowend_probe.skip_s, win = lowend_probe.window_s;
        lowend_fake_fps = atof(test);
        if (const char* c = strchr(test, ',')) {
            skip = atof(c + 1);
            if (const char* d = strchr(c + 1, ',')) win = atof(d + 1);
        }
        lowend_probe.skip_s = skip, lowend_probe.window_s = win;
        integrated = true;
    }
    if (const char* t = getenv("HL_LOWEND_FPS"); t && atof(t) > 0) lowend_probe.threshold = atof(t);   // (the threshold, frames a second)
    const Options dflt;   // (untouched: the preset and the rows it sets are what a new options file has)
    const bool untouched = opts.preset == dflt.preset && opts.resolution == dflt.resolution && opts.handheld == dflt.handheld && opts.fog == dflt.fog &&
                           opts.upscale == dflt.upscale && opts.sharpness == dflt.sharpness && opts.pace == dflt.pace && opts.realtime == dflt.realtime;
    const lowend_rule::Apply a = lowend_rule::first_start(env_int("HL_LOWEND"), lowend_marked, integrated, untouched);
    const char* why = mesh_shader ? "with mesh shaders" : "no mesh shaders";
    if (a == lowend_rule::NONE) {
        if (integrated) logf("vk: integrated GPU (%s): the Lowest preset's measure: %s", why,
                             env_int("HL_LOWEND") == 0 ? "not measured (HL_LOWEND=0)"
                             : lowend_marked ? "made once already (lowend_rule=1 in the options file): the Quality preset stays as set"
                                             : "the Quality preset or the rows it sets are the player's: not measured");
        return;
    }
    if (a == lowend_rule::PROBE) {
        lowend_probing.store(true, std::memory_order_relaxed);
        logf("vk: integrated GPU (%s): the frames of play are measured (%.0f s after play begins, over %.0f s, under %.0f fps the Quality "
             "preset turns to Lowest (integrated GPU))%s", why, lowend_probe.skip_s, lowend_probe.window_s, lowend_probe.threshold,
             lowend_fake_fps >= 0 ? "; HL_LOWEND_TEST gives it a rate" : "");
        return;
    }
    // (HL_LOWEND=1: the Lowest preset for this run only, on any GPU; nothing saved)
    lowend_turn_on(false);
    logf("vk: HL_LOWEND=1: the Quality preset Lowest (integrated GPU) for this run only: Render resolution %d%%, Volumetric fog off, the "
         "handheld profile (from the next start: the game asks for it before the GPU is known); nothing saved", ur::percent(opts.resolution));
}
/// A frame was shown (menu_note_frame): feeds the measure while it runs; at its end the row turns On or the question is closed
static void lowend_probe_frame() {
    if (!lowend_probing.load(std::memory_order_relaxed)) return;
    const bool on = host && host->play_on && host->play_on();
    if (lowend_probe.feed(prof_now(), on) == lowend_rule::PENDING) return;
    lowend_probing.store(false, std::memory_order_relaxed);
    const double fps = lowend_fake_fps >= 0 ? lowend_fake_fps : lowend_probe.fps;
    if (lowend_rule::weak(fps, lowend_probe.threshold) && !lowend_touched) {
        lowend_turn_on(true);
        logf("vk: integrated GPU measured at %.1f fps over %.0f s of play, under %.0f: the Quality preset (Graphics tab) turned to Lowest "
             "(integrated GPU): Render resolution %d%% now, Volumetric fog off and the handheld profile from the next start; saved to the options "
             "file (the preset and the rows it sets only); another preset in the menu changes them", fps, lowend_probe.window_s,
             lowend_probe.threshold, ur::percent(opts.resolution));
    } else {
        lowend_marked = true;
        ini_put_only({{"lowend_rule", 1}});
        if (lowend_rule::weak(fps, lowend_probe.threshold))
            logf("vk: integrated GPU measured at %.1f fps, under %.0f, but the Quality preset or its rows were set by the player: left as they are", fps,
                 lowend_probe.threshold);
        else
            logf("vk: this integrated GPU keeps its settings: %.1f fps over %.0f s of play (at least %.0f); asked once, never again (lowend_rule=1)",
                 fps, lowend_probe.window_s, lowend_probe.threshold);
    }
}
static bool options_file_found;   // (hookshot_options.ini existed when the menu started: menu_first_run_textures)
void menu_first_run_textures() {
    static const bool on = getenv("RX_VK_TEX_LOW_FIRST_RUN") && atoi(getenv("RX_VK_TEX_LOW_FIRST_RUN")) != 0;
    if (!on || options_file_found || getenv("RX_VK_TEX_LOW")) return;
    options_file_found = true;
    const uint64_t vram = device_vram_bytes();
    if (!vram || vram > (6ull << 30) + (512ull << 20)) return;
    opts.tex_low = 1;
    save();
    logf("vk: Texture resolution: first run on a card of %llu MB, started at Low (saved to the options file; the player's choice from now on)",
         (unsigned long long)(vram >> 20));
}
static bool dlss_model_saved;   // (hookshot_options.ini has a dlss_model line)
void menu_first_run_model(uint32_t vendor_id, uint32_t device_id) {
    if (dlss_model_saved || vendor_id != 0x10de) return;   // (only NVIDIA GPUs run DLSS; a saved value is never changed)
    dlss_model_saved = true;
    opts.dlss_model = ur::default_model(vendor_id, device_id, device_vram_bytes());
    save();   // (at once: the menu shows the real value, and the player's change is saved on top of it)
    logf("vk: DLSS model: first run, started at %s for this GPU (%#x), saved to the options file",
         opts.dlss_model == ur::MODEL_CNN ? "DLSS 3 (CNN, preset E)" : opts.dlss_model == ur::MODEL_M ? "DLSS 4.5 (preset M)" : "DLSS 4 (preset K)",
         (unsigned)device_id);
}
void menu_init() {
    static bool done;
    if (done) return;
    done = true;
    for (auto& count : item_counts) count.store(-1, std::memory_order_relaxed);
    bool has_options = false;
    const char* cheats = getenv("HOS_OOT_CHEATS");
    cheats_env = cheats && !strcmp(cheats, "1");
    if (cheats_env) {
        const char* hearts = getenv("HOS_OOT_PROTECT_HEARTS");
        protect_hearts.store(hearts && !strcmp(hearts, "1"));
    }
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        has_options = options_file_found = true;
        char line[80];
        bool has_resolution = false;
        bool v2 = false;
        int version = 0;   // (options_version: 4 has the Quality preset's Lowest at 1)
        int saved_preset = 0;
        int old_dynres = -1;   // (the row before Resolution: 0, the full scale, is its 100%; 1 is Auto)
        int saved_vsync = -1;  // (the last vsync= line: 2 was the old Adaptive, now On with FIFO (triple buffering))
        bool has_vsync_mode = false;
        bool saved_hdr_ours = false;   // (the HDR row saved as 3: "Native + our tone mapper", from before PsychoV35 replaced it)
        while (fgets(line, sizeof line, f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (Row& r : rows) {
                if (!r.value || strcmp(line, ini_key(r))) continue;
                const int v = atoi(eq + 1);
                *r.value = r.value == &opts.output_res ? output_choice_text(eq + 1) : std::min(std::max(v, r.lo), r.hi);
                if (r.value == &opts.dlss_model) dlss_model_saved = true;   // (a saved model is the player's: menu_first_run_model)
                if (r.value == &opts.hdr && v == 3) saved_hdr_ours = true;   // (clamped to Native above: the old fourth value, below)
            }
            has_resolution |= !strcmp(line, "resolution");
            v2 |= !strcmp(line, "options_version");
            if (!strcmp(line, "options_version")) version = atoi(eq + 1);
            if (!strcmp(line, "quality_preset")) saved_preset = atoi(eq + 1);
            if (!strcmp(line, "lowend_settings") && atoi(eq + 1) == 1) lowend_old_row = true;
            if (!strcmp(line, "dynamic_resolution")) old_dynres = atoi(eq + 1);
            if (!strcmp(line, "lowend_rule")) lowend_marked = true;   // (menu_first_run_lowend: applied once, never again)
            if (!strcmp(line, "vsync")) saved_vsync = atoi(eq + 1);
            if (!strcmp(line, "vsync_mode")) has_vsync_mode = true;
            if (!strcmp(line, "output_device_id") || !strcmp(line, "output_device_name")) {   // (heylisten: the output, by its id)
                std::string& want = !strcmp(line, "output_device_id") ? out_want_id : out_want_name;
                want = mic_key(eq + 1);
                while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
            }
            if (!strcmp(line, "microphone_device")) {   // (hookshot: the microphone, by its name)
                mic_want = mic_key(eq + 1);
                while (!mic_want.empty() && (mic_want.back() == '\n' || mic_want.back() == '\r')) mic_want.pop_back();
            }
        }
        fclose(f);
        // (an options file from before version 4: the Quality preset's Minimum to Ultra were 1 to 5, now 2 to 6. A file of a build of 25 Nov
        // with the Low-end settings row On (lowend_settings=1; its rows kept as they are) reads as the Lowest preset: the old lines are
        // dropped at the next save)
        opts.preset = std::min(std::max(lowend_rule::migrate_preset(version, saved_preset, lowend_old_row), 0), 6);
        if (lowend_old_row) {
            logf("vk: options: the saved Low-end settings row (On) is the Quality preset Lowest (integrated GPU) now; its lines go at the next save");
        }
        // (an options file from the branch that had Adaptive (vsync=2): V-Sync On with FIFO (triple buffering), today's On. The
        // V-Sync mode row is new, so nothing else can contradict it; a saved mode of its own wins)
        if (saved_vsync >= 2 && !has_vsync_mode) opts.vsync_mode = present_mode::kFifoTriple;
        // (the saved microphone by its name, not its place; the host told the row's choice. Off or the default: no list, no SDL
        // audio at the start)
        if (!mic_want.empty()) refresh_mics();
        else {
            if (opts.mic >= 2) opts.mic = 1;
            mic_tell_host();
        }
        if (!out_want_id.empty()) refresh_outs();   // (heylisten: Default needs no list at the start)
        else opts.out_dev = 0, out_tell_host();
        // (HDR saved as "Native + our tone mapper", the fourth value of the row in v0.4.3's work: that tone mapper is gone, the
        // PsychoV35 one is the HDR tone mapper row's now, and the nearest to it: Native with the tone mapper row on PsychoV35)
        if (saved_hdr_ours) {
            opts.hdr = 2, opts.hdr_tonemap = 1;
            logf("vk: options: the saved HDR was Native + our tone mapper: now Native with the HDR tone mapper PsychoV35");
        }
        if (!has_resolution && old_dynres >= 0) opts.resolution = old_dynres ? 0 : 1;
        // (an Upscaler saved as DLAA or FSR 4's native AA, which are the Render resolution's native choice now: DLSS or FSR 4 with
        // it. The 3D was at full size with them, whatever the Render resolution row kept)
        if (ur::migrate(opts.dlss, opts.resolution))
            logf("vk: options: the saved Upscaler was DLAA or FSR 4's Native AA: now the Upscaler %s with Render resolution %s",
                 opts.dlss == ur::FSR4 ? "AMD FSR 4" : "NVIDIA DLSS", opts.dlss == ur::FSR4 ? "Native AA" : "DLAA (native)");
        opts.rumble = rumble_step(opts.rumble);   // (a hand-edited 60: 50, on the arrows' steps)
        opts.volume = opts.volume / 5 * 5;   // (a hand-edited 63: 60; the row is clamped to 0..100 above; host/nn_audio.c)
        // (Game speed is Real time by default now: Per frame played at half speed at 30 fps, the "slow motion" players
        // reported. An options file from before kept the old default whether chosen or not: Real time for it too)
        if (!v2) opts.realtime = 1;
    }
    // An opt-in profile for 4 GB cards: keep existing options authoritative, but make a first run usable on
    // low-VRAM hardware without requiring the menu to render first.
    if (!has_options && getenv("RX_VK_LOW_SPEC") && atoi(getenv("RX_VK_LOW_SPEC")) != 0) {
        apply_preset(2);
        opts.upscale = 0;
        opts.sharpness = 0;
    }
    if (opts.preset > 0) sync_preset_budgets(opts.preset);
    if (const char* e = getenv("RX_VK_FRAMEGEN_MULT")) opts.framegen = std::min(std::max(atoi(e), 2), 8);
    if (const char* e = getenv("RX_VK_PACE")) opts.pace = atoi(e) != 0;
    if (const char* e = getenv("RX_VK_HDR")) {   // (for one run: the HDR row's value; 3, the old Native + our tone mapper, is Native with PsychoV35)
        opts.hdr = std::min(std::max(atoi(e), 0), 3);
        if (opts.hdr == 3) opts.hdr = 2, opts.hdr_tonemap = 1;
    }
    if (const char* e = getenv("RX_VK_DLSS_PRE_BLOOM"); e && *e) opts.dlss_pre_bloom = atoi(e) != 0;   // (after Longshot 83b161e3)
    if (const char* e = getenv("RX_VK_TEX_LOW"); e && *e) opts.tex_low = atoi(e) != 0;   // (HeyListen: for one run, tex_low.h)
    if (const char* e = getenv("RX_VK_RENDER_RES"); e && *e)   // (HeyListen, a measurement: the Render resolution's value for one run, upscaler_rows.h kPercent)
        opts.resolution = std::min(std::max(atoi(e), 0), ur::kValues - 1);
    if (const char* e = getenv("RX_VK_UPSCALE"))
        for (Row& r : rows)
            if (r.value == &opts.upscale) *r.value = std::min(std::max(atoi(e), r.lo), r.hi);
    if (const char* e = getenv("HOS_REALTIME_STEP")) opts.realtime = atoi(e) != 0;
    if (const char* e = getenv("HOS_DYNRES")) opts.resolution = atoi(e) != 0 ? 0 : 1;
    if (const char* e = getenv("HOS_OUTPUT_RES")) opts.output_res = output_choice_text(e);
    restart_init();   // (what this run started with: the rows that take a restart differ from it once changed)
    options_read.store(true, std::memory_order_release);
}

int menu_base_fps() {
    if (!opts.realtime) return 0;   // (Per frame: the limit does not apply, the base one neither)
    static const int env = [] {   // (the environment's limit stands over both rows, as host/nn_vi.c fps_plan has it)
        const char* e = getenv("HOS_FPS_LIMIT") ? getenv("HOS_FPS_LIMIT") : getenv("HOS_FPS_MAX");
        return e ? atoi(e) : 0;
    }();
    return env > 0 ? 0 : fgr::base_fps(opts.fps_base);
}
int menu_fps_limit() {
    if (!opts.realtime) return -1;   // (Per frame: the game's steps are whole frames of 1/60 s, the limit does not apply)
    static const int env = [] {   // (as host/nn_vi.c reads them: HOS_FPS_LIMIT, or Hookshot's HOS_FPS_MAX)
        const char* e = getenv("HOS_FPS_LIMIT") ? getenv("HOS_FPS_LIMIT") : getenv("HOS_FPS_MAX");
        return e ? atoi(e) : 0;
    }();
    if (env > 0) return std::clamp(env, 30, 360);
    // (the Base frame rate limit on: the game's own rate is held, the rate shown is that times the multiplier and nothing holds it:
    // Unlimited to the rest of the renderer)
    if (menu_base_fps() > 0) return 0;
    return fgr::limit_fps(opts.fps_limit);
}
bool resolution_auto() { return upscaler_rows::snap(upscaler(), opts.resolution) == 0; }
int resolution_scale() {
    const int drawn = upscaler_rows::snap(upscaler(), opts.resolution);
    {   // (said when it changes: the choice kept and what the Upscaler shown draws at, e.g. 58% kept, 50% drawn by the game's own)
        static int said = -1;
        if (said != drawn * 16 + opts.resolution) {
            said = drawn * 16 + opts.resolution;
            logf("vk: Render resolution: %d%% chosen, drawn at %d%% with Upscaler %d", upscaler_rows::percent(opts.resolution),
                 upscaler_rows::percent(drawn), upscaler());
        }
    }
    const float s = upscaler_rows::percent(drawn) / 100.0f;
    return s > 0 ? (int)(twelfths(s) * 10000.0f / 12.0f + 0.5f) : 0;   // (the twelfth game_size labels)
}

void menu_set_gpu_count(int count) {
    for (Row& r : rows) {
        if (r.value == &opts.gpu_device) {
            r.hi = count;
            if (opts.gpu_device > count) opts.gpu_device = 0;
        }
    }
    restart_init();
    for (int i = 0; i < kRows; i++)   // (the row's value the game started with, the cards counted)
        if (rows[i].value == &opts.gpu_device) restart_state.restart_value(restart_slot[i], opts.gpu_device);
}

bool menu_is_open() { return open_now.load(std::memory_order_relaxed); }
// (HeyListen, after Longshot ddb156df and 79a34381)
bool scene_tonemap_on() { return hdr_game_native() && opts.hdr_tonemap == 1; }   // (RX_VK_HDR_FORCE too: the game is told HDR10)
void psychov35_bank(float* out) {
    namespace pv = Shader::Optimization::PsychoV35;
    const Options& o = opts;
    out[pv::kSettingHueClip] = o.pv35_hue_clip * 0.01f;
    out[pv::kSettingHighlights] = o.pv35_highlights * 0.02f;
    out[pv::kSettingShadows] = o.pv35_shadows * 0.02f;
    out[pv::kSettingContrast] = o.pv35_contrast * 0.02f;
    out[pv::kSettingPurity] = o.pv35_saturation * 0.02f;
    out[pv::kSettingConeExponent] = (o.pv35_match_slope ? pv::kConeExponent : 1.0f) * (o.pv35_tone_slope * 0.02f);
    out[pv::kSettingAnchorScale] = (o.pv35_midgray ? pv::kReferenceExposure : 1.0f) * (o.pv35_exposure * 0.01f);
    // (the mod's Peak and Game brightness are the HDR rows': the peak brightness over the paper white)
    const float white = (float)std::clamp(o.hdr_white, 80, 500);
    out[pv::kSettingPeak] = std::max((float)std::clamp(o.hdr_peak, 400, 10000), white) / white;
    out[pv::kSettingBloom] = o.pv35_bloom * 0.02f;
    // (the mod's UI Brightness: the scene in its units, the encoder scaling by it)
    const float ui = (float)std::clamp(o.pv35_ui_white, 80, 500);
    out[pv::kSettingSceneScale] = white / ui;
    out[pv::kSettingColorSpace] = (float)std::clamp(o.pv35_color_space, 0, 2);
    out[pv::kSettingOutputScale] = ui / 10000.0f;
}
// (the tools off on the Debug tab: flight, the free camera and heart protection put away; -1, heart protection never set, is the
// host's own HOS_OOT_PROTECT_HEARTS)
int menu_heart_protection() {
    const int v = protect_hearts.load(std::memory_order_relaxed);
    return cheats_on() || v < 0 ? v : 0;
}
int menu_player_flight() { return cheats_on() ? player_flight.load(std::memory_order_relaxed) : -1; }
int menu_free_camera() { return cheats_on() ? free_camera.load(std::memory_order_relaxed) : 0; }
int menu_take_cheat_action() { return cheat_action.exchange(0, std::memory_order_relaxed); }

void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h) {
    const auto c = [](uint32_t v) { return (uint64_t)std::min(v, 0xffffu); };
    present_sizes.store(c(picture_w) << 48 | c(picture_h) << 32 | c(window_w) << 16 | c(window_h), std::memory_order_relaxed);
}
std::atomic<bool> pacer_presents{false};   // (the present pacer's thread shows the frames: the overlay's times are its presents')
void menu_pacer_presents(bool on) { pacer_presents.store(on, std::memory_order_relaxed); }
void menu_note_paced_present() {
    if (opts.show_fps) perf_note_frame();   // (the overlay's frame times: vk_perf.cpp)
}
void menu_note_frame(bool game_frame, int shown) {
    if (shown) {
        shown_frames.fetch_add(shown, std::memory_order_relaxed);
        lowend_probe_frame();   // (the Lowest preset measure, while it runs)
        perf_frame_shown();   // (the performance summary's frame, when it is on: perf_report.cpp)
        // (the overlay's frame times, unless the present pacer's thread shows the frames: then at its presents, above)
        if (!pacer_presents.load(std::memory_order_relaxed)) menu_note_paced_present();
    }
    if (game_frame) game_frames.fetch_add(1, std::memory_order_relaxed);
}

// The Fullscreen row and F11 are one setting: the row asks the window's thread (host/display.c), F11 shows in the row
void fullscreen_tick() {
    static int applied = 0, settle = 0;
    const HWND main_window = app_window();
    if (!main_window) return;
    static const bool env = getenv("HOS_FULLSCREEN") != nullptr;   // (a test's own choice: the row is left alone)
    if (env) return;
    if (opts.fullscreen != applied) {
        applied = opts.fullscreen;
        settle = 60;
        PostMessageA(main_window, WM_APP + 7, (WPARAM)applied, 0);
        return;
    }
    if (settle > 0) { settle--; return; }
    const int now = !(GetWindowLongA(main_window, GWL_STYLE) & WS_CAPTION);
    if (now != applied) {
        opts.fullscreen = applied = now;
        menu_refresh = true;
        save();
    }
}

void menu_option_set(int id, int value) {
    if (id >= OOT_CHEAT_ITEM_BASE && id < OOT_CHEAT_ITEM_BASE + kItems) {   // (the game side's inventory counts)
        if (item_counts[id - OOT_CHEAT_ITEM_BASE].exchange(value, std::memory_order_relaxed) != value && page == 4)
            menu_refresh = true;
        return;
    }
    if (id == HL_OPT_PROMPTS_MISSING) {   // (host/prompts.c: no prompts/ folder in the game's)
        prompts_missing.store(value, std::memory_order_relaxed);
        return;
    }
    if (id == perf_parts::kOptPace) {   // (the frame's pacing sleep, host/nvn_native.c's push: the summary's pace part)
        perf_parts::note_pace(value);
        return;
    }
    if (perf_parts::note_option(id, value)) return;   // (v0.4.4: the presenting thread's waits by kind, the cold pages' hold)
    if (id == perf_parts::kOptLoading) {   // (the game loads a map, host/hos_main.c's push: the hitch rows' load side)
        perf_parts::note_loading(value);
        return;
    }
    if (id == HOS_OPT_CHEAT_FLIGHT) {
        if (cheats_on()) {
            int expected = value ? 0 : 1;
            player_flight.compare_exchange_strong(expected, value != 0, std::memory_order_relaxed);
        }
        return;
    }
    if (id == HL_OPT_PROMPTS_LIVE) {   // (host/prompts.c: the parts of the button prompts it can change while the game runs)
        prompts_live.store(value, std::memory_order_relaxed);
        return;
    }
    if (id != 7) return;
    opts.mouse_sens = std::min(std::max(value, 10), 500);   // (the keys work while the menu is closed: it shows it when opened)
    save();
}

/// A list page's line (from the top of the list, headings included) to its item: -1 a section's heading
int list_item(int line, const TeleportSection* sec, int sections) {
    int section = 0;
    while (section + 1 < sections && line >= sec[section + 1].first + section + 1) section++;
    return line == sec[section].first + section ? -1 : line - section - 1;
}
/// (hookshot) the mouse on a list page (the teleport list, the inventory): the item under the cursor chosen, a click on it
/// as Enter (on an amount's left or right half, with amounts, as Left or Right), the wheel scrolls the list (the choice and
/// the amounts stay: *scrolled), a right click goes back
void list_mouse(const MkwVkPointer& ptr, int mx, int my, bool moved, bool amounts, int n, int sections, const TeleportSection* sec,
                int& scroll, int& choice, bool* press, bool& changed, bool& scrolled) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    const int item = hit.part == menu_hit::kListLine ? list_item(scroll + hit.index, sec, sections) : -1;
    if (item >= 0 && (moved || ptr.pressed) && item != choice) choice = item, changed = true;
    if ((ptr.pressed & HOS_MOUSE_LEFT) && item >= 0) {
        if (amounts && item_kinds[item] && hit.side != 0) press[hit.side < 0 ? 3 : 4] = true;
        else press[7] = true;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) {
        const int was = scroll;
        scroll = std::max(0, std::min(scroll - ptr.wheel, n + sections - list_lines));
        scrolled = scroll != was;
    }
}
/// Scrolls the teleport list so the chosen map (and its section heading, at the top) is in view
void teleport_reveal() {
    int section = 0;
    while (section + 1 < kTeleportSections && teleport_choice >= teleport_sections[section + 1].first) section++;
    const int line = teleport_choice + section + 1;
    if (teleport_choice == teleport_sections[section].first) teleport_scroll = std::min(teleport_scroll, line - 1);
    if (line < teleport_scroll) teleport_scroll = line;
    if (line >= teleport_scroll + list_lines) teleport_scroll = line - list_lines + 1;
    teleport_scroll = std::max(0, std::min(teleport_scroll, kTeleports + kTeleportSections - list_lines));
}
/// Scrolls the inventory so the chosen item (and its section heading, at the top) is in view
void item_reveal() {
    int section = 0;
    while (section + 1 < kItemSections && item_choice >= item_sections.s[section + 1].first) section++;
    const int line = item_choice + section + 1;
    if (item_choice == item_sections.s[section].first) item_scroll = std::min(item_scroll, line - 1);
    if (line < item_scroll) item_scroll = line;
    if (line >= item_scroll + list_lines) item_scroll = line - list_lines + 1;
    item_scroll = std::max(0, std::min(item_scroll, kItems + kItemSections - list_lines));
}
/// A debug tool's row on the Debug tab (Row::cheat i): Left and Right, and confirm (press[7]: A, Enter, a click). Teleport and
/// Inventory open their pages; back from them comes to the Debug tab, the row still selected
void cheat_press(int i, const bool* press, bool& changed) {
    if (i < 2 && (press[3] || press[4] || press[7])) {
        if (i == 0) protect_hearts.store(!protect_hearts.load());
        else player_flight.store(player_flight.load() < 0 ? 0 : -1);
        changed = true;
    }
    if (i == 4 && (press[3] || press[4])) {
        clock_hour = (clock_hour + (press[4] ? 1 : 23)) % 24;
        changed = true;
    }
    if (i == 5 && (press[3] || press[4])) {
        pouch_item = (pouch_item + kPouchItems + (press[4] ? 1 : -1)) % kPouchItems;
        changed = true;
    }
    if (i == 6 && (press[3] || press[4])) {
        const int step = press[4] ? 1 : -1;
        pouch_delta = std::clamp(pouch_delta + step, -10, 10);
        if (!pouch_delta) pouch_delta = step;
        changed = true;
    }
    if (press[7] && i >= 2 && i <= 4) cheat_action.store(i == 4 ? 100 + clock_hour : i);
    if (press[7] && i == 6) cheat_action.store(200 + pouch_item * 32 + pouch_delta + 10);
    if ((press[3] || press[4] || press[7]) && i == 7) {   // (an On / Off as the first two: Left and Right flip it too)
        free_camera.store(!free_camera.load());
        changed = true;
    }
    if (press[7] && i == 8) page = 3, teleport_reveal(), changed = true;
    if (press[7] && i == 10) page = 4, item_reveal(), changed = true;
    if (press[7] && i == 9) {
        cheat_action.store(999);   // (host/hos_main.c OOT_AGE_ACTION)
        open_now = false;
        changed = true;
    }
}

void menu_tick() {
    fullscreen_tick();
    {   // (the lists keep the choice in view when the drawing fits another number of lines: a resize, Menu size)
        static int seen = list_lines;
        if (list_lines != seen) seen = list_lines, teleport_reveal(), item_reveal();
    }
    enum { K_N = 18 };
    static bool held[K_N];
    static const int keys[K_N] = {VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1,   // (F1: for keyboards without Insert)
                                  VK_RETURN, VK_DELETE, VK_BACK,
                                  'Q', 'E', VK_TAB,   // (the tabs: Q and E, Tab and Shift+Tab)
                                  VK_PRIOR, VK_NEXT, VK_HOME, VK_END,   // (the long lists: a page, the ends)
                                  VK_F10};   // (HeyListen: F10, the overlay mode, index 17)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool mine = pid == GetCurrentProcessId();
    bool press[K_N] = {};
    for (int i = 0; i < K_N; i++) {
        const bool down = mine && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        const bool queued = host && host->take_key_press && host->take_key_press(keys[i]);
        // Read both keys even if one is disabled, so old presses cannot toggle the menu after a change.
        const bool enabled = (keys[i] != VK_INSERT || opts.menu_shortcut != 2) &&
                             (keys[i] != VK_F1 || opts.menu_shortcut != 1);
        press[i] = enabled && (host && host->take_key_press ? mine && queued : down && !held[i]);
        held[i] = down;
        if (press[i]) pad_last = false;   // (the keys: their names in the prompts)
    }
    // (hookshot) the gamepads, by position: Home, the touchpad's click, or Select and Start together, as Insert; the D-pad as the arrows; the
    // buttons the Switch's A and B are bound to as Enter and Esc, as in the game (vk_menu_keys.h; with the Nintendo layout A
    // is the right face button), not while a binding waits for its button
    static uint32_t pad_held = 0;
    const uint32_t raw = mine && host && host->pad_raw ? host->pad_raw() : 0, pad_press = raw & ~pad_held;
    const uint32_t chord = 1u << HOS_PAD_BACK | 1u << HOS_PAD_START;
    // (the Controls page's Open menu: its input or both of its two, any of the pad's; not while a binding waits for its button)
    uint32_t combo = 0;
    if (host && host->binding)
        for (int k = 0; k < 2; k++)
            if (const int b = host->binding(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, k); b >= 0 && b < HOS_PAD_N) combo |= 1u << b;
    const bool chord_now = ((raw & chord) == chord && (pad_held & chord) != chord) ||
                           (combo && listening != 1 && (raw & combo) == combo && (pad_held & combo) != combo);
    pad_held = raw;
    if (pad_press) pad_last = auto_pad = true;   // (a controller: its buttons in the prompts, and Menu size Auto's choice)
    {   // (RX_VK_UI_PROMPTS=pad or keys: the prompts for screenshots of test runs, which have no controller and no focus)
        static const char* force = getenv("RX_VK_UI_PROMPTS");
        if (force) pad_last = !strcmp(force, "pad");
    }
    auto pad = [&](int p) { return listening != 1 && (pad_press >> p & 1) != 0; };
    if (host && host->binding)   // (A and B as bound now: the Controls page or the Layout preset can change them)
        menu_keys::confirm_back(host->binding(HOS_BIND_PAD, HOS_SW_A, 0), host->binding(HOS_BIND_PAD, HOS_SW_B, 0), &pad_confirm,
                                &pad_back);
    if (pad(HOS_PAD_GUIDE) || (opts.touchpad_menu && pad(HOS_PAD_TOUCHPAD)) || chord_now) press[0] = true;   // (Home, the touchpad's click unless "Touchpad opens menu" is off, Select and Start)
    {   // holding an arrow or the D-pad repeats it: after 0.35 s, 12 a second, then 30 a second after 1.5 s held
        static int64_t held_since[5], next_repeat[5];
        const int pads[5] = {0, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT, HOS_PAD_RIGHT};
        const int64_t t = prof_now();
        for (int i = 1; i <= 4; i++) {
            const bool down = held[i] || (open_now.load() && listening != 1 && (raw >> pads[i] & 1));
            if (!down) { held_since[i] = 0; continue; }
            if (!held_since[i]) { held_since[i] = t, next_repeat[i] = t + 350000000LL; continue; }
            if (t >= next_repeat[i] && open_now.load() && listening == 0) {
                press[i] = true;
                next_repeat[i] = t + (t - held_since[i] > 1500000000LL ? 33000000LL : 83000000LL);
            }
        }
    }
    if (open_now.load() && listening != 1) {
        press[1] |= pad(HOS_PAD_UP), press[2] |= pad(HOS_PAD_DOWN), press[3] |= pad(HOS_PAD_LEFT);
        press[4] |= pad(HOS_PAD_RIGHT), press[5] |= pad(pad_back), press[7] |= pad(pad_confirm);
    }
    {   // RX_VK_MENU_SCRIPT (test runs, which never have the focus): timed menu keys, "<seconds>:<INS|UP|DOWN|LEFT|RIGHT|ESC|ENTER|TABL|TABR>,...";
        // and the gamepad's face buttons by position, PADS PADE PADW PADN (bottom, right, left, top), as a controller's press
        static const char* next = getenv("RX_VK_MENU_SCRIPT");
        static const int64_t start = prof_now();
        double at;
        char key[8];
        int n = 0;
        if (next && sscanf(next, "%lf:%7[A-Z]%n", &at, key, &n) == 2 && (prof_now() - start) / 1e9 >= at) {
            static const char* names[] = {"INS", "UP", "DOWN", "LEFT", "RIGHT", "ESC", "", "ENTER", "", "", "TABL", "TABR"};
            for (int i = 0; i < 12; i++) press[i] |= *names[i] && !strcmp(key, names[i]);
            static const char* pads[] = {"PADS", "PADE", "PADW", "PADN"};
            for (int p = 0; p < 4; p++)
                if (!strcmp(key, pads[p]) && open_now.load() && listening != 1) {
                    press[5] |= p == pad_back, press[7] |= p == pad_confirm;
                    pad_last = auto_pad = true;
                }
            next = next[n] == ',' ? next + n + 1 : nullptr;
        }
    }
    bool tab_left = press[10] || pad(HOS_PAD_LSHOULDER) || pad(HOS_PAD_LTRIGGER);
    bool tab_right = press[11] || press[12] || pad(HOS_PAD_RSHOULDER) || pad(HOS_PAD_RTRIGGER);
    if (press[12] && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) tab_left = true, tab_right = false;
    bool changed = menu_refresh;
    menu_refresh = false;
    if (press[17]) {   // (HeyListen: F10 cycles the overlay Off, FPS, Full; 13-16 are Page Up/Down, Home, End)
        opts.show_fps = (opts.show_fps + 1) % 3;
        menu_refresh = true;
        save();
    }
    // (hookshot) the mouse, while the menu is open: the cursor in the window image's pixels (mx, my), tested against the
    // controls as last drawn (hits, vk_menu_hit.h), whether it moved since the last frame, and its clicks and wheel since then
    MkwVkPointer ptr{};
    int mx = -1, my = -1;
    bool mouse = false, moved = false;
    if (open_now.load() && host && host->pointer) {
        mouse = host->pointer(&ptr) != 0;
        int ix = 0, iy = 0;
        menu_hit::to_image(ptr.x, ptr.y, ptr.width, ptr.height, (int)dev.swap_extent.width, (int)dev.swap_extent.height,
                           &ix, &iy);
        if (mouse) mx = ix - (int)menu_ox, my = iy;   // (the menu's column's pixels: draw_menu)
        static int last_x = -1, last_y = -1;
        moved = mouse && mouse_read && (mx != last_x || my != last_y);   // (not the first read: a cursor resting over the
        last_x = mx, last_y = my, mouse_read = true;                     // menu as it opens leaves the selection alone)
        if (moved) pad_last = false;
        if (!mouse) ptr.pressed = 0, ptr.wheel = 0;
    }
    // (closing with a setting that takes a restart changed: the restart question first, Later already chosen on it)
    auto later = [&] {
        int now[mr::kMax];
        restart_values(now);
        restart_state.choose_later(now);
        open_now = false, page = 0, changed = true;
    };
    if ((press[0] || press[6]) && open_now.load() && page == 5) {
        later();
    } else if ((press[0] || press[6]) && open_now.load() && restart_ask()) {
        page = 5, ask_sel = 0, listening = 0, changed = true;
    } else if (press[0] || press[6]) {
        open_now = !open_now.load(), changed = true;
        page = 0, listening = 0;
        // (the layout row shows the controls as they are: host/nn_hid.c keeps them, the ini's pad_ lines, and a saved
        // layout is not applied over them at start)
        if (open_now.load()) auto_pad = pad_last;   // (Menu size Auto: by what opened the menu)
        if (open_now.load()) opts.pad_layout = detect_pad_layout();
        if (open_now.load()) dlss_probe(), dlss_fg_probe(), fsr4_probe(), fsr4fg_probe();   // (whether the Upscaler row can offer DLSS and FSR here: asked once)
        if (open_now.load()) refresh_mics();   // (the microphones as they are now)
        if (open_now.load()) refresh_outs();   // (heylisten: and the outputs)
    }
    if (open_now.load() && page == 5) {   // (hookshot) the restart question: Later (the default) or Restart now
        if (mouse) {   // (a button under the cursor chosen, a click on it pressed, a right click is Later; no list: the wheel does nothing)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            if (hit.part == menu_hit::kButton && (moved || ptr.pressed) && hit.index != ask_sel) ask_sel = hit.index, changed = true;
            if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kButton) press[7] = true;
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
        }
        if (press[1] || press[2] || press[3] || press[4] || tab_left || tab_right) ask_sel ^= 1, changed = true;
        if (press[5] || (press[7] && ask_sel == 0)) {
            later();
        } else if (press[7]) {   // (Restart now: everything saved, then the window closes as the player would close it)
            logf("menu: restart now, to apply %s", restart_list(", ").c_str());
            save();
            pipeline_cache_save(true);
            open_now = false, page = 0, changed = true;
            PostMessageA(app_window(), WM_APP + 9, 0, 0);
        }
    } else if (open_now.load() && page == 1) {   // (hookshot) the Controls page
        if (!can_bind()) page = 0;
        else if (listening) {
            if (press[5] || prof_now() > listen_end) listening = 0, changed = true;   // (Esc, or nothing for a while)
            else if (listening == 1) {
                // (Home and the touchpad's click open and close the menu: not inputs to bind)
                const uint32_t menu_only = 1u << HOS_PAD_GUIDE | 1u << HOS_PAD_TOUCHPAD;
                static int first = -1;   // (Open menu: the first input pressed, waiting a moment for a second one)
                static int64_t first_at;
                if (!listen_armed) listen_armed = (raw & ~menu_only) == 0, first = -1;
                else if (controls[csel - kTop].button == HOS_BTN_OPEN_MENU) {
                    // (one input, or two pressed together: the second within 0.7 s of the first, the first still held)
                    const uint32_t p = pad_press & ~menu_only;
                    int second = -1;
                    for (int bit = 0; bit < HOS_PAD_N; bit++)
                        if ((p >> bit & 1) && bit != first) {
                            if (first < 0) first = bit, first_at = prof_now();   // (two in one tick: the lower is the first)
                            else if (second < 0) second = bit;
                        }
                    if (first >= 0 && (second >= 0 || !(raw >> first & 1) || prof_now() - first_at > 700000000LL)) {
                        host->bind(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 0, first);
                        host->bind(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 1, second);
                        first = -1;
                        listening = 0, changed = true;
                        save();
                    }
                } else if (const uint32_t p = pad_press & ~menu_only) {
                    int bit = 0;
                    while (!(p >> bit & 1)) bit++;
                    host->bind(HOS_BIND_PAD, controls[csel - kTop].button, 0, bit);
                    listening = 0, changed = true;
                    opts.pad_layout = detect_pad_layout();
                    save();
                }
            } else {
                int down = 0;
                for (int k : bindable_keys())
                    if (mine && (GetAsyncKeyState(k) & 0x8000)) {
                        down = k;
                        break;
                    }
                if (!listen_armed) listen_armed = !down;
                else if (down) {
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 0, down);
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 1, 0);
                    listening = 0, changed = true;
                    save();
                }
            }
        } else {
            if (mouse) {   // (the mouse: the row under it selected, a click on a binding listens for its new input, on
                           // Reset resets; the wheel scrolls the page, a right click goes back)
                const menu_hit::Hit hit = hits.at((float)mx, (float)my);
                const bool option = hit.part == menu_hit::kCtlOption;
                const bool on = option || hit.part == menu_hit::kCtlRow || hit.part == menu_hit::kCtlCell;
                const int at = option ? hit.index : kTop + hit.index;   // (the page's row: its own rows first)
                if (on && (moved || ptr.pressed)) {
                    if (at != csel) csel = at, changed = true;
                    if (hit.part == menu_hit::kCtlCell && !keys_only(hit.index) && !menu_row(hit.index) && ccol != hit.side)
                        ccol = hit.side, changed = true;
                }
                if (ptr.pressed & HOS_MOUSE_LEFT) {
                    if (option) press[hit.side < 0 ? 3 : 4] = true;   // (a value's halves: Left and Right; the name: Right)
                    else if (hit.part == menu_hit::kCtlCell || (hit.part == menu_hit::kCtlRow && hit.index == kControls))
                        press[7] = true;   // (as Enter)
                }
                if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
                if (ptr.wheel) wheel_rows[kControlsSlot] += ptr.wheel, changed = true;   // (scrolls; the selection stays)
            }
            if (press[5]) page = 0, changed = true;
            const int n = kTop + kControls + 1, i = csel - kTop;   // (i: the control's, or reset's)
            if (press[1]) csel = (csel + n - 1) % n, changed = true;
            if (press[2]) csel = (csel + 1) % n, changed = true;
            if (i < 0 && (press[3] || press[4] || press[7])) {   // (the page's own rows: Enter as Right)
                Row& r = rows[page_row_global(csel)];   // (Custom is only what the bindings are: set_row skips it)
                change_row(r, *r.value + (press[3] ? -r.step : r.step), press[3] ? -1 : 1);
                changed = true;
            } else if (i >= 0) {
                if ((press[3] || press[4]) && i < kControls && !keys_only(i) && !menu_row(i)) ccol ^= 1, changed = true;
                if (keys_only(i)) ccol = 1;   // (the stick's and the camera's directions: keys only)
                if (menu_row(i)) ccol = 0;    // (Open menu: gamepad only)
                if (press[7]) {
                    if (i == kControls) {
                        host->bind(HOS_BIND_PAD, -1, 0, 0);
                        opts.pad_layout = 0;
                        save();
                    }
                    else listening = ccol ? 2 : 1, listen_armed = false, listen_end = prof_now() + 6000000000LL;
                    changed = true;
                }
                if ((press[8] || press[9]) && i < kControls) {
                    if (menu_row(i)) {
                        host->bind(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 1, -1);
                        host->bind(HOS_BIND_PAD, HOS_BTN_OPEN_MENU, 0, -1);
                    } else if (ccol == 0) {
                        host->bind(HOS_BIND_PAD, controls[i].button, 0, -1);
                        opts.pad_layout = detect_pad_layout();
                    }
                    else for (int k = 0; k < 2; k++) host->bind(HOS_BIND_KEY, controls[i].button, k, 0);
                    changed = true;
                    save();
                }
            }
        }
    } else if (open_now.load() && page == 4) {   // the inventory
        bool scrolled = false;   // (the wheel: the list moves, the choice stays, so it is not brought back into view)
        if (mouse)
            list_mouse(ptr, mx, my, moved, true, kItems, kItemSections, item_sections.s, item_scroll,
                       item_choice, press, changed, scrolled);
        if (press[5]) page = 0, changed = true;   // (to the Debug tab, Inventory still selected)
        else {
            if (press[1] || press[2]) {
                item_choice = (item_choice + kItems + (press[2] ? 1 : -1)) % kItems;
                changed = true;
            }
            if (press[13] || press[14] || press[15] || press[16]) {   // (Page Up / Page Down, Home / End)
                item_choice = press[15] ? 0 : press[16] ? kItems - 1
                            : std::clamp(item_choice + (press[14] ? 1 : -1) * (list_lines - 2), 0, kItems - 1);
                changed = true;
            }
            if (tab_left || tab_right) {
                int section = 0;
                while (section + 1 < kItemSections && item_choice >= item_sections.s[section + 1].first) section++;
                if (tab_right || item_choice == item_sections.s[section].first)
                    section = (section + kItemSections + (tab_left ? -1 : 1)) % kItemSections;
                item_choice = item_sections.s[section].first;
                changed = true;
            }
            if (press[7]) cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 200);
            else if (press[3] || press[4]) cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 64 + (press[4] ? 1 : -1));
            if (changed) item_reveal();
        }
        changed |= scrolled;
    } else if (open_now.load() && page == 3) {   // the teleport list
        bool scrolled = false;   // (as the inventory's)
        if (mouse)
            list_mouse(ptr, mx, my, moved, false, kTeleports, kTeleportSections, teleport_sections,
                       teleport_scroll, teleport_choice, press, changed, scrolled);
        if (press[5]) page = 0, changed = true;   // (to the Debug tab, Teleport still selected)
        else if (press[7]) {
            cheat_action.store(1000 + teleport_choice);
            open_now = false, page = 0;   // (back to the game: the map change starts with an event)
            changed = true;
        } else {
            if (press[1] || press[2]) {
                teleport_choice = (teleport_choice + kTeleports + (press[2] ? 1 : -1)) % kTeleports;
                changed = true;
            }
            if (press[13] || press[14] || press[15] || press[16]) {   // (Page Up / Page Down, Home / End)
                teleport_choice = press[15] ? 0 : press[16] ? kTeleports - 1
                                : std::clamp(teleport_choice + (press[14] ? 1 : -1) * (list_lines - 2), 0, kTeleports - 1);
                changed = true;
            }
            if (press[3] || press[4] || tab_left || tab_right) {   // (to the first map of the previous or next section)
                int section = 0;
                while (section + 1 < kTeleportSections && teleport_choice >= teleport_sections[section + 1].first) section++;
                const bool back = press[3] || tab_left;
                if (!back || teleport_choice == teleport_sections[section].first)
                    section = (section + kTeleportSections + (back ? -1 : 1)) % kTeleportSections;
                teleport_choice = teleport_sections[section].first;
                changed = true;
            }
            if (changed) teleport_reveal();
        }
        changed |= scrolled;
    } else if (open_now.load()) {
        if (mouse) {   // (the mouse: see the top of this file)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            const int ht = hit.part == menu_hit::kTab ? hit.index : hit.part == menu_hit::kTabPrev ? TAB_COUNT
                         : hit.part == menu_hit::kTabNext ? TAB_COUNT + 1 : -1;
            if (ht != hover_tab) hover_tab = ht, changed = true;
            int& sel = tab_selected[cur_tab];
            const bool on_row = hit.part == menu_hit::kRow || hit.part == menu_hit::kSlider;
            if (drag_row >= 0) {   // (a slider held: it follows the cursor's x until let go)
                Row& d = rows[tab_row_to_global(cur_tab, drag_row)];
                if (!(ptr.held & HOS_MOUSE_LEFT) || !d.value) {
                    drag_row = -1;
                    save();
                } else {
                    const int permille = hits.drag((float)mx, drag_row);
                    const int v = permille < 0 ? *d.value : menu_hit::slider_value(permille, d.lo, d.hi, d.step);
                    if (v != *d.value) change_row(d, v, v > *d.value ? 1 : -1, false), changed = true;
                }
            } else if (on_row && (moved || ptr.pressed) && hit.index != sel) {
                sel = hit.index, changed = true;
            }
            if ((ptr.pressed & HOS_MOUSE_LEFT) && drag_row < 0) {
                if (hit.part == menu_hit::kTabPrev) tab_left = true;
                else if (hit.part == menu_hit::kTabNext) tab_right = true;
                else if (hit.part == menu_hit::kTab && hit.index != cur_tab) cur_tab = hit.index, changed = true;
                else if (hit.part == menu_hit::kSlider) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    drag_row = hit.index;
                    const int v = menu_hit::slider_value(hit.permille, d.lo, d.hi, d.step);
                    if (v != *d.value) change_row(d, v, v > *d.value ? 1 : -1, false);
                    changed = true;
                } else if (hit.part == menu_hit::kRow) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    if (!d.value && d.cheat >= 0 && cheat_chooses(d.cheat) && hit.side != 0)
                        press[hit.side < 0 ? 3 : 4] = true;   // (a tool's choice: its halves as Left and Right)
                    else if (!d.value) press[7] = true;   // (a page, a tool: as Enter)
                    else if (d.hi - d.lo == d.step || hit.side >= 0) press[4] = true;   // (a toggle flips; the right half, or the name, as Right)
                    else press[3] = true;                                               // (the left half as Left)
                }
            }
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;   // (as Esc)
            // (the wheel scrolls the rows, wherever the cursor is; it never changes a value, the selection or the tab: the
            // values change by a click on their arrows or halves, a slider's drag, Left and Right, A and Enter)
            if (ptr.wheel && drag_row < 0) wheel_rows[cur_tab] += ptr.wheel, changed = true;
        }
        if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        if (press[5] && pending) pending = nullptr, changed = true;   // (a Restart or Quit waiting for its second press: cancelled)
        else if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        else if (press[5] && restart_ask()) page = 5, ask_sel = 0, changed = true;   // (the restart question first)
        else if (press[5]) open_now = false;
        // (the tabs: LB and RB, Q and E, or Tab; from the Motion page, as from Controls)
        if ((tab_left || tab_right) && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS;
        if (tab_left) cur_tab = (cur_tab + TAB_COUNT - 1) % TAB_COUNT, changed = true;
        else if (tab_right) cur_tab = (cur_tab + 1) % TAB_COUNT, changed = true;
        const int tab_count = tab_row_count(cur_tab);
        int& selected = tab_selected[cur_tab];
        if (selected >= tab_count) selected = tab_count - 1;   // (a row gone since: the debug tools with them turned off)
        auto heading_at = [&](int k) { return rows[tab_row_to_global(cur_tab, k)].heading; };   // (stepped over)
        if (heading_at(selected)) selected = (selected + 1) % tab_count;
        if (press[1]) {
            do selected = (selected + tab_count - 1) % tab_count;
            while (heading_at(selected));
            changed = true;
        }
        if (press[2]) {
            do selected = (selected + 1) % tab_count;
            while (heading_at(selected));
            changed = true;
        }
        Row& r = rows[tab_row_to_global(cur_tab, selected)];
        const bool confirm_selected = &r == &rows[kQuitRow] || &r == &rows[kRestartRow];
        if (pending && (pending != &r || !mine)) pending = nullptr, changed = true;
        if (press[7] && confirm_selected) {
            if (mine && !press[5]) {
                if (pending != &r) {
                    pending = &r, changed = true;
                } else {   // (as Restart now: saved first, as the window's own close does not force the pipeline cache)
                    save();
                    pipeline_cache_save(true);
                    pending = nullptr;
                    if (&r == &rows[kRestartRow]) {   // (the restart question's Restart now: the window closes, then the game starts again)
                        logf("menu: restart");
                        open_now = false, changed = true;
                        if (const HWND window = app_window()) PostMessageA(window, WM_APP + 9, 0, 0);
                    } else if (const HWND window = app_window()) {
                        PostMessageA(window, WM_CLOSE, 0, 0);
                    }
                }
            }
        } else if (r.cheat >= 0) cheat_press(r.cheat, press, changed);   // (a debug tool: on the Debug tab itself)
        else if (press[7] && is_motion_row(r)) cur_tab = TAB_MOTION, tab_selected[TAB_MOTION] = 0, press[7] = false, changed = true;
        else if (press[7] && is_psychov35_reset_row(r)) psychov35_reset(), save(), press[7] = false, changed = true;
        else if (press[7] && !r.value && can_bind()) page = 1, csel = 0, ccol = 0, changed = true;
        if ((press[3] || press[4] || press[7]) && r.value) {   // (Enter, or A, as Right)
            if (press[7]) press[4] = true;
            change_row(r, *r.value + (press[4] ? r.step : -r.step), press[4] ? 1 : -1);
            changed = true;
        }
        const int64_t now = prof_now();
        if (changed) {   // (a new row or value: its name from the start, after a rest)
            marquee_pos = 0, marquee_next = now + kMarqueeRest;
        } else if (marquee_max > 0 && now >= marquee_next) {
            if (marquee_pos >= marquee_max) marquee_pos = 0, marquee_next = now + kMarqueeRest;   // (back to the start)
            else if (++marquee_pos >= marquee_max) marquee_next = now + kMarqueeRest;            // (rest at the end)
            else marquee_next = now + kMarqueeStep;
            changed = true;
        }
    }
    {   // (hookshot) the window lets the mouse go while the menu is open, and takes it back after (host/display.c)
        static bool told = false;
        const bool now_open = open_now.load();
        if (!now_open) {
            if (drag_row >= 0) save();   // (a slider let go off the window: kept now)
            hover_tab = -1, drag_row = -1, mouse_read = false;
            pending = nullptr;
        }
        if (now_open != told && app_window()) {
            told = now_open;
            PostMessageA(app_window(), WM_APP + 8, (WPARAM)now_open, 0);
        }
    }
    {   // (the full overlay's sampler runs only while it is shown)
        static bool sampling = false;
        const bool want = opts.show_fps == 2;
        if (want != sampling) {
            sampling = want;
            if (want) {
                VkPhysicalDeviceProperties pr{};
                vkGetPhysicalDeviceProperties(dev.physical, &pr);
                VkPhysicalDeviceMemoryProperties mp{};
                vkGetPhysicalDeviceMemoryProperties(dev.physical, &mp);
                uint64_t vram = 0;
                for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
                    if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[i].size;
                PerfGpuId id;   // (the card's sysfs node on Linux: amdgpu_stats.h)
                id.vendor = pr.vendorID, id.device = pr.deviceID;
                if (dev.pci_bus_info) {
                    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
                    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                    p2.pNext = &pci;
                    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
                    id.has_pci = true;
                    id.domain = pci.pciDomain, id.bus = pci.pciBus, id.dev = pci.pciDevice, id.function = pci.pciFunction;
                }
                perf_sampling(true, pr.deviceName, (float)(vram >> 20), &id);
            } else {
                perf_sampling(false, nullptr, -1);
            }
        }
    }
}

void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {
    const bool start = startup_on.load(std::memory_order_acquire);
    const bool fps = opts.show_fps && !start;   // (no game frames yet: not "0 FPS" over the start-up screen)
    const bool open = open_now.load(std::memory_order_relaxed);
    if (!start && !fps && !open) {   // (nothing shows: no ImGui frame, no render pass)
        // (but a new window size or Menu size since the last bake: one frame with nothing drawn that bakes the menu's glyphs
        // and measures its page for the new size, so the next open has no hitch whatever was resized while nothing showed)
        if (prebake_needed(w, h) && ui::begin(w, h)) {
            ui::Ctx c = ui_ctx(w, h);
            prebake(c);
            ui::record(cmd, index, w, h);
            return;
        }
        ui::idle();
        return;
    }
    overlay_prepare();   // (HeyListen: the overlay's font, before the frame)
    if (!ui::begin(w, h)) return;
    {   // (the easing's step: the time since the last drawing, a quarter of the way in 1/60 s)
        static int64_t last = 0;
        const int64_t now = prof_now();
        ease_step = last ? std::min(1.0f, (float)((now - last) / 1e9) * 15.0f) : 1.0f;
        last = now;
    }
    ui::Ctx c = ui_ctx(w, h);
    prebake(c);   // (the menu's glyphs at this size, before it opens: no hitch then)
    if (start) draw_startup(c);   // (under the menu)
    overlay_draw(w, h, fps ? opts.show_fps : 0, open);   // (HeyListen: the performance overlay, F10; the menu over its FPS box)
    if (open) draw_menu(c);
    ui::record(cmd, index, w, h);
}

void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    const bool open = open_now.load(std::memory_order_relaxed);
    const bool fps = opts.show_fps && !startup_on.load(std::memory_order_acquire);
    if (!open && !fps) return;
    overlay_prepare();
    if (!ui::begin(w, h)) return;
    ui::Ctx c = ui_ctx(w, h);
    overlay_draw(w, h, fps ? opts.show_fps : 0, open);   // (HeyListen: the performance overlay)
    if (open) draw_menu(c);
    ui::record_to(cmd, image, view, w, h);
}

} // namespace mkwvk
