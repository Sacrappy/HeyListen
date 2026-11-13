// HeyListen native engine: the native dispatcher (hl/bridge/Dispatcher.h) run with no game, on a model of the game's
// scheduler in host memory: its rings, job records, dispatch contexts and lane status word laid out as the guest has
// them, and the game's own functions the dispatcher calls (events, the mutex, forwarding, the address waiters, job
// bodies) played by this file as docs/native-engine/jobs-boundary.md describes them.
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

#include "hl/bridge/Dispatcher.h"

using namespace hl;
using bridge::GuestAddr;
namespace g = bridge::guest;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

constexpr GuestAddr kBase = 0x10000000, kSize = 8u << 20;
constexpr GuestAddr kGlobals = 0x84e9f000;   // (the page of the dispatcher's timed-sleep constant and its guard)
constexpr GuestAddr kBody = 0x70000010, kObserver = 0x70000020, kNestBody = 0x70000030, kTranslatedDispatcher = bridge::kDispatcher;

constexpr GuestAddr kSched = kBase + 0x1000, kList = kBase + 0x2000, kContexts = kBase + 0x3000, kLaneWord = kBase + 0x4000,
                    kQueues = kBase + 0x5000, kCells = kBase + 0x10000, kJobs = kBase + 0x100000, kNodes = kBase + 0x200000,
                    kObjects = kBase + 0x300000, kVtable = kBase + 0x380000, kObsObject = kBase + 0x380100,
                    kObsVtable = kBase + 0x380200, kNestVtable = kBase + 0x380300, kStacks = kBase + 0x400000;
// A second scheduler, its list, contexts, lane word, queues and cells, this far above the first's.
constexpr GuestAddr kSecond = 0x500000;
constexpr int kContextsMax = 6;

struct Thread {   // a guest thread: its lane (its context's) and its stack
    int lane;
    GuestAddr sp;
};
thread_local Thread* tls_thread = nullptr;

