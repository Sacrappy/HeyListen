/* (HeyListen) The guest window's substrate for pages given back and restored atomically (hos_guestpages.c;
   docs/native-engine/memory-boundary.md section 13).

   With HOS_GUEST_PAGES=1 the heap and the address regions of the guest window ([HOS_HEAP_BASE, HOS_GUEST_SPACE)) are
   reserved as a placeholder replaced at once by one private reservation: committed, decommitted and committed on first
   touch as before. A dropped granule is given back to the system as a placeholder of its own (no RAM, no commit charge;
   any touch faults); a restored granule is a view of a page-file-backed section that was filled through a second, private
   view before it was mapped at the guest address, so no thread ever sees it before it holds its bytes. Off (the default),
   the window is reserved as before and nothing here runs. Windows only: elsewhere hos_gp_ready() is 0. */
#ifndef HOS_GUESTPAGES_H
#define HOS_GUESTPAGES_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- the interface (fixed with the orchestrator; stream M2's policy codes against it) ---------------------------- */
/* All addresses are host pointers inside the guest window, sizes multiples of the substrate's granule. */
size_t hos_gp_granule(void);                 /* 64 KB on Windows; 0 when the substrate is off or unsupported */
int    hos_gp_ready(void);                   /* 1 when the window was reserved in the form that allows drop/restore */
/* Give [p, p+size) back to the system: no RAM, no commit; any touch faults into the handler below. The caller has
   made sure nobody needs the bytes except through the restore. Returns 0 on success. */
int    hos_gp_drop(void *p, size_t size);
/* Make [p, p+size) exist again with given contents, atomically: `fill(user, buf, p, size)` writes the bytes into
   `buf`, a private buffer no other thread sees; only when it returns 0 does the range become visible, in one step,
   already holding those bytes. A fill that returns nonzero leaves the range dropped. */
typedef int (*hos_gp_fill)(void *user, void *buf, void *p, size_t size);
int    hos_gp_restore(void *p, size_t size, hos_gp_fill fill, void *user);
/* The policy's hook: called by the exception handler (and by hos_commit and every other path that would commit or
   hand guest memory to the kernel or the driver) for a touched dropped granule; must restore it (through
   hos_gp_restore) and return 1, or return 0 if the range is not its own. One owner at a time. */
typedef int (*hos_gp_on_touch)(void *p, size_t size, int write);
void   hos_gp_set_owner(hos_gp_on_touch cb);
int    hos_gp_is_dropped(const void *p);     /* cheap: a bitmap, one bit per granule */

/* ---- what the rules above mean in detail ----------------------------------------------------------------------
   - hos_gp_drop refuses (nothing dropped) a range outside the substrate's part of the window or not granule aligned
     (returns -1), or with a granule the Vulkan back end ever imported (returns -2: the driver locks those pages, and
     giving them back would leave the GPU on other pages than the CPU). Granules already dropped are left as they are.
   - hos_gp_restore restores the granules of the range that are dropped and leaves the others as they are (their bytes
     in `buf` are not used); a range with nothing dropped returns 0 at once without calling `fill`. Two restores or a
     restore and a drop that overlap run one after the other: a touch of a granule being restored waits for it.
     `fill` runs without any lock of the substrate held but must not touch dropped granules of its own range.
   - Each restored granule is its own view (so that any granule can be dropped again on its own); the granules of one
     restore share one section, whose commit charge is given back when the last of them is dropped again.
   - Touches: a dropped granule touched by any thread (a read or a write by the game, the back end, any host code) is
     handed to the owner by the exception handler, and the instruction runs again once it is restored. The kernel and
     the driver do not fault: hos_commit (before a system call is given a guest buffer) and the back end's imports
     (hos_noaccess_import) ask the owner first. A dropped granule the owner declines, or with no owner, is a crash.
   - A range the game gives up (its memory unmapped, decommitted or freed: hos_mem.c, hos_svc.c) forgets the dropped
     granules wholly inside it: they become reserved memory again, read as zero after the next touch, and the owner is
     not asked for them (dropped granules cut by the range's ends are restored first, then cut); the forget hook below
     is told of the range afterwards. */
/* (an addition to the fixed interface) told, after the OS layer gave up [p, p + size) (an unmap, decommit or discard of
   the game's memory), that dropped granules inside it were forgotten; called with no lock of the substrate held. One at a
   time; NULL for none */
typedef void (*hos_gp_on_forget)(void *p, size_t size);
void   hos_gp_set_on_forget(hos_gp_on_forget cb);

/* ---- the OS layer's side (mkw_glue.c, hos_mem.c, hos_svc.c, hos_memsplit.c) ----------------------------------- */
int  hos_gp_wanted(void);                    /* HOS_GUEST_PAGES=1 (read once) */
/* reserve [base, base + size) as a placeholder replaced by one private reservation; 1 on success (hos_gp_ready then) */
int  hos_gp_reserve(void *base, size_t size);
/* the handler's part: p faulted; 1 restored (run the instruction again), 0 not a dropped granule, -1 dropped and not
   restored (the owner declined or there is none) */
int  hos_gp_fault(void *p, int write);
/* the paths that hand [p, p + n) to the kernel or the driver: the owner restores its dropped granules first */
enum { HOS_GP_PATH_HANDLER, HOS_GP_PATH_COMMIT, HOS_GP_PATH_IMPORT, HOS_GP_PATH_DIRECT, HOS_GP_PATHS };
void hos_gp_prepare(void *p, size_t n, int path);
/* 1 when [p, p + n) holds a dropped granule or one apart from the window's reservation (a view, or a forgotten granule:
   a reservation of its own), which one VirtualAlloc / VirtualFree / VirtualProtect call cannot cross: then the three
   calls below, which work piece by piece; otherwise the plain calls of before */
int  hos_gp_touches(const void *p, size_t n);
/* VirtualAlloc(MEM_COMMIT), VirtualFree(MEM_DECOMMIT) and MEM_RESET over [p, p + n); 1 when all went through. The
   decommit and the reset forget the dropped granules wholly inside the range (above) and make its whole views
   reservations of their own again; a view the range only cuts is zeroed where it is cut (its pages cannot be
   decommitted). The commit asks the owner for the dropped granules first (HOS_GP_PATH_COMMIT). */
int  hos_gp_commit(void *p, size_t n);
int  hos_gp_decommit(void *p, size_t n);
int  hos_gp_reset(void *p, size_t n);
/* the back end imports [p, p + n): its granules are never dropped from now on (and dropped ones are restored first) */
void hos_gp_imported(void *p, size_t n);
/* the counters, as one line (0 when off) */
int  hos_gp_report(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
