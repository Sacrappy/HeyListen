/* (HeyListen native engine) What the OS layer's files for the native modules share (host/hl_host.c): how a native module
   answers a guest function at its address, and what the native side (engine/native, reached only through hl/c/) is given
   to work in the guest's memory and on its threads. One file a module: hl_jobs.c, and the next ones beside it. */
#ifndef HL_HOST_H
#define HL_HOST_H
#include "hos.h"
#include "hl/c/hl_bridge.h"

/* The guest function at `guest` is answered by `fn` from the first time the game looks it up, when on() says 1 (asked
   once, then; NULL: always). For functions the game reaches through a pointer (a job's body, a callback, a virtual
   function): the runtime's lookup asks here before the lifted table (a64_lookup_replaced), so no lifted code changes.
   A function the game calls directly gets an entry gate in the lifted code instead (nx2/hl_gates.py), which calls the
   same kind of function. Call it from a constructor (HL_ANSWER below): the list is closed when the game starts. */
typedef int (*HlOn)(void);
void hl_answer(uint64_t guest, A64Fn fn, HlOn on, const char *what);
#define HL_ANSWER(guest, fn, on, what) \
    __attribute__((constructor)) static void hl_answer_##fn(void) { hl_answer((guest), (fn), (on), (what)); }

/* HL_GATE(80ba6d80, "<hash>") { ...; return 1; }: answers a guest function the game calls directly. nx2/hl_gates.py reads
   these lines and puts an entry gate at the head of the lifted f_80ba6d80, when its lifted text hashes as given (the text
   the native code was written against; `python -I nx2/hl_gates.py <gen> --hash 80ba6d80` prints it). The body is
   int hl_gate_80ba6d80(Ctx *c): the function's arguments are in c as at its entry; return 1 with its results in c (x0,
   memory) when it answered, 0 to let the translated body run. It is called only while hl_gate_on_80ba6d80 is 1: set it
   when the module's switch is on and hl_supported_build() says 1. */
#define HL_GATE(addr, hash) \
    __attribute__((constructor)) static void hl_gate_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    volatile int hl_gate_on_##addr; int hl_gate_##addr(Ctx *c)

/* HL_STEP_HOOK(80706be0, "<hash>", <vreg>, <probe>); a line nx2/hl_gates.py puts after f_80706be0's one float divide of a
   time step, making it base x hos_frame_seconds_bits when that is not 0 (host/hl_step.c). Nothing in C. */
#define HL_STEP_HOOK(addr, hash, reg, probe) \
    __attribute__((constructor)) static void hl_step_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_step_hook_##addr

/* HL_MOVIE_HOOK(83b7c710, "<hash>", <offset>); a line nx2/hl_gates.py puts after f_83b7c710's "+1" of the count at
   +<offset> of the movie player (x20), making the step hos_movie_frames(player) (host/hl_step.c). Nothing in C. */
#define HL_MOVIE_HOOK(addr, hash, offset) \
    __attribute__((constructor)) static void hl_movie_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_movie_hook_##addr

/* HL_SOUND_HOOK(80435300, "<hash>"); HL_SOLVER_HOOK(838ceb80, "<hash>"): lift hooks (nx2/hl_gates.py) for systems that need
   whole 60 fps frames above a 60 cap: the sound manager's job held to 60 Hz (its head and its load of the game step), the
   physics solver's step count rounded up (after its cut). host/hl_high_fps.c has the host side. Nothing in C. */
#define HL_SOUND_HOOK(addr, hash) \
    __attribute__((constructor)) static void hl_sound_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_sound_hook_##addr
#define HL_SOLVER_HOOK(addr, hash) \
    __attribute__((constructor)) static void hl_solver_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_solver_hook_##addr

/* HL_BGM_HOOK(83373400, "<hash>"): a lift hook (nx2/hl_gates.py) in the music manager's update (inside the sound job): its clock
   counts whole 60 Hz steps and the change flags the sound gate held back reach it (host/hl_high_fps.c). Nothing in C. */
