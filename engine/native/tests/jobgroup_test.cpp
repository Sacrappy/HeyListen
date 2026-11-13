// HeyListen native engine: the module system's job group (hl/bridge/JobGroup.h) run with no game, on a model of the
// game's group scheduler in host memory: its heap, mutex, take count, job records, lists of dependents, dispatch contexts
// and lane status word laid out as the guest has them, and the game's own functions the group calls (events, mutexes, the
// timed sleep and its guard, a hook's push, job bodies) played by this file as docs/native-engine/jobs-boundary.md
// describes them. The heap's expected orders come from a reference heap written here from that description.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "hl/bridge/JobGroup.h"

using namespace hl;
using bridge::GuestAddr;
namespace g = bridge::guest;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

constexpr GuestAddr kBase = 0x10000000, kSize = 8u << 20;
constexpr GuestAddr kGlobals = 0x84e9f000;   // (the page of the group's timed-sleep constant and its guard)
constexpr GuestAddr kBody = 0x70000010, kObserver = 0x70000020;
constexpr GuestAddr kGroup = kBase + 0x1000, kList = kBase + 0x2000, kContexts = kBase + 0x3000, kLaneWord = kBase + 0x4000,
                    kQueues = kBase + 0x5000, kItems = kBase + 0x10000, kJobs = kBase + 0x100000, kNodes = kBase + 0x200000,
                    kObjects = kBase + 0x300000, kVtable = kBase + 0x380000, kObsObject = kBase + 0x380100,
                    kObsVtable = kBase + 0x380200, kHook = kBase + 0x380300, kStacks = kBase + 0x400000;
constexpr int kContextsMax = 6;
constexpr int64_t kFrequency = 19200000;
constexpr int32_t kNodeOffset = 8;   // (a list element: +0 the dependent job, +8 its node)

struct Thread {   // a guest thread: its lane (its context's) and its stack
    int lane;
    GuestAddr sp;
};
thread_local Thread* tls_thread = nullptr;

struct Model final : bridge::GuestRuntime {
    std::vector<uint8_t> memory = std::vector<uint8_t>(kSize);
    std::vector<uint8_t> globals = std::vector<uint8_t>(0x1000);
    std::mutex ev_mu;
    std::condition_variable ev_cv;
    std::map<GuestAddr, int> events;
    std::mutex mx_mu;
    std::map<GuestAddr, std::unique_ptr<std::mutex>> mutexes;
    std::vector<std::atomic<int>> runs = std::vector<std::atomic<int>>(4096);
    std::atomic<int> wrong_lane{0}, wrong_frame{0}, hook_push{0}, observed{0}, guard_taken{0}, polls{0};
    std::mutex order_mu;
    std::vector<int> order;
    std::atomic<bool> slow_bodies{false};

    void* ptr(GuestAddr at) const override {
        if (at >= kGlobals && at < kGlobals + 0x1000) return const_cast<uint8_t*>(globals.data()) + (at - kGlobals);
        CHECK(at >= kBase && at < kBase + kSize);
        return const_cast<uint8_t*>(memory.data()) + (at - kBase);
    }
    template <class T>
    T* at(GuestAddr a) { return static_cast<T*>(ptr(a)); }
    bridge::GuestThread current() const override { return tls_thread; }
    void log(const char* line) override { std::fprintf(stderr, "%s\n", line); }
    uint64_t call(bridge::GuestThread as, GuestAddr fn, uint64_t a0, uint64_t a1) override {
        const uint64_t args[2] = {a0, a1};
        return call_with(as, fn, args, 2, nullptr, 0);
    }
    uint64_t call_with(bridge::GuestThread as, GuestAddr fn, const uint64_t* args, unsigned nargs, const void* frame,
                       unsigned frame_size) override {
        Thread* t = static_cast<Thread*>(as);
        CHECK(t && t == tls_thread);
        uint64_t a[4] = {};
        for (unsigned i = 0; i < nargs; i++) a[i] = args[i];
        GuestAddr sp = t->sp;
        if (frame_size) {
            sp = (sp - frame_size) & ~(GuestAddr)15;
            std::memcpy(ptr(sp), frame, frame_size);
            a[nargs] = sp;
        }
        const GuestAddr saved = t->sp;
        t->sp = sp;
        const uint64_t r = guest_function(t, fn, a);
        t->sp = saved;
        return r;
    }

