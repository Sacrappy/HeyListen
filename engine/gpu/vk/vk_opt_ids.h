// Copyright (C) 2026 Sacrappy

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

/* The Base frame rate limit row (host/nn_vi.c asks, gpu/vk/vk_replay.cpp answers in frames a second: 30 to 120, 0 when it is Off): the
   game's OWN frame rate, before frame generation. 94 to 99 are perf_parts.h's (the wait kinds), 100 the next free one. */
#define HL_OPT_BASE_LIMIT 100

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

/* The Button prompts row (after Longshot #2366, Gabriel Moisan Matte; host/prompts.c asks, gpu/vk/vk_replay.cpp answers): 0 Auto,
   else HOS_PROMPT_* + 1, the value the row saves as button_prompts=. 46, Longshot's id for it in its vk_api.h, so the two agree.
   A new face shows while the game runs where the game has every part of the prompts from the files host/prompts.c serves. */
#define HL_OPT_PROMPTS 46

/* What a change of the Button prompts row reaches at once, which host/prompts.c pushes to the menu (option_set, whenever it
   changes) as bits: the textures (the ocarina's notes, the Talk ring's letters, the System page's controller), the layouts (the
   Talk ring's places, the ocarina hint's spacing), the icons in texts. The row asks for a restart unless all three are there
   (a mod that replaces one of those files, a build without the prompts, a game not loaded far enough yet). Not Longshot's:
   there the host gives it to the menu in its own call (MkwVkHost.prompts_live), which a change of vk_api.h would put in this
   tree's shader stamp. */
#define HL_OPT_PROMPTS_LIVE 95
enum { HL_PROMPTS_LIVE_TEXTURES = 1, HL_PROMPTS_LIVE_LAYOUTS = 2, HL_PROMPTS_LIVE_TEXTS = 4, HL_PROMPTS_LIVE_ALL = 7 };

/* The Controls tab's Sprint with L3 row (host/nn_hid.c asks at each read of the controllers, host/oot_sprint.c acts;
   gpu/vk/vk_replay.cpp answers 0 off, 1 hold, 2 toggle). 97 is Longshot's id for its Sprint row (HOS_OPT_SPRINT in its vk_api.h); free here. */
#define HL_OPT_SPRINT 97

/* The Controls tab's Button prompts row (host/prompts.c tells at the start, gpu/vk/vk_menu.cpp shows): 1 when the game folder has no
   prompts/ folder (the Builder did not make it: no fontTools or Pillow), so the row changes nothing and the game shows its own
   icons. 96 is free here. */
#define HL_OPT_PROMPTS_MISSING 96
/* The Graphics tab's Handheld profile row (host/nn_applet.c asks at each poll for notifications, gpu/vk/vk_replay.cpp answers 0 docked
   or 1 handheld, -1 while the options are not read yet): a change of the row is told to the game as an undock or a dock. */
#define HL_OPT_HANDHELD 101
/* The Gameplay page's First person row (host/hos_main.c and host/nn_hid.c ask, gpu/vk/vk_replay.cpp answers 0 or 1) */
#define HL_OPT_FIRST_PERSON 102

/* The Gameplay tab's Volume row (after Hookshot 2c87e357; host/nn_audio.c asks as each frame is mixed, gpu/vk/vk_replay.cpp
   answers in percent, 0 to 100). Hookshot's id for it is 44, which is the Skip title screen's here; 94 is free (90 to 93 are the
   rows above). The options file keys it by the row's name ("volume"), so the id is not stored. */
#define HL_OPT_VOLUME 94

/* The Ocarina pitch help row (host/nn_audio_in.c asks, gpu/vk/vk_replay.cpp answers 0 or 1). 45 is free in vk_api.h too. */
#define HL_OPT_MIC_PITCH_HELP 45
