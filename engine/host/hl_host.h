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
#define HL_GATE(addr, hash) volatile int hl_gate_on_##addr; int hl_gate_##addr(Ctx *c)

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