    uint64_t guest_function(Thread* t, GuestAddr fn, const uint64_t* a) {
        switch (fn) {
        case bridge::kEventWait: {
            std::unique_lock lock(ev_mu);
            ev_cv.wait(lock, [&] { return events[a[0]] != 0; });
            events[a[0]] = 0;
            return 0;
        }
        case bridge::kEventSet: {
            std::lock_guard lock(ev_mu);
            events[a[0]] = 1;
            ev_cv.notify_all();
            return 0;
        }
        case bridge::kMutexLock: mutex_of(a[0]).lock(); return 0;
        case bridge::kMutexUnlock: mutex_of(a[0]).unlock(); return 0;
        case bridge::kTicksToSpan: CHECK(a[0] == (uint64_t)(kFrequency / 10000)); return a[0];
        case bridge::kSleepFor: polls++; std::this_thread::sleep_for(std::chrono::microseconds(20)); return 0;
        case bridge::kGuardAcquire: {   // (the guard taken: 1 to the one thread that makes the static)
            CHECK(a[0] == bridge::kGroupPollGuard);
            std::lock_guard lock(mx_mu);
            if (*at<uint8_t>(a[0]) & 1) return 0;
            guard_taken++;
            return 1;
        }
        case bridge::kGuardRelease: __atomic_store_n(at<uint8_t>(a[0]), (uint8_t)1, __ATOMIC_RELEASE); return 0;
        case bridge::kTickFrequency: return (uint64_t)kFrequency;
        case bridge::kGroupPush: hook_push_of(a[0], a[1]); return 0;
        case kObserver: observed++; return 0;
        case kBody: body(t, a[0], a[1]); return 0;
        default: CHECK(!"a guest function the model does not have"); return 0;
        }
    }
    std::mutex& mutex_of(GuestAddr a) {
        std::lock_guard lock(mx_mu);
        std::unique_ptr<std::mutex>& m = mutexes[a];
        if (!m) m = std::make_unique<std::mutex>();
        return *m;
    }
    // A job's body: the frame is (group, job, 0) for a tagged object; it records that it ran, and where.
    void body(Thread* t, GuestAddr object, GuestAddr frame) {
        const GuestAddr job = *at<GuestAddr>(frame + 8);
        const int index = (int)((job - kJobs) / sizeof(g::GroupJob));
        g::GroupJob* j = at<g::GroupJob>(job);
        if ((j->object & 1) && ((j->object & ~(GuestAddr)1) != object || *at<GuestAddr>(frame) != kGroup ||
                                *at<uint32_t>(frame + 16) != 0))
            wrong_frame++;
        if (j->lane != bridge::kAnyLane && (int)j->lane != t->lane) wrong_lane++;
        runs[(size_t)index]++;
        {
            std::lock_guard lock(order_mu);
            order.push_back(index);
        }
        if (slow_bodies && index % 7 == 0) std::this_thread::sleep_for(std::chrono::microseconds(30));
    }

