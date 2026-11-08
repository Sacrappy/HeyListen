/* The C ABI between the recompiled game (mkw_game.dll: lifted code + a64-kit's runtime + eden_shim.c) and the
   emulator that hosts it (Eglantine's ArmRecomp backend, eglantine/src/core/arm/recomp/).

   The DLL knows nothing about Eden: every way out of lifted code (SVC, interrupt poll, trap, an indirect call to an
   address with no lifted function, the counter) goes through the MkwHost table the backend hands to
   mkw_recomp_attach. The backend knows nothing about the lifted code beyond this header: it looks functions up by
   guest address and calls them with a context.

   Guest memory is the 64-bit window at A64_HOST_BASE (nx2/rtgen.py): the backend must have placed the process's
   fastmem arena there (attach checks it). */
#ifndef MKW_HOST_H
#define MKW_HOST_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MKW_HOST_VERSION 1

/* The lifted code's context (a64-kit rt/a64.h GuestContext with nx2/rtgen.py's poll flag), as the host sees it.
   eden_shim.c checks every offset against the real one at compile time. */
typedef struct MkwGuestContext {
    uint64_t x[32];             /* x[31] is SP */
    uint64_t pc;                /* written where a thread stops (traps); otherwise the thread's start address */
    uint8_t n, z, c, v;
    int32_t halted;
    uint64_t vreg[32][2];
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    uint64_t fpcr, fpsr;
    uint64_t excl_addr, excl_lo, excl_hi;
    uint32_t excl_size;
    int32_t excl_valid;
    void *thread;               /* the host's per-thread object */
    uint32_t poll;              /* interrupt request: the next loop header calls MkwHost::poll */
} MkwGuestContext;

struct GuestContext;
typedef void (*MkwFn)(struct GuestContext *c);

typedef struct MkwHost {
    uint32_t version;   /* MKW_HOST_VERSION */
    uint32_t ctx_size;  /* sizeof(GuestContext) as the host sees it */
    void *user;
    /* SVC #imm: the context holds the arguments; returns when the thread runs again, results in the context */
    void (*svc)(void *user, struct GuestContext *c, uint32_t imm);
    /* c->poll was set (an interrupt): returns when the thread runs again */
    void (*poll)(void *user, struct GuestContext *c);
    /* the thread cannot continue (UDF, BRK, a branch to a non-function): does not return */
    void (*trap)(void *user, struct GuestContext *c, uint64_t pc, uint32_t insn, const char *why);
    /* BLR / BR / a thread start at an address with no lifted function: run it some other way (the JIT) until it
       returns to c->x[30] (calls) or forever (jumps); returns 1 if it did */
    int (*call_miss)(void *user, struct GuestContext *c, uint64_t target);
    /* CNTPCT_EL0 */
    uint64_t (*cntpct)(void *user);
} MkwHost;

#if defined(_WIN32)
#define MKW_EXPORT __declspec(dllexport)
#else
#define MKW_EXPORT __attribute__((visibility("default")))
#endif

/* exported by mkw_game.dll */
typedef struct MkwGameInfo {
    uint32_t version;          /* MKW_HOST_VERSION */
    uint32_t ctx_size;         /* sizeof(GuestContext) in the DLL */
    uint64_t mem_base;         /* A64_HOST_BASE: where the guest window must be */
    uint64_t image_base;       /* the guest address the image was lifted at (rtld's load address) */
    uint32_t functions;        /* lifted entries */
    uint32_t polls;            /* 1 if built with A64_POLLING */
    const char *main_build_id; /* hex, the main module's NSO build id */
} MkwGameInfo;

typedef int (*PFN_mkw_recomp_attach)(const MkwHost *host, MkwGameInfo *info);   /* 0: ok */
typedef MkwFn (*PFN_mkw_recomp_lookup)(uint64_t guest_addr);                    /* NULL: no lifted function */

#ifdef __cplusplus
}
#endif
#endif
