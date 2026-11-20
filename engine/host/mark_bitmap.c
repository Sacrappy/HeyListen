/* (longshot 11-18, 35a4a9fc #1617) The pages the GPU can read, a bit each, for the lifted code's page marks
   (build.py RX_MARK_BITMAP, nx2/rtgen.py A64_MARK_BITMAP): a store marks its pages only if its first byte is on one (and
   the page before each mapping counts as one, for a store that crosses into it). The renderer reads the marks of GPU pages
   only, and OoT's GPU pools lie in the heap among its CPU data; Longshot's no-marks timing build ran the game's threads
   2.2-2.7 ms a frame faster while moving (their measurement, not ours).

   A page's bit is set when a GPU mapping covering it is made (host/nvdrv.c map_add), before the mapping counts, and is
   never cleared: a page once GPU memory stays marked, the same as without the bitmap. Writes made before the mapping need
   no mark: the renderer imports a new pool's memory as it is. The host's own writes to guest memory (HOS_WRITTEN,
   host/hos_mem.c) keep marking every page they touch, whatever this bitmap says.

   Longshot sets bits only for mappings whose GPU address is the guest address (their pools). Here map_add's guest range is
   the memory behind every mapping, whatever its GPU address, so every mapping's guest pages are set: a page the GPU can
   reach through any mapping must be a page that marks, or the renderer would read it stale.

   16 MB of zeroed static data for the 512 GB window, of which only the heap's part is ever touched (~210 KB).
   HOS_MARK_BITMAP_ALL=1 sets every bit at the start: every store marks, as without the bitmap, plus the test's cost (the
   control of an A/B, and the way to check a picture against the bitmap's). */
#include "a64.h"   /* (A64_DIRTY_PAGES: the window's page count, nx2/rtgen.py) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef A64_DIRTY_PAGES
#define A64_DIRTY_PAGES (1ull << 27)
#endif
#if defined(A64_MARK_BITMAP)   /* (build.py RX_MARK_BITMAP=1: with it off none of this is built, and map_add sets no bits) */
uint8_t a64_gpu_pages[A64_DIRTY_PAGES / 8];

__attribute__((constructor)) static void gpu_pages_all(void) {
    const char *e = getenv("HOS_MARK_BITMAP_ALL");
    if (e && e[0] && e[0] != '0') memset(a64_gpu_pages, 0xff, sizeof a64_gpu_pages);
}

void hos_gpu_pages_set(uint64_t guest, uint64_t size) {
    if (!size) return;
    /* (and the page before: the lifted code tests a store's first byte only, and a store from there may cross into this one) */
    uint64_t p = guest >> 12 ? (guest >> 12) - 1 : 0;
    const uint64_t end = (guest + size + 0xFFF) >> 12;
    for (; p < end; p++) {
        const uint64_t i = p & (A64_DIRTY_PAGES - 1);
        __atomic_fetch_or(&a64_gpu_pages[i >> 3], (uint8_t)(1u << (i & 7)), __ATOMIC_RELEASE);
    }
}
#endif