    // ---- the reference heap, from docs/native-engine/jobs-boundary.md ("The heap and the take count") ----
    uint16_t prio(GuestAddr job) { return at<g::GroupJob>(job)->priority; }
    GuestAddr* items() { return at<GuestAddr>(at<g::Group>(kGroup)->heap.items); }
    void ref_push(GuestAddr job) {   // (up: while the parent's priority is strictly below)
        g::Group* gr = at<g::Group>(kGroup);
        if (gr->heap.size != gr->heap.room) {
            int i = gr->heap.size++;
            while (i > 0 && prio(items()[(i - 1) / 2]) < prio(job)) {
                items()[i] = items()[(i - 1) / 2];
                i = (i - 1) / 2;
            }
            items()[i] = job;
        }
        gr->take_count++;
    }
    void ref_pop() {   // (down: the larger child, the left one on a tie; while strictly below it)
        g::Group* gr = at<g::Group>(kGroup);
        const int size = --gr->heap.size;
        if (!size) return;
        const GuestAddr job = items()[size];
        int i = 0;
        for (;;) {
            int c = 2 * i + 1;
            if (c >= size) break;
            if (c + 1 < size && prio(items()[c]) < prio(items()[c + 1])) c++;
            if (prio(job) >= prio(items()[c])) break;
            items()[i] = items()[c];
            i = c;
        }
        items()[i] = job;
    }
    bool ordered() {
        g::Group* gr = at<g::Group>(kGroup);
        for (int i = 1; i < gr->heap.size; i++)
            if (prio(items()[(i - 1) / 2]) < prio(items()[i])) return false;
        return true;
    }
    // The guest's push for a job with a hook (0x80083350), as far as the model needs it: the hook gives two runners.
    void hook_push_of(GuestAddr group, GuestAddr job) {
        CHECK(group == kGroup);
        hook_push++;
        g::GroupJob* j = at<g::GroupJob>(job);
        j->runners = 2;
        j->cancel = 0;
        ref_push(job);
    }
};

struct Setup {
    Model& m;
    int contexts;
    Thread threads[kContextsMax + 1];
    GuestAddr next_node = kNodes;
    int jobs = 0;

    Setup(Model& model, int n, int room) : m(model), contexts(n) {
        g::Group* gr = m.at<g::Group>(kGroup);
        gr->context_count = n;
        gr->context_room = kContextsMax;
        gr->contexts = kList;
        gr->heap = {kItems, room, 0};
        for (int i = 0; i < n; i++) {
            const GuestAddr ctx = kContexts + (GuestAddr)i * 0x100;
            m.at<GuestAddr>(kList)[i] = ctx;
            g::DispatchContext* c = m.at<g::DispatchContext>(ctx);
            c->lane = i + 1;
            c->lane_status = kLaneWord;
            c->queue = kQueues + (GuestAddr)i * 0x400;
            c->queue_room = 128;
        }
        for (int i = 0; i < kContextsMax + 1; i++) threads[i] = {i + 1, kStacks + (GuestAddr)(i + 1) * 0x10000};
    }
    static GuestAddr job_at(int i) { return kJobs + (GuestAddr)i * sizeof(g::GroupJob); }
    static GuestAddr end() { return kGroup + offsetof(g::Group, end_job); }
    static void empty_list(g::GroupJob* j, GuestAddr at) {
        j->dep_last = j->dep_first = at + offsetof(g::GroupJob, dep_last);
        j->dep_count = 0;
        j->dep_offset = kNodeOffset;
    }
    // The graph made as 0x80070ea0 makes it: the end job waits for every job; then the edges.
    void begin(int n) {
        jobs = n;
        next_node = kNodes;
        g::Group* gr = m.at<g::Group>(kGroup);
        g::GroupJob* e = &gr->end_job;
        std::memset(e, 0, sizeof *e);
        e->runners = 1;
        e->lane = bridge::kAnyLane;
        e->id = 0xff00ff;
        empty_list(e, end());
        gr->flags = 0;
        gr->take_count = 0;
        gr->heap.size = 0;
        for (int i = 0; i < contexts; i++) {
            g::DispatchContext* c = m.at<g::DispatchContext>(m.at<GuestAddr>(kList)[i]);
            c->flags = 1;
            c->state = 0;
        }
    }
    void job(int i, uint16_t priority, uint16_t runners, uint32_t lane, bool tagged, bool cancel = false, uint8_t kind = 0) {
        g::GroupJob* j = m.at<g::GroupJob>(job_at(i));
        std::memset(j, 0, sizeof *j);
        const GuestAddr object = kObjects + (GuestAddr)i * 16;
        *m.at<GuestAddr>(object) = kVtable;
        *m.at<GuestAddr>(kVtable + 0x10) = kBody;
        j->object = object | (tagged ? 1 : 0);
        j->index = (uint32_t)i;
        j->priority = priority;
        j->cancel = cancel;
        j->kind = kind;
        j->lane = lane;
        j->id = (uint32_t)i;
        j->runners = runners;
        empty_list(j, job_at(i));
        depends(end(), job_at(i));
    }
    void depends(GuestAddr job, GuestAddr on) {   // `job` waits for `on`: an element at the end of on's list
        g::GroupJob* o = m.at<g::GroupJob>(on);
        const GuestAddr element = next_node;
        next_node += 24;
        *m.at<GuestAddr>(element) = job;
        const GuestAddr node = element + kNodeOffset, head = on + offsetof(g::GroupJob, dep_last);
        const GuestAddr last = o->dep_last;
        *m.at<g::GroupNode>(node) = {last, head};
        o->dep_last = node;
        m.at<g::GroupNode>(last)->next = node;
        o->dep_count++;
        m.at<g::GroupJob>(job)->waiting++;
    }
    void seed() {   // (the twin registration's seed: every job that waits for none, in the graph's order)
        for (int i = 0; i < jobs; i++)
            if (m.at<g::GroupJob>(job_at(i))->waiting == 0) m.ref_push(job_at(i));
    }
};

