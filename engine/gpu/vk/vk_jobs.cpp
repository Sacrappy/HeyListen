// The worker threads (vk_jobs.h). As many as a third of the hardware threads (2 to 4): the game itself runs six
// virtual cores, and the replay thread is one of the busiest.
#include <algorithm>
#include <atomic>
#include <exception>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "vk_jobs.h"
#include "vk_internal.h"
#include "vk_draw.h"   // (ub_thread_off)

namespace mkwvk {
namespace {

std::mutex mu;
std::condition_variable work_cv, done_cv;
struct Entry {
    std::function<void()> fn;
    const std::atomic<bool>* flag;   // (what the job sets when done, for jobs_wait to find it while queued)
};
std::deque<Entry> queue;
std::atomic<int> pending{0};
bool started = false;

// (a job that throws: the game ended at once ("approach Malon, crash": an exception escaped a worker, KERNELBASE's RaiseException
// with no handler, 2026-11-08). It is logged and let go; what it was making stays not ready and is asked for again)
void run_guarded(std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        static std::atomic<int> logged;
        if (logged++ < 20) logf("vk: a background job failed: %s", e.what());
    } catch (...) {
        static std::atomic<int> logged;
        if (logged++ < 20) logf("vk: a background job failed (not a std::exception)");
    }
}

void worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"mkw_vk worker");
    ub_thread_off();
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lk(mu);
            work_cv.wait(lk, [] { return !queue.empty(); });
            fn = std::move(queue.front().fn);
            queue.pop_front();
        }
        run_guarded(fn);
        pending--;
        jobs_notify();
    }
}

void start() {
    // (hookshot: with 16 hardware threads or more, 6 workers; RX_VK_WORKERS=<n>: n, 1 to 16)
    const unsigned hw = std::thread::hardware_concurrency();
    unsigned n = hw >= 16 ? 6u : std::clamp(hw / 3, 2u, 4u);
    if (const char* e = getenv("RX_VK_WORKERS")) n = (unsigned)std::clamp(atoi(e), 1, 16);
    for (unsigned i = 0; i < n; i++) std::thread(worker).detach();
    started = true;
    logf("vk: %u background workers (shader translation, pipelines)", n);
}

} // namespace

void jobs_push(std::function<void()> fn, bool urgent, const std::atomic<bool>* flag) {
    std::lock_guard lk(mu);
    if (!started) start();
    pending++;
    if (urgent) queue.push_front({std::move(fn), flag});
    else queue.push_back({std::move(fn), flag});
    work_cv.notify_one();
}
// (issue 496) Intel A750: four of the game's big compute pipelines built at once by Intel's compiler (up to 58 s each) froze
// the whole PC. One thread builds them one at a time there, below normal priority, beside the shared workers.
// (longshot #404) Between two of its builds no compute build is under way: the moment Intel's pipeline cache fetch waits for
// (pipeline_cache_between_builds). first: the front lane, in order among its own
void jobs_push_serial(std::function<void()> fn, bool first) {
    static std::mutex smu;
    static std::condition_variable scv;
    static std::deque<std::function<void()>> sq, sq_first;
    static bool sstarted = false;
    std::lock_guard lk(smu);
    if (!sstarted) {
        sstarted = true;
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SetThreadDescription(GetCurrentThread(), L"mkw_vk serial builds");
            ub_thread_off();
            for (;;) {
                std::function<void()> fn;
                {
                    std::unique_lock lk(smu);
                    scv.wait(lk, [] { return !sq.empty() || !sq_first.empty(); });
                    auto& q = sq_first.empty() ? sq : sq_first;
                    fn = std::move(q.front());
                    q.pop_front();
                }
                run_guarded(fn);
                jobs_notify();
                std::function<void()> between = pipeline_cache_between_builds;
                run_guarded(between);
            }
        }).detach();
        logf("vk: compute pipelines are built one at a time (the game's dispatches before the mesh fallback's)");
    }
    (first ? sq_first : sq).push_back(std::move(fn));
    scv.notify_one();
}
void jobs_notify() {
    std::lock_guard lk(mu);
    done_cv.notify_all();
}
bool jobs_wait(const std::atomic<bool>& flag) {
    std::unique_lock lk(mu);
    const bool fresh = !flag.load(std::memory_order_acquire);   // (done already: nothing ran during this wait)
    // (still queued: run here, now. A draw that must wait for its pipeline waited for every job queued before it, and a new
    // area queues a hundred pipelines: with ten workers at 0.3-2 s a build, frames stopped for 6-12 s, the draw's own build
    // still in the queue. Off by default since v0.4.3 (2026-11-19): frames drawn during a new area's translation storm
    // sampled state not yet settled (cyan/white areas, green bands on slower GPUs), and the hold that hid them left a wash
    // at its release; the arrival waits again as in v0.4.2 until that is fixed. RX_VK_JOB_STEAL=1: run here)
    static const bool steal = getenv("RX_VK_JOB_STEAL") && atoi(getenv("RX_VK_JOB_STEAL")) != 0;
    if (steal && !flag.load(std::memory_order_acquire))
        for (auto it = queue.begin(); it != queue.end(); ++it)
            if (it->flag == &flag) {
                std::function<void()> fn = std::move(it->fn);
                queue.erase(it);
                lk.unlock();
                run_guarded(fn);
                pending--;
                jobs_notify();
                lk.lock();
                break;
            }
    done_cv.wait(lk, [&] { return flag.load(std::memory_order_acquire); });
    return fresh;   // (the caller that watched its own translation end in its wait can leave its draw out of that frame)
}
int jobs_pending() {
    return pending.load();
}

} // namespace mkwvk
