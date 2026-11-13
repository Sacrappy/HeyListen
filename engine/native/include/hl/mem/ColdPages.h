// HeyListen native engine: which pages of static GPU data may leave the process's working set, and which must stay.
//
// The game keeps the CPU side of its GPU resources (textures, vertex and index data, shader code) in CPU-cached memory
// pools for as long as the resources live, while the back end has its own copy in VRAM. A page trap showed game code
// touching under 1 MB of those 3.35 GB in 35 s (docs/native-engine/memory-boundary.md, section 7). The OS layer takes
// such pages out of the working set: they stay committed and keep their bytes (the system's standby list, then the page
// file), and any later touch brings the same bytes back with no handler involved. This class is the policy: it is told
// how many pages of a unit (a fixed run of pages, 64 KB in the game) are in the working set at each look, says whether
// to take them out, and remembers the units that came back so that pages someone keeps reading are left alone.
//
// The same policy serves the game's heaps (memory-boundary.md, section 12), with one more rule (`settle_passes`): a unit
// is taken out only at a look that comes at least that many passes after an earlier look, and only if it has not gained
// resident pages in between, so that memory being filled (a file being loaded into fresh pages) is left alone.
//
// It knows nothing of the guest's addresses: units are numbers in [0, units). Thread use: one thread looks and takes
// out (the OS layer's pass); forget() and mark_imported() may come from any thread at the same time.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hl::mem {

class ColdPages {
public:
    struct Config {
        uint64_t units = 0;             // the unit index space
        uint32_t unit_pages = 16;       // pages a unit (at most 31)
        uint32_t page_bytes = 4096;     // for the report only
        uint32_t backoff_passes = 60;   // passes a unit that came back is left in; doubled at each further come-back
        uint32_t give_up = 4;           // a unit that came back this many times stays in for good
        uint32_t budget_pages = 16384;  // pages taken out a pass at most
        uint32_t settle_passes = 0;     // 0: a unit may be taken out at its first look (the pools). n: only at a look n
                                        // passes or more after the one before, with no more resident pages than then
    };

    struct Stats {
        std::atomic<uint64_t> passes{0};
        std::atomic<uint64_t> taken_pages{0}, taken_units{0};   // taken out, in all
        std::atomic<int64_t> out_pages{0};                      // taken out and not seen back since
        std::atomic<uint64_t> back_pages{0}, back_units{0};     // came back, in all (a unit seen resident again)
        std::atomic<uint64_t> stayed_pages{0};                  // still resident right after being taken out
        std::atomic<uint64_t> given_up_units{0};                // came back `give_up` times: left in for good
        std::atomic<uint64_t> imported_units{0};                // marked imported (never taken out)
    };

    explicit ColdPages(const Config& config);
    ~ColdPages();
    ColdPages(const ColdPages&) = delete;
    ColdPages& operator=(const ColdPages&) = delete;

    bool ok() const { return state_ != nullptr && imported_ != nullptr; }
    const Config& config() const { return config_; }

    /// A new pass: the budget is full again and the holds count down by one.
    void begin_pass();

    /// A unit seen with `resident` of its pages in the working set. Returns true when they are to be taken out now (and
    /// then taken_out() is to be told what happened). A unit taken out earlier and seen resident again came back: it is
    /// counted and held for backoff_passes << (come-backs - 1) passes, or for good after `give_up` come-backs.
    bool look(uint64_t unit, uint32_t resident);

    /// What a take-out did: `left` pages left the working set, `stayed` did not (pinned by someone else).
    void taken_out(uint64_t unit, uint32_t left, uint32_t stayed);

    /// Units [lo, hi) hold new data (a pool made over them): their history no longer applies.
    void forget(uint64_t lo, uint64_t hi);

    /// Units [lo, hi) are pinned by the GPU driver (an import): never taken out, from now on.
    void mark_imported(uint64_t lo, uint64_t hi);
    /// Units [lo, hi) are no longer pinned (the import was given back): they may be taken out again.
    void unmark_imported(uint64_t lo, uint64_t hi);
    bool imported(uint64_t unit) const;

    const Stats& stats() const { return stats_; }

    /// One line of counters (MB and units, in all and since the last call, and the most that came back between two
    /// calls). Returns the length written.
    int report(char* out, size_t cap);

private:
    // A unit's state, one word: pages out (bits 0-4), come-backs (bits 5-8), resident pages at the last look while not
    // out (9-13, with settle_passes), the pass it may be taken out again (14-31: 262,143 passes, 36 h at 500 ms; past
    // that the holds end, and units left in for good stay in).
    static constexpr uint32_t kOutMask = 31, kLevelShift = 5, kLevelMask = 15, kSeenShift = 9, kHoldShift = 14,
                              kHoldMax = (1u << 18) - 1;
    static uint32_t out_of(uint32_t s) { return s & kOutMask; }
    static uint32_t level_of(uint32_t s) { return s >> kLevelShift & kLevelMask; }
    static uint32_t seen_of(uint32_t s) { return s >> kSeenShift & kOutMask; }
    static uint32_t hold_of(uint32_t s) { return s >> kHoldShift; }
    static uint32_t make(uint32_t out, uint32_t level, uint32_t hold, uint32_t seen = 0) {
        return out | level << kLevelShift | (seen > kOutMask ? kOutMask : seen) << kSeenShift |
               (hold > kHoldMax ? kHoldMax : hold) << kHoldShift;
    }
    uint32_t hold_after(uint32_t level) const;
    uint32_t pass_plus(uint64_t passes) const;

    Config config_;
    uint32_t* state_ = nullptr;      // a word a unit (zeroed memory: only the units looked at cost RAM)
    uint64_t* imported_ = nullptr;   // a bit a unit
    uint32_t pass_ = 0;
    int64_t budget_ = 0;
    Stats stats_;
    uint64_t last_taken_ = 0, last_back_pages_ = 0, last_back_units_ = 0;   // (report(): since the last call)
    uint64_t worst_back_pages_ = 0;                                          // (report(): the most between two calls)
};

}   // namespace hl::mem
