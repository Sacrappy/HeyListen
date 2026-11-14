/* (HeyListen) The game's time step under a frame rate cap above 60: the host's measured frame time at the engine's two
   step divides (after upstream Hookshot a2a89ae11, "keep the game at real speed under a Frame rate cap above 60").

   The engine makes its time step in two places from its integer nominal frame rate (the framework's +0x2c8), each a float
   divide "base / nominal": f_80706be0 (the module system's frame: base 60, 30 in the half-rate mode; the frame's step,
   added to the game clock at +0xd58 and handed to the frame-time object, docs/native-engine/main-loop.md) and f_83b6ea90
   (the game's framework's prepare: the global step 60 / nominal, the float at 0x84d61fc8). The engine's own rate is 60
   over a whole number of display intervals, never above 60, and its controller puts 60 back before the steps are made:
   the old way (the measured rate written into +0x2c8, nn_os_sync.c TICK-014) reached the first divide only, and the
   global step stayed a whole 1/60 s a frame: what reads it ran fps / 60 too fast (cutscenes, voice lines, the Deku Tree
   boss above 60). With the hooks the step is base x the frame's seconds at both, whatever the rate.

   The hooks are lines nx2/hl_gates.py puts after the two divides (HL_STEP_HOOK below: both or neither) in the lifted work
   folder, which every worktree shares: a tree without this file asks for none and takes them out, so it links without
   the two symbols. nn_vi.c publishes the frame's seconds (hos_frame_seconds_bits) while the cap is above 60 and the hook
   is in (hos_step_probe set by the first one), and keeps the nominal rate at 60 at most then. HOS_FPS_STEP=0: as before. */
#include "hl_host.h"

volatile uint32_t hos_frame_seconds_bits = 0;   /* the frame's seconds as a float's bits; 0: the game's own divide */
volatile uint64_t hos_step_probe = 0;           /* f_80706be0's object (its +0xd58: the game clock in 1/60 s) once the hook ran */

HL_STEP_HOOK(80706be0, "9e4c8d7da24dafcf", 8, 1);   /* the frame's step (vreg 8), and the probe */
HL_STEP_HOOK(83b6ea90, "1f02e7327f22f9b9", 0, 0);   /* the global step (vreg 0) */