// One context alone: the heap's order (priorities with ties, jobs with several runners staying on top), the frame, the
// observer, the lane status, the group's end.
void the_heap_order() {
    Model m;
    Setup su(m, 1, 64);
    bridge::JobGroup grp(m, {.check = true});
    tls_thread = &su.threads[0];
    su.begin(24);
    std::mt19937 rng(7);
    std::vector<int> want_runs(24);
    for (int i = 0; i < 24; i++) {
        const uint16_t runners = i % 5 == 0 ? 3 : 1;
        su.job(i, (uint16_t)(rng() % 6), runners, bridge::kAnyLane, i % 2 == 0, false, (uint8_t)(i % 3 == 0));
        want_runs[(size_t)i] = runners;
    }
    // (the expected order from the reference heap: the top runs once a runner; its last runner takes it down)
    su.seed();
    std::vector<int> want;
    {
        std::vector<uint8_t> saved(m.memory);
        g::Group* gr = m.at<g::Group>(kGroup);
        std::vector<int> left(want_runs);
        while (gr->heap.size) {
            const int top = (int)((m.items()[0] - kJobs) / sizeof(g::GroupJob));
            want.push_back(top);
            if (--left[(size_t)top] == 0) m.ref_pop();
        }
        m.memory = saved;
    }
    *m.at<GuestAddr>(kObsObject) = kObsVtable;
    *m.at<GuestAddr>(kObsVtable) = kObserver;
    m.at<g::DispatchContext>(kContexts)->observer = kObsObject;
    grp.run(&su.threads[0], kGroup, kContexts);
    CHECK(m.order == want);
    g::Group* gr = m.at<g::Group>(kGroup);
    CHECK(gr->flags & 2);
    CHECK(gr->heap.size == 0 && gr->take_count == 0);
    CHECK(m.observed == 2 * (int)want.size());
    CHECK(*m.at<uint64_t>(kLaneWord) == 0);
    CHECK((m.at<g::DispatchContext>(kContexts)->flags & 1) == 0);
    CHECK(m.wrong_frame == 0 && m.wrong_lane == 0);
    for (int i = 0; i < 24; i++) {
        CHECK(m.at<g::GroupJob>(Setup::job_at(i))->waiting == -1);
        CHECK(m.at<g::GroupJob>(Setup::job_at(i))->runners == 0);
    }
    const bridge::JobGroup::Stats& st = grp.stats();
    CHECK(st.runs == want.size() + 1 && st.finishes == st.runs && st.completes == 25 && st.ends == 1);   // (and the end job)
    CHECK(st.takes == st.runs_from_takes && st.given_back == 5 * 2);   // (the five three-runner jobs give back twice)
    CHECK(st.bad_finish == 0 && st.bad_count == 0 && st.double_complete == 0 && st.double_run == 0 && st.heap_bad == 0);
    CHECK(st.end_left == 0 && st.end_count == 0 && st.lane_drift == 0 && st.below_zero == 0);
    tls_thread = nullptr;
}

