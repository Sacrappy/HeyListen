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
    worker_count_ = n;
    starts_.resize(n);
    for (uint32_t w = 0; w < n; w++) starts_[w] = {this, w};
    if (config_.thread_host) {
        hosted_.reserve(n);
        for (uint32_t w = 0; w < n; w++) hosted_.push_back(config_.thread_host->start(&JobSystem::worker_entry, &starts_[w], w));
    } else {
        workers_.reserve(n);
        for (uint32_t w = 0; w < n; w++) workers_.emplace_back([this, w] { worker_main(w); });
    }
}

void JobSystem::worker_entry(void* start) {
    const WorkerStart* s = static_cast<const WorkerStart*>(start);
    s->self->worker_main(s->worker);
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    work_cv_.notify_all();
    done_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
    for (void* h : hosted_) config_.thread_host->join(h);
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
        const Pick pick = take_locked(tls_lane, true);
        if (pick.found) {
            lock.unlock();
            run(pick, tls_worker, true);
            lock.lock();
            finish_locked(pick);
        } else {
            wait_done(lock);
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
        wake_for_locked(desc.cls);
    }
    return handle;
}

// A job became ready: one worker that can run it, and the threads that help while they wait.
void JobSystem::wake_for_locked(JobClass cls) {
    if (idle_workers_ && (cls == JobClass::Native || config_.guest_on_workers)) work_cv_.notify_one();
    if (waiters_) done_cv_.notify_all();
}

void JobSystem::wait_done(std::unique_lock<std::mutex>& lock) {
    waiters_++;
    done_cv_.wait(lock);
    waiters_--;
}

// The lane job `index` would run under on this thread: the one the thread already has (inside a guest job), else a free
// one of the job's, taken. False when it cannot run here now.
bool JobSystem::lane_for_locked(uint32_t index, int held_lane, Pick& pick) {
    const uint8_t allowed = jobs_[index].desc.lanes;
    if (held_lane >= 0) {
        if (!(allowed >> held_lane & 1)) return false;
        pick.lane = held_lane;
        return true;
    }
    pick.lane = lanes_.take(allowed);
    if (pick.lane < 0) return false;
    pick.took_lane = true;
    return true;
}

// One job, if it is ready and can run on this thread now.
JobSystem::Pick JobSystem::take_job_locked(uint32_t index, int held_lane) {
    const JobDesc& desc = jobs_[index].desc;
    std::deque<uint32_t>& q = ready_[(int)desc.cls][(int)desc.priority];
    const auto at = std::find(q.begin(), q.end(), index);
    if (at == q.end()) return {};
    Pick pick{index, -1, false, true};
    if (desc.cls == JobClass::Guest && !lane_for_locked(index, held_lane, pick)) return {};
    q.erase(at);
    return pick;
}

JobSystem::Pick JobSystem::take_locked(int held_lane, bool guest_too) {
    for (int p = 0; p < kPriorities; p++) {
        std::deque<uint32_t>& native = ready_[(int)JobClass::Native][p];
        if (!native.empty()) {
            Pick pick{native.front(), -1, false, true};
            native.pop_front();
            return pick;
        }
        if (!guest_too) continue;
        std::deque<uint32_t>& guest = ready_[(int)JobClass::Guest][p];
        for (size_t i = 0; i < guest.size(); i++) {
            Pick pick{guest[i], -1, false, true};
            if (!lane_for_locked(guest[i], held_lane, pick)) continue;
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
        else if (desc.cls == JobClass::Guest) stats_.guest_on_workers++;
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
            woke = woke || dep.desc.cls == JobClass::Native || config_.guest_on_workers;
        }
    }
    j.dependents.clear();
    j.live = false;
    if (++j.generation == 0) j.generation = 1;
    free_.push_back(pick.index);
    // (a lane given back can make a queued guest job runnable, so the workers are told in every case)
    if (idle_workers_ && (woke || (pick.took_lane && config_.guest_on_workers))) work_cv_.notify_all();
    if (waiters_) done_cv_.notify_all();
}

void JobSystem::worker_main(uint32_t worker) {
    tls_worker = worker;
    std::unique_lock lock(mu_);
    for (;;) {
        if (stop_) return;
        const Pick pick = take_locked(-1, config_.guest_on_workers);
        if (!pick.found) {
            idle_workers_++;
            work_cv_.wait(lock);
            idle_workers_--;
            continue;
        }
        lock.unlock();
        run(pick, worker, false);
        lock.lock();
        finish_locked(pick);
    }
}

void JobSystem::help_until(std::unique_lock<std::mutex>& lock, JobHandle job, Help help) {
    while (alive_locked(job) && !stop_) {
        // (the job waited for first: nothing is gained by waking another thread to run it)
        Pick pick = help == Help::None ? Pick{} : take_job_locked(job.index, tls_lane);
        if (!pick.found && help == Help::Any) pick = take_locked(tls_lane, true);
        if (pick.found) {
            lock.unlock();
            run(pick, tls_worker, true);
            lock.lock();
            finish_locked(pick);
        } else {
            wait_done(lock);
        }
    }
}

void JobSystem::wait(JobHandle job, Help help) {
    std::unique_lock lock(mu_);
    help_until(lock, job, help);
}

void JobSystem::wait_all(std::span<const JobHandle> jobs, Help help) {
    std::unique_lock lock(mu_);
    for (const JobHandle job : jobs) help_until(lock, job, help);
}

}   // namespace hl::jobs
