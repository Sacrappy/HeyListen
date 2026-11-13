// HeyListen native engine: where the bytes of a decompressed resource came from.
//
// The game decompresses every resource file it loads into one destination range (docs/native-engine/stream-boundary.md).
// A later brick will give such a range's pages back to the system for good and make them again on demand by decoding the
// file again (memory-boundary.md, section 9); that needs, for any address, the decode that wrote it. This table keeps,
// per decode, the destination range, what identifies the file (its name, as the game's per-file heap has it, and the
// file object), the compressed and decompressed sizes, the dictionary and a fingerprint of the decoded bytes. It is
// recorded, not used yet.
//
// It knows nothing of the guest: addresses are plain numbers. A destination can be freed and given to another file
// without this table seeing the free: a new decode that overlaps older ranges drops them, and a lookup returns the
// fingerprint so that the caller can check that the bytes are still the decode's before trusting the entry.
// Bounded: at most `capacity` ranges, the oldest dropped first. Thread use: any thread, one lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace hl::stream {

struct Decode {
    uint64_t dest = 0;          // first byte written
    uint64_t size = 0;          // bytes the decode gave (the frame's content size)
    uint64_t compressed = 0;    // bytes of the compressed file
    uint64_t file = 0;          // the file object the bytes were read through (0: none)
    uint32_t dict_id = 0;       // the frame's dictionary id (0: none)
    bool chunked = false;       // decoded chunk by chunk (a file larger than the decompressor's read buffer)
    uint64_t fingerprint = 0;   // of the decoded bytes, as the caller computes it
    std::string name;           // the file, as the request's heap names it ("" when unknown)
};

class Provenance {
public:
    struct Stats {
        uint64_t recorded = 0, dropped_overlap = 0, dropped_old = 0, live = 0, live_bytes = 0;
    };

    explicit Provenance(size_t capacity) : capacity_(capacity ? capacity : 1) {}

    /// A decode wrote [d.dest, d.dest + d.size): older ranges it overlaps are dropped, then it is kept.
    void record(Decode d);

    /// The decode whose range holds `addr`, copied into `out`. False when none does.
    bool find(uint64_t addr, Decode* out) const;

    /// Forget every range that overlaps [lo, hi) (a free seen by the caller).
    void forget(uint64_t lo, uint64_t hi);

    Stats stats() const;

private:
    struct Entry {
        Decode d;
        uint64_t seq = 0;
    };
    void drop_overlapping(uint64_t lo, uint64_t hi, uint64_t* counter);

    size_t capacity_;
    mutable std::mutex lock_;
    std::map<uint64_t, Entry> by_dest_;   // (ranges do not overlap: keyed by their first byte)
    std::map<uint64_t, uint64_t> by_age_; // seq -> dest
    uint64_t seq_ = 0;
    Stats stats_;
};

/// 64-bit FNV-1a of a byte range: the fingerprint the OS layer stores and checks (a few sampled spans of a range).
uint64_t fingerprint(const void* data, size_t n, uint64_t seed = 0xcbf29ce484222325ULL);

}   // namespace hl::stream
