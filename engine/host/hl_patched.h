/* (HeyListen native engine) Guest code an exeFS (code) mod patches, and so what no native answer may override.
   The builder (nx2/exefs_mods.py, make_game.py --code-mods) patches the game's modules before the translation and writes the
   game folder's hookshot_code_patches.txt: the guest address ranges the patches changed and the guest functions containing them,
   all in guest addresses after relocation, the form the answers and gates use. At start the OS layer (hl_host.c) reads it, and a
   native answer (HL_ANSWER), entry gate (HL_GATE), step hook (HL_STEP_HOOK), movie hook (HL_MOVIE_HOOK) or lift hook
   (HL_SOUND_HOOK, HL_SOLVER_HOOK, HL_WHOLE_HOOK, HL_HEAP_HOOK, HL_DRIVE_HOOK, HL_LOOP_HOOK, HL_POSE_HOOK, HL_SONG_HOOK) for a patched function is left to the lifted code, which carries the patch.
   Plain C with no OS call, so that a test can run it on made-up lists. */
#ifndef HL_PATCHED_H
#define HL_PATCHED_H
#include <stdint.h>

typedef void (*HlPatchedLog)(const char *line);

/* Replaces the list with the text of a hookshot_code_patches.txt ("range 0xLO 0xHI <module> <mod>", "function 0xLO 0xHI <mod>",
   "#" comments); returns the entries read. NULL or empty text: no patches. */
int hl_patched_parse(const char *text);

/* The name of the mod whose patch touches the function (or range) holding guest, or NULL. */
const char *hl_patched_mod(uint64_t guest);

/* 1 when an exeFS mod patches the function at guest, which is then not to be answered natively: says so through log, one line,
   "hl: <what> 0x<guest> left to the lifted code: an exeFS mod patches it (mods/<name>)". 0 when nothing patches it. */
int hl_patched_left(uint64_t guest, const char *what, HlPatchedLog log);

#endif
