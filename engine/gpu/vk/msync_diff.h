// (HeyListen, TICK-048) The mirror syncs' diff: a 64-bit hash of each 256-byte chunk of a page, to find the chunks of a dirty
// page whose bytes are the ones its mirror already holds (the last sync of the page copied them, and nothing wrote the mirror
// there since) and leave them out of the upload. The accumulation is XXH3's (a stripe of 32 bytes: each 64-bit word added to
// its neighbour lane, plus the product of the low and high halves of word ^ key), with a key of its own for each of a chunk's
// 8 stripes, so chunks that hold the same words in another order hash apart; murmur3's finaliser mixes the four lanes.
// Same result with or without AVX2 (tests/test_msync_diff.py checks both forms against a Python model, and the run splitting).
#pragma once
#include <cstdint>
#include <cstring>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace msync_diff {

constexpr unsigned kChunk = 256, kChunks = 4096 / kChunk;   // (16 chunks a page)

struct Keys {
    alignas(32) uint64_t k[8][4];
    alignas(32) uint64_t init[4];
    constexpr Keys() : k{}, init{} {
        uint64_t s = 0x48654C697374656Eull;   // (splitmix64)
        auto next = [&s]() {
            uint64_t z = (s += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        };
        for (auto& row : k)
            for (auto& x : row) x = next();
        for (auto& x : init) x = next();
    }
};
inline constexpr Keys kKeys{};

inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return k;
}
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t finish(const uint64_t a[4]) {
    return fmix64(a[0] ^ rotl(a[1], 29) ^ fmix64(a[2] + rotl(a[3], 47) + 0x9E3779B97F4A7C15ull));
}

/// One 256-byte chunk, portable form
inline uint64_t chunk_hash_scalar(const uint8_t* p) {
    uint64_t a[4] = {kKeys.init[0], kKeys.init[1], kKeys.init[2], kKeys.init[3]};
    for (int s = 0; s < 8; s++) {
        uint64_t v[4];
        std::memcpy(v, p + s * 32, 32);
        for (int i = 0; i < 4; i++) {
            const uint64_t dk = v[i] ^ kKeys.k[s][i];
            a[i] += v[i ^ 1] + (dk & 0xffffffffull) * (dk >> 32);
        }
    }
    return finish(a);
}

/// One 256-byte chunk
inline uint64_t chunk_hash(const uint8_t* p) {
#if defined(__AVX2__)
    __m256i acc = _mm256_load_si256((const __m256i*)kKeys.init);
    for (int s = 0; s < 8; s++) {
        const __m256i v = _mm256_loadu_si256((const __m256i*)(p + s * 32));
        const __m256i dk = _mm256_xor_si256(v, _mm256_load_si256((const __m256i*)kKeys.k[s]));
        const __m256i prod = _mm256_mul_epu32(dk, _mm256_srli_epi64(dk, 32));
        const __m256i swapped = _mm256_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2));   // (each word into its neighbour lane: i ^ 1)
        acc = _mm256_add_epi64(acc, _mm256_add_epi64(swapped, prod));
    }
    alignas(32) uint64_t a[4];
    _mm256_store_si256((__m256i*)a, acc);
    return finish(a);
#else
    return chunk_hash_scalar(p);
#endif
}

/// A page's 16 chunk hashes
inline void page_hash(const uint8_t* page, uint64_t h[kChunks]) {
    for (unsigned c = 0; c < kChunks; c++) h[c] = chunk_hash(page + c * kChunk);
}

/// A chunk mask widened to groups of `group` chunks
inline uint32_t group_mask(uint32_t m, unsigned group) {
    if (group <= 1) return m & 0xffffu;
    uint32_t out = 0;
    const uint32_t gmask = group >= 16 ? 0xffffu : (1u << group) - 1;
    for (unsigned c = 0; c < kChunks; c += group)
        if (m & (gmask << c)) out |= gmask << c;
    return out & 0xffffu;
}

/// Which chunks differ (bit c: chunk c), at a granularity of `group` chunks (1, 2, 4, 8 or 16): a group differs when any of its
/// chunks does, and all its chunks' bits are then set
inline uint32_t changed_mask(const uint64_t a[kChunks], const uint64_t b[kChunks], unsigned group) {
    uint32_t m = 0;
    for (unsigned c = 0; c < kChunks; c++)
        if (a[c] != b[c]) m |= 1u << c;
    return group_mask(m, group);
}
/// The runs of set bits of a 16-bit chunk mask, as (byte offset in the page, bytes), in order; returns how many (at most 8)
inline int mask_runs(uint32_t mask, uint32_t off[8], uint32_t len[8]) {
    int n = 0;
    for (unsigned c = 0; c < kChunks;) {
        if (!(mask >> c & 1)) { c++; continue; }
        unsigned e = c;
        while (e < kChunks && (mask >> e & 1)) e++;
        off[n] = c * kChunk, len[n] = (e - c) * kChunk, n++;
        c = e;
    }
    return n;
}

} // namespace msync_diff