struct Model final : bridge::GuestRuntime {
    std::vector<uint8_t> memory = std::vector<uint8_t>(kSize);
    std::vector<uint8_t> globals = std::vector<uint8_t>(0x1000);
    // the address waiters (one generation for all addresses: a wake-up for nothing is allowed, a lost one is not)
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    uint64_t generation = 0;
    // events (set until a wait takes them) and mutexes, by address
    std::mutex ev_mu;
    std::condition_variable ev_cv;
    std::map<GuestAddr, int> events;
    std::mutex mx_mu;
    std::map<GuestAddr, std::unique_ptr<std::mutex>> mutexes;
    // what ran
    std::vector<std::atomic<int>> runs = std::vector<std::atomic<int>>(8192);
    std::atomic<int> wrong_lane{0}, wrong_frame{0}, translated{0}, hook_push{0}, observed{0};
    std::mutex order_mu;
    std::vector<int> order;
    std::atomic<bool> slow_bodies{false};
    // the translated dispatcher's entry gate (null: none), and the stage a nesting job's body runs (its scheduler, context)
    bridge::Dispatcher* gate = nullptr;
    GuestAddr nest_sched = 0, nest_ctx = 0;

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
        case bridge::kAddrNotify: {
            std::lock_guard lock(wait_mu);
            generation++;
            wait_cv.notify_all();
            return 0;
        }
        case bridge::kAddrMonitor: {
            std::lock_guard lock(wait_mu);
            return generation;
        }
        case bridge::kAddrWait: {
            std::unique_lock lock(wait_mu);
            wait_cv.wait(lock, [&] { return generation != a[1]; });
            return 0;
        }
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
        case bridge::kTicksToSpan: return a[0];
        case bridge::kSleepFor: std::this_thread::sleep_for(std::chrono::microseconds(20)); return 0;
        case bridge::kForward: forward(t, a[0], (uint32_t)a[1], a[2]); return 0;
        case bridge::kPush: hook_push++; return 0;
        case kTranslatedDispatcher:   // (its entry gate first; the translated body makes the timed sleep's constant)
            if (gate && gate->gated(t, a[0], a[1])) return 0;
            translated++;
            *at<uint8_t>(bridge::kPollTicksGuard) = 1;
            return 0;
        case kObserver: observed++; return 0;
        case kBody: body(t, a[0], a[1]); return 0;
        case kNestBody: {   // (a job that runs a stage of another scheduler and waits for it, as 0x806afc40's callers do)
            body(t, a[0], a[1]);
            const uint64_t args[2] = {nest_sched, nest_ctx};
            call_with(t, kTranslatedDispatcher, args, 2, nullptr, 0);
            return 0;
        }
        default: CHECK(!"a guest function the model does not have"); return 0;
        }
    }
    std::mutex& mutex_of(GuestAddr a) {
        std::lock_guard lock(mx_mu);
        std::unique_ptr<std::mutex>& m = mutexes[a];
        if (!m) m = std::make_unique<std::mutex>();
        return *m;
    }
    // A job's body: the frame is (scheduler, job, 1) for a tagged object; it records that it ran, and where.
    void body(Thread* t, GuestAddr object, GuestAddr frame) {
        const GuestAddr job = *at<GuestAddr>(frame + 8);
        const int index = (int)((job - kJobs) / sizeof(g::Job));
        g::Job* j = at<g::Job>(job);
        const GuestAddr sched = *at<GuestAddr>(frame);
        if ((j->object & ~(GuestAddr)1) != object || (sched != kSched && sched != kSched + kSecond) || *at<uint32_t>(frame + 16) != 1)
            wrong_frame++;
        if (j->lane != bridge::kAnyLane && (int)j->lane != t->lane) wrong_lane++;
        runs[(size_t)index]++;
        {
            std::lock_guard lock(order_mu);
            order.push_back(index);
        }
        if (slow_bodies && index % 7 == 0) std::this_thread::sleep_for(std::chrono::microseconds(30));
    }
    // 0x810cc640 as read: to the context of that lane, unless it is not to run.
    void forward(Thread* t, GuestAddr s_at, uint32_t lane, GuestAddr job) {
        g::Scheduler* s = at<g::Scheduler>(s_at);
        GuestAddr ctx = 0;
        for (int i = 0; i < s->context_count; i++) {
            const GuestAddr c = at<GuestAddr>(s->contexts)[i];
            if ((uint32_t)at<g::DispatchContext>(c)->lane == lane) { ctx = c; break; }
        }
        if (!ctx) return;
        g::DispatchContext* c = at<g::DispatchContext>(ctx);
        if (c->flags & 2) return;
        for (;;) {
            uint64_t v = __atomic_load_n(&c->state, __ATOMIC_SEQ_CST);
            if (v > 1) {
                std::mutex& m = mutex_of(ctx + offsetof(g::DispatchContext, mutex));
                std::lock_guard lock(m);
                if (__atomic_load_n(&c->state, __ATOMIC_SEQ_CST) < 2) continue;
                if (c->queue_count < c->queue_room) {
                    const int32_t i = c->queue_first + c->queue_count;
                    at<GuestAddr>(c->queue)[i - (i < c->queue_room ? 0 : c->queue_room)] = job;
                    c->queue_count++;
                }
                return;
            }
            if (__atomic_compare_exchange_n(&c->state, &v, job, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                if (v == 1) {
                    const uint64_t args[1] = {ctx + offsetof(g::DispatchContext, wake_event)};
                    call_with(t, bridge::kEventSet, args, 1, nullptr, 0);
                }
                return;
            }
        }
    }
};

struct Setup {
    Model& m;
    int contexts;
    GuestAddr off;   // (0: the first scheduler; kSecond: the second)
    Thread threads[kContextsMax + 2];
    GuestAddr next_cells, next_node = kNodes;

