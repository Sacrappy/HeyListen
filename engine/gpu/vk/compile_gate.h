// (Longshot c392df3a, Tomiss, PR #426: "the shader preparation and first start finish on Linux iGPUs: Mesa's Intel driver and
// RADV APUs compile what the memory holds", as gpu/vk/compute_gate.h) How many of the GPU driver's compiles run at once: the
// shader preparation, the start-up pipeline build (precompile_run), the prediction, and play's own compute builds.
//
// A driver's compile takes the PC's RAM, not the GPU's memory: Mesa's Intel compiler (ANV) spent gigabytes on the game's large
// programs and holds them until the pipeline is made, and on a Linux PC with 31 GB (issue 88: Fedora 45, Arc B580, Mesa
// 26.2.6) the start-up build ran twelve pipelines at once on twelve threads and the kernel killed the process at 1,360 of
// 16,358, with the shader preparation finished and nothing else in the log. Upstream measured the same shape on an Iris Xe
// (32 GB): above 24 GB of private memory and 7 GB of swap, killed at the end of the build and at every start.
//
// So each compile is weighed, and a compile starts beside the ones running only while the weights of all of them fit in what
// was available past kReserveMB, and the system still has its weight available past kReserveMB now (for whatever else took
// the memory meanwhile). One always starts when none runs; never more than kMost. A worker whose compile has to wait, waits:
// its program is compiled when its turn comes, never left out.
//
// The weights, by what was measured here and upstream:
//  * a compute compile: upstream's kPinnedMB / kHeavyMB / kMeshMB (a program held to 32 lanes, one of 2,000 phis or more,
//    a mesh fallback stage) and on RADV by its phis (kRadv*). Unchanged from c392df3a.
//  * a start-up graphics pipeline, by its program's SPIR-V (weight_words): the PC 3050's run measured 133k words at 25 s and a
//    few hundred MB, 502k words past 11 GB, 1014k words 29 GB (44ecd248's note, gpu/vk/predict_limits.h). The thresholds
//    below are that run's, rounded up to the next of the sizes it measured.
//
// A compile that held its turn for kTrimAfterMs or longer hands the memory it freed back to the system when it leaves
// (trims(), glibc's malloc_trim): the compile runs on whichever worker's turn it is, glibc gives each thread its own heap,
// and what the compile freed stays pinned under the pipeline it left behind.
//
// RX_VK_COMPILE_AT_ONCE=<n>: at most n at once on any GPU, whatever the memory; 0: no limit, as before. Unset: by memory, on
// Linux with Mesa's Intel driver or on an integrated AMD GPU with under kRadvSmallRamMB of RAM (upstream's rule for RADV),
// and on no other driver (nobody has run the gate there). Windows keeps its own gates: IntelDriverGate, and the commit rule
// that predates this (44ecd248's prediction reserve).
// No Vulkan here: tests/test_compile_gate.py and tests/compile_gate_test.cpp.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace compile_gate {

