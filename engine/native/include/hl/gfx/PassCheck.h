// HeyListen native engine: the check of the graphics front end's recording pass (docs/native-engine/gfx-record-job.md,
// "The check").
//
// A pass's visible output is the stream of NVN calls it records. The check records that stream for every pass of a run
// (PassRecording, a CommandSink fed by the OS layer's NVN layer while the pass runs on that thread), on two paths chosen
// pass by pass at random inside one process: the translated pass as the game always ran it, and the native one. It then
// compares what the two paths recorded, by the pass's mode and the shape of its stream (the sequence of procedures).
//
// What it proves: over a run, the stream shapes the native path recorded were also recorded by the translated path in the
// same process, scene and minutes (and the reverse), and each procedure is called as often a pass on both paths (the
// largest difference of any procedure called at least once a pass on average). What it does not: that one given pass
// would have recorded the same arguments on the other path (a pass cannot be run twice: it advances the game's slot
// generations and records GPU events), nor anything about the pass's writes to the game's own structures.
// A shape that varies from frame to frame (a moving camera changes the draws) shows on both sides as unmatched shapes of
// the same order; a native path that differs shows as unmatched shapes on its side only.
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hl/gfx/Commands.h"

namespace hl::gfx {

/// The commands of one pass, in order.
class PassRecording final : public CommandSink {
public:
    void emit(const Command& command) override { calls_.push_back(command); }
    void clear() { calls_.clear(); }
    const std::vector<Command>& calls() const { return calls_; }
    /// A 64-bit digest of the sequence of procedures (not their arguments).
    uint64_t shape() const;

private:
    std::vector<Command> calls_;
};

enum class PassPath : uint8_t { Translated = 0, Native = 1 };

class PassCheck {
public:
    struct PathStats {
        uint64_t passes = 0;
        uint64_t calls = 0;
        uint64_t most_calls = 0;      ///< in one pass
        uint64_t shapes = 0;          ///< distinct (mode, shape)
        uint64_t unmatched = 0;       ///< passes whose (mode, shape) the other path never recorded
        uint64_t unmatched_shapes = 0;
    };
    struct Report {
        PathStats path[2];
        uint64_t by_mode[3] = {};   ///< passes of mode 0, 1, and any other, both paths
        uint32_t ops_compared = 0;  ///< procedures called at least once a pass on average on the translated path
        double most_op_difference = 0;   ///< the largest relative difference of their calls a pass, native against translated
        uint32_t most_op = 0;            ///< the procedure it is of
    };

    /// Which path the next pass takes: each pass at random, half and half, so that passes that come in a fixed order in a
    /// frame are seen on both paths.
    PassPath next_path();
    void add(PassPath path, uint32_t mode, const PassRecording& recording);
    Report report() const;

private:
    mutable std::mutex lock_;
    uint64_t draw_ = 0x9e3779b97f4a7c15ull;
    uint64_t by_mode_[3] = {};
    PathStats stats_[2];
    std::unordered_map<uint64_t, uint64_t> seen_[2];   ///< (mode, shape) -> passes
    std::vector<uint64_t> op_calls_[2];                ///< calls by procedure
};

}   // namespace hl::gfx