#define HL_BGM_HOOK(addr, hash)     __attribute__((constructor)) static void hl_bgm_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); }     extern const int hl_bgm_hook_##addr

/* HL_TURN_HOOK(8263e8e0, "<hash>"); HL_HORSE_HOOK(82c59eb0, "<hash>"); HL_FLOOR_HOOK(80159dc0, "<hash>", "<label>"): lift hooks
   (nx2/hl_gates.py) for three places that break above a 60 cap: Gohma's steering rate (Longshot 3ab8cae6), Epona's jump blend
   (8e8a06db) and a moving floor's carry, the Shadow Temple's barge (4f755527). host/hl_high_fps.c has the host side. Nothing in C. */
#define HL_TURN_HOOK(addr, hash) \
    __attribute__((constructor)) static void hl_turn_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_turn_hook_##addr
#define HL_HORSE_HOOK(addr, hash) \
    __attribute__((constructor)) static void hl_horse_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_horse_hook_##addr
#define HL_FLOOR_HOOK(addr, hash, label) \
    __attribute__((constructor)) static void hl_floor_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_floor_hook_##addr

/* HL_HEAP_HOOK(804566e0, "<hash>"): a lift hook (nx2/hl_gates.py) around f_804566e0's four allocations that are not checked
   (two CPU buffers, two GPU-memory calls): a failed one is tried again in the heap's parents (host/hl_heap_retry.c). Both
   pairs of lines or none. Nothing in C. */
#define HL_HEAP_HOOK(addr, hash) \
    __attribute__((constructor)) static void hl_heap_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_heap_hook_##addr

/* HL_WHOLE_HOOK(80a87e50, "<hash>", <reg>, "<key>"[, "u" | "u<N>"]); a lift hook (nx2/hl_gates.py) after the function's cut of an amount of
   the frame step to whole frames into x<reg>, making the count hos_frame_whole(<key>, amount, count) (host/hl_step.c): the
   fractions carried per counter above a 60 cap ("u": the cut is unsigned; "u<N>": the unsigned cut that the addition to
   x<N> follows). Nothing in C. */
#define HL_WHOLE_HOOK(addr, hash, reg, key, ...) \
    __attribute__((constructor)) static void hl_whole_decl_##addr(void) { hl_gate_declared(0x##addr##ULL); } \
    extern const int hl_whole_hook_##addr

/* Whether the gates are in the lifted code: nx2/hl_gates.py puts, beside each gate it puts in, a constructor that calls
   hl_gate_present(address) at start; a gate left out (its function's text hashes otherwise in this lift) never says so.
   hl_gates_in: 1 when every one of the n addresses is in; otherwise 0, and one log line naming `what` and the missing
   ones. A module whose gates share state (half native, half translated otherwise) switches them on only together. */
void hl_gate_present(unsigned long long guest);

/* exeFS (code) mods (hl_patched.h): every gate and step hook declares its function here, from a constructor, so that the start can
   say which of them an exeFS mod patches (the builder left those out of the lifted code: nx2/hl_gates.py skip). A native answer
   (HL_ANSWER) for a patched function is not installed: hl_answer's lookup leaves it to the lifted code. */
void hl_gate_declared(unsigned long long guest);
int hl_gates_in(const char *what, const uint64_t *guest, int n);

/* The translated function at a guest address (the lifted table's, whatever answers the address now); NULL when there is
   none. For a module that runs the translation itself, beside its native answer (a check, a fallback). */
A64Fn hl_translated(uint64_t guest);

/* 1 when the game is the build the native engine's guest addresses are of. Any other game or update runs as before. */
int hl_supported_build(void);

/* The callbacks the native side is given (hl/c/hl_bridge.h): guest memory, the calling thread's guest thread, a guest
   call on a guest thread's own context, host threads fit to run guest code. */
const hl_guest_host *hl_host(void);

/* Calls the guest function fn(a0, a1) on the calling host thread as guest thread `as` (NULL: the thread it already is),
   on that thread's own guest context. x0 after the call is the result. */
uint64_t hl_guest_call(HosThread *as, uint64_t fn, uint64_t a0, uint64_t a1);

#endif
