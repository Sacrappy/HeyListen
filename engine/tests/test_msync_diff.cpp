// (TICK-048) gpu/vk/msync_diff.h: the chunk hash's two forms agree, a change anywhere in a chunk changes its hash, the masks
// widen to their groups and the runs cover exactly the set chunks. Built with and without -mavx2 by test_msync_diff.py.
#include "../gpu/vk/msync_diff.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>

using namespace msync_diff;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    std::mt19937_64 rng(48);
    alignas(64) uint8_t page[4096], other[4096];
    for (int round = 0; round < 2000; round++) {
        for (auto& b : page) b = (uint8_t)rng();
        if (round % 7 == 0) std::memset(page, 0, sizeof page);   // (zero pages: common in the game's pools)
        for (unsigned c = 0; c < kChunks; c++) CHECK(chunk_hash(page + c * kChunk) == chunk_hash_scalar(page + c * kChunk));
        uint64_t a[kChunks], b[kChunks];
        page_hash(page, a);
        std::memcpy(other, page, sizeof page);
        page_hash(other, b);
        CHECK(changed_mask(a, b, 1) == 0);
        // (one bit flipped, one word swapped with its neighbour, two words exchanged across stripes: each changes that chunk only)
        const unsigned at = (unsigned)(rng() % 4096);
        other[at] ^= (uint8_t)(1u << (rng() % 8));
        page_hash(other, b);
        CHECK(changed_mask(a, b, 1) == 1u << (at / kChunk));
        std::memcpy(other, page, sizeof page);
        const unsigned c = (unsigned)(rng() % kChunks);
        uint64_t w0, w1;
        std::memcpy(&w0, other + c * kChunk, 8), std::memcpy(&w1, other + c * kChunk + 8, 8);
        if (w0 != w1) {
            std::memcpy(other + c * kChunk, &w1, 8), std::memcpy(other + c * kChunk + 8, &w0, 8);
            page_hash(other, b);
            CHECK(changed_mask(a, b, 1) == 1u << c);
        }
        std::memcpy(other, page, sizeof page);
        std::memcpy(&w0, other + c * kChunk + 16, 8), std::memcpy(&w1, other + c * kChunk + 16 + 96, 8);
        if (w0 != w1) {
            std::memcpy(other + c * kChunk + 16, &w1, 8), std::memcpy(other + c * kChunk + 16 + 96, &w0, 8);
            page_hash(other, b);
            CHECK(changed_mask(a, b, 1) == 1u << c);
        }
    }
    // (the same chunk at another place in the page hashes the same: the hash is of the bytes only)
    for (auto& x : page) x = (uint8_t)rng();
    std::memcpy(other + 3 * kChunk, page + 5 * kChunk, kChunk);
    CHECK(chunk_hash(other + 3 * kChunk) == chunk_hash(page + 5 * kChunk));
    // (no two of many random chunks collide)
    std::set<uint64_t> seen;
    for (int i = 0; i < 200000; i++) {
        for (unsigned k = 0; k < kChunk; k++) page[k] = (uint8_t)rng();
        seen.insert(chunk_hash(page));
    }
    CHECK(seen.size() == 200000);
    // masks and runs
    for (uint32_t m = 0; m < 0x10000; m++) {
        for (unsigned g : {1u, 2u, 4u, 8u, 16u}) {
            const uint32_t w = group_mask(m, g);
            CHECK((w & m) == m);
            for (unsigned c = 0; c < kChunks; c += g) {
                const uint32_t gm = (g >= 16 ? 0xffffu : (1u << g) - 1) << c;
                CHECK((w & gm) == 0 || (w & gm) == gm);
                CHECK(((w & gm) != 0) == ((m & gm) != 0));
            }
        }
        uint32_t off[8], len[8];
        const int n = mask_runs(m, off, len);
        uint32_t back = 0;
        for (int k = 0; k < n; k++) {
            CHECK(off[k] % kChunk == 0 && len[k] % kChunk == 0 && len[k]);
            if (k) CHECK(off[k] > off[k - 1] + len[k - 1]);   // (runs apart: adjacent chunks are one run)
            for (uint32_t c = off[k] / kChunk; c < (off[k] + len[k]) / kChunk; c++) back |= 1u << c;
        }
        CHECK(back == m);
        CHECK(n <= 8);
    }
    {   // (TICK-060) copy_hash_page: dst gets src's bytes and h page_hash(dst), aligned (streaming stores) and not
        alignas(64) static uint8_t src[4096], dst[4096 + 64];
        for (int round = 0; round < 500; round++) {
            for (auto& b : src) b = (uint8_t)rng();
            if (round % 5 == 0) std::memset(src + (rng() % 16) * kChunk, 0, kChunk);
            for (unsigned off : {0u, 8u, 32u}) {
                uint8_t* d = dst + off;
                std::memset(d, 0xcd, 4096);
                uint64_t h[kChunks], ref[kChunks];
                copy_hash_page(d, src, h);
#if defined(__AVX2__)
                _mm_sfence();
#endif
                CHECK(std::memcmp(d, src, 4096) == 0);
                page_hash(src, ref);
                CHECK(std::memcmp(h, ref, sizeof h) == 0);
            }
        }
    }
    std::printf("%s (%s)\n", fails ? "FAILED" : "msync_diff ok",
#if defined(__AVX2__)
                "AVX2"
#else
                "portable"
#endif
    );
    return fails ? 1 : 0;
}
