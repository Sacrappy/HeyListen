/* SPDX-License-Identifier: GPL-3.0-or-later */
/* (heylisten) What the detailed performance profile (gpu/vk/profile_run.cpp) asks of the executable: which kind of code a sampled
   program counter of the executable is in, and the executable's build id. The renderer's sampler thread asks, after the sampled thread
   is running again, so the lookups here may allocate (the first one builds the lifted code's table) and read the symbol table.

   A program counter is the game's lifted code (the answer is its guest function's address, the crash report's guest_frames name them
   the same), our native engine (the hl:: modules and their hl_ glue), the audio code or the rest of the host. The last three answer
   with the function's offset inside this executable (its unwind data's start, else the counter rounded down to 16 bytes): the build id
   says which build the offset is for. The lifted code is the range of its functions' host addresses (a64_fn_ptr: hl_host.c). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hos_crash_names.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void hl_lifted_table(const uint32_t **addrs, void *const **fns, uint32_t *count);   /* hl_host.c */
uint64_t hos_crash_module_identity(uintptr_t base);                                  /* hos_crash_names.c */

enum { PROFILE_GAME_LIFTED = 0, PROFILE_NATIVE_ENGINE = 1, PROFILE_HOST = 3, PROFILE_AUDIO = 4 };   /* (gpu/vk/profile_report.h Module) */

typedef struct Lifted { uintptr_t fn; uint32_t guest; } Lifted;
static Lifted *lifted;
static uint32_t lifted_n;
static uintptr_t lifted_lo, lifted_hi, exe_base;
static int names_ok;   /* (the symbol table is for this very binary) */
static int ready;

static int by_fn(const void *a, const void *b) {
    const uintptr_t x = ((const Lifted *)a)->fn, y = ((const Lifted *)b)->fn;
    return x < y ? -1 : x > y;
}

static void init(void) {
    ready = 1;
    exe_base = (uintptr_t)GetModuleHandleA(NULL);
    names_ok = hos_crash_names_loaded(HOS_CRASH_NAMES_EXE) && hos_crash_names_identity(HOS_CRASH_NAMES_EXE) == hos_crash_module_identity(exe_base);
    const uint32_t *addrs = NULL;
    void *const *fns = NULL;
    uint32_t n = 0;
    hl_lifted_table(&addrs, &fns, &n);
    if (!addrs || !fns || !n) return;
    lifted = (Lifted *)malloc((size_t)n * sizeof *lifted);
    if (!lifted) return;
    for (uint32_t i = 0; i < n; i++) lifted[i].fn = (uintptr_t)fns[i], lifted[i].guest = addrs[i];
    qsort(lifted, n, sizeof *lifted, by_fn);
    lifted_n = n;
    lifted_lo = lifted[0].fn;
    DWORD64 image = 0;
    const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)lifted[n - 1].fn, &image, NULL);
    lifted_hi = f ? (uintptr_t)(image + f->EndAddress) : lifted[n - 1].fn + 0x10000;
}

/* The lifted function whose host code holds pc: the nearest start at or below it (within 64 KiB: the crash report's guess) */
static int lifted_of(uintptr_t pc, uint32_t *guest) {
    if (!lifted_n || pc < lifted_lo || pc >= lifted_hi) return 0;
    uint32_t lo = 0, hi = lifted_n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (lifted[mid].fn <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - lifted[lo - 1].fn >= 0x10000) return 0;
    *guest = lifted[lo - 1].guest;
    return 1;
}

/* A function's bucket by its name, cached by start offset (direct-mapped: the same few hundred functions are most of the samples) */
typedef struct Cached { uint32_t rva; int bucket; } Cached;
static Cached cache[8192];

static int engine_bucket(uint32_t rva) {
    Cached *c = &cache[(rva >> 4) & 8191];
    if (c->rva == rva && c->bucket) return c->bucket;
    int bucket = PROFILE_HOST;
    char name[160];
    if (names_ok && hos_crash_name("HeyListen.exe", rva, 0, name, sizeof name)) {
        char *plus = strrchr(name, '+');
        if (plus) *plus = 0;
        const char *n = name;
        if (!strncmp(n, "hl::", 4) || !strncmp(n, "hl_", 3) || strstr(n, "::hl::") || !strncmp(n, "_ZN2hl", 6)) bucket = PROFILE_NATIVE_ENGINE;
        else if (!strncmp(n, "nn_audio", 8) || !strncmp(n, "hos_audio", 9) || !strncmp(n, "opus_", 5) || !strncmp(n, "celt_", 5) ||
                 !strncmp(n, "silk_", 5) || strstr(n, "audio") || strstr(n, "Audio"))
            bucket = PROFILE_AUDIO;
    }
    c->rva = rva, c->bucket = bucket;
    return bucket;
}

/* (MkwVkHost profile_pc) pc is in this executable. Returns the bucket (PROFILE_*) and the key in *key: the guest function's address
   for the lifted code, else the function's offset in this executable */
int hos_profile_pc(uintptr_t pc, uint64_t *key) {
    if (!ready) init();
    uint32_t guest = 0;
    if (lifted_of(pc, &guest)) {
        *key = guest;
        return PROFILE_GAME_LIFTED;
    }
    DWORD64 image = 0;
    const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)pc, &image, NULL);
    const uintptr_t start = f ? (uintptr_t)(image + f->BeginAddress) : (pc & ~(uintptr_t)15);
    const uint32_t rva = (uint32_t)(start - exe_base);
    *key = rva;
    return engine_bucket(rva);
}

/* (MkwVkHost build_id) the executable's build id: the PE fields' hash that the symbol tables carry too (nx2/symtab.py pe_identity) */
uint64_t hos_profile_build_id(void) { return hos_crash_module_identity((uintptr_t)GetModuleHandleA(NULL)); }
/* (MkwVkHost profile_threads) the OS ids of the guest's threads (hos_thread.c keeps their handles) */
int hos_thread_handles(HANDLE *out, const char **names, int max);
int hos_profile_threads(uint32_t *tids, int max) {
    static HANDLE handles[256];
    static const char *names[256];
    const int n = hos_thread_handles(handles, names, max < 256 ? max : 256);
    int k = 0;
    for (int i = 0; i < n; i++) {
        const DWORD id = GetThreadId(handles[i]);
        if (id) tids[k++] = (uint32_t)id;
    }
    return k;
}
#else
int hos_profile_threads(uint32_t *tids, int max) {
    (void)tids, (void)max;
    return 0;
}
int hos_profile_pc(uintptr_t pc, uint64_t *key) {   /* (Linux: the sampler is not built: nothing asks) */
    (void)pc;
    *key = 0;
    return 3;
}
uint64_t hos_profile_build_id(void) { return 0; }
#endif
