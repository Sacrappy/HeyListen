// HeyListen native engine: the graphics recording pass (docs/native-engine/gfx-record-job.md, unit A).
//
// The game's graphics module records a frame's scene into NVN command buffers in jobs; the largest part of that recording
// is one pass, which the recording job reaches through a list of listeners while it holds a global lock. The pass takes
// the listener and an argument block, and by the block's mode records either the scene pass or a second, smaller one.
//
// RecordPass is that pass's own level: it reads the mode and runs the phase it names. The phases are still the game's
// translated functions (PassHost runs them); they become native here one at a time, each under the check (PassCheck.h),
// and what they record goes out through one seam (Commands.h).
#pragma once

#include <atomic>
#include <cstdint>

namespace hl::gfx {

/// The pass's modes, as the argument block gives them.
enum class PassMode : uint32_t { Scene = 0, Second = 1 };

/// What the pass needs from the game's side (the bridge: hl/bridge/GfxBridge.h).
struct PassHost {
    virtual ~PassHost() = default;
    /// The mode in the argument block.
    virtual uint32_t mode(uint64_t args) const = 0;
    /// Runs one phase of the pass on the calling thread, which is running the pass for the game.
    virtual void run_phase(PassMode phase, uint64_t listener, uint64_t args) = 0;
};

class RecordPass {
public:
    struct Stats {
        std::atomic<uint64_t> passes{0};
        std::atomic<uint64_t> scene{0};    ///< of them, mode Scene
        std::atomic<uint64_t> second{0};   ///< mode Second
    };

    explicit RecordPass(PassHost& host) : host_(host) {}

    /// The pass, for the listener and argument block the game gave it. Returns what the listener returns to the list's
    /// walk (1: go on).
    uint32_t run(uint64_t listener, uint64_t args);

    const Stats& stats() const { return stats_; }

private:
    PassHost& host_;
    Stats stats_;
};

}   // namespace hl::gfx
