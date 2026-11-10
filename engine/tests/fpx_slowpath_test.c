/* (HeyListen TICK-044) tests/test_fpx_slowpath.py's driver: each guest word lifted against the default a64_fpx.h
   (fn_old, cases TU built with it) and against the A64_FPX_SLOWPATH=1 one (fn_new), on random registers and FP states:
   the whole Ctx must match. Then recomp_fp_estimate with and without its fast path (recomp_fp_estimate_new), binary32:
   every sign, exponent and top 10 significand bits (the estimate reads the top 9) with random low bits, under each FPCR,
   twice (the tables fill on the first pass).
     fpx_slowpath_test <cases> <seed>      exit 0: no mismatch; 1: a mismatch; 3: this CPU lacks the -march it was built for */
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
#include <cpuid.h>

typedef void (*GuestFn)(Ctx *);
extern const GuestFn fn_old[], fn_new[];
extern const char *const fn_names[];
extern const int fn_count;
uint64_t recomp_fp_estimate_new(Ctx *, uint64_t, unsigned, int);

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
    default: return s | ((uint32_t)(100 + rnd() % 56) << 23) | ((uint32_t)rnd() & 0x7fffffu);
    }
}

int main(int argc, char **argv) {
    unsigned eax, ebx, ecx, edx;
#if defined(__AVX2__)
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) || !(ebx >> 5 & 1)) return 3;
#endif
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx >> 20 & 1)) return 3;   /* (SSE4.2: x86-64-v2) */
    const long n = argc > 1 ? atol(argv[1]) : 1000000;
    rng = (argc > 2 ? strtoull(argv[2], 0, 10) : 1) | 1;
    static const uint64_t fpsrs[] = {0x9f, 0x10, 0x00, 0x1f, 0x90, 0x98, 0x94, 0x18, 0x14, 0x80, 0x08, 0x88, 0x17, 0x97,
                                     0x96, 0x12, 0x92, 0x11};
    static const uint64_t fpcrs[] = {0, 1u << 24, (1u << 24) | (1u << 25), 1u << 22, 0x9f00, 1u << 25, 3u << 22};
    long bad = 0;
    static Ctx A, B;
    for (long it = 0; it < n; it++) {
        const int g = (int)(rnd() % (uint64_t)fn_count);
        const int shape = (int)(rnd() % 4);   /* 0: any lanes; 1: normal lanes; 2: normal lanes, one quiet NaN lane; 3: specials */
        memset(&A, 0, sizeof A);
        for (int r = 0; r < 4; r++)
            for (int l = 0; l < 4; l++) {
                uint32_t v = rf(shape == 3 ? 2 : shape ? 1 : 0);
                if (shape == 2 && l == 3 && (rnd() & 1)) v = 0x7fc00000u | ((uint32_t)rnd() & 0x803fffffu);
                memcpy((uint8_t *)A.vreg[r] + 4 * l, &v, 4);
            }
        A.fpsr = fpsrs[rnd() % (sizeof fpsrs / sizeof *fpsrs)];
        A.fpcr = rnd() % 4 ? fpcrs[rnd() % 2] : fpcrs[rnd() % (sizeof fpcrs / sizeof *fpcrs)];
        B = A;
        fn_new[g](&A);
        fn_old[g](&B);
        if (memcmp(&A, &B, sizeof A)) {
            if (bad++ < 8) {
                printf("MISMATCH %s: fpcr %llx fpsr %llx/%llx", fn_names[g], (unsigned long long)B.fpcr,
                       (unsigned long long)A.fpsr, (unsigned long long)B.fpsr);
                for (int r = 0; r < 4; r++) printf(" v%d %016llx%016llx/%016llx%016llx", r, (unsigned long long)A.vreg[r][1],
                    (unsigned long long)A.vreg[r][0], (unsigned long long)B.vreg[r][1], (unsigned long long)B.vreg[r][0]);
                printf("\n");
            }
        }
    }
    printf("fpx slowpath (%s): %ld runs of %d guest words, %ld mismatches\n",
#if defined(__AVX2__)
           "x86-64-v3",
#else
           "x86-64-v2",
#endif
           n, fn_count, bad);
    /* the estimates */
    long ebad = 0, en = 0;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned f = 0; f < sizeof fpcrs / sizeof *fpcrs; f++)
            for (int rs = 0; rs < 2; rs++)
                for (uint32_t hi = 0; hi < (1u << 19); hi++) {   /* sign, exponent, top 10 significand bits */
                    const uint32_t x = hi << 13 | ((uint32_t)rnd() & 0x1fffu);
                    const uint64_t bits = x | (rnd() & 1 ? (uint64_t)rnd() << 32 : 0);   /* (high bits: never read) */
                    memset(&A, 0, sizeof A);
                    A.fpcr = fpcrs[f];
                    A.fpsr = fpsrs[rnd() % (sizeof fpsrs / sizeof *fpsrs)];
                    B = A;
                    const uint64_t va = recomp_fp_estimate_new(&A, bits, 32, rs), vb = recomp_fp_estimate(&B, bits, 32, rs);
                    en++;
                    if (va != vb || A.fpsr != B.fpsr) {
                        if (ebad++ < 8)
                            printf("MISMATCH %s %08x fpcr %llx: %llx/%llx fpsr %llx/%llx\n", rs ? "FRSQRTE" : "FRECPE", x,
                                   (unsigned long long)A.fpcr, (unsigned long long)va, (unsigned long long)vb,
                                   (unsigned long long)A.fpsr, (unsigned long long)B.fpsr);
                    }
                }
    for (long i = 0; i < 200000; i++) {   /* binary64: the fast path must leave it alone */
        const uint64_t bits = rnd();
        const int rs = (int)(rnd() & 1);
        memset(&A, 0, sizeof A);
        A.fpcr = fpcrs[rnd() % (sizeof fpcrs / sizeof *fpcrs)];
        B = A;
        en++;
        if (recomp_fp_estimate_new(&A, bits, 64, rs) != recomp_fp_estimate(&B, bits, 64, rs) || A.fpsr != B.fpsr) ebad++;
    }
    printf("fpx slowpath estimates: %ld inputs, %ld mismatches\n", en, ebad);
    return bad != 0 || ebad != 0;
}