    Setup(Model& model, int n, int a_size, int b_size, GuestAddr offset = 0)
        : m(model), contexts(n), off(offset), next_cells(kCells + offset) {
        *m.at<uint64_t>(bridge::kPollTicks) = 1000;
        *m.at<uint8_t>(bridge::kPollTicksGuard) = 1;
        g::Scheduler* s = m.at<g::Scheduler>(kSched + off);
        s->context_count = n;
        s->context_room = kContextsMax;
        s->contexts = kList + off;
        for (int l = 0; l < 3; l++) {
            ring(s->levels[l].a, a_size);
            ring(s->levels[l].b, b_size);
        }
        for (int i = 0; i < n; i++) {
            const GuestAddr ctx = kContexts + off + (GuestAddr)i * 0x100;
            m.at<GuestAddr>(kList + off)[i] = ctx;
            g::DispatchContext* c = m.at<g::DispatchContext>(ctx);
            c->lane = i + 1;
            c->lane_status = kLaneWord + off;
            c->queue = kQueues + off + (GuestAddr)i * 0x100;
            c->queue_room = 16;
        }
        for (int i = 0; i < kContextsMax + 2; i++) threads[i] = {i + 1, kStacks + (GuestAddr)(i + 1) * 0x10000};
    }
    void ring(g::Ring& r, int size) {
        r.size = size;
        r.cells = next_cells;
        for (int i = 0; i < size; i++) m.at<g::Cell>(next_cells)[i] = {0, (uint64_t)i};
        next_cells += (GuestAddr)size * sizeof(g::Cell);
    }
    static GuestAddr job_at(int i) { return kJobs + (GuestAddr)i * sizeof(g::Job); }
    // a job record: its object's third virtual function is the model's body
    void job(int i, int16_t level, uint16_t runners, uint32_t lane, bool tagged) {
        g::Job* j = m.at<g::Job>(job_at(i));
        std::memset(j, 0, sizeof *j);
        const GuestAddr object = kObjects + (GuestAddr)i * 16;
        *m.at<GuestAddr>(object) = kVtable;
        *m.at<GuestAddr>(kVtable + 0x10) = kBody;
        j->object = object | (tagged ? 1 : 0);
        j->level = level;
        j->lane = lane;
        j->runners = runners;
        const GuestAddr head = job_at(i) + offsetof(g::Job, dep_node);
        j->dep_first = j->dep_last = head;
    }
    GuestAddr end() const { return kSched + off + offsetof(g::Scheduler, end_job); }
    void end_job(int waiting) {
        g::Job* e = &m.at<g::Scheduler>(kSched + off)->end_job;
        std::memset(e, 0, sizeof *e);
        e->waiting = waiting;
        e->runners = 1;
        e->lane = bridge::kAnyLane;
        e->dep_first = e->dep_last = end() + offsetof(g::Job, dep_node);
    }
    void depends(GuestAddr job, GuestAddr on) {   // `job` waits for `on`: a node at the end of on's list
        g::Job* o = m.at<g::Job>(on);
        const GuestAddr head = on + offsetof(g::Job, dep_node);
        const GuestAddr node = next_node;
        next_node += sizeof(g::DepNode);
        *m.at<g::DepNode>(node) = {job, head};
        if (o->dep_first == head) o->dep_first = node;
        else m.at<g::DepNode>(o->dep_last)->next = node;
        o->dep_last = node;
        m.at<g::Job>(job)->waiting++;
    }
    int32_t queued() {
        g::Scheduler* s = m.at<g::Scheduler>(kSched + off);
        int32_t n = 0;
        for (auto& l : s->levels) n += (int32_t)(l.a.tail - l.a.head) + (int32_t)(l.b.tail - l.b.head);
        return n;
    }
    void begin_stage() {
        g::Scheduler* s = m.at<g::Scheduler>(kSched + off);
        s->flags = 0;
        for (int i = 0; i < contexts; i++) {
            g::DispatchContext* c = m.at<g::DispatchContext>(m.at<GuestAddr>(kList + off)[i]);
            c->flags = 1;
            c->state = 0;
        }
    }
};

// A model push into an A ring (single-threaded, for setting up the order test).
void push_a(Model& m, g::Ring& r, GuestAddr job) {
    g::Cell* cell = m.at<g::Cell>(r.cells) + (r.tail & (uint64_t)(r.size - 1));
    CHECK(cell->seq == r.tail);
    cell->job = job;
    cell->seq = r.tail + 1;
    r.tail++;
}

