#pragma once
/* (HeyListen) Runtime option ids that are ours alone, in a header of their own: gpu/vk/vk_api.h is part of the shader stamp
   (nx2/build.py), and a change there makes every player prepare shaders again. The ids upstream uses (Hookshot's and
   Longshot's, in vk_api.h and the renderer's option table) are left alone: 10 is upstream's Text size, 60 Longshot's item
   wheel. The options file is keyed by row name, so the id is not stored anywhere. */

/* The Frame rate limit row (host/nn_vi.c asks, gpu/vk/vk_replay.cpp answers in frames a second: 60, 90, 120, 144, 165, 240; 0
   for Unlimited). The limit is the rate presented: the game's frames and the frame generation's made ones. */
#define HL_OPT_FPS_LIMIT 90

/* The multiplier fixed frame generation runs at now (host/nn_vi.c asks, gpu/vk/vk_replay.cpp answers: 2 to 8, or 1 for none made:
   Off, Auto, or it is not running). The game's own rate is the limit over it. */
#define HL_OPT_FRAMEGEN_MULT 91

/* The VRR row's cap (host/nn_vi.c asks, gpu/vk/vk_replay.cpp answers in frames a second): the display's refresh - 3 while VRR is
   on, 0 when it is off or the refresh is not known. A limit above 60 is held to it, whether it comes from the row or HOS_FPS_LIMIT.
   90 is the Frame rate limit's and 91 the frame generation multiplier's on this tree, 92 the V-Sync mode row's, so 93: free here */
#define HL_OPT_VRR_CAP 93

/* The Skip title screen row (host/oot_autoload.c asks, gpu/vk/vk_replay.cpp answers 0 or 1). 44 is the id Longshot's change
   of the same row uses in vk_api.h (free in this tree: the ids there run 32 to 35, 40 to 43), so the two agree. */
#define HL_OPT_SKIP_TITLE 44

/* The Graphics tab's HUD layout row (longshot #1133; host/hos_wide_ui.c asks at each present, gpu/vk/vk_replay.cpp answers):
   on a screen that is not 16:9, 0 the HUD's corners at the screen's edges, 1 the HUD in the middle 16:9 as before
   host/hos_wide_ui.c (on a narrower screen fitted to its width, as the menus). HOS_WIDE_UI, when set, stands over it. 71 is
   Longshot's id for it; free here (90 is the Frame rate limit's). */
#define HL_OPT_HUD_LAYOUT 71

/* The V-Sync mode row (gpu/vk/vk_replay.cpp answers): 0 FIFO (double buffering), 1 FIFO (triple buffering), 2 Mailbox. A new
   id, after the VRR row's (on feat/vsync-buffering's base: 90, 91; here 93, the VRR cap moved past it); nothing renumbered. */
#define HL_OPT_VSYNC_MODE 92

/* The HDR tone mapper (after Longshot ddb156df, #2481): HDR set to Native with its Tone mapper row on PsychoV35 (vk_menu.cpp
   scene_tonemap_on; gpu/vk/vk_replay.cpp answers it). host/nn_vi.c makes the scene buffer RGBA16F only then, so Native HDR with the
   row on Vanilla keeps the game's R11G11B10F and the temporal upscalers, which take only that scene. 47, Longshot's id for it in
   its vk_api.h, so the two agree. */
#define HL_OPT_HDR_TONEMAP 47

/* The Gameplay page's First person row (host/hos_main.c and host/nn_hid.c ask, gpu/vk/vk_replay.cpp answers 0 or 1). 120: well clear
   of the 90s, which the frame rate rows use (90 to 93) */
#define HL_OPT_FIRST_PERSON 120
