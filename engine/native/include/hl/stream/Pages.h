// HeyListen native engine: which pages of the decoded resources still hold the decoder's bytes, by file kind.
//
// A page of a decoded range can be given back to the system and made again by decoding the file again only while its
// bytes are exactly what the decoder wrote (docs/native-engine/stream-boundary.md, section 9). Resource formats are
// patched in place after loading (relocated pointers in headers), so some pages change. This table takes, for every
// decode, a hash of each whole 4 KB page of its destination (the unit Windows decommits; the pages at the range's two
// ends that it shares with other data are not candidates), hashes the pages again when the range is 30 s old and when it
// is 60 s old, and counts by file kind how many still match. It measures, it decides nothing.
//
// It knows nothing of the guest: the caller gives the decoded bytes at decode time and a view of memory at check time
// (which says when a page is not readable any more: freed, decommitted). A range given to a new decode before its checks
// is dropped and counted as replaced. Bounded: at most `max_pages` page hashes held, more ranges are counted and not
// tracked. Thread use: track() from any thread; check() from one thread at a time (it hashes outside the lock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hl::stream {

/// A 64-bit hash of a page's bytes (four independent multiply-xor lanes over 8-byte words, then the tail): one changed
/// word always changes it.
uint64_t page_hash(const void* data, size_t n);

/// A file's kind from its name: its folder and its extension ("Tex/a.bntx" -> "Tex/*.bntx", "a.pack" -> "*.pack").
std::string file_kind(const std::string& name);

class Pages {
public:
    static constexpr uint64_t kPage = 4096;

    struct Config {
        int64_t first_ns = 30'000'000'000;    // the two checks, by a range's age
        int64_t second_ns = 60'000'000'000;
        size_t max_pages = size_t(8) << 20;   // page hashes held at most (32 GB of ranges; 64 MB of hashes)
        size_t max_kinds = 48;                // kinds counted apart; the rest are "(other)"
    };

    /// The host bytes of [addr, addr + n) when that memory is committed and readable now, else null.
    using View = const uint8_t* (*)(uint64_t addr, size_t n, void* ctx);

    /// A range at one of its checks: its whole pages, their hashes at decode time and whether each matched now.
    struct Checked {
        uint64_t dest = 0, size = 0, first_page = 0;   // first_page: the address of the first whole page
        std::string name;
        int stage = 0;                                 // 1: the first check, 2: the second
        std::vector<uint64_t> hashes;
        std::vector<uint8_t> equal;                    // 1 equal, 0 changed, 2 not readable
    };

    struct KindStats {
        std::string kind;
        uint64_t ranges = 0, bytes = 0, pages = 0;        // tracked
        uint64_t replaced = 0;                            // ranges given to another decode before their second check
        uint64_t checked[2] = {}, pages_checked[2] = {}, equal[2] = {}, unreadable[2] = {};
    };

    explicit Pages(Config c) : cfg_(c) {}

    /// A decode wrote [dest, dest + size) at `now`; `bytes` are those bytes. False when not tracked (no whole page, or
    /// the table is full). Older ranges it overlaps are dropped (counted as replaced).
    bool track(uint64_t dest, const uint8_t* bytes, uint64_t size, const std::string& name, int64_t now);

    /// Checks the ranges whose next check is due at `now`, at most `budget_pages` pages; each one checked is appended to
    /// `out` when given. Returns the number of ranges checked.
    size_t check(int64_t now, View view, void* ctx, size_t budget_pages, std::vector<Checked>* out);

    std::vector<KindStats> stats() const;
    uint64_t not_tracked() const;

private:
    struct Entry {
        uint64_t size = 0, first_page = 0;
        int64_t t0 = 0;
        int stage = 0;   // checks done
        size_t kind = 0;
        std::string name;
        std::vector<uint64_t> hashes;
    };
    size_t kind_of(const std::string& name);   // (under the lock)
    void drop_overlapping(uint64_t lo, uint64_t hi);

    Config cfg_;
    mutable std::mutex lock_;
    std::map<uint64_t, Entry> by_dest_;
    std::vector<KindStats> kinds_;
    size_t held_pages_ = 0;
    uint64_t not_tracked_ = 0;
};

}   // namespace hl::stream