// One context alone: the order of service, FIFO in a ring, a parallel job's runners, an observer, the stage's end.
void the_order_of_service() {
    Model m;
    Setup su(m, 1, 8, 8);
    bridge::Dispatcher d(m, {.native = true, .check = true});
    g::Scheduler* s = m.at<g::Scheduler>(kSched);
    tls_thread = &su.threads[0];
    // jobs 0-11: two in each ring, pushed in the order 2B 2A 1B 1A 0B 0A, so that service order differs from push order
    int n = 0;
    const int16_t level_of[3] = {-2, 7, 0};
    for (int l = 2; l >= 0; l--) {
        for (int k = 0; k < 2; k++) {
            su.job(n, level_of[l], 1, bridge::kAnyLane, n % 2 == 0);
            d.push(&su.threads[0], kSched, Setup::job_at(n));
            n++;
        }
        for (int k = 0; k < 2; k++) {
            su.job(n, level_of[l], 1, bridge::kAnyLane, n % 2 == 1);
            push_a(m, s->levels[l].a, Setup::job_at(n));
            n++;
        }
    }
    // job 12: three runners, level 1 B; job 13: tied to this context's lane
    su.job(12, 7, 3, bridge::kAnyLane, true);
    d.push(&su.threads[0], kSched, Setup::job_at(12));
    su.job(13, 0, 1, 1, false);
    d.push(&su.threads[0], kSched, Setup::job_at(13));
    su.end_job(0);
    for (int i = 0; i < 14; i++) su.depends(kSched + offsetof(g::Scheduler, end_job), Setup::job_at(i));
    // an observer whose first virtual function is the model's
    *m.at<GuestAddr>(kObsObject) = kObsVtable;
    *m.at<GuestAddr>(kObsVtable) = kObserver;
    m.at<g::DispatchContext>(kContexts)->observer = kObsObject;
    su.begin_stage();
    d.dispatch(&su.threads[0], kSched, kContexts);
    // 0A (10, 11), 0B (8, 9), 1A (6, 7), 1B (4, 5, then 12 three times: from B into 1A, taken back from 1A), 2A, 2B (0, 1, 13)
    const std::vector<int> want = {10, 11, 8, 9, 6, 7, 4, 5, 12, 12, 12, 2, 3, 0, 1, 13};
    CHECK(m.order == want);
    CHECK(s->flags & 2);
    CHECK(m.observed == 2 * 16);
    CHECK(*m.at<uint64_t>(kLaneWord) == 0);
    CHECK(su.queued() == 0);
    CHECK((m.at<g::DispatchContext>(kContexts)->flags & 1) == 0);
    CHECK(m.wrong_frame == 0 && m.wrong_lane == 0 && m.translated == 0);
    const bridge::Dispatcher::Stats& st = d.stats();
    CHECK(st.runs == 17 && st.finishes == 17 && st.completes == 15 && st.stage_ends == 1);   // (and the end job, with no body)
    CHECK(st.bad_finish == 0 && st.bad_count == 0 && st.double_run == 0);
    CHECK(st.pops_a + st.pops_b == st.runs_from_pops + st.forwards + st.drops);
    tls_thread = nullptr;
}

// The first dispatch of a process, before the guest made its constant: the translated dispatcher, whole.
void the_first_dispatch_is_translated() {
    Model m;
    Setup su(m, 1, 8, 8);
    *m.at<uint8_t>(bridge::kPollTicksGuard) = 0;
    bridge::Dispatcher d(m, {.native = true, .check = false});
    tls_thread = &su.threads[0];
    d.dispatch(&su.threads[0], kSched, kContexts);
    CHECK(m.translated == 1 && d.stats().translated == 1 && d.stats().stages == 0);
    tls_thread = nullptr;
}

