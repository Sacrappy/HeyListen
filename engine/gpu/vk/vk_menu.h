// Copyright (C) 2026 Sacrappy

// The Insert menu (vk_menu.cpp): options that apply while the game runs, kept in hookshot_options.ini beside the executable.
#pragma once

#include "vk_internal.h"

namespace mkwvk {

// (out of the box every feature is off: what the game does on its own. The ini beside the executable keeps a player's choices)
struct Options {
    int framegen = 0;     // made frames: 0 off, 1 Auto (to the display's refresh), or 2 to 8 times the game's frames
    int framegen_by = 0;  // who makes the frames at a fixed multiplier: 0 the renderer's own (AMD FSR 3.1 at 2x, else its block matching), 1 NVIDIA DLSS Frame Generation (vk_dlss.cpp; RTX 40 and newer, with its runtime; up to its runtime's most + 1), 2 AMD FSR 3.1's frame interpolation alone (2x); off by default. Saved as frame_generation_by (framegen_rows.h)
    int pace = 0;         // frames shown on an even line (vk_framegen.cpp)
    int hdr = 0;          // (hookshot) HDR10 out (vk_framegen.cpp framegen_hdr): 0 off, 1 the SDR picture expanded, 2 the game's own (native) HDR (its tone mapper: the hdr_tonemap row); when the display has it (the swapchain made again at once)
    int hdr_white = 200;  // HDR paper white, nits (the picture's white)
    int hdr_peak = 1000;  // HDR peak brightness, nits (its brightest highlights)
    int hdr_tonemap = 0;  // (HeyListen, after Longshot ddb156df) the scene's tone mapper with Native HDR: 0 the game's (Vanilla), 1 PsychoV35,
                          // as RenoDX's zelda-oot-oz (ir_opt/scene_tonemap_pass.h; vk_shader.cpp scene_tonemap_for)
    // (after Longshot 79a34381) PsychoV35's grading, RenoDX's zelda-oot-oz's settings (0 to 100, 50 neutral; Exposure in percent;
    // Midgray 0 Render, 1 Vanilla): the scene's bank each draw, so a change shows at once (psychov35_bank, ir_opt/psychov35.h)
    int pv35_exposure = 100, pv35_highlights = 50, pv35_shadows = 50, pv35_contrast = 50, pv35_saturation = 50;
    int pv35_tone_slope = 50, pv35_hue_clip = 50, pv35_midgray = 1, pv35_match_slope = 1, pv35_bloom = 50;
    // PsychoV35's output, the mod's UI Brightness (nits) and Global Color Space (0 Vanilla, 1 BT.709, 2 DCI-P3): the output
    // encoder's bank (ir_opt/output_encode_pass.h)
    int pv35_ui_white = 203, pv35_color_space = 1;
    int hdr_contrast = 100;   // HDR contrast in percent around mid grey, as RenoDX grades it; 100 none (vk_framegen.cpp framegen_hdr)
    int fps_cap = 0;      // (hookshot) Frame rate cap: 0 60 (the console's), 90, 120, 144, 165, 240 (host/nn_vi.c reads frame_rate_cap= at the start)
    int resolution = 0;   // the 3D's (Render resolution): 0 Auto (the game's dynamic resolution), else a share of the output,
                          // 1..6 = 100%, 75%, 67%, 50%, 58%, 33%, 7 = native: 100% with DLSS's (DLAA) or FSR 4's anti-aliasing
                          // (upscaler_rows.h; nx2/oot_patches.py)
    int output_res = 0;   // the game's whole frame, docked: 0 its 2048x1152, 1 2560x1440, 2 3840x2160, 3 1920x1080, 4 5120x1440; read once, at the start (host/nn_applet.c)
    int upscale = 0;      // to the window (Window scaling filter, saved as upscaling): 0 off (the plain copy), 1 Catmull-Rom, 2 Lanczos
    int reflex = 0;       // NVIDIA Reflex: 0 off, 1 on, 2 on + boost (vk_reflex.cpp; NVIDIA GPUs with VK_NV_low_latency2)
    int dlss = 0;         // the Upscaler (its name from before FSR: options files and the tests' stand-ins keep it): 0 the game's
                          // own network, 1 NVIDIA DLSS, 2 NVIDIA DLAA (an options file from before: the load makes it 1 with the Render
                          // resolution's native choice), 3 AMD FSR 3.1 (vk_fsr3.cpp), 4 AMD FSR 4, 5 its native AA (the same: 4 and
                          // native), 6 Off. upscaler_rows.h; vk_temporal.cpp. Saved as dlss
    int dlss_model = 0;   // DLSS's network for every mode, by its preset: 0 DLSS 4 (K), 1 DLSS 4.5 (M), 2 DLSS 4.5 (L), 3 NVIDIA's default, 4 DLSS 3 (CNN, E) (upscaler_rows.h
                          // model_preset; vk_dlss.cpp). First run on an NVIDIA GPU: menu_first_run_model. RX_VK_DLSS_PRESET over it
    int dlss_pre_bloom = 0;   // (HeyListen, after Longshot 83b161e3) DLAA before the bloom: 0 off (DLAA after the game's bloom
                              // and tone map), 1 on (vk_temporal.cpp pre_bloom_wanted; DLAA only). RX_VK_DLSS_PRE_BLOOM over it
    int sharpness = 0;    // 0 to 100
    int fps_limit = 0;    // the rate presented, the game's frames and the made ones (Real time only): 0 60, 1 90, 2 120, 3 144, 4 165, 5 240, 6 Unlimited
                          // (framegen_rows.h; host/nn_vi.c; HOS_FPS_LIMIT)
    int fps_base = 0;     // the Base frame rate limit: the game's OWN rate before frame generation (framegen_rows.h kBaseFps): 0 Off (the Frame rate limit
                          // alone), 1 30, 2 40, 3 45, 4 48, 5 50, 6 60, 7 72, 8 75, 9 90, 10 120. On: the game runs at it, the frames shown are it times the
                          // multiplier, and the Frame rate limit holds nothing back (menu_fps_limit). Saved as base_frame_rate_limit (host/nn_vi.c; HL_OPT_BASE_LIMIT)
    int realtime = 1;     // game time follows the clock (host/nn_vi.c); 0: per frame (slower frames, slower game)
    int fog = 1;          // the volumetric fog: 1 on, 0 off (its programs left out: lowend_rule.h); read once, at the first dispatch. RX_VK_FOG over it
    int handheld = 0;     // the game's handheld profile: ~4 fps, softer; read once, at the start (host/nn_applet.c)
    int ultrawide = 0;    // the camera at the monitor's shape when it is wider than 16:9; read once, at the start (host/hos_main.c)
    int hud_layout = 0;   // on a screen that is not 16:9: 0 the HUD's corners at the screen's edges (the default), 1 in the middle 16:9 (HL_OPT_HUD_LAYOUT; host/hos_wide_ui.c)
    int show_fps = 0;
    int menu_shortcut = 0;   // 0 Insert or F1, 1 Insert only, 2 F1 only
    int menu_size = 0;   // (hookshot) the overlays' size: 0 Auto, 1 Small, 2 Medium, 3 Large (gpu/vk/vk_ui.h unit)
    int fullscreen = 0;   // borderless, at the monitor's size: F11 is the same setting
    int gpu_device = 0;   // graphics card index: 0 Auto, 1..N device 0..N-1 (gpu/vk/vk_main.cpp; RX_VK_DEVICE)
    int vsync = 0;   // V-Sync (present_mode.h): 0 Off (mailbox, else immediate: as before), 1 On (FIFO, the count below); the swapchain made again at once (RX_VK_FIFO, RX_VK_IMMEDIATE over it)
    int vsync_mode = 1;   // V-Sync mode (present_mode.h), with V-Sync On: 0 FIFO (double buffering) (2 images: less delay, a missed refresh waits a whole one), 1 FIFO (triple buffering) (3: the default, today's On), 2 Mailbox (triple buffering) (MAILBOX, 3 or 4 with frame generation, where the surface lists it); saved as vsync_mode
    int vrr = 0;   // VRR (G-Sync / FreeSync): 0 Off (the default: nothing changes), 1 On, 2 Auto (On in fullscreen when the display lists a variable refresh range): FIFO, a borderless popup window that can flip, the frame rate limit at the refresh - 3 (present_mode.h vrr_resolve)
    int pad_background = 0;   // the controllers count while another window has the focus (host/nn_hid.c; HOS_INPUT_BACKGROUND)
    int sprint = 0;   // (longshot #1929, #2038) "Sprint with L3": the left stick's press runs Link with the game's own dash input (host/oot_sprint.c); 0 off (the default), 1 hold, 2 toggle
    int touchpad_menu = 1;   // (heylisten, issue 120) "Touchpad opens menu": 1 the DualSense / DualShock 4 touchpad click opens and closes this menu (the default), 0 it does not
    int mouse_native = 0; // the mouse turns the camera's own angles (host/mouse_native.c); 0: as the right stick
    int mouse_sens = 100; // the mouse's sensitivity in percent: 0.1 degrees a count at 100 with native look (host/nn_hid.c)
    int mouse_invert = 0; // the mouse's up and down swapped
    int text_speed = 0;   // how fast dialogue prints: 0 the game's, 1 2x, 2 4x, 3 instant (host/nn_vi.c, nx2/oot_patches.py; HOS_TEXT_SPEED)
    int invert_cam_x = 0; // right-stick camera horizontal inversion: 0 normal, 1 inverted
    int invert_cam_y = 0; // right-stick camera vertical inversion: 0 normal, 1 inverted
    int rumble = 100;     // the controllers' rumble in percent of the game's: 0 Off, 25 to 150 (host/pad_sdl.c)
    int stick_dead_zone = 0;  // a stick axis below it is 0: 0 XInput's 7849 (~24%), 1..5 5% to 30% (host/nn_hid.c)
    int pad_layout = 0;   // 0 Nintendo (default), 1 Xbox / PC, 2 Swap Face Buttons, 3 Custom
    int gyro = 1;         // motion controls from a pad's gyroscope (host/pad_sdl.c, host/hos_motion.h); on by default
    int gyro_sens_x = 100;   // the motion aim's horizontal sensitivity in percent
    int gyro_sens_y = 100;   // the motion aim's vertical sensitivity in percent
    int gyro_inv_x = 0;   // the motion aim's horizontal turned around
    int gyro_inv_y = 0;   // the motion aim's vertical turned around
    int gyro_deadzone = 1;   // the motion's deadzone: 0 tight, 1 standard (the console's default), 2 loose
    int prompts = 0;      // the button icons: 0 Auto (the controller), 1 Switch, 2 Xbox, 3 PS4, 4 PS5, 5 Steam Deck, 6 Keyboard (host/prompts.c)
    int tex_low = 0;      // (HeyListen) Texture resolution: 0 High (every level of the game's textures), 1 Low (large ones lose their top level, 4x smaller; vk_texture.cpp, tex_low.h). Read once, at the start; saved as texture_resolution; RX_VK_TEX_LOW over it
    int robust = 0;       // bounds-checked buffer access: 0 Auto (on), 1 Off, 2 On (gpu/vk/vk_main.cpp; RX_VK_ROBUST)
    int descriptor_reuse = 0;  // a draw keeps its descriptors through other slots' registrations (gpu/vk/vk_draw.cpp; RX_VK_DESC_SLOTGEN)
    int sampler_cache = 0;     // sampler ids answer their parameters without the registrations' lock (gpu/vk/vk_draw.cpp; RX_VK_SAMPLER_CACHE)
    int intel_compiles = 0;    // (longshot #252) Intel's shader compiler on Windows: 0 one thread at a time, 1 side by side (gpu/vk/vk_internal.h
                               // IntelDriverGate, vk_draw.cpp intel_compiles_side_by_side; RX_VK_NO_INTEL_SERIAL=1 over it). Read once, at the start
    int preset = 0;       // quality preset: 0 Custom, 1 Lowest (integrated GPU), 2 Minimum (4GB), 3 Low (6GB), 4 Balanced (8GB), 5 High (12GB), 6 Ultra (16GB+)
    int vram_mb = 0;      // dynamic texture cache budget limit in MB
    int mirror_mb = 0;    // staging mirror limit in MB
    int volume = 100;     // (hookshot, 2c87e357) the game's whole sound in percent, 0 to 100, heard at once (host/nn_audio.c, HL_OPT_VOLUME in vk_opt_ids.h)
    int audio_output = 0; // (hookshot) the speakers the game mixes for: 0 Auto (the default output's), 1 Stereo, 2 5.1; read once, at the start (host/nn_audio.c; HOS_AUDIO_CHANNELS)
    int first_person = 0;   // the view from Link's eyes, his head hidden (host/hos_main.c; HL_OPT_FIRST_PERSON)
    int skip_title = 0;   // (longshot) start with the most recent save, pressing through the title and its file list (host/oot_autoload.c); read at the start
    int discord = 1;   // (hookshot) the Discord status (host/discord_rpc.c): 1 on, shown while a Discord client runs on this PC
    int update_check = 1;   // (heylisten) "Check for updates" (host/hos_update.c): 1 the offer at the start (the default), 0 off; saved as update_check=
    int send_perf = 0;    // (heylisten) "Send performance statistics" (gpu/vk/perf_report.cpp): 0 off (the default), 1 on; saved as send_performance_stats=
    int send_crash = 0;   // (heylisten) "Send crash reports automatically" (host/hos_crash_dialog.c): 0 off, the crash window asks (the default), 1 on; send_crash_reports=
    int debug_tools = 0;   // (hookshot) the Debug tab's tools (#785: flight, free camera, teleport, inventory): 0 off; HOS_OOT_CHEATS=1 also turns them on
    int out_dev = 0;      // (heylisten) the Output device row: 0 the system's default, 1.. a device as host->out_name lists it (host/nn_audio.c), saved as output_device= (and its id, output_device_id=)
    int mic = 1;          // (hookshot) the ocarina's microphone: 0 Off, 1 the system's default, 2.. a device as host->mic_name lists it (host/nn_audio_in.c)
};
extern Options opts;
/// (vk_memory.cpp) true when the quality preset's memory budgets (vram_mb, mirror_mb) are to be used: the card has no more
/// video memory than the preset is for. A 4GB preset on an 8 GB card kept 256 MB of mirrors and 2.2 GB of textures: the
/// game's memory was read over PCIe and the textures made again every frame, 5 fps with the CPU idle (Discord, 11-08)
bool preset_caps_memory();
/// The card's video memory: its largest device-local heap (vk_memory.cpp), 0 when not known
uint64_t device_vram_bytes();
/// A first run (no dlss_model in hookshot_options.ini): the DLSS model row starts at the GPU's default (upscaler_rows.h
/// default_model) and is written to the ini at once. NVIDIA GPUs only; a saved value is never changed
void menu_first_run_model(uint32_t vendor_id, uint32_t device_id);
/// (HeyListen, a proposal: RX_VK_TEX_LOW_FIRST_RUN=1, off by default) A first run, with no options file at all: the Texture resolution row starts on
/// Low when the card's video memory is 6 GB or less (device_vram_bytes), saved to the file at once; a saved file's choice is never changed
void menu_first_run_textures();
/// (a test: RX_VK_TEST_VRAM_MB=<n> takes the card's VRAM as at most n MB wherever the renderer sizes something by it: the
/// mirrors' budget, the DLSS model row's small-card preset. With RX_VK_TEST_ALLOWED_MB a large card plays a small one's whole
/// policy. Unset: the card's own)
uint64_t test_vram_bytes(uint64_t heap);

void menu_init();             // (the options from the file, once)
void menu_set_gpu_count(int count);
void menu_tick();             // (each frame shown: the keys, while the game's window has the focus)
bool menu_is_open();
/// The Frame rate limit in frames a second as host/nn_vi.c paces: the rate presented (the game's frames and the made ones), 0 for
/// Unlimited; the row (HOS_FPS_LIMIT over it). -1 in Game speed Per frame: the game's steps are whole frames of 1/60 s there,
/// so the limit does not apply (it needs Real time)
int menu_fps_limit();
/// (the present pacer's thread, HL_PRESENT_PACE=3 / 4) The overlay's frame times are taken where the pictures really go out:
/// menu_pacer_presents(true) while the pacer thread shows the frames (menu_note_frame, called at the hand-over, then leaves the
/// overlay's times alone), menu_note_paced_present() at each of its presents. Before 27 Nov 2026 the overlay timed the hand-overs:
/// with the pacer on its 1% low read lower than without it while 99 % of the pictures went out exactly a period apart
void menu_pacer_presents(bool on);
void menu_note_paced_present();
/// The Base frame rate limit in frames a second: the game's own rate before frame generation, 0 for Off (the row; Game speed Per frame:
/// none, as the Frame rate limit; HOS_FPS_LIMIT stands over it). With it on menu_fps_limit answers 0 (the shown rate is not held)
int menu_base_fps();
/// (HeyListen, after Longshot ddb156df) The game's scene tone mapped by PsychoV35: HDR set to Native on an HDR display, and the Tone
/// mapper row so
bool scene_tonemap_on();
/// Whether the game is told it has an HDR10 display and its PQ picture is taken as it is (the HDR row on Native; RX_VK_HDR_FORCE: no HDR display needed). vk_replay.cpp
bool hdr_game_native();
/// RX_VK_HDR_FORCE=1: the game is told its display is HDR10 although the swapchain is SDR. vk_replay.cpp
bool hdr_forced();
/// (after Longshot 79a34381) PsychoV35's grading as the menu has it now, in ir_opt/psychov35.h's bank order (kSetting*, kSettingCount floats)
void psychov35_bank(float* out);
bool resolution_auto();       // (the game's dynamic resolution: Resolution Auto)
bool fog_runs();              // (the Volumetric fog row, RX_VK_FOG over it: lowend_rule.h; asked at each fog dispatch: the row applies at once)
int menu_handheld_now();      // (the Handheld profile row, 0 or 1; -1 until the options file is read: host/nn_applet.c tells the game of a change)
void menu_first_run_lowend(uint32_t vendor_id, bool integrated, bool mesh_shader);
bool lowend_mirror_not_wait();   // (lowend_rule.h mirror_not_wait for this device: set by menu_first_run_lowend)   // (the Minimum preset and the fog off on a low-end GPU: lowend_rule.h)
int resolution_scale();       // (the fixed path's scale in 1/10000: the Resolution row's, as a twelfth of the output)
void menu_option_set(int id, int value);   // (from the game's side: 7 the mouse's sensitivity, the - and = keys)
int menu_heart_protection();
int menu_player_flight();
int menu_free_camera();
int menu_take_cheat_action();
void menu_note_frame(bool game_frame, int shown = 1);   // (the frame rates the FPS counter shows)
/// The game's picture's size (the Output resolution, its crop) and the window's, at each present: the Window scaling filter
/// row says when it is not needed
void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h);
/// Over the window's image `index` (TRANSFER_DST_OPTIMAL before and after), w x h: the start-up screen, the menu and the FPS
/// counter, drawn with Dear ImGui (vk_imgui.h, vk_ui.h). Nothing recorded while none of them shows
void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h);
/// (headless shots, RX_VK_SHOT_MENU) The menu and the counter over another image: TRANSFER_DST_OPTIMAL before and after,
/// R8G8B8A8_UNORM with colour-attachment use, view a view of it
void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h);
/// (hookshot) The start-up screen while the shaders are prepared (vk_main.cpp, vk_progress.h), drawn by menu_record under the
/// counter and the menu. Null takes it away.
struct StartupView;
void startup_screen(const StartupView* view);

/// (HeyListen) The performance overlay (vk_overlay.cpp): mode 1 FPS, 2 Full, drawn into the menu's Dear ImGui frame (menu_record,
/// between ui::begin and ui::record). overlay_prepare: before the frame begins (its font into the atlas)
void overlay_prepare();
void overlay_draw(uint32_t w, uint32_t h, int mode, bool menu_open);
/// The game image's size the present shows (the overlay's picture lines)
void overlay_note_image(int w, int h);

} // namespace mkwvk
