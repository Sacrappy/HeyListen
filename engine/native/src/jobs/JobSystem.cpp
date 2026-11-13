// HeyListen native engine: the job system (hl/jobs/JobSystem.h). First version: one lock, correct before fast.
#include "hl/jobs/JobSystem.h"

#include <algorithm>

namespace hl::jobs {

namespace {
// What the current thread is doing for this pool: its worker number, and the lane of the guest job it is inside of
// (-1: none). A thread that waits inside a guest job may run another guest job only under that same lane.
thread_local uint32_t tls_worker = kExternalWorker;
thread_local int tls_lane = -1;
}   // namespace

JobSystem::JobSystem(const Config& config) : config_(config) {
    unsigned n = config_.workers;
    if (!n) {
        const unsigned hw = std::thread::hardware_concurrency();
        n = hw > 1 ? hw - 1 : 1;
    }
    if (config_.capacity < 16) config_.capacity = 16;
    jobs_.resize(config_.capacity);
    free_.reserve(config_.capacity);
    for (uint32_t i = config_.capacity; i-- > 0;) free_.push_back(i);
    workers_.reserve(n);
    for (uint32_t w = 0; w < n; w++) workers_.emplace_back([this, w] { worker_main(w); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    work_cv_.notify_all();
    done_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
}

bool JobSystem::alive_locked(JobHandle job) const {
    return job.generation && job.index < jobs_.size() && jobs_[job.index].live && jobs_[job.index].generation == job.generation;
}

bool JobSystem::done(JobHandle job) const {
    std::lock_guard lock(mu_);
    return !alive_locked(job);
}

JobSystem::Stats JobSystem::stats() const {
    std::lock_guard lock(mu_);
    Stats s = stats_;
    s.most_lanes_at_once = lanes_.most_at_once();
    return s;
}

JobHandle JobSystem::submit(const JobDesc& desc, std::span<const JobHandle> after) {
    std::unique_lock lock(mu_);
    while (free_.empty() && !stop_) {   // (every slot taken: run what is ready, or wait for a job to end)
        const Pick pick = take_locked(tls_lane);
        if (pick.found) {
            lock.unlock();
            run(pick, tls_worker, true);
            lock.lock();
            finish_locked(pick);
        } else {
            done_cv_.wait(lock);
        }
    }
    if (free_.empty()) return {};
    const uint32_t index = free_.back();
    free_.pop_back();
    Job& j = jobs_[index];
    j.desc = desc;
    j.live = true;
    j.pending = 0;
    j.dependents.clear();
    for (const JobHandle dep : after)
        if (alive_locked(dep)) {
            jobs_[dep.index].dependents.push_back(index);
            j.pending++;
        }
    const JobHandle handle{index, j.generation};
    if (!j.pending) {
        ready_[(int)desc.cls][(int)desc.priority].push_back(index);
        lock.unlock();
        work_cv_.notify_one();
        done_cv_.notify_all();   // (a thread helping while it waits takes ready jobs too)
    }
    return handle;
}

JobSystem::Pick JobSystem::take_locked(int held_lane) {
    for (int p = 0; p < kPriorities; p++) {
        std::deque<uint32_t>& native = ready_[(int)JobClass::Native][p];
        if (!native.empty()) {
            Pick pick{native.front(), -1, false, true};
            native.pop_front();
            return pick;
        }
        std::deque<uint32_t>& guest = ready_[(int)JobClass::Guest][p];
        for (size_t i = 0; i < guest.size(); i++) {
            const uint8_t allowed = jobs_[guest[i]].desc.lanes;
            Pick pick{guest[i], -1, false, true};
            if (held_lane >= 0) {   // (inside a guest job: only under the lane this thread already has)
                if (!(allowed >> held_lane & 1)) continue;
                pick.lane = held_lane;
            } else {
                pick.lane = lanes_.take(allowed);
                if (pick.lane < 0) continue;
                pick.took_lane = true;
            }
            guest.erase(guest.begin() + (std::ptrdiff_t)i);
            return pick;
        }
    }
    return {};
}

void JobSystem::run(const Pick& pick, uint32_t worker, bool helping) {
    JobDesc desc;
    {
        std::lock_guard lock(mu_);
        desc = jobs_[pick.index].desc;
        (desc.cls == JobClass::Guest ? stats_.guest_jobs : stats_.native_jobs)++;
        if (helping) stats_.helped++;
    }
    const int lane_before = tls_lane;
    if (pick.took_lane && config_.lane_host) config_.lane_host->enter(pick.lane, worker);
    if (pick.lane >= 0) tls_lane = pick.lane;
    desc.fn(desc.arg, JobContext{worker, pick.lane});
    tls_lane = lane_before;
    if (pick.took_lane && config_.lane_host) config_.lane_host->leave(pick.lane, worker);
}

void JobSystem::finish_locked(const Pick& pick) {
    Job& j = jobs_[pick.index];
    if (pick.took_lane) lanes_.give_back(pick.lane);
    bool woke = false;
    for (const uint32_t d : j.dependents) {
        Job& dep = jobs_[d];
        if (--dep.pending == 0) {
            ready_[(int)dep.desc.cls][(int)dep.desc.priority].push_back(d);
            woke = true;
        }
    }
    j.dependents.clear();
    j.live = false;
    if (++j.generation == 0) j.generation = 1;
    free_.push_back(pick.index);
    // (a lane given back can make a queued guest job runnable, so the workers are told in every case)
    if (woke || pick.took_lane) work_cv_.notify_all();
    done_cv_.notify_all();
}

void JobSystem::worker_main(uint32_t worker) {
    tls_worker = worker;
    std::unique_lock lock(mu_);
    for (;;) {
        if (stop_) return;
        const Pick pick = take_locked(-1);
        if (!pick.found) {
            work_cv_.wait(lock);
            continue;
        }
        lock.unlock();
        run(pick, worker, false);
        lock.lock();
        finish_locked(pick);
    }
}

void JobSystem::help_until(std::unique_lock<std::mutex>& lock, JobHandle job) {
    while (alive_locked(job) && !stop_) {
        const Pick pick = take_locked(tls_lane);
        if (pick.found) {
            lock.unlock();
            run(pick, tls_worker, true);
            lock.lock();
            finish_locked(pick);
        } else {
            done_cv_.wait(lock);
        }
    }
}

void JobSystem::wait(JobHandle job) {
    std::unique_lock lock(mu_);
    help_until(lock, job);
}

void JobSystem::wait_all(std::span<const JobHandle> jobs) {
    std::unique_lock lock(mu_);
    for (const JobHandle job : jobs) help_until(lock, job);
}

}   // namespace hl::jobs