// Every caller through the translated dispatcher's entry gate (HL_DISPATCH_ALL): the first call of the process is let
// through to the translated dispatcher once (the bypass), and the calls after it are native, among them a stage begun and
// waited for from inside a job's body (0x806afc40's callers): it runs nested on the same thread, on the second
// scheduler's context of the same lane (a worker set's own context, whose lane is the owner's core).
void the_gate_and_a_nested_stage() {
    Model m;
    Setup su(m, 1, 8, 8);
    Setup su2(m, 1, 8, 8, kSecond);
    su2.next_node = kNodes + 0x80000;
    bridge::Dispatcher d(m, {.native = true, .check = true});
    m.gate = &d;
    Thread* t = &su.threads[0];
    tls_thread = t;
    *m.at<uint8_t>(bridge::kPollTicksGuard) = 0;
    CHECK(d.gated(t, kSched, kContexts));
    CHECK(m.translated == 1 && d.stats().bypassed == 1 && d.stats().translated == 1 && d.stats().stages == 0);
    // the first scheduler: jobs 0-3, job 1's body runs the second's stage (jobs 100-104) and waits for it
    for (int i = 0; i < 4; i++) su.job(i, 7, 1, bridge::kAnyLane, i % 2 == 0);
    const GuestAddr nest_object = m.at<g::Job>(Setup::job_at(1))->object & ~(GuestAddr)1;
    *m.at<GuestAddr>(nest_object) = kNestVtable;
    *m.at<GuestAddr>(kNestVtable + 0x10) = kNestBody;
    su.end_job(0);
    for (int i = 0; i < 4; i++) su.depends(su.end(), Setup::job_at(i));
    for (int i = 100; i < 105; i++) su2.job(i, 7, 1, bridge::kAnyLane, true);
    su2.end_job(0);
    for (int i = 100; i < 105; i++) su2.depends(su2.end(), Setup::job_at(i));
    for (int i = 0; i < 4; i++) d.push(t, kSched, Setup::job_at(i));
    for (int i = 100; i < 105; i++) d.push(t, kSched + kSecond, Setup::job_at(i));
    m.nest_sched = kSched + kSecond;
    m.nest_ctx = kContexts + kSecond;
    su.begin_stage();
    su2.begin_stage();
    CHECK(d.gated(t, kSched, kContexts));
    const std::vector<int> want = {0, 1, 100, 101, 102, 103, 104, 2, 3};
    CHECK(m.order == want);
    CHECK(m.translated == 1 && m.wrong_frame == 0 && m.wrong_lane == 0);
    for (const Setup* s : {&su, &su2}) {
        CHECK(m.at<g::Scheduler>(kSched + s->off)->flags & 2);
        CHECK(*m.at<uint64_t>(kLaneWord + s->off) == 0);
        CHECK(const_cast<Setup*>(s)->queued() == 0);
        CHECK((m.at<g::DispatchContext>(kContexts + s->off)->flags & 1) == 0);
    }
    const bridge::Dispatcher::Stats& st = d.stats();
    // (through the gate: the first call, its call let through, the first scheduler's stage, the nested one)
    CHECK(st.gated == 4 && st.bypassed == 1 && st.stages == 2 && st.stage_ends == 2);
    CHECK(st.runs == 11 && st.finishes == 11);   // (and the two end jobs, with no body)
    CHECK(st.bad_finish == 0 && st.bad_count == 0 && st.double_run == 0);
    CHECK(st.pops_a + st.pops_b == st.runs_from_pops + st.forwards + st.drops);
    tls_thread = nullptr;
}

// A full ring: the push returns without pushing, as the guest's does.
void a_full_ring_drops_the_push() {
    Model m;
    Setup su(m, 1, 4, 4);
    bridge::Dispatcher d(m, {.native = true, .check = true});
    tls_thread = &su.threads[0];
    for (int i = 0; i < 5; i++) {
        su.job(i, 7, 1, bridge::kAnyLane, false);
        d.push(&su.threads[0], kSched, Setup::job_at(i));
    }
    CHECK(su.queued() == 4 && d.stats().readied == 4 && d.stats().full == 1);
    tls_thread = nullptr;
}

