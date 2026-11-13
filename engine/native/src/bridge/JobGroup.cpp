// HeyListen native engine: the module system's job group, natively (hl/bridge/JobGroup.h). Each step below follows a step
// of docs/native-engine/jobs-boundary.md, "The module system's job group"; the guest instruction addresses in the comments
// are where the reading of that step is, in the translated group 0x8007fe60 (or the push 0x80083350, the heap's erase
// 0x811c4160 and its sifts 0x811c41f0, 0x811c4290).
#include "hl/bridge/JobGroup.h"

#include <cstdio>

#include "DispatchLoop.h"

namespace hl::bridge {

using loop::cas;
using loop::ld;
using loop::ld_acquire;
using loop::st;

namespace {

constexpr uint32_t kOver = loop::kOver;
constexpr GuestAddr kEndJob = offsetof(guest::Group, end_job);
constexpr GuestAddr kListHead = offsetof(guest::GroupJob, dep_last);   // (the head node of a job's list of dependents)

}   // namespace

// The heap as the loop's queue policy (DispatchLoop.h): one per call of the group.
struct JobGroup::Heap {
    static constexpr int32_t kIdleRounds = 0;   // (no timed sleep after idle rounds: 0x80082e80)
    static constexpr GuestAddr kKindAt = offsetof(guest::GroupJob, kind);
    static constexpr uint32_t kFrameWord = 0;   // (0x80080844)

    JobGroup& d;
    loop::Context& cx;
    GuestAddr g_at;
    guest::Group* g;
    int lane_byte;         // (checking: the context's lane byte before its entry; -1: none)
    bool single = false;   // (checking: the job taken is a one-runner job, marked as running)

    GuestAddr scheduler() const { return g_at; }
    bool over() const { return ld(&g->flags) & kOver; }
    int32_t queued() const { return ld(&g->take_count); }
    uint32_t poll_lane() const { return ld(&g->poll_lane); }
    void poll() { d.timed_sleep(cx.as); }
    int32_t context_count() const { return ld(&g->context_count); }
    GuestAddr contexts() const { return ld(&g->contexts); }

    // (checking: the lane byte of the context, or -1 when it has no lane or no word)
    int read_lane_byte() const {
        const GuestAddr word_at = ld(&cx.c->lane_status);
        const int32_t lane = ld(&cx.c->lane);
        if (!word_at || !lane) return -1;
        return (int)((ld(cx.mem<uint64_t>(word_at)) >> (((uint32_t)lane << 3) & 63)) & 0xff);
    }
    void left() {
        if (d.options_.check && lane_byte >= 0 && read_lane_byte() != lane_byte)
            d.stats_.lane_drift.fetch_add(1, std::memory_order_relaxed);
    }

    void finish(GuestAddr job) {
        d.finish(cx.as, g_at, job);
        if (single) {
            std::lock_guard lock(d.single_mu_);
            d.single_.erase(job);
            single = false;
        }
    }

