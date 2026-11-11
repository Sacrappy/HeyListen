/* (HeyListen TICK-055) tests/test_fpx_xmm.py's driver: each guest word or sequence lifted with the default lowering (fn_old)
   and with A64_FPX_XMM=1 (fn_new), on random registers, guest memory and FP states: the whole Ctx and the guest memory
   must match.
     fpx_xmm_test <cases> <seed>      exit 0: no mismatch; 1: a mismatch; 2: no guest window; 3: this CPU lacks the -march */
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
#include <cpuid.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

typedef void (*GuestFn)(Ctx *);
extern const GuestFn fn_old[], fn_new[];
extern const char *const fn_names[];
extern const int fn_count;

uint8_t a64_dirty[A64_DIRTY_PAGES];
void a64_poll(Ctx *c) { c->poll = 0; }

#define SCRATCH 0x10000u   /* guest addresses [SCRATCH, SCRATCH + SIZE) hold random values */
#define SIZE 0x1000u

static uint64_t rng;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static uint32_t rf(int mode) {
    const uint32_t s = (uint32_t)rnd() & 0x80000000u;
    switch (mode == 1 ? 5 + rnd() % 11 : mode == 2 ? rnd() % 5 : rnd() % 16) {
    case 0: return s;
    case 1: return s | ((uint32_t)rnd() & 0x7fffffu);                               /* subnormal (or zero) */
    case 2: return s | 0x7f800000u;
    case 3: return s | 0x7fc00000u | ((uint32_t)rnd() & 0x3fffffu);                 /* quiet NaN */
    case 4: return s | 0x7f800001u | ((uint32_t)rnd() & 0x3ffffeu);                 /* signalling NaN */
    case 5: return s | 0x00800000u;
    case 6: return s | ((uint32_t)(1 + rnd() % 40) << 23) | ((uint32_t)rnd() & 0x7fffffu);
    case 7: return s | ((uint32_t)(215 + rnd() % 39) << 23) | ((uint32_t)rnd() & 0x7fffffu);
    case 8: case 9: return s | ((uint32_t)(110 + rnd() % 36) << 23) | ((uint32_t)rnd() & 0x7f0000u);   /* midpoints */
    case 10: return (uint32_t)rnd();
    default: return s | ((uint32_t)(100 + rnd() % 56) << 23) | ((uint32_t)rnd() & 0x7fffffu);
    }
}

static uint8_t mem_a[SIZE], mem_b[SIZE];

int main(int argc, char **argv) {
    unsigned eax, ebx, ecx, edx;
#if defined(__AVX2__)
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) || !(ebx >> 5 & 1)) return 3;
#endif
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx >> 20 & 1)) return 3;   /* (SSE4.2: x86-64-v2) */
#ifdef _WIN32
    if (!VirtualAlloc((void *)A64_PTR(0), SCRATCH + SIZE + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 2;
#else
    if (mmap((void *)A64_PTR(0), SCRATCH + SIZE + 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0)
        == MAP_FAILED) return 2;
#endif
    const long n = argc > 1 ? atol(argv[1]) : 1000000;
    rng = (argc > 2 ? strtoull(argv[2], 0, 10) : 1) | 1;
    static const uint64_t fpsrs[] = {0x9f, 0x10, 0x00, 0x1f, 0x90, 0x98, 0x94, 0x18, 0x14, 0x80, 0x08, 0x88, 0x17, 0x97,
                                     0x96, 0x12, 0x92, 0x11};
    static const uint64_t fpcrs[] = {0, 1u << 24, (1u << 24) | (1u << 25), 1u << 22, 0x9f00, 1u << 25, 3u << 22,
                                     (1ull << 32) | (1u << 24)};
    long bad = 0;
    static Ctx A, B;
    for (long it = 0; it < n; it++) {
        const int g = (int)(rnd() % (uint64_t)fn_count);
        const int shape = (int)(rnd() % 4);   /* 0: any values; 1: normal values; 2: normal, a quiet NaN here and there; 3: specials */
        memset(&A, 0, sizeof A);
        for (int r = 0; r < 8; r++)
            for (int l = 0; l < 4; l++) {
                uint32_t v = rf(shape == 3 ? 2 : shape ? 1 : 0);
                if (shape == 2 && !(rnd() % 6)) v = 0x7fc00000u | ((uint32_t)rnd() & 0x803fffffu);
                memcpy((uint8_t *)A.vreg[r] + 4 * l, &v, 4);
            }
        for (unsigned o = 0; o < SIZE; o += 4) {
            uint32_t v = rf(shape == 3 ? 2 : shape ? 1 : 0);
            memcpy(mem_a + o, &v, 4);
        }
        /* x0 / x1: guest addresses into the scratch area, aligned or not, far enough from its end for a 32-byte reach */
        A.x[0] = SCRATCH + (rnd() % 3 ? rnd() % ((SIZE - 64) / 16) * 16 : rnd() % (SIZE - 64));
        /* x1: an address for the sequences that store through it, else random data (FMOV from it) */
        A.x[1] = strstr(fn_names[g], "STR") || rnd() % 2 ? SCRATCH + rnd() % (SIZE - 64) : rf(0) | (uint64_t)rf(0) << 32;
        A.x[2] = rnd();
        A.fpsr = fpsrs[rnd() % (sizeof fpsrs / sizeof *fpsrs)];
        A.fpcr = rnd() % 4 ? fpcrs[rnd() % 2] : fpcrs[rnd() % (sizeof fpcrs / sizeof *fpcrs)];
        B = A;
        memcpy(A64_PTR(SCRATCH), mem_a, SIZE);
        fn_new[g](&A);
        memcpy(mem_b, A64_PTR(SCRATCH), SIZE);
        memcpy(A64_PTR(SCRATCH), mem_a, SIZE);
        fn_old[g](&B);
        if (memcmp(&A, &B, sizeof A) || memcmp(mem_b, A64_PTR(SCRATCH), SIZE)) {
            if (bad++ < 8) {
                printf("MISMATCH %s: fpcr %llx fpsr %llx/%llx x0 %llx/%llx", fn_names[g], (unsigned long long)B.fpcr,
                       (unsigned long long)A.fpsr, (unsigned long long)B.fpsr, (unsigned long long)A.x[0],
                       (unsigned long long)B.x[0]);
                for (int r = 0; r < 8; r++)
                    if (memcmp(A.vreg[r], B.vreg[r], 16))
                        printf(" v%d %016llx%016llx/%016llx%016llx", r, (unsigned long long)A.vreg[r][1],
                               (unsigned long long)A.vreg[r][0], (unsigned long long)B.vreg[r][1], (unsigned long long)B.vreg[r][0]);
                if (memcmp(mem_b, A64_PTR(SCRATCH), SIZE)) printf(" (guest memory differs)");
                printf("\n");
            }
        }
    }
    printf("fpx xmm (%s): %ld runs of %d guest words / sequences, %ld mismatches\n",
#if defined(__AVX2__)
           "x86-64-v3",
#else
           "x86-64-v2",
#endif
           n, fn_count, bad);
    return bad != 0;
}
