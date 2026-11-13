// HeyListen native engine: the main loop (hl/app/MainLoop.h).
#include "hl/app/MainLoop.h"

#include <algorithm>
#include <thread>

namespace hl::app {

namespace {
constexpr size_t kRecent = 240;
}

MainLoop::MainLoop(jobs::JobSystem& jobs, const Config& config) : jobs_(jobs), config_(config) { recent_ms_.reserve(kRecent); }

void MainLoop::add_phase(const Phase& phase) {
    if (phase.run) phases_.push_back(phase);
}

void MainLoop::pace(Clock::time_point frame_begin) {
    if (config_.target_hz <= 0) return;
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / config_.target_hz));
    const Clock::time_point due = frame_begin + period;
    // (sleeping wakes late by up to a scheduler tick: sleep to 1.5 ms before the deadline, then yield up to it)
    const auto margin = std::chrono::microseconds(1500);
    if (Clock::now() + margin < due) std::this_thread::sleep_until(due - margin);
    while (Clock::now() < due) std::this_thread::yield();
}

void MainLoop::step() {
    const Clock::time_point begin = Clock::now();
    FrameTime time;
    time.frame = stats_.frames;
    time.target_hz = config_.target_hz;
    time.step = first_ ? (config_.target_hz > 0 ? 1.0 / config_.target_hz : 1.0 / 60.0)
                       : std::min(std::chrono::duration<double>(begin - last_begin_).count(), config_.longest_step);
    first_ = false;
    last_begin_ = begin;
    for (const Phase& phase : phases_) phase.run(phase.user, time, jobs_);
    pace(begin);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    if (recent_ms_.size() < kRecent) recent_ms_.push_back(ms);
    else recent_ms_[recent_at_] = ms;
    recent_at_ = (recent_at_ + 1) % kRecent;
    double sum = 0, worst = 0;
    for (const double v : recent_ms_) sum += v, worst = std::max(worst, v);
    stats_.frames++;
    stats_.last_ms = ms;
    stats_.mean_ms = sum / (double)recent_ms_.size();
    stats_.worst_ms = worst;
}

void MainLoop::run(const std::atomic<bool>& quit) {
    while (!quit.load(std::memory_order_relaxed)) step();
}

}   // namespace hl::app
