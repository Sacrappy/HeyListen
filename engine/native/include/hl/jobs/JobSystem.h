// HeyListen native engine: the job system.
//
// Native x64 code, written for this engine: nothing here comes from the game. It schedules two kinds of work on one pool
// of worker threads:
//   - native jobs, the engine's own subsystems: any worker, as many at once as there are workers;
//   - guest jobs, gameplay code still run from the user's locally generated translation: only under a lane
//     (GuestLanes.h), one of the six job-thread identities that code was built around.
// A job may depend on other jobs; it becomes ready when they are done. A thread that waits for a job runs other jobs
// meanwhile.
//
// This is the first version: one lock around the queues, correct before fast. The interface is the one the rest of the
// engine is written against; the queues behind it are to become per-worker deques once the bridge to the game is on.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include "hl/jobs/GuestLanes.h"

namespace hl::jobs {

enum class JobClass : uint8_t { Native, Guest };
enum class Priority : uint8_t { High, Normal, Low };
inline constexpr int kPriorities = 3;

/// A job that was submitted. A handle whose job is done (and its slot given to another job since) reads as done.
struct JobHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
    explicit operator bool() const { return generation != 0; }
};

/// What a running job is told: the worker it is on (kExternalWorker: a thread that is not one of the pool's) and, for a
/// guest job, the lane it runs under (-1 for a native job).
struct JobContext {
    uint32_t worker;
    int lane;
};
inline constexpr uint32_t kExternalWorker = 0xFFFFFFFFu;

using JobFn = void (*)(void* arg, const JobContext& ctx);

struct JobDesc {
    JobFn fn = nullptr;
    void* arg = nullptr;
    JobClass cls = JobClass::Native;
    Priority priority = Priority::Normal;
    uint8_t lanes = kAllLanes;   ///< a guest job: the lanes it may run under (bit i: lane i)
    const char* name = "";       ///< for traces; not copied
};

class JobSystem {
public:
    struct Config {
        unsigned workers = 0;            ///< 0: the hardware's threads but one, at least one
        unsigned capacity = 1u << 14;    ///< jobs alive at once
        GuestLaneHost* lane_host = nullptr;   ///< told when a lane is taken and given back (the bridge); may be null
    };
    struct Stats {
        uint64_t native_jobs = 0, guest_jobs = 0;   ///< run so far
        uint64_t helped = 0;                        ///< of them, run by a thread that was waiting for another job
        unsigned most_lanes_at_once = 0;
    };

    explicit JobSystem(const Config& config);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Queues a job once every job of `after` is done. Blocks (running other jobs) while no slot is free.
    JobHandle submit(const JobDesc& desc, std::span<const JobHandle> after = {});
    /// Returns when the job is done; the calling thread runs other jobs meanwhile.
    void wait(JobHandle job);
    void wait_all(std::span<const JobHandle> jobs);
    bool done(JobHandle job) const;

    /// f(begin, end) over [0, count) in pieces of at most `grain`, as native jobs; returns when all are done.
    template <class F>
    void parallel_for(size_t count, size_t grain, F&& f);

    unsigned worker_count() const { return (unsigned)workers_.size(); }
    Stats stats() const;

private:
    struct Job {
        JobDesc desc;
        uint32_t generation = 1;   // (never 0: a zero handle is "no job")
        int32_t pending = 0;       // jobs it waits for
        bool live = false;
        std::vector<uint32_t> dependents;
    };
    struct Pick {
        uint32_t index = 0;
        int lane = -1;
        bool took_lane = false;
        bool found = false;
    };

    void worker_main(uint32_t worker);
    Pick take_locked(int held_lane);
    void run(const Pick& pick, uint32_t worker, bool helping);
    void finish_locked(const Pick& pick);
    bool alive_locked(JobHandle job) const;
    void help_until(std::unique_lock<std::mutex>& lock, JobHandle job);

    Config config_;
    mutable std::mutex mu_;
    std::condition_variable work_cv_, done_cv_;
    std::vector<Job> jobs_;
    std::vector<uint32_t> free_;
    std::deque<uint32_t> ready_[2][kPriorities];   // [class][priority]
    GuestLanes lanes_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
    Stats stats_;
};

template <class F>
void JobSystem::parallel_for(size_t count, size_t grain, F&& f) {
    if (!count) return;
    if (!grain) grain = 1;
    using Fn = std::remove_reference_t<F>;
    struct Piece {
        Fn* f;
        size_t begin, end;
    };
    const size_t pieces = (count + grain - 1) / grain;
    std::vector<Piece> piece(pieces);
    std::vector<JobHandle> handles(pieces);
    for (size_t i = 0; i < pieces; i++) {
        piece[i] = {&f, i * grain, std::min(count, (i + 1) * grain)};
        JobDesc d;
        d.fn = [](void* arg, const JobContext&) {
            Piece* p = static_cast<Piece*>(arg);
            (*p->f)(p->begin, p->end);
        };
        d.arg = &piece[i];
        d.name = "parallel_for";
        handles[i] = submit(d);
    }
    wait_all(handles);
}

}   // namespace hl::jobs
