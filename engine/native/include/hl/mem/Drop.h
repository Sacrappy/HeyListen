// HeyListen native engine: GPU data's system-memory copy given back for good, and made again on demand.
//
// The game keeps the CPU side of its resources (textures, models, shader archives decoded from its files) in its own
// memory for as long as they live, while the back end has its own copy in VRAM. Cold pages (ColdPages.h) take such
// pages out of the working set, but they stay committed: the commit charge keeps them, and the page file holds them.
// This module gives them back to the system for good (no RAM, no commit) when their bytes can be had again exactly,
// and makes them again, atomically, when anything touches them (docs/native-engine/memory-boundary.md section 14).
//
// The source this version knows: a resource file's decode (docs/native-engine/stream-boundary.md section 9). A range a
// decode wrote is recorded with what makes it again (the file's name, its dictionary, its sizes). A whole granule (the
// substrate's unit, 64 KB on Windows) of such a range is dropped only when all of this holds, checked in this order:
//   - the range is `age_ns` old or more, and the granule is not held back (a restore holds the range for a while, longer
//     each time, for good after `give_up` restores);
//   - the back end never imported it (the driver pins imported pages), and the kernel was not given it lately (a file read
//     into it: `kernel_use`);
//   - it is cold: none of its pages is in the working set (cold pages took them out and nobody touched them since), when
//     `require_cold`;
//   - it is unchanged: made read-only, then compared byte for byte with the source decoded again, and dropped only if
//     equal. A writer meanwhile faults, waits for the decision (`wait_pending`), and writes on afterwards: on the bytes
//     if the granule was kept, on the restored bytes if it was dropped.
// A touch of a dropped granule (`touch`, from the substrate's handler) restores every dropped granule of its range from
// the source, under the range's lock: a second thread touching the same range waits, then finds it restored. A range a
// new decode overlaps is replaced as a candidate; its granules still dropped keep their source (their bytes are still
// what memory holds). Since every drop is checked against the source at that moment, a stale record can only refuse a
// drop, never make a wrong one.
//
// It knows nothing of the guest or of the system: addresses are plain numbers, and the OS layer gives the substrate, the
// source's bytes, a view of memory, the working set and the protection through callbacks. Thread use: pass() on one
// thread; touch(), wait_pending(), kernel_use(), mark_imported(), decoded(), forget() from any thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hl::mem {

/// What makes a range's bytes again: a resource file's decode.
struct DropSource {
    uint64_t dest = 0, size = 0;   // the range the decode wrote
    uint64_t compressed = 0;       // the compressed file's size (the restore checks it)
    uint32_t dict_id = 0;          // the frame's dictionary (0: none)
    std::string name;              // the file, as the game's per-file heap names it
};

/// Why a granule was not dropped.
enum class Refuse : int { Imported, KernelUse, Resident, Changed, NoSource, NotPlain, Substrate, GivenUp, kCount };

/// Who touched a dropped granule (the restore's path).
enum class TouchPath : int { GuestRead, GuestWrite, HostRead, HostWrite, Kernel, Import, kCount };

class Drop {
public:
    struct Config {
        uint64_t granule = 65536;
        int64_t age_ns = 30'000'000'000;       // a range is looked at this long after its decode
        int64_t retry_ns = 10'000'000'000;     // a range with granules still in use is looked at again after this
        int64_t hold_ns = 60'000'000'000;      // a restored range is held this long, doubled at each further restore
        uint32_t give_up = 4;                  // restores after which a range is never dropped again
        int64_t kernel_ns = 5'000'000'000;     // a granule given to the kernel is not dropped for this long
        uint64_t pass_bytes = 64ull << 20;     // granule bytes looked at a pass at most
        int64_t pass_ns = 250'000'000;        // no further range looked at once a pass has taken this long (with Host::clock)
        bool require_cold = true;              // only granules with no page in the working set
        bool check = false;                    // a hash of each granule at its drop, compared at its restore
        size_t max_ranges = 65536;             // candidate ranges held at most (the oldest without drops go first)
    };

    // The OS layer: every callback gets `ctx`.
    struct Host {
        void* ctx = nullptr;
        // the substrate (hos_guestpages.h)
        int (*is_dropped)(void* ctx, uint64_t addr) = nullptr;
        int (*drop)(void* ctx, uint64_t addr, uint64_t size) = nullptr;   // 0: done
        // writes [addr, addr + size) from `fill` atomically; 0: done
        int (*restore)(void* ctx, uint64_t addr, uint64_t size,
                       int (*fill)(void* fill_ctx, uint8_t* buf, uint64_t addr, uint64_t size), void* fill_ctx) = nullptr;
        // the source decoded again: its bytes (size == src.size) or null; release() gives them back
        const uint8_t* (*source)(void* ctx, const DropSource& src) = nullptr;
        void (*release)(void* ctx, const uint8_t* bytes) = nullptr;
        // memory as it is now (a committed granule's bytes), pages of a granule in the working set, the protection
        const uint8_t* (*view)(void* ctx, uint64_t addr) = nullptr;
        uint32_t (*resident)(void* ctx, uint64_t granule) = nullptr;
        int (*protect)(void* ctx, uint64_t addr, uint64_t size, int read_only) = nullptr;   // 0: done
        // the protection of a granule is plain read-write committed memory (the only kind dropped)
        int (*plain)(void* ctx, uint64_t addr) = nullptr;
        // the time now, in ns (the pass's time budget; none without it)
        int64_t (*clock)(void* ctx) = nullptr;
    };