// Six contexts on six threads, two threads pushing the roots meanwhile (and waking a sleeper, as the game's pushers do),
// stage after stage on one scheduler (the rings' counters go round many times): every job runs as many times as it has
// runners, on its lane when tied to one, its dependents become ready once, and the stage ends with nothing left.
void six_contexts_and_two_pushers() {
    Model m;
    m.slow_bodies = true;
    constexpr int kJobsPerStage = 600;
    Setup su(m, kContextsMax, 64, 256);
    bridge::Dispatcher d(m, {.native = true, .check = true});
    std::mt19937 rng(12345);
    g::Scheduler* s = m.at<g::Scheduler>(kSched);
    const GuestAddr end = kSched + offsetof(g::Scheduler, end_job);
    for (int stage = 0; stage < 30; stage++) {
        su.next_node = kNodes;
        std::vector<int> want(kJobsPerStage);
        std::vector<int> roots;
        for (int i = 0; i < kJobsPerStage; i++) {
            const int16_t levels[3] = {-2, 0, 3};
            const int r = (int)(rng() % 10);
            const uint16_t runners = r < 2 ? (uint16_t)(2 + rng() % 3) : 1;
            const uint32_t lane = (runners == 1 && r >= 8) ? (uint32_t)(1 + rng() % kContextsMax) : bridge::kAnyLane;
            su.job(i, levels[rng() % 3], runners, lane, rng() % 2);
            want[(size_t)i] = runners;
            m.runs[(size_t)i] = 0;
        }
        su.end_job(0);
        // each job waits for 0-2 earlier ones; those that nothing waits for are waited for by the end job
        std::vector<bool> waited(kJobsPerStage);
        for (int i = 0; i < kJobsPerStage; i++) {
            const int k = i < 20 ? 0 : (int)(rng() % 3);
            for (int e = 0; e < k; e++) {
                const int on = (int)(rng() % (unsigned)i);
                su.depends(Setup::job_at(i), Setup::job_at(on));
                waited[(size_t)on] = true;
            }
        }
        for (int i = 0; i < kJobsPerStage; i++) {
            if (!waited[(size_t)i]) su.depends(end, Setup::job_at(i));
            if (m.at<g::Job>(Setup::job_at(i))->waiting == 0) roots.push_back(i);
        }
        s->poll_lane = stage % 3 == 1 ? 3 : 0;
        su.begin_stage();
        std::vector<std::thread> threads;
        for (int i = 0; i < kContextsMax; i++)
            threads.emplace_back([&, i] {
                tls_thread = &su.threads[i];
                d.dispatch(&su.threads[i], kSched, kContexts + (GuestAddr)i * 0x100);
                tls_thread = nullptr;
            });
        for (int p = 0; p < 2; p++)
            threads.emplace_back([&, p] {
                Thread* t = &su.threads[kContextsMax + p];
                t->lane = 0;
                tls_thread = t;
                for (size_t k = (size_t)p; k < roots.size(); k += 2) {
                    d.push(t, kSched, Setup::job_at(roots[k]));
                    // (0x8389af90: then the first context asleep is woken)
                    for (int c = 0; c < kContextsMax; c++) {
                        const GuestAddr ctx = kContexts + (GuestAddr)c * 0x100;
                        uint64_t one = 1;
                        if (__atomic_compare_exchange_n(&m.at<g::DispatchContext>(ctx)->state, &one, 0, false, __ATOMIC_SEQ_CST,
                                                        __ATOMIC_SEQ_CST)) {
                            const uint64_t args[1] = {ctx + offsetof(g::DispatchContext, wake_event)};
                            m.call_with(t, bridge::kEventSet, args, 1, nullptr, 0);
                            break;
                        }
                    }
                    if (k % 16 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                tls_thread = nullptr;
            });
        for (std::thread& t : threads) t.join();
        for (int i = 0; i < kJobsPerStage; i++) {
            if (m.runs[(size_t)i] != want[(size_t)i]) std::fprintf(stderr, "stage %d job %d: ran %d, runners %d\n", stage, i, m.runs[(size_t)i].load(), want[(size_t)i]);
            CHECK(m.runs[(size_t)i] == want[(size_t)i]);
            CHECK(m.at<g::Job>(Setup::job_at(i))->waiting == 0);
        }
        CHECK(s->flags & 2);
        CHECK(su.queued() == 0);
        CHECK(*m.at<uint64_t>(kLaneWord) == 0);
        for (int i = 0; i < kContextsMax; i++) {
            const g::DispatchContext* c = m.at<g::DispatchContext>(kContexts + (GuestAddr)i * 0x100);
            CHECK((c->flags & 1) == 0 && c->state <= 1 && c->queue_count == 0);
        }
        CHECK(m.wrong_lane == 0 && m.wrong_frame == 0 && m.translated == 0);
    }
    const bridge::Dispatcher::Stats& st = d.stats();
    CHECK(st.bad_finish == 0 && st.bad_count == 0 && st.double_run == 0);
    CHECK(st.runs == st.finishes);
    CHECK(st.pops_a + st.pops_b == st.runs_from_pops + st.forwards + st.drops);
    CHECK(st.stage_ends == 30 && st.full == 0);
    CHECK(st.forwards > 0 && st.requeues > 0 && st.sleeps > 0 && st.wakes > 0);
    char line[1024];
    d.report(line, sizeof line);
    std::printf("%s\n", line);
}

}   // namespace

int main() {
    the_order_of_service();
    the_first_dispatch_is_translated();
    the_gate_and_a_nested_stage();
    a_full_ring_drops_the_push();
    six_contexts_and_two_pushers();
    std::puts("native engine: dispatcher on a model of the game's scheduler: ok");
    return 0;
}
