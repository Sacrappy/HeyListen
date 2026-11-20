/* The Insert menu's scaling rows (vk_menu.cpp), as plain functions: what the Render resolution row steps through for each
   Upscaler, and the names it shows for DLSS's (and FSR 3.1's and FSR 4's) modes. No Vulkan, no menu: tests/test_upscaler_rows.cpp checks them. */
#pragma once

#include <cstdint>

namespace mkwvk::upscaler_rows {

/// RX_VK_PSYCHOV35_DLSS as parsed by env_switch (-1 when unset/empty): compatible Native HDR paths run by default.
/// Explicit zero opts out; the tone mapper, engine support and format guards still decide admission.
inline bool psychov35_compatibility(int setting) { return setting != 0; }

/// The Upscaler row (opts.dlss, RX_VK_DLSS): the game's own network, NVIDIA DLSS, NVIDIA DLAA, AMD FSR 3.1 (built from AMD's
/// shader sources: gpu/vk/vk_fsr3.cpp), AMD FSR 4 and its native anti-aliasing (AMD's library, a build with FSR 4:
/// gpu/vk/vk_fsr4.cpp), and Off: no upscaling network at all, the 3D stretched to the output (vk_draw.cpp off_stand_in). The
/// saved values keep their meaning: each came after the ones before, so a saved 1 or 2 is still DLSS or DLAA, a saved 3
/// FSR 3.1 and a saved 6 Off. The row steps in this order: Off comes last, then the game's own again
enum { GAME = 0, DLSS = 1, DLAA = 2, FSR3 = 3, FSR4 = 4, FSR4_AA = 5, OFF = 6, kUpscalers = 7 };
/// DLAA and FSR 4's native AA are not choices of the Upscaler row any more: they are the Render resolution's native choice
/// (kNativeAa) of DLSS and of FSR 4, with the Upscaler on DLSS or FSR 4. The values 2 and 5 stay as the modes the chain runs
/// (what temporal_mode returns, RX_VK_DLSS=2 and RX_VK_FSR4=2 give) and as the numbers an options file from before saved
/// (migrate)
/// One bit per upscaler that can run here (1 << GAME and 1 << OFF always: Off runs on every GPU): what the row steps through
inline unsigned bit(int upscaler) { return 1u << upscaler; }
/// The DLSS bit, for a caller that knows only whether DLSS runs (its anti-aliasing is the Render resolution's native choice)
inline unsigned dlss_bits(bool dlss_ok) { return dlss_ok ? bit(DLSS) : 0u; }
/// FSR 4's, the same way (fsr4_ok: fsr4_why() == FSR_OK)
inline unsigned fsr4_bits(bool fsr4_ok) { return fsr4_ok ? bit(FSR4) : 0u; }
inline bool is_dlss(int upscaler) { return upscaler == DLSS || upscaler == DLAA; }
inline bool is_fsr4(int upscaler) { return upscaler == FSR4 || upscaler == FSR4_AA; }
/// No upscaling network. The Render resolution row applies as with the game's own (the 3D is drawn at that share), and so does
/// Sharpness (it works on the present, whatever made the picture)
inline bool is_off(int upscaler) { return upscaler == OFF; }
/// The engines that take Native PsychoV35's RGBA16F scene unless RX_VK_PSYCHOV35_DLSS=0 (vk_temporal.cpp temporal_scene_format):
/// those the frame chain gives linear light in HDR: DLSS / DLAA, FSR 4 / its native AA and FSR 3.1 on the shared chain (HeyListen: while PsychoV35 is on, vk_fsr3.cpp fsr3_on_chain).
/// FSR 3.1's legacy own-input route is not a chain mode; the game's network and Off are separate paths.
inline bool takes_psychov35(int upscaler) { return is_dlss(upscaler) || is_fsr4(upscaler) || upscaler == FSR3; }
/// The 3D drawn at full size, the upscaler anti-aliasing alone (DLAA, FSR 4's native AA): the modes the chain runs for the
/// Render resolution's native choice
inline bool full_scale(int upscaler) { return upscaler == DLAA || upscaler == FSR4_AA; }
/// The Upscaler row's choice for a mode the chain runs: DLAA is DLSS drawing at the native choice, FSR 4's native AA is FSR 4
inline int engine(int mode) { return mode == DLAA ? DLSS : mode == FSR4_AA ? FSR4 : mode; }
/// FSR 3.1 and FSR 4 scale from a smaller 3D at the shares DLSS's modes use: Quality 1.5x (67%), Balanced 1.7x (58%),
/// Performance 2x (50%) and Ultra Performance 3x (33%) per axis, AMD's own ratios too
inline bool scales_by_mode(int upscaler) { return upscaler == DLSS || upscaler == FSR3 || upscaler == FSR4; }

/// opts.resolution: 0 Auto (the game's dynamic resolution), else a share of the output in percent. 1 to 4 are the values from
/// before (100, 75, 67, 50: saved options keep their meaning); 5 and 6 came with DLSS's Balanced and Ultra Performance. Each
/// is a twelfth of the output (vk_menu.cpp twelfths): 8/12, 7/12, 6/12 and 4/12 are DLSS's own ratios. 7 is the native choice,
/// kNativeAa, which came with DLAA and FSR 4's native AA leaving the Upscaler row: the 3D at 100% with the upscaler's
/// anti-aliasing (DLAA with DLSS, Native AA with FSR 4), offered with those two only
constexpr int kNativeAa = 7;
constexpr int kPercent[] = {0, 100, 75, 67, 50, 58, 33, 100};
constexpr int kValues = sizeof kPercent / sizeof kPercent[0];
inline int percent(int v) { return v > 0 && v < kValues ? kPercent[v] : 0; }
/// The Upscalers with a native choice in their Render resolution: DLSS (DLAA) and FSR 4 (its Native AA)
inline bool has_native_aa(int upscaler) { return engine(upscaler) == DLSS || engine(upscaler) == FSR4; }
/// The mode the chain runs for the Upscaler row's choice and the Render resolution's value: DLAA for DLSS at the native choice,
/// FSR 4's native AA for FSR 4 at it; the choice itself otherwise
inline int mode_for(int upscaler, int resolution) {
    if (resolution == kNativeAa && engine(upscaler) == DLSS) return DLAA;
    if (resolution == kNativeAa && engine(upscaler) == FSR4) return FSR4_AA;
    return upscaler;
}
/// An options file from before: a saved Upscaler of DLAA (2) or FSR 4's native AA (5) is DLSS or FSR 4 with the native choice
/// as the Render resolution (what it ran: the Render resolution row did not apply to them). Returns whether it changed them
inline bool migrate(int& upscaler, int& resolution) {
    if (upscaler == DLAA) return upscaler = DLSS, resolution = kNativeAa, true;
    if (upscaler == FSR4_AA) return upscaler = FSR4, resolution = kNativeAa, true;
    return false;
}

/// The steps the Render resolution row takes, best first after Auto. DLSS: its native choice (DLAA) and Quality, Balanced,
/// Performance and Ultra Performance; Auto too, since DLSS follows the game's dynamic size (vk_dlss.cpp makes its feature again
/// when the size leaves the mode's range; FSR 4's context takes any size up to the output). FSR 4 the same, its native
/// choice being its Native AA. FSR 3.1: the modes alone
constexpr int kGameSteps[] = {0, 1, 2, 3, 4};
constexpr int kDlssSteps[] = {0, 3, 5, 4, 6};
constexpr int kNativeSteps[] = {0, kNativeAa, 3, 5, 4, 6};
inline const int* steps(int upscaler, int& n) {
    if (has_native_aa(upscaler)) return n = sizeof kNativeSteps / sizeof kNativeSteps[0], kNativeSteps;
    if (scales_by_mode(upscaler)) return n = sizeof kDlssSteps / sizeof kDlssSteps[0], kDlssSteps;
    return n = sizeof kGameSteps / sizeof kGameSteps[0], kGameSteps;
}

/// DLSS's name for a Render resolution value: nullptr for Auto, or a share that is not one of its modes
inline const char* dlss_mode_name(int v) {
    switch (percent(v)) {
    case 67: return "Quality";
    case 58: return "Balanced";
    case 50: return "Performance";
    case 33: return "Ultra Performance";
    default: return nullptr;
    }
}

/// The Render resolution row's next value from v (dir +1 Right, -1 Left), wrapping. A value that is not one of the upscaler's
/// steps (one saved with another upscaler) moves to its neighbour by size
inline int step(int upscaler, int v, int dir) {
    int n = 0;
    const int* s = steps(upscaler, n);
    for (int i = 0; i < n; i++)
        if (s[i] == v) return s[(i + (dir > 0 ? 1 : n - 1)) % n];
    // (not a step: where it would sit, Auto first and then larger shares first)
    const int p = percent(v);
    int pos = 1;
    while (pos < n && percent(s[pos]) > p) pos++;
    return dir > 0 ? s[pos % n] : s[pos - 1];
}

/// v as one of the upscaler's steps: itself when it is one, else the step nearest in size (the larger on a tie). Auto stays
/// Auto. The native choice is a step of DLSS and FSR 4 only, and is never the nearest step of a share: 100% (1) of the game's
/// own becomes Quality with DLSS, not DLAA (a preset's 100% would otherwise turn DLAA on)
inline int snap(int upscaler, int v) {
    if (percent(v) == 0) return 0;
    int n = 0;
    const int* s = steps(upscaler, n);
    int best = v, best_d = 1000;
    for (int i = 0; i < n; i++) {
        if (s[i] == v) return v;
        if (!percent(s[i]) || s[i] == kNativeAa) continue;
        const int d = percent(s[i]) > percent(v) ? percent(s[i]) - percent(v) : percent(v) - percent(s[i]);
        if (d < best_d || (d == best_d && percent(s[i]) > percent(best))) best = s[i], best_d = d;
    }
    return best;
}

/// The Upscaler row's next value from v (dir +1 / -1), wrapping over the choices there are (available: bit() of each that
/// runs here; the game's own and Off always do). A value that cannot run here steps as the game's own does. DLAA and FSR 4's
/// native AA are not stops (a mode the chain runs is its engine's choice: engine)
inline int step_upscaler(int v, int dir, unsigned available) {
    available |= bit(GAME) | bit(OFF);
    available &= ~(bit(DLAA) | bit(FSR4_AA));
    v = v >= 0 && v < kUpscalers ? engine(v) : v;
    int at = v < 0 || v >= kUpscalers || !(available & bit(v)) ? GAME : v;
    for (int i = 0; i < kUpscalers; i++) {
        at = (at + (dir > 0 ? 1 : kUpscalers - 1)) % kUpscalers;
        if (available & bit(at)) return at;
    }
    return GAME;
}
/// The same, where the choice is DLSS only when dlss_ok (a build with NVIDIA's SDK, on a GPU DLSS runs on)
inline int step_upscaler(int v, int dir, bool dlss_ok) { return step_upscaler(v, dir, bit(GAME) | dlss_bits(dlss_ok)); }

/// The DLSS model row (opts.dlss_model, saved as dlss_model): which of NVIDIA's networks DLSS and DLAA draw with, for EVERY
/// mode and render size and on any RTX card, by its preset (NVSDK_NGX_DLSS_Hint_Render_Preset_*): 0 DLSS 4, the first
/// transformer, preset K; 1 DLSS 4.5, the second generation transformer, preset M; 2 DLSS 4.5, preset L; 3 NVIDIA's default,
/// preset 0 (the runtime picks per mode, and a runtime update can change it); 4 DLSS 3, NVIDIA's last CNN model, preset E (older,
/// lighter on video memory and GPU time). The saved numbers keep their meaning from before DLSS 4.5 was named (the CNN is a
/// new number: an old file is unchanged). Nothing overrides a chosen model: not the mode, not the GPU
enum { MODEL_K = 0, MODEL_M = 1, MODEL_L = 2, MODEL_DEFAULT = 3, MODEL_CNN = 4, kModels = 5 };
/// Preset E (vk_dlss.cpp checks the number against the SDK's)
constexpr unsigned kPresetE = 5;
/// Presets of NVIDIA's transformers (K = DLSS 4; M and L = DLSS 4.5)
constexpr unsigned kPresetK = 11, kPresetL = 12, kPresetM = 13;
/// Whether the GPU has FP8 tensor cores, the reason DLSS 4.5 (M and L) costs RTX 20 and 30 more: an NVIDIA GPU (vendor 0x10de)
/// of the Ada generation or later. NVIDIA's PCI device ids run by generation: Turing 0x1e00 to 0x21ff, Ampere (GA10x) 0x2200
/// to 0x25ff, Ada (AD10x) 0x2600 to 0x28ff, Blackwell 0x2900 and up (an RTX 4090 is 0x2684, a 3090 0x2204, a 3050 0x2584, a
/// 5090 0x2b85), so 0x2600 is the line. The device id is the one Vulkan reports (VkPhysicalDeviceProperties)
inline bool fp8_gpu(uint32_t vendor_id, uint32_t device_id) { return vendor_id == 0x10de && device_id >= 0x2600; }
/// The model a first run starts with, by the GPU (only until the player has one in hookshot_options.ini; menu_first_run_model
/// writes it there at once): with less than 7 GB of video memory (vram: device_vram_bytes(), 0 when not known: not) the CNN, on any
/// RTX (less video memory and GPU time, the same picture at Performance: an RTX 3050 6 GB, 1080p, 1.55 ms less and 27 MB less of
/// NGX's memory); else DLSS 4 (K) on RTX 20 and 30, which NVIDIA says pay more for DLSS 4.5; DLSS 4.5 (M) on RTX 40 and 50
inline int default_model(uint32_t vendor_id, uint32_t device_id, uint64_t vram = 0) {
    return vram && vram < (7ull << 30) ? MODEL_CNN : fp8_gpu(vendor_id, device_id) ? MODEL_M : MODEL_K;
}
/// The row's preset
inline unsigned model_preset(int model) {
    switch (model) {
    case MODEL_M: return kPresetM;
    case MODEL_L: return kPresetL;
    case MODEL_DEFAULT: return 0;
    case MODEL_CNN: return kPresetE;
    default: return kPresetK;   // (K, also for a value past the row)
    }
}
/// The preset DLSS is asked for: RX_VK_DLSS_PRESET over the row (env: its value, or nullptr), a letter (A = 1 ... Z = 26,
/// either case) or 0 for NVIDIA's default; anything else leaves the row's
inline bool preset_env(const char* env) {
    return env && ((*env >= 'A' && *env <= 'Z') || (*env >= 'a' && *env <= 'z') || (env[0] == '0' && !env[1]));
}
inline unsigned dlss_preset(int model, const char* env) {
    if (!preset_env(env)) return model_preset(model);
    if (*env == '0') return 0;
    return (unsigned)(*env >= 'a' ? *env - 'a' + 1 : *env - 'A' + 1);
}
/// A preset's letter for the log ('A' + preset - 1), '0' for NVIDIA's default
inline char preset_letter(unsigned preset) { return preset ? (char)('A' + preset - 1) : '0'; }

/// The Upscaler row is in every build and on every GPU (it always has the game's own and Off): no shown or fixed rule for it.
/// Rows with nothing else to choose here (shown without arrows; Left and Right leave the saved value alone): the DLSS model
/// while DLSS is not drawing. The Render resolution always has steps (DLAA and FSR 4's native AA are among them)
/// The DLSS model row: shown while NVIDIA DLSS or DLAA is the Upscaler (a build with DLSS); hidden with any other, where it would
/// have nothing to say
inline bool model_shown(bool built, int upscaler) { return built && is_dlss(upscaler); }
/// (env_set: RX_VK_DLSS_PRESET names a preset, which wins over the row for this run)
inline bool model_fixed(int upscaler, bool env_set) { return !is_dlss(upscaler) || env_set; }
/// (HeyListen, after Longshot 83b161e3) The DLAA before the bloom row (opts.dlss_pre_bloom): shown with NVIDIA DLSS chosen where
/// it runs (DLSS or DLAA; hidden with any other Upscaler), and changeable with DLAA alone: in DLSS's other modes the scene, the
/// bloom and the tone map are drawn smaller than the output DLSS draws, so its picture cannot go back into the scene
/// (vk_temporal.cpp temporal_pre_bloom). The Upscaler row shows DLSS for DLAA too (engine), so the rule takes the mode the chain
/// runs (temporal_mode: DLAA is DLSS at the Render resolution's native choice, mode_for), never the row's choice
inline bool pre_bloom_shown(bool dlss_ok, int mode) { return dlss_ok && is_dlss(mode); }
/// (env_set: RX_VK_DLSS_PRE_BLOOM is set, and wins over the row for this run; parallel: RX_VK_PARALLEL=2 or more, whose replay
/// threads record a frame's passes out of order, so DLAA stays after the bloom)
inline bool pre_bloom_fixed(int mode, bool env_set, bool parallel = false) { return mode != DLAA || env_set || parallel; }
/// The Window scaling filter does something only when the window's size is not the game's picture's (the Output resolution);
/// a size not known yet (0) counts as different
inline bool filter_needed(unsigned picture_w, unsigned picture_h, unsigned window_w, unsigned window_h) {
    return !picture_w || !window_w || picture_w != window_w || picture_h != window_h;
}

} // namespace mkwvk::upscaler_rows
