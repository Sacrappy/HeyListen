// HeyListen native engine: the main loop.
//
// One frame is a fixed list of phases run in order on the thread that owns the loop (input, the gameplay step, render
// submission, present ...). A phase gets the job system and spreads its work over it; the loop itself only keeps time:
// the frame's step, an optional frame rate cap, and the last frames' durations.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include "hl/jobs/JobSystem.h"

namespace hl::app {

struct FrameTime {
    uint64_t frame = 0;        ///< frames run before this one
    double step = 0;           ///< seconds since the frame before began (clamped to Config::longest_step)
    double target_hz = 0;      ///< the cap, 0: none
};

struct Phase {
    const char* name = "";
    void (*run)(void* user, const FrameTime& time, jobs::JobSystem& jobs) = nullptr;
    void* user = nullptr;
};

class MainLoop {
public:
    struct Config {
        double target_hz = 0;          ///< frames a second, 0: as fast as the phases run
        double longest_step = 0.05;    ///< a frame's step is never longer (a stall is not simulated as one long step)
    };
    struct Stats {
        uint64_t frames = 0;
        double last_ms = 0, mean_ms = 0, worst_ms = 0;   ///< over the last 240 frames (worst_ms too)
    };

    MainLoop(jobs::JobSystem& jobs, const Config& config);

    void add_phase(const Phase& phase);
    void step();                                   ///< one frame
    void run(const std::atomic<bool>& quit);       ///< frames until `quit` is set
    Stats stats() const { return stats_; }

private:
    using Clock = std::chrono::steady_clock;
    void pace(Clock::time_point frame_begin);

    jobs::JobSystem& jobs_;
    Config config_;
    std::vector<Phase> phases_;
    Clock::time_point last_begin_{};
    bool first_ = true;
    Stats stats_;
    std::vector<double> recent_ms_;
    size_t recent_at_ = 0;
};

}   // namespace hl::app