    struct Stats {
        std::atomic<uint64_t> ranges{0}, replaced{0}, passes{0};
        std::atomic<uint64_t> dropped_granules{0}, dropped_now{0};   // granules dropped in all, dropped now
        std::atomic<uint64_t> restores{0}, restored_granules{0};     // touches that restored a range; granules restored
        std::atomic<uint64_t> by_path[int(TouchPath::kCount)] = {};
        std::atomic<uint64_t> refused[int(Refuse::kCount)] = {};
        std::atomic<uint64_t> checked{0}, mismatches{0};             // restored granules hashed against the drop's hash
        std::atomic<uint64_t> restore_failed{0}, forgotten{0}, waited{0}, sources{0}, source_failed{0};
    };

    Drop(const Config& config, const Host& host);
    Drop(const Drop&) = delete;
    Drop& operator=(const Drop&) = delete;

    const Config& config() const { return config_; }
    const Stats& stats() const { return stats_; }

    /// A decode wrote [src.dest, src.dest + src.size) at `now`: a candidate from `now + age_ns`.
    void decoded(DropSource src, int64_t now);

    /// One pass of the drop thread: the ranges due, at most `pass_bytes` of granules looked at. Returns granules dropped.
    uint64_t pass(int64_t now);

    /// The substrate's hook: `addr` lies in a dropped granule. Restores its range; true when it was ours (restored now or
    /// by another thread meanwhile), false when no range of ours holds it.
    /// The kernel's and the driver's paths (Kernel, Import) restore only the dropped granules of the range inside [lo, hi)
    /// when hi > lo: an import of a 16 MB chunk would otherwise bring back every range it cuts, whole.
    bool touch(uint64_t addr, TouchPath path, int64_t now, uint64_t lo = 0, uint64_t hi = 0);

    /// A writer's fault on a granule being decided: waits for the decision. True when `addr` was being decided.
    bool wait_pending(uint64_t addr);
    /// The kernel or the driver is about to use [lo, hi) (a file read into it, a commit): waits for a decision under
    /// way there, and keeps its granules from being dropped for `kernel_ns`.
    void kernel_use(uint64_t lo, uint64_t hi, int64_t now);
    /// [lo, hi) is imported by the back end: never dropped from now on.
    void mark_imported(uint64_t lo, uint64_t hi);
    /// [lo, hi) is no longer imported (the back end gave its import back): its granules may be dropped again.
    void unmark_imported(uint64_t lo, uint64_t hi);
    /// [lo, hi) was unmapped: its ranges and dropped granules are forgotten (their bytes were given up).
    void forget(uint64_t lo, uint64_t hi);

    /// Dropped granules whose substrate says they are not dropped any more (unmapped by the OS layer meanwhile) are
    /// forgotten. Returns how many.
    uint64_t reconcile();

    /// One line of counters for the log. Returns the length written.
    int report(char* out, size_t cap, int64_t now);

private:
    enum GState : uint8_t { kCandidate = 0, kDropped = 1, kNever = 2 };
    struct Range {
        DropSource src;
        uint64_t id = 0;
        int64_t next_ns = 0;              // looked at no earlier than this
        uint32_t restores = 0;
        bool candidate = true;            // false once replaced, forgotten, or given up
        std::vector<uint8_t> state;       // a GState a whole granule, from `first`
        std::vector<uint64_t> hash;       // (check) the hash of each dropped granule
        uint64_t first = 0;               // the first whole granule's address
        uint32_t dropped = 0;
    };
    std::mutex& range_lock(uint64_t id) { return range_locks_[id % kRangeLocks]; }
    void evict_locked();
    void refuse(Refuse why, uint64_t n = 1) { stats_.refused[int(why)].fetch_add(n, std::memory_order_relaxed); }
    bool imported(uint64_t granule) const;
    bool kernel_busy(uint64_t granule, int64_t now) const;
    // (returns false when the range has nothing left to try: it stops being a candidate)
    bool decide(Range& r, int64_t now, uint64_t& looked, uint64_t& dropped);

    Config config_;
    Host host_;
    Stats stats_;

    mutable std::mutex table_lock_;
    std::map<uint64_t, Range*> by_dest_;                // candidate ranges, by first byte
    std::map<uint64_t, Range*> by_granule_;             // dropped granule -> its range
    std::unordered_map<uint64_t, Range*> all_;          // every range alive, by id
    uint64_t next_id_ = 0;
    uint64_t cursor_ = 0;                               // where the next pass goes on

    static constexpr size_t kRangeLocks = 256;
    std::mutex range_locks_[kRangeLocks];

    // the decision under way: [pending_lo_, pending_hi_), with its lock held exclusively by the drop thread
    std::shared_mutex pending_lock_;
    std::atomic<uint64_t> pending_lo_{0}, pending_hi_{0};

    mutable std::mutex marks_lock_;
    std::unordered_map<uint64_t, int64_t> kernel_until_;   // granule -> until when the kernel may use it
    std::vector<std::pair<uint64_t, uint64_t>> imported_;  // ranges ever imported, merged
    uint64_t last_dropped_report_ = 0, last_restores_report_ = 0;
    int64_t last_report_ns_ = 0;
};

}   // namespace hl::mem
