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
    bool imported(uint64_t unit) const;

    const Stats& stats() const { return stats_; }

    /// One line of counters (MB and units, in all and since the last call). Returns the length written.
    int report(char* out, size_t cap);

private:
    // A unit's state, one word: pages out (bits 0-4), come-backs (bits 5-8), the pass it may be taken out again (9-31).
    static constexpr uint32_t kOutMask = 31, kLevelShift = 5, kLevelMask = 15, kHoldShift = 9, kHoldMax = (1u << 23) - 1;
    static uint32_t out_of(uint32_t s) { return s & kOutMask; }
    static uint32_t level_of(uint32_t s) { return s >> kLevelShift & kLevelMask; }
    static uint32_t hold_of(uint32_t s) { return s >> kHoldShift; }
    static uint32_t make(uint32_t out, uint32_t level, uint32_t hold) {
        return out | level << kLevelShift | (hold > kHoldMax ? kHoldMax : hold) << kHoldShift;
    }
    uint32_t hold_after(uint32_t level) const;

    Config config_;
    uint32_t* state_ = nullptr;      // a word a unit (zeroed memory: only the units looked at cost RAM)
    uint64_t* imported_ = nullptr;   // a bit a unit
    uint32_t pass_ = 0;
    int64_t budget_ = 0;
    Stats stats_;
    uint64_t last_taken_ = 0, last_back_pages_ = 0, last_back_units_ = 0;   // (report(): since the last call)
};

}   // namespace hl::mem