// The native push against the reference heap, a full heap (counted, not put in), and a hook's push left to the guest's.
void the_push() {
    Model m;
    Setup su(m, 1, 40);
    bridge::JobGroup grp(m, {.check = true});
    tls_thread = &su.threads[0];
    su.begin(41);
    std::mt19937 rng(99);
    for (int i = 0; i < 41; i++) su.job(i, (uint16_t)(rng() % 9), 1, bridge::kAnyLane, false);
    std::vector<uint8_t> before(m.memory);
    for (int i = 0; i < 41; i++) m.ref_push(Setup::job_at(i));
    const std::vector<GuestAddr> want(m.items(), m.items() + 40);
    m.memory = before;
    for (int i = 0; i < 41; i++) grp.push(&su.threads[0], kGroup, Setup::job_at(i));
    g::Group* gr = m.at<g::Group>(kGroup);
    CHECK(std::equal(want.begin(), want.end(), m.items()));
    CHECK(gr->heap.size == 40 && gr->take_count == 41 && grp.stats().full == 1 && grp.stats().readied == 40);
    su.job(41, 3, 1, bridge::kAnyLane, false);
    m.at<g::GroupJob>(Setup::job_at(41))->hook = kHook;
    gr->heap.room = 64;
    grp.push(&su.threads[0], kGroup, Setup::job_at(41));
    CHECK(m.hook_push == 1 && grp.stats().hook_pushes == 1 && gr->take_count == 42);
    tls_thread = nullptr;
}

// A job whose first finished runner cancels its others (the cancel flag): taken out of the heap, the take count lowered,
// complete at its one run.
void a_cancel() {
    Model m;
    Setup su(m, 1, 16);
    bridge::JobGroup grp(m, {.check = true});
    tls_thread = &su.threads[0];
    su.begin(4);
    su.job(0, 9, 4, bridge::kAnyLane, true, true);
    for (int i = 1; i < 4; i++) su.job(i, (uint16_t)i, 1, bridge::kAnyLane, false);
    su.seed();
    grp.run(&su.threads[0], kGroup, kContexts);
    const std::vector<int> want = {0, 3, 2, 1};
    CHECK(m.order == want);
    g::Group* gr = m.at<g::Group>(kGroup);
    CHECK((gr->flags & 2) && gr->heap.size == 0 && gr->take_count == 0);
    const bridge::JobGroup::Stats& st = grp.stats();
    CHECK(st.cancels == 1 && st.completes == 5 && st.heap_bad == 0 && st.bad_finish == 0 && st.end_count == 0);
    tls_thread = nullptr;
}