    // A unit of the take count, then the heap's top under the mutex (0x8007fffc).
    loop::Taken take() {
        Stats& stats = d.stats_;
        int32_t* count = &g->take_count;
        int32_t v = ld(count);
        if (d.options_.check && v < 0) stats.below_zero.fetch_add(1, std::memory_order_relaxed);
        if (v < 1) return {};
        for (;;) {
            int32_t e = v;
            if (cas(count, e, v - 1)) break;
            v = e;
            if (v <= 0) break;
        }
        if (v < 1) return {};
        d.lock(cx.as, g_at);
        if (ld(&g->flags) & kOver) {   // (0x80082f98: over: the unit given back, and leave)
            __atomic_fetch_add(count, 1, __ATOMIC_SEQ_CST);
            d.unlock(cx.as, g_at);
            return {loop::Taken::Leave, 0, 0};
        }
        const GuestAddr items = ld(&g->heap.items);
        const int32_t size = ld(&g->heap.size);
        if (!size || !items) {   // (0x80082fb8: empty: the unit given back)
            __atomic_fetch_add(count, 1, __ATOMIC_SEQ_CST);
            d.unlock(cx.as, g_at);
            stats.empty.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        const GuestAddr top = ld(cx.mem<GuestAddr>(items));
        guest::GroupJob* t = cx.mem<guest::GroupJob>(top);
        const uint32_t was = __atomic_fetch_add(&t->runners, 0xffffu, __ATOMIC_SEQ_CST);   // (one started)
        stats.takes.fetch_add(1, std::memory_order_relaxed);
        if ((was & 0xffff) != 1) {   // (0x8008034c: more to start: it stays the top, the unit given back)
            __atomic_fetch_add(count, 1, __ATOMIC_SEQ_CST);
            stats.given_back.fetch_add(1, std::memory_order_relaxed);
            d.unlock(cx.as, g_at);
            return taken(top, v);
        }
        stats.last_takes.fetch_add(1, std::memory_order_relaxed);
        const uint32_t lane = ld(&t->lane);
        d.take_top(g);
        if (lane == kAnyLane || lane == (uint32_t)ld(&cx.c->lane)) {   // (0x80080084)
            d.unlock(cx.as, g_at);
            if (d.options_.check && (was >> 16) == 0) {   // (its one runner, none running before: no other may run it now)
                std::lock_guard lock(d.single_mu_);
                if (!d.single_.insert(top).second) {
                    stats.double_run.fetch_add(1, std::memory_order_relaxed);
                    d.violation("a one-runner job taken while a native context runs it", top);
                } else {
                    single = true;
                }
            }
            return taken(top, v - 1);
        }
        d.forward(cx.as, g_at, lane, top);   // (0x8008067c: tied to another lane, under the mutex)
        d.unlock(cx.as, g_at);
        return {loop::Taken::Passed, 0, 0};
    }

    loop::Taken taken(GuestAddr job, int32_t wake) {
        d.stats_.runs_from_takes.fetch_add(1, std::memory_order_relaxed);
        return {loop::Taken::Job, job, wake};
    }
};

JobGroup::JobGroup(GuestRuntime& runtime, Options options) : runtime_(runtime), options_(options) {}

uint64_t JobGroup::call(GuestThread as, GuestAddr fn, uint64_t a0, uint64_t a1) {
    const uint64_t args[2] = {a0, a1};
    return runtime_.call_with(as, fn, args, 2, nullptr, 0);
}

void JobGroup::violation(const char* what, GuestAddr job) {
    if (stats_.violations_logged.fetch_add(1, std::memory_order_relaxed) >= 20) return;
    char line[160];
    std::snprintf(line, sizeof line, "hl jobgroup: CHECK: %s (job %#llx)", what, (unsigned long long)job);
    runtime_.log(line);
}

void JobGroup::run(GuestThread as, GuestAddr g_at, GuestAddr c_at) {
    stats_.groups.fetch_add(1, std::memory_order_relaxed);
    loop::Context cx(runtime_, as, c_at);
    Heap heap{*this, cx, g_at, at<guest::Group>(g_at), -1};
    if (options_.check) heap.lane_byte = heap.read_lane_byte();
    loop::run(cx, heap, stats_);
}

// ---- the heap (always under the group's mutex) --------------------------------------------------------------------------

namespace {
uint16_t priority(const GuestRuntime& rt, GuestAddr job) {
    return ld(&static_cast<const guest::GroupJob*>(rt.ptr(job))->priority);
}
}   // namespace

// The job `job`, standing at `i`, goes up while its parent's priority is below its own (0x811c41f0, and the push's own
// copy). Returns where it stands.
int32_t JobGroup::sift_up(guest::Group* g, int32_t i, GuestAddr job) {
    GuestAddr* items = at<GuestAddr>(ld(&g->heap.items));
    const uint16_t p = priority(runtime_, job);
    const int32_t from = i;
    while (i > 0) {
        const int32_t up = (i + 1) / 2 - 1;
        const GuestAddr parent = ld(&items[up]);
        if (priority(runtime_, parent) >= p) break;
        st(&items[i], parent);
        i = up;
    }
    if (i != from) st(&items[i], job);
    return i;
}

// The job `job`, standing at `i` of a heap of `size`, sinks: its child is the left one, or the right one when the left's
// priority is below the right's; while its own is below that child's, the child comes up (0x811c4290, and the take's copy).
void JobGroup::sift_down(guest::Group* g, int32_t i, GuestAddr job, int32_t size) {
    GuestAddr* items = at<GuestAddr>(ld(&g->heap.items));
    const uint16_t p = priority(runtime_, job);
    const int32_t from = i;
    for (;;) {
        const int32_t left = 2 * i + 1;
        if (left >= size) break;
        int32_t child = left;
        GuestAddr c = ld(&items[left]);
        uint16_t cp = priority(runtime_, c);
        if (left + 1 < size) {
            const GuestAddr r = ld(&items[left + 1]);
            const uint16_t rp = priority(runtime_, r);
            if (cp < rp) {
                child = left + 1;
                c = r;
                cp = rp;
            }
        }
        if (p >= cp) break;
        st(&items[i], c);
        i = child;
    }
    if (i != from) st(&items[i], job);
}

// The top taken down (0x80080084): the last item takes its place and sinks; when none is left, nothing moves.
void JobGroup::take_top(guest::Group* g) {
    const int32_t size = ld(&g->heap.size);
    if (size - 1 == 0) {
        st(&g->heap.size, 0);
    } else if (size) {
        GuestAddr* items = at<GuestAddr>(ld(&g->heap.items));
        st(&g->heap.size, size - 1);
        const GuestAddr last = ld(&items[size - 1]);
        st(&items[0], last);
        sift_down(g, 0, last, size - 1);
    }
    if (options_.check) check_heap(g);
}

// The item at `i` erased, the heap kept in order (0x811c4160 with its re-heaping): the last takes its place, goes up, and
// when it did not move goes down.
void JobGroup::erase(guest::Group* g, int32_t i) {
    const int32_t size = ld(&g->heap.size) - 1;
    if (size == 0) {
        st(&g->heap.size, 0);
    } else {
        st(&g->heap.size, size);
        if (i != size) {
            GuestAddr* items = at<GuestAddr>(ld(&g->heap.items));
            const GuestAddr last = ld(&items[size]);
            st(&items[i], last);
            if (sift_up(g, i, last) == i) sift_down(g, i, last, size);
        }
    }
    if (options_.check) check_heap(g);
}

void JobGroup::check_heap(guest::Group* g) {
    const GuestAddr* items = at<GuestAddr>(ld(&g->heap.items));
    const int32_t size = ld(&g->heap.size);
    for (int32_t i = 1; i < size; i++) {
        if (priority(runtime_, ld(&items[(i - 1) / 2])) < priority(runtime_, ld(&items[i]))) {
            stats_.heap_bad.fetch_add(1, std::memory_order_relaxed);
            violation("the heap's order is broken", ld(&items[i]));
            return;
        }
    }
}

void JobGroup::push(GuestThread as, GuestAddr g_at, GuestAddr job) {
    guest::GroupJob* j = at<guest::GroupJob>(job);
    if (ld(&j->hook)) {   // (0x80083414: the hook decides the runners, and may complete it at once: the guest's push)
        call(as, kGroupPush, g_at, job);
        stats_.hook_pushes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    guest::Group* g = at<guest::Group>(g_at);
    const int32_t room = ld(&g->heap.room), size = ld(&g->heap.size);
    if (size != room) {   // (0x80083384: at the end, then up)
        GuestAddr* items = at<GuestAddr>(ld(&g->heap.items));
        st(&items[size], job);
        st(&g->heap.size, size + 1);
        sift_up(g, size, job);
        stats_.readied.fetch_add(1, std::memory_order_relaxed);
        if (options_.check) check_heap(g);
    } else {   // (full: not put in, but counted all the same)
        stats_.full.fetch_add(1, std::memory_order_relaxed);
    }
    __atomic_fetch_add(&g->take_count, 1, __ATOMIC_SEQ_CST);
}

// ---- jobs --------------------------------------------------------------------------------------------------------------

// A job tied to another lane, handed to the context of that lane, under the group's mutex (0x800806d0, 0x80080fd0).
void JobGroup::forward(GuestThread as, GuestAddr g_at, uint32_t lane, GuestAddr job) {
    guest::Group* g = at<guest::Group>(g_at);
    const int32_t n = ld(&g->context_count);
    const GuestAddr list = ld(&g->contexts);
    GuestAddr ctx = 0;
    for (int32_t i = 0; i < n && !ctx; i++) {
        const GuestAddr c = ld(at<GuestAddr>(list + (GuestAddr)i * 8));
        if ((uint32_t)ld(&at<guest::DispatchContext>(c)->lane) == lane) ctx = c;
    }
    guest::DispatchContext* c = ctx ? at<guest::DispatchContext>(ctx) : nullptr;
    if (!c || (ld(&c->flags) & loop::kNotToRun)) {   // (0x80081004: dropped)
        stats_.drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t v = ld(&c->state);
    for (;;) {
        if (v > 1) {   // (0x800831a0: it holds a job: to the end of its own queue, under its mutex)
            const GuestAddr mutex = ctx + offsetof(guest::DispatchContext, mutex);
            call(as, kMutexLock, mutex);
            v = ld(&c->state);
            if (v < 2) {
                call(as, kMutexUnlock, mutex);
                continue;
            }
            const int32_t count = ld(&c->queue_count), room = ld(&c->queue_room);
            if (count < room) {
                const int32_t first = ld(&c->queue_first);
                st(&c->queue_count, count + 1);
                const int32_t slot = first + count - (first + count < room ? 0 : room);
                st(at<GuestAddr>(ld(&c->queue) + (GuestAddr)(int64_t)slot * 8), job);
                stats_.passed.fetch_add(1, std::memory_order_relaxed);
                stats_.queued_passes.fetch_add(1, std::memory_order_relaxed);
            } else {
                stats_.drops.fetch_add(1, std::memory_order_relaxed);
            }
            call(as, kMutexUnlock, mutex);
            return;
        }
        uint64_t e = v;
        if (cas(&c->state, e, (uint64_t)job)) {
            if (v == 1) call(as, kEventSet, ctx + offsetof(guest::DispatchContext, wake_event));
            stats_.passed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        v = e;
    }
}

// A run of the job finished (0x8007ff20): a cancel first, then one running runner fewer; the last one completes the job.
void JobGroup::finish(GuestThread as, GuestAddr g_at, GuestAddr job) {
    guest::GroupJob* j = at<guest::GroupJob>(job);
    guest::Group* g = at<guest::Group>(g_at);
    if (ld(&j->cancel) == 1 && (ld(&j->runners) & 0xffff)) {   // (0x80082448: the runners not started are cancelled)
        lock(as, g_at);
        const uint32_t was = __atomic_fetch_and(&j->runners, ~0xffffu, __ATOMIC_SEQ_CST);
        const int32_t size = ld(&g->heap.size);
        if ((was & 0xffff) && size) {
            const GuestAddr* items = at<GuestAddr>(ld(&g->heap.items));
            for (int32_t i = 0; i < size; i++) {
                if (ld(&items[i]) != job) continue;
                const int32_t count = __atomic_fetch_add(&g->take_count, -1, __ATOMIC_SEQ_CST);
                if (options_.check && count <= 0) stats_.below_zero.fetch_add(1, std::memory_order_relaxed);
                erase(g, i);
                stats_.cancels.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        unlock(as, g_at);
    }
    const uint32_t was = __atomic_fetch_add(&j->runners, (uint32_t)-0x10000, __ATOMIC_SEQ_CST);
    stats_.finishes.fetch_add(1, std::memory_order_relaxed);
    if (options_.check && (was >> 16) == 0) {
        stats_.bad_finish.fetch_add(1, std::memory_order_relaxed);
        violation("a job finished with no runner running", job);
    }
    if (was != 0x10000) return;   // (others still run it, or runners are still to start)
    // Complete (0x8007ff44): each dependent waits for one job fewer, those that wait for none are pushed, under the mutex.
    stats_.completes.fetch_add(1, std::memory_order_relaxed);
    if (options_.check && ld(&j->waiting) == -1) {
        stats_.double_complete.fetch_add(1, std::memory_order_relaxed);
        violation("a job completed twice", job);
    }
    const GuestAddr head = job + kListHead;
    lock(as, g_at);
    GuestAddr node = ld(&j->dep_first);
    if (node != head) {
        const GuestAddr off = (GuestAddr)(int64_t)ld(&j->dep_offset);
        for (; node != head; node = ld(&at<guest::GroupNode>(node)->next)) {
            const GuestAddr dep = ld(at<GuestAddr>(node - off));
            const int32_t was_waiting = __atomic_fetch_add(&at<guest::GroupJob>(dep)->waiting, -1, __ATOMIC_SEQ_CST);
            if (options_.check && was_waiting <= 0) {
                stats_.bad_count.fetch_add(1, std::memory_order_relaxed);
                violation("a dependent's waiting count went below 0", dep);
            }
            if (was_waiting == 1) push(as, g_at, dep);
        }
    }
    unlock(as, g_at);
    st(&j->waiting, -1);
    for (node = ld(&j->dep_first); node != head;) {   // (0x8007ffb4: the list emptied)
        guest::GroupNode* n = at<guest::GroupNode>(node);
        const GuestAddr next = ld(&n->next);
        st(&n->prev, (GuestAddr)0);
        st(&n->next, (GuestAddr)0);
        node = next;
    }
    st(&j->dep_count, 0);
    st(&j->dep_last, head);
    st(&j->dep_first, head);
    if (job == g_at + kEndJob) over(as, g_at);
}

// The end job completed (0x800816bc): under the mutex, the group is over and every context of its list is woken to leave.
void JobGroup::over(GuestThread as, GuestAddr g_at) {
    guest::Group* g = at<guest::Group>(g_at);
    lock(as, g_at);
    __atomic_fetch_or(&g->flags, kOver, __ATOMIC_SEQ_CST);
    stats_.ends.fetch_add(1, std::memory_order_relaxed);
    const int32_t n = ld(&g->context_count);
    const GuestAddr list = ld(&g->contexts);
    if (options_.check) {
        stats_.end_checks.fetch_add(1, std::memory_order_relaxed);
        const bool left = ld(&g->heap.size) != 0;
        if (left) stats_.end_left.fetch_add(1, std::memory_order_relaxed);
        if (ld(&g->take_count) != 0) stats_.end_count.fetch_add(1, std::memory_order_relaxed);
        if (left || ld(&g->take_count) > 0)
            for (int32_t i = 0; i < n; i++)
                if (ld(&at<guest::DispatchContext>(ld(at<GuestAddr>(list + (GuestAddr)i * 8)))->state) == 1)
                    stats_.end_asleep.fetch_add(1, std::memory_order_relaxed);
    }
    for (int32_t i = 0; i < n; i++) {
        const GuestAddr ctx = ld(at<GuestAddr>(list + (GuestAddr)i * 8));
        uint64_t* state = &at<guest::DispatchContext>(ctx)->state;
        for (;;) {
            uint64_t v = ld(state);
            if (v != 1 || cas(state, v, (uint64_t)0)) break;
        }
        call(as, kEventSet, ctx + offsetof(guest::DispatchContext, wake_event));
    }
    unlock(as, g_at);
}

// The polling lane's timed sleep (0x80082fd8). Its length is a static of the guest function, made here the first time
// under the guest's own guard, as the guest makes it: the tick frequency divided by 10,000 (0x80083300).
void JobGroup::timed_sleep(GuestThread as) {
    stats_.polls.fetch_add(1, std::memory_order_relaxed);
    if (!(ld_acquire(at<uint8_t>(kGroupPollGuard)) & 1)) {
        const uint64_t take = call(as, kGuardAcquire, kGroupPollGuard);
        if ((uint32_t)take) {
            const int64_t frequency = (int64_t)call(as, kTickFrequency, take);
            st(at<int64_t>(kGroupPollTicks), frequency / 10000);
            call(as, kGuardRelease, kGroupPollGuard);
        }
    }
    const uint64_t span = call(as, kTicksToSpan, ld(at<uint64_t>(kGroupPollTicks)));
    call(as, kSleepFor, span);
}

int JobGroup::report(char* out, size_t cap) const {
    const Stats& s = stats_;
    auto v = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    int n = std::snprintf(out, cap,
                          "jobgroup native: groups %llu; takes %llu (last runners %llu, given back %llu; heap empty %llu), "
                          "handed %llu, runs %llu (from takes %llu), finishes %llu, completes %llu, cancels %llu, readied "
                          "%llu (hook %llu), full %llu, passed %llu (queued %llu), drops %llu; sleeps %llu, polls %llu, "
                          "wakes %llu, group ends %llu",
                          v(s.groups), v(s.takes), v(s.last_takes), v(s.given_back), v(s.empty), v(s.handed), v(s.runs),
                          v(s.runs_from_takes), v(s.finishes), v(s.completes), v(s.cancels), v(s.readied), v(s.hook_pushes),
                          v(s.full), v(s.passed), v(s.queued_passes), v(s.drops), v(s.sleeps), v(s.polls), v(s.wakes),
                          v(s.ends));
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    if (!options_.check) return n;
    const long long unaccounted = (long long)v(s.takes) - (long long)(v(s.runs_from_takes) + v(s.passed) + v(s.drops));
    const long long unhanded = (long long)v(s.passed) - (long long)v(s.handed);
    const long long unfinished = (long long)v(s.runs) - (long long)v(s.finishes);
    const int m = std::snprintf(out + n, cap - (size_t)n,
                                "; CHECK: takes not run %lld, passed not run %lld, runs not finished %lld, bad finishes %llu, "
                                "bad counts %llu, double completes %llu, double runs %llu, heap order broken %llu; group ends "
                                "checked %llu: heap not empty %llu, take count not 0 %llu, contexts asleep then %llu; take "
                                "count seen below 0 %llu, lane drift %llu",
                                unaccounted, unhanded, unfinished, v(s.bad_finish), v(s.bad_count), v(s.double_complete),
                                v(s.double_run), v(s.heap_bad), v(s.end_checks), v(s.end_left), v(s.end_count),
                                v(s.end_asleep), v(s.below_zero), v(s.lane_drift));
    if (m < 0) return n;
    return (size_t)(n + m) >= cap ? (int)cap - 1 : n + m;
}

}   // namespace hl::bridge
