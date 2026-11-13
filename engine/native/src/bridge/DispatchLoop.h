// HeyListen native engine: the dispatch loop the actors' dispatcher (hl/bridge/Dispatcher.h, a scheduler's rings) and the
// module system's job group (hl/bridge/JobGroup.h, a heap under a mutex) share, over a queue policy. Private to
// src/bridge. docs/native-engine/jobs-boundary.md reads both protocols step by step: they differ only in how a job is
// taken and finished, how the scheduler says it is over, and a few constants; everything a dispatch context does around
// that (entry and leave, the lane status, handed jobs, waking and sleeping, the run of a job's body with its observer) is
// the same, and is here once.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hl/bridge/Dispatcher.h"
#include "hl/bridge/JobBridge.h"

namespace hl::bridge::loop {

// Every shared word is read and written as the guest does: plain loads (relaxed), acquire loads where the guest has
// load-acquire, release stores where it has store-release, and every read-modify-write as one sequentially consistent
// atomic operation (as the translation makes them; on x64 a locked instruction, at least as strong as the guest's).
template <class T>
T ld(const T* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
template <class T>
T ld_acquire(const T* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
template <class T>
void st(T* p, T v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
template <class T>
void st_release(T* p, T v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
template <class T>
bool cas(T* p, T& expected, T desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

constexpr uint32_t kOver = 2;        // a scheduler's flags (both kinds): the stage or the group is over
constexpr uint32_t kNotToRun = 2;    // a context's flags
constexpr uint32_t kSentMessage = 1;

/// The frame a tagged job's function gets as its second argument: the scheduler, the job record, and a word that is 1 for
/// the actors' dispatcher and 0 for the module system's group.
struct JobFrame {
    GuestAddr scheduler, job;
    uint32_t word, pad;
};
static_assert(sizeof(JobFrame) == 24);

/// What a queue policy's take() found.
struct Taken {
    enum What : uint8_t {
        Nothing,   ///< nothing to take: the context goes to sleep (or polls)
        Job,       ///< `job` is this context's to run, after waking up to `wake` others
        Passed,    ///< a job was handed to another context (or dropped where the guest drops it): look again
        Leave      ///< the scheduler was found over where the guest leaves at once
    } what = Nothing;
    GuestAddr job = 0;
    int32_t wake = 0;
};

/// One dispatch context running a scheduler's jobs on a guest thread: what both protocols do the same way.
class Context {
public:
    Context(GuestRuntime& runtime, GuestThread as, GuestAddr c_at)
        : rt(runtime), as(as), at(c_at), c(static_cast<guest::DispatchContext*>(runtime.ptr(c_at))) {}

    template <class T>
    T* mem(GuestAddr a) const { return static_cast<T*>(rt.ptr(a)); }
    uint64_t call(GuestAddr fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0) const {
        const uint64_t args[3] = {a0, a1, a2};
        return rt.call_with(as, fn, args, 3, nullptr, 0);
    }

    /// One more or one fewer in the four bits `nibble` (0xf or 0xf0) of the context's lane's byte, staying within 0..15,
    /// only when the lane and the word's address are not 0.
    void lane_status(uint64_t nibble, bool up) const {
        const GuestAddr word_at = ld(&c->lane_status);
        const int32_t lane = ld(&c->lane);
        if (!word_at || !lane) return;
        const unsigned shift = ((uint32_t)lane << 3) & 63;
        const uint64_t mask = nibble << shift, unit = (nibble == 0xf ? 1ull : 0x10ull) << shift;
        uint64_t* word = mem<uint64_t>(word_at);
        uint64_t old = ld(word);
        for (;;) {
            const uint64_t part = old & mask;
            const uint64_t next = up ? (part == mask ? part : part + unit) : (part == 0 ? 0 : part - unit);
            if (cas(word, old, next | (old & ~mask))) return;
        }
    }

    /// A job handed to this context (its state word, `state`): under its mutex the next job of its own queue (or 0) takes
    /// its place. Returns the handed job.
    GuestAddr take_handed(uint64_t state) const {
        const GuestAddr mutex = at + offsetof(guest::DispatchContext, mutex);
        call(kMutexLock, mutex);
        GuestAddr next = 0;
        const int32_t count = ld(&c->queue_count);
        if (count) {
            const int32_t room = ld(&c->queue_room), first = ld(&c->queue_first);
            const int32_t index = first - (first < room ? 0 : room);
            next = ld(mem<GuestAddr>(ld(&c->queue) + (GuestAddr)(int64_t)index * 8));
            const int32_t after = first + 1 - (first + 1 < room ? 0 : room);
            st(&c->queue_first, after);
            st(&c->queue_count, count - 1);
        }
        __atomic_exchange_n(&c->state, next, __ATOMIC_SEQ_CST);
        call(kMutexUnlock, mutex);
        return state;
    }

    /// Waits on the context's wake event, its lane given up meanwhile.
    template <class Stats>
    void sleep_on_event(Stats& stats) const {
        stats.sleeps.fetch_add(1, std::memory_order_relaxed);
        lane_status(0xf, false);
        call(kEventWait, at + offsetof(guest::DispatchContext, wake_event));
        lane_status(0xf, true);
    }

    /// Wakes up to `count` sleeping contexts of a scheduler's list (`n` contexts at `list`), in its order.
    template <class Stats>
    void wake(Stats& stats, int32_t count, int32_t n, GuestAddr list) const {
        for (int32_t i = 0; i < n; i++) {
            const GuestAddr ctx = ld(mem<GuestAddr>(list + (GuestAddr)i * 8));
            guest::DispatchContext* other = mem<guest::DispatchContext>(ctx);
            if (!(ld(&other->flags) & kNotToRun)) {
                for (uint64_t v = ld(&other->state); v == 1;) {
                    if (cas(&other->state, v, (uint64_t)0)) {
                        call(kEventSet, ctx + offsetof(guest::DispatchContext, wake_event));
                        stats.wakes.fetch_add(1, std::memory_order_relaxed);
                        count--;
                        break;
                    }
                }
            }
            if (count <= 0) break;
        }
    }

    /// Runs one job's body: the lane status up in the four bits of the job's kind (the byte at `kind_at` in its record),
    /// the observer shown the job, the object's third virtual function (a tagged object gets the frame), the observer shown
    /// the end, the lane status down. A job whose object word is 0 has no body.
    void run_job(GuestAddr s_at, GuestAddr job, GuestAddr kind_at, uint32_t frame_word) const {
        const GuestAddr object_word = ld(mem<GuestAddr>(job));
        if (!object_word) return;
        const uint8_t* kind = mem<uint8_t>(job + kind_at);
        lane_status(ld(kind) ? 0xf0 : 0xf, true);
        const GuestAddr shown = at + offsetof(guest::DispatchContext, shown_object);
        if (const GuestAddr o = ld(&c->observer)) {
            st(&c->shown_object, object_word);
            st(&c->shown_begin, (uint8_t)1);
            st(&c->shown_id, ld(mem<uint32_t>(job + 0x14)));
            call(ld(mem<GuestAddr>(ld(mem<GuestAddr>(o)))), o, shown);
        }
        const GuestAddr object = ld(mem<GuestAddr>(job));
        const GuestAddr self = object & ~(GuestAddr)1;
        const GuestAddr fn = ld(mem<GuestAddr>(ld(mem<GuestAddr>(self)) + 0x10));
        const JobFrame frame{s_at, job, frame_word, 0};
        rt.call_with(as, fn, &self, 1, &frame, sizeof frame);
        if (const GuestAddr o = ld(&c->observer)) {
            st(&c->shown_begin, (uint8_t)0);
            call(ld(mem<GuestAddr>(ld(mem<GuestAddr>(o)))), o, shown);
        }
        lane_status(ld(kind) ? 0xf0 : 0xf, false);
    }

    GuestRuntime& rt;
    GuestThread as;
    GuestAddr at;
    guest::DispatchContext* c;
};

// The loop (docs/native-engine/jobs-boundary.md, "The dispatcher (scheduler S, context C)" and "The group (G, C)"). A
// queue policy Q gives, for its scheduler:
//   bool over()                         the scheduler's flags say it is over
//   Taken take()                        a job taken from its queues (the policy's whole protocol for it)
//   void finish(GuestAddr job)          a run of the job finished: its runner word, its completion, the end
//   int32_t queued()                    the jobs others could take, for waking them (and for staying awake)
//   uint32_t poll_lane(); void poll()   the lane that polls instead of sleeping, and its timed sleep
//   int32_t context_count(); GuestAddr contexts()    the scheduler's list of contexts
//   void left()                         called once the context has left (the policy's checks)
//   static kIdleRounds                  rounds without a job after which a context with jobs in sight sleeps a timed
//                                       span (0: never)
//   static kKindAt, kFrameWord          where its job record keeps the lane status kind; the tagged frame's word
// Stats has the counters sleeps, wakes, handed and runs.
template <class Q, class Stats>
void run(Context& cx, Q& q, Stats& stats) {
    guest::DispatchContext* c = cx.c;
    cx.lane_status(0xf, true);
    GuestAddr job = 0;
    int32_t idle = 0;   // rounds without a job since the last one ran
    for (;;) {
        // Finish the job just run; leave when this context is not to run or the scheduler is over.
        if (job) {
            q.finish(job);
            job = 0;
        }
        if (ld(&c->flags) & kNotToRun) break;
        if (q.over()) break;
        idle = 0;
        int32_t n = 0;   // the count of jobs others could take, for waking them
        for (;;) {
            const uint64_t state = ld(&c->state);
            if (state > 1) {   // a job handed to this context
                job = cx.take_handed(state);
                stats.handed.fetch_add(1, std::memory_order_relaxed);
                n = q.queued();
                break;
            }
            const Taken t = q.take();
            if (t.what == Taken::Leave) goto leave;
            if (t.what == Taken::Job) {
                job = t.job;
                n = t.wake;
                break;
            }
            int32_t next_idle = idle + 1;
            if (t.what == Taken::Nothing) {
                if (q.poll_lane() == (uint32_t)ld(&c->lane)) {
                    q.poll();
                } else {
                    bool handed = false;
                    for (;;) {   // (asleep: 0 to 1, unless a job was handed meanwhile)
                        const uint64_t v = ld(&c->state);
                        if (v == 1) break;
                        if (v != 0) { handed = true; break; }
                        uint64_t e = 0;
                        if (cas(&c->state, e, (uint64_t)1)) break;
                    }
                    if (handed) continue;   // (from the top: the handed job)
                    if (q.queued() >= 1) {   // (jobs arrived: awake again)
                        bool woken = false;
                        for (;;) {
                            if (ld(&c->state) != 1) { woken = true; break; }
                            uint64_t e = 1;
                            if (cas(&c->state, e, (uint64_t)0)) break;
                        }
                        // (another context set it 0 and signals: take that signal)
                        if (woken) cx.sleep_on_event(stats);
                        else if (Q::kIdleRounds && idle >= Q::kIdleRounds) q.poll();
                    } else {
                        cx.sleep_on_event(stats);
                    }
                }
            } else {
                next_idle = 0;   // (Passed)
            }
            if (q.over()) goto leave;
            idle = next_idle;
        }
        // Run it, after waking contexts for the jobs left.
        if (n >= 1) cx.wake(stats, n, q.context_count(), q.contexts());
        if (!job) break;
        stats.runs.fetch_add(1, std::memory_order_relaxed);
        cx.run_job(q.scheduler(), job, Q::kKindAt, Q::kFrameWord);
    }
leave:
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_fetch_and(&c->flags, ~kSentMessage, __ATOMIC_SEQ_CST);
    cx.lane_status(0xf, false);
    q.left();
}

}   // namespace hl::bridge::loop