// The main thread's context and five workers' on six threads, group after group on one scheduler: random graphs with
// priorities, jobs of several runners, jobs that cancel, jobs tied to a lane (handed to that lane's context, by its state
// word or its queue), a polling lane in some groups. Every job runs as many times as it has runners (a cancelling one at
// least once), on its lane when tied to one, its dependents become ready once, and each group ends with the heap empty.
void six_contexts() {
    Model m;
    m.slow_bodies = true;
    constexpr int kJobsPerGroup = 500;
    Setup su(m, kContextsMax, 1024);
    bridge::JobGroup grp(m, {.check = true});
    std::mt19937 rng(4242);
    g::Group* gr = m.at<g::Group>(kGroup);
    for (int round = 0; round < 30; round++) {
        su.begin(kJobsPerGroup);
        std::vector<int> want(kJobsPerGroup);
        std::vector<bool> cancels(kJobsPerGroup);
        for (int i = 0; i < kJobsPerGroup; i++) {
            const int r = (int)(rng() % 10);
            const uint16_t runners = r < 2 ? (uint16_t)(2 + rng() % 4) : 1;
            const bool cancel = runners > 1 && rng() % 3 == 0;
            const uint32_t lane = (runners == 1 && r >= 8) ? (uint32_t)(1 + rng() % kContextsMax) : bridge::kAnyLane;
            su.job(i, (uint16_t)(rng() % 8), runners, lane, rng() % 2, cancel, (uint8_t)(rng() % 2));
            want[(size_t)i] = runners;
            cancels[(size_t)i] = cancel;
            m.runs[(size_t)i] = 0;
        }
        for (int i = 20; i < kJobsPerGroup; i++) {
            const int k = (int)(rng() % 3);
            for (int e = 0; e < k; e++) su.depends(Setup::job_at(i), Setup::job_at((int)(rng() % (unsigned)i)));
        }
        su.seed();
        gr->poll_lane = round % 4 == 1 ? 4 : 0;
        std::vector<std::thread> threads;
        for (int i = 0; i < kContextsMax; i++)
            threads.emplace_back([&, i] {
                tls_thread = &su.threads[i];
                grp.run(&su.threads[i], kGroup, kContexts + (GuestAddr)i * 0x100);
                tls_thread = nullptr;
            });
        for (std::thread& t : threads) t.join();
        for (int i = 0; i < kJobsPerGroup; i++) {
            const int ran = m.runs[(size_t)i];
            if (cancels[(size_t)i] ? (ran < 1 || ran > want[(size_t)i]) : ran != want[(size_t)i])
                std::fprintf(stderr, "group %d job %d: ran %d, runners %d\n", round, i, ran, want[(size_t)i]);
            CHECK(cancels[(size_t)i] ? (ran >= 1 && ran <= want[(size_t)i]) : ran == want[(size_t)i]);
            CHECK(m.at<g::GroupJob>(Setup::job_at(i))->waiting == -1);
        }
        CHECK(gr->flags & 2);
        CHECK(gr->heap.size == 0 && gr->take_count == 0);
        CHECK(*m.at<uint64_t>(kLaneWord) == 0);
        for (int i = 0; i < kContextsMax; i++) {
            const g::DispatchContext* c = m.at<g::DispatchContext>(kContexts + (GuestAddr)i * 0x100);
            CHECK((c->flags & 1) == 0 && c->state <= 1 && c->queue_count == 0);
        }
        CHECK(m.wrong_lane == 0 && m.wrong_frame == 0);
    }
    const bridge::JobGroup::Stats& st = grp.stats();
    CHECK(st.bad_finish == 0 && st.bad_count == 0 && st.double_complete == 0 && st.double_run == 0 && st.heap_bad == 0);
    CHECK(st.runs == st.finishes && st.passed == st.handed && st.drops == 0);
    CHECK(st.takes == st.runs_from_takes + st.passed + st.drops);
    CHECK(st.ends == 30 && st.end_checks == 30 && st.end_left == 0 && st.end_count == 0 && st.end_asleep == 0);
    CHECK(st.passed > 0 && st.given_back > 0 && st.cancels > 0 && st.sleeps > 0 && st.wakes > 0 && st.polls > 0);
    CHECK(m.guard_taken == 1 && *m.at<int64_t>(bridge::kGroupPollTicks) == kFrequency / 10000);
    char line[1024];
    grp.report(line, sizeof line);
    std::printf("%s\n", line);
}

}   // namespace

int main() {
    the_heap_order();
    the_push();
    a_cancel();
    six_contexts();
    std::puts("native engine: job group on a model of the game's group scheduler: ok");
    return 0;
}