constexpr uint64_t kReserveMB = 4096;   // (memory left to the rest of the PC, as upstream's compute_gate.h)
constexpr uint64_t kPinnedMB = 2048;    // (an ordinary compute compile held to 32 lanes: twice upstream's measured peak)
constexpr uint32_t kHeavyPhis = 2000;   // (a pinned program this big may take minutes and gigabytes)
constexpr uint64_t kHeavyMB = 8192;     // (upstream's 7.6 GB measured)
constexpr uint64_t kMeshMB = 3072;      // (a mesh fallback stage: twice the largest measured, 1.55 GB)
constexpr uint64_t kFreeMB = 512;       // (one at the lanes the driver chooses: the largest measured, with room)
constexpr unsigned kMost = 16;
constexpr uint64_t kUnknownMB = UINT64_MAX;   // (the system does not say what is available: one at a time)
constexpr int64_t kTrimAfterMs = 1000;        // (a compile this long held memory worth handing back; the short ones did not)
// (RADV: the two mesh fallback stages of 80,000 phis took 2.9 GB, those up to 29,400 under 700 MB, on discrete cards: upstream's
// fallback_size_policy.h. Each weighs twice that, as kPinnedMB and kMeshMB are twice their measured peaks; not measured on an APU)
constexpr uint32_t kRadvBigPhis = 40000;
constexpr uint64_t kRadvBigMB = 6144;
constexpr uint32_t kRadvMidPhis = 10000;
constexpr uint64_t kRadvMidMB = 1536;
constexpr uint64_t kRadvMB = 512;   // (twice the under 200 MB Mesa's Intel compiler took for most: not RADV's own figure)
constexpr uint64_t kRadvSmallRamMB = 13312;   // (#1398's 12 GB is under it; a 16 GB PC or a Steam Deck, about 14.5 GB, is not)
// (a start-up graphics pipeline, by the program's SPIR-V in 32-bit words: predict_limits.h's PC 3050 measurements. 133k words
// took 25 s and a few hundred MB; 502k words passed 11 GB and 1014k words 29 GB over three minutes: 22 and 28.6 MB for each
// thousand words, so above kWordsHuge the weight grows with the words, at 28 MB a thousand, up to kWordsMaxMB)
constexpr uint64_t kWordsSmall = 32;    // (thousand words)
constexpr uint64_t kWordsMedium = 96;
constexpr uint64_t kWordsLarge = 192;
constexpr uint64_t kWordsHuge = 320;
constexpr uint64_t kWordsSmallMB = 256;
constexpr uint64_t kWordsMediumMB = 768;
constexpr uint64_t kWordsLargeMB = 1536;
constexpr uint64_t kWordsHugeMB = 4096;
constexpr uint64_t kWordsMaxMBPerKWord = 28;
constexpr uint64_t kWordsMaxMB = 30000;   // (the most a compile is ever weighed: nothing measured passed 29 GB

/// A compute compile's weight: held to 32 lanes or not, how many phis its SPIR-V has, and whether it is a mesh fallback stage
inline uint64_t weight(bool pinned, uint32_t phis, bool mesh_fallback = false) {
    return !pinned ? kFreeMB : phis >= kHeavyPhis ? kHeavyMB : mesh_fallback ? kMeshMB : kPinnedMB;
}

/// A compute compile's weight on RADV: by its SPIR-V's phis alone (its compiler's memory grows with them, whatever the lanes)
inline uint64_t weight_radv(uint32_t phis) {
    return phis >= kRadvBigPhis ? kRadvBigMB : phis >= kRadvMidPhis ? kRadvMidMB : kRadvMB;
}

/// A start-up graphics pipeline's weight, by its program's SPIR-V (all stages, in 32-bit words)
inline uint64_t weight_words(uint64_t spirv_words) {
    const uint64_t k = spirv_words / 1000;
    if (k > kWordsHuge) return std::min(kWordsMaxMB, k * kWordsMaxMBPerKWord);
    return k <= kWordsSmall ? kWordsSmallMB : k <= kWordsMedium ? kWordsMediumMB : k <= kWordsLarge ? kWordsLargeMB : kWordsHugeMB;
}

/// Whether RADV's compiles are gated by the memory by default: an integrated GPU (its memory is the system's) on a PC with
/// under kRadvSmallRamMB of RAM, ram_mb as the system reports it (0: not known, no gate)
inline bool radv_by_memory(bool integrated, uint64_t ram_mb) { return integrated && ram_mb && ram_mb < kRadvSmallRamMB; }

/// What a cgroup's limit leaves, in MB: memory.max's text less memory.current's bytes; 0 at or past the limit. No limit
/// ("max") or text that is not a number: kUnknownMB, no bound from it. memory.current counts the page cache too, which the
/// kernel takes back first: the safe side
inline uint64_t cgroup_room_mb(const char* max_text, uint64_t current_bytes) {
    if (!max_text || *max_text < '0' || *max_text > '9') return kUnknownMB;
    uint64_t max = 0;
    for (const char* c = max_text; *c >= '0' && *c <= '9'; c++) max = max * 10 + (uint64_t)(*c - '0');
    return max > current_bytes ? (max - current_bytes) >> 20 : 0;
}

/// What the compiles may hold together, with avail_mb available when the first one starts: what is past kReserveMB.
/// 0 (one at a time) when the system does not say
inline uint64_t budget_for(uint64_t avail_mb) {
    return avail_mb == kUnknownMB || avail_mb <= kReserveMB ? 0 : avail_mb - kReserveMB;
}

/// Whether a compile of weight_mb fits the budget alone at all: a compile heavier than everything past the reserve would be
/// the process's death, so the start-up build leaves it to the draw that needs it (predict_limits' rule, for the compiles whose
/// size the notes did not catch). No gate or no memory rule: it fits.
inline bool fits_alone(uint64_t weight_mb, bool by_memory, uint64_t budget_mb) {
    return !by_memory || budget_mb == 0 || weight_mb <= budget_mb;
}

/// Whether a compile of weight_mb may start now, running compiles holding running_mb by their weights.
/// limit: a count that was asked for (RX_VK_COMPILE_AT_ONCE), 0 for none. by_memory: the budget and the memory decide.
/// Neither: no gate. With none running: always (the preparation never stops for lack of memory, it only stops adding to
/// what it holds)
inline bool may_start(unsigned running, uint64_t running_mb, uint64_t weight_mb, unsigned limit, bool by_memory,
                      uint64_t budget_mb, uint64_t avail_mb) {
    if (!limit && !by_memory) return true;
    if (!running) return true;
    if (limit && running >= limit) return false;
    if (!by_memory) return true;
    if (running >= kMost || running_mb + weight_mb > budget_mb) return false;
    return avail_mb != kUnknownMB && avail_mb >= kReserveMB + weight_mb;
}

/// Whether a compile that held its turn for held_ms hands the freed memory back to the system as it leaves: only where
/// there is a gate (none: nothing changes), and only after a long one
inline bool trims(bool gated, int64_t held_ms) { return gated && held_ms >= kTrimAfterMs; }

/// The gate itself. No gate until set: every enter() returns at once
class Gate {
public:
    /// (before the first enter, or at any time: the waiting compiles see it at their next look)
    void set(unsigned limit, bool by_memory, uint64_t budget_mb = 0) {
        std::lock_guard lk(mu);
        limit_ = limit, by_memory_ = by_memory, budget_mb_ = budget_mb;
        cv.notify_all();
    }
    bool on() {
        std::lock_guard lk(mu);
        return limit_ || by_memory_;
    }
    /// Waits for a turn for a compile of weight_mb. avail(): the memory available now, in MB (kUnknownMB when the system
    /// does not say), asked only when it decides. stop(): looked at while waiting; once true the wait ends and enter
    /// returns false, with no turn taken (the caller does not compile). True: the turn is taken, leave(weight_mb) gives
    /// it back
    template <class Avail, class Stop>
    bool enter(uint64_t weight_mb, Avail&& avail, Stop&& stop) {
        std::unique_lock lk(mu);
        bool waited = false;
        for (;;) {
            const bool memory_decides = by_memory_ && running_ && running_ < kMost && (!limit_ || running_ < limit_) &&
                                        running_mb_ + weight_mb <= budget_mb_;
            if (may_start(running_, running_mb_, weight_mb, limit_, by_memory_, budget_mb_, memory_decides ? avail() : kUnknownMB))
                break;
            if (stop()) return false;
            if (!waited) waited = true, waits_++;
            // (a look every 250 ms as well as at each leave(): memory comes back without one, and stop() is only asked here)
            cv.wait_for(lk, std::chrono::milliseconds(250));
        }
        running_++, running_mb_ += weight_mb;
        most_ = std::max(most_, running_);
        return true;
    }
    void leave(uint64_t weight_mb) {
        {
            std::lock_guard lk(mu);
            running_--, running_mb_ -= std::min(running_mb_, weight_mb);
        }
        cv.notify_all();
    }
    /// (for the log: the most that ran at once, and how many compiles had to wait for a turn)
    void seen(unsigned* most, uint64_t* waits) {
        std::lock_guard lk(mu);
        *most = most_, *waits = waits_;
    }

private:
    std::mutex mu;
    std::condition_variable cv;
    unsigned limit_ = 0, running_ = 0, most_ = 0;
    bool by_memory_ = false;
    uint64_t budget_mb_ = 0, running_mb_ = 0, waits_ = 0;
};

}  // namespace compile_gate
