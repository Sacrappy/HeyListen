// HeyListen native engine: the native stage protocol (hl/bridge/Stages.h) run with no game, on a model of the game's
// worker set, its workers (each a thread with a message queue that runs the dispatcher on message 1 and then signals its
// done event, as the worker message 0x8004aae0 does), a stage's scheduler and the actor manager, laid out as the guest has
// them; the game's functions they call (events, the message queue, the thread-local core, the address waiters) are
// played by this file as docs/native-engine/jobs-boundary.md describes them. The translated registration the check
// compares with is played by a second reading of the same section (reference_register).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "hl/bridge/Dispatcher.h"
#include "hl/bridge/Stages.h"

using namespace hl;
using bridge::GuestAddr;
namespace g = bridge::guest;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

constexpr GuestAddr kBase = 0x10000000, kSize = 8u << 20;
constexpr GuestAddr kPage1 = 0x84e9f000, kPage2 = 0x84cab000;   // (the dispatcher's constant and the core key; the slot)
constexpr GuestAddr kSched = kBase + 0x1000, kList = kBase + 0x2000, kLaneWord = kBase + 0x2800, kSet = kBase + 0x3000,
                    kSetArray = kBase + 0x3800, kWrappers = kBase + 0x3900, kOwnQueue = kBase + 0x3a00,
                    kWorkers = kBase + 0x4000, kHolder = kBase + 0x7000, kManager = kBase + 0x7100,
                    kUsualVtable = kBase + 0x8000, kOtherVtable = kBase + 0x8100, kJobVtable = kBase + 0x8200,
                    kCells = kBase + 0x10000, kJobs = kBase + 0x100000, kNodes = kBase + 0x200000,
                    kObjects = kBase + 0x300000, kStacks = kBase + 0x400000;
constexpr GuestAddr kUsualPost = 0x70000100, kOtherPost = 0x70000200, kBody = 0x70000010;
constexpr int kCores = 6, kThreads = 16;

struct Thread {   // a guest thread: its core (and whether its thread-local core is set), its stack
    uint32_t core;
    bool tls;
    GuestAddr sp;
};
thread_local Thread* tls_thread = nullptr;

GuestAddr worker(int i) { return kWorkers + (GuestAddr)i * 0x400; }
GuestAddr job_at(int i) { return kJobs + (GuestAddr)i * sizeof(g::Job); }
constexpr GuestAddr kEnd = kSched + offsetof(g::Scheduler, end_job);

struct Model final : bridge::GuestRuntime {
    std::vector<uint8_t> memory = std::vector<uint8_t>(kSize);
    std::vector<uint8_t> page1 = std::vector<uint8_t>(0x1000), page2 = std::vector<uint8_t>(0x1000);
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    uint64_t generation = 0;
    std::mutex ev_mu;
    std::condition_variable ev_cv;
    std::map<GuestAddr, int> events;
    std::mutex mx_mu;
    std::map<GuestAddr, std::unique_ptr<std::mutex>> mutexes;
    // the workers' message queues
    std::mutex mail_mu;
    std::condition_variable mail_cv;
    std::deque<uint64_t> mail[kCores];
    std::atomic<int> runs[1024];
    std::atomic<int> virtual_posts{0}, wrong_frame{0};
    bridge::Stages* stages = nullptr;
    Thread threads[kThreads];

    Model() {
        for (auto& r : runs) r = 0;
        for (int i = 0; i < kThreads; i++) threads[i] = {(uint32_t)(i % kCores), true, kStacks + (GuestAddr)(i + 1) * 0x10000};
    }
    void* ptr(GuestAddr at) const override {
        if (at >= kPage1 && at < kPage1 + 0x1000) return const_cast<uint8_t*>(page1.data()) + (at - kPage1);
        if (at >= kPage2 && at < kPage2 + 0x1000) return const_cast<uint8_t*>(page2.data()) + (at - kPage2);
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
    void event_set(GuestAddr e) {
        std::lock_guard lock(ev_mu);
        events[e] = 1;
        ev_cv.notify_all();
    }
    void post(GuestAddr k, uint64_t message) {
        std::lock_guard lock(mail_mu);
        mail[(k - kWorkers) / 0x400].push_back(message);
        mail_cv.notify_all();
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
        case bridge::kEventSet: event_set(a[0]); return 0;
        case bridge::kEventClear: {
            std::lock_guard lock(ev_mu);
            events[a[0]] = 0;
            return 0;
        }
        case bridge::kMutexLock: mutex_of(a[0]).lock(); return 0;
        case bridge::kMutexUnlock: mutex_of(a[0]).unlock(); return 0;
        case bridge::kTicksToSpan: return a[0];
        case bridge::kSleepFor: std::this_thread::sleep_for(std::chrono::microseconds(20)); return 0;
        case bridge::kThreadLocal: CHECK(a[0] == 0x55); return t->tls ? t->core + 1 : 0;
        case bridge::kCurrentCore: return t->core;
        case bridge::kQueueSend: CHECK(a[1] == 1); post(a[0] - offsetof(g::Worker, queue), 1); return 0;
        case kOtherPost: CHECK(a[1] == 1 && a[2] == 1); virtual_posts++; post(a[0], 1); return 0;
        case kBody: {
            const GuestAddr job = *at<GuestAddr>(a[1] + 8);
            if (*at<GuestAddr>(a[1]) != kSched || (at<g::Job>(job)->object & ~(GuestAddr)1) != a[0]) wrong_frame++;
            runs[(job - kJobs) / sizeof(g::Job)]++;
            return 0;
        }
        case bridge::kStageRegister:   // (the check's call: through the gate, which lets it through to the "translated" one)
            CHECK(stages && !stages->register_gate(t, a[0], a[1]));
            reference_register(t, a[0], a[1]);
            return 0;
        default: CHECK(!"a guest function the model does not have"); return 0;
        }
    }
    std::mutex& mutex_of(GuestAddr a) {
        std::lock_guard lock(mx_mu);
        std::unique_ptr<std::mutex>& m = mutexes[a];
        if (!m) m = std::make_unique<std::mutex>();
        return *m;
    }
    // The registration as jobs-boundary.md reads it, without the message (the check calls it so).
    void reference_register(Thread* t, GuestAddr set, uint64_t send) {
        CHECK(send == 0);
        g::WorkerSet* w = at<g::WorkerSet>(set);
        g::Scheduler* s = at<g::Scheduler>(w->scheduler);
        s->context_count = 0;
        w->registered_mask = 0;
        w->registered_count = 0;
        const uint32_t c = t->core;
        auto add = [&](GuestAddr ctx) {
            if (s->context_count < (int32_t)s->context_room) at<GuestAddr>(s->contexts)[s->context_count++] = ctx;
        };
        if (!w->stage_cores || (w->flags & 1)) {
            w->own_context.lane = (int32_t)c;
            w->own_context.state = 0;
            w->own_context.flags = 0;
            events[set + offsetof(g::WorkerSet, own_context) + offsetof(g::DispatchContext, wake_event)] = 0;
            add(set + offsetof(g::WorkerSet, own_context));
            if ((w->flags & 3) == 1) s->flags |= 4;
            else s->flags &= ~4u;
            w->registered_count++;
            if (!w->stage_cores) return;
        } else {
            s->flags &= ~4u;
        }
        for (uint32_t i = 0; i < w->count; i++) {
            g::Worker* k = at<g::Worker>(worker(i));
            if ((w->flags & 1) && k->core_mask == 1u << c) continue;
            if (!(k->core_mask & w->stage_cores)) continue;
            add(worker(i) + offsetof(g::Worker, context));
            w->registered_mask |= 1u << i;
            w->registered_count++;
        }
        for (uint32_t i = 0; i < w->count; i++) {
            if (!(w->registered_mask >> i & 1)) continue;
            g::Worker* k = at<g::Worker>(worker(i));
            k->context.state = 0;
            k->context.flags = 0;
            events[worker(i) + offsetof(g::Worker, context) + offsetof(g::DispatchContext, wake_event)] = 0;
            events[worker(i) + offsetof(g::Worker, done_event)] = 0;
            k->scheduler = w->scheduler;
        }
    }
};

// The world: one worker set of six workers (worker i on core i, its context on lane i), its scheduler, the actor manager.
struct World {
    Model& m;
    bridge::Dispatcher d;
    bridge::Stages stages;
    std::vector<std::thread> workers;
    std::mt19937 rng{777};
    GuestAddr next_cells = kCells, next_node = kNodes;

    World(Model& model, bool check)
        : m(model), d(model, {.native = true, .check = true}), stages(model, &d, {.check = check}) {
        m.stages = &stages;
        *m.at<uint64_t>(bridge::kPollTicks) = 1000;
        *m.at<uint8_t>(bridge::kPollTicksGuard) = 1;
        *m.at<uint32_t>(bridge::kCoreKey) = 0x55;
        *m.at<GuestAddr>(bridge::kUsualMessageSlot) = kUsualPost;
        *m.at<GuestAddr>(kUsualVtable + 0x10) = kUsualPost;
        *m.at<GuestAddr>(kOtherVtable + 0x10) = kOtherPost;
        *m.at<GuestAddr>(kJobVtable + 0x10) = kBody;
        g::Scheduler* s = m.at<g::Scheduler>(kSched);
        s->context_room = 8;
        s->contexts = kList;
        for (auto& l : s->levels) {
            ring(l.a, 64);
            ring(l.b, 256);
        }
        g::WorkerSet* w = m.at<g::WorkerSet>(kSet);
        w->count = kCores;
        w->workers = kSetArray;
        w->own_context.lane_status = kLaneWord;
        w->own_context.queue = kOwnQueue;
        w->own_context.queue_room = 16;
        for (int i = 0; i < kCores; i++) {
            m.at<GuestAddr>(kSetArray)[i] = kWrappers + (GuestAddr)i * 16;
            *m.at<GuestAddr>(kWrappers + (GuestAddr)i * 16 + 8) = worker(i);
            g::Worker* k = m.at<g::Worker>(worker(i));
            k->vtable = kUsualVtable;
            k->core_mask = 1u << i;
            k->core = (uint32_t)i;
            k->context.lane = i;
            k->context.lane_status = kLaneWord;
            k->context.queue = worker(i) + 0x300;
            k->context.queue_room = 16;
        }
        m.at<g::StageHolder>(kHolder)->scheduler = kSched;
        m.at<g::StageHolder>(kHolder)->set = kSet;
        m.at<g::ActorManager>(kManager)->holder = kHolder;
        for (int i = 0; i < kCores; i++)
            workers.emplace_back([this, i] {
                Thread* t = &m.threads[i];
                tls_thread = t;
                for (;;) {
                    uint64_t message;
                    {
                        std::unique_lock lock(m.mail_mu);
                        m.mail_cv.wait(lock, [&] { return !m.mail[i].empty(); });
                        message = m.mail[i].front();
                        m.mail[i].pop_front();
                    }
                    if (message == 0) break;
                    const GuestAddr k = worker(i);
                    const GuestAddr s_at = m.at<g::Worker>(k)->scheduler;
                    if (s_at) d.dispatch(t, s_at, k + offsetof(g::Worker, context));   // (0x8004aae0: then its end)
                    m.event_set(k + offsetof(g::Worker, done_event));
                }
                tls_thread = nullptr;
            });
    }
    ~World() {
        for (int i = 0; i < kCores; i++) m.post(worker(i), 0);
        for (std::thread& t : workers) t.join();
        m.stages = nullptr;
    }
    void ring(g::Ring& r, int size) {
        r.size = size;
        r.cells = next_cells;
        for (int i = 0; i < size; i++) m.at<g::Cell>(next_cells)[i] = {0, (uint64_t)i};
        next_cells += (GuestAddr)size * sizeof(g::Cell);
    }
    void depends(GuestAddr job, GuestAddr on) {
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
    // A stage's graph on the scheduler and its roots pushed (the graph's making and the seed, as the begin forms do).
    std::vector<int> make_stage(Thread* t, int jobs) {
        next_node = kNodes;
        g::Job* e = &m.at<g::Scheduler>(kSched)->end_job;
        std::memset(e, 0, sizeof *e);
        e->runners = 1;
        e->lane = bridge::kAnyLane;
        e->dep_first = e->dep_last = kEnd + offsetof(g::Job, dep_node);
        std::vector<int> want((size_t)jobs);
        for (int i = 0; i < jobs; i++) {
            g::Job* j = m.at<g::Job>(job_at(i));
            std::memset(j, 0, sizeof *j);
            const GuestAddr object = kObjects + (GuestAddr)i * 16;
            *m.at<GuestAddr>(object) = kJobVtable;
            j->object = object | 1;
            const int16_t levels[3] = {-2, 0, 3};
            j->level = levels[rng() % 3];
            j->lane = bridge::kAnyLane;
            j->runners = rng() % 8 == 0 ? (uint16_t)(2 + rng() % 3) : 1;
            j->dep_first = j->dep_last = job_at(i) + offsetof(g::Job, dep_node);
            want[(size_t)i] = j->runners;
            m.runs[i] = 0;
        }
        std::vector<bool> waited((size_t)jobs);
        for (int i = 10; i < jobs; i++)
            for (int k = (int)(rng() % 3); k > 0; k--) {
                const int on = (int)(rng() % (unsigned)i);
                depends(job_at(i), job_at(on));
                waited[(size_t)on] = true;
            }
        for (int i = 0; i < jobs; i++)
            if (!waited[(size_t)i]) depends(kEnd, job_at(i));
        m.at<g::Scheduler>(kSched)->flags = 1;
        for (int i = 0; i < jobs; i++)
            if (m.at<g::Job>(job_at(i))->waiting == 0) d.push(t, kSched, job_at(i));
        m.at<g::WorkerSet>(kSet)->scheduler = kSched;
        return want;
    }
    void check_ran(const std::vector<int>& want) {
        for (size_t i = 0; i < want.size(); i++) CHECK(m.runs[i] == want[i]);
        CHECK(m.at<g::Scheduler>(kSched)->flags & 2);
        CHECK(m.wrong_frame == 0);
    }
    void check_clean() {
        const bridge::Stages::Stats& s = stages.stats();
        CHECK(s.overlap == 0 && s.unstarted == 0 && s.double_start == 0 && s.still_in == 0 && s.not_over == 0);
        CHECK(s.changed == 0 && s.below_zero == 0 && s.reg_diff == 0 && s.untracked == 0);
        const bridge::Dispatcher::Stats& ds = d.stats();
        CHECK(ds.bad_finish == 0 && ds.bad_count == 0 && ds.double_run == 0 && ds.end_left == 0);
        CHECK(ds.runs == ds.finishes);
    }
};

int popcount(uint32_t v) { return __builtin_popcount(v); }

// Stages in a row, each registered with its message and waited for by an owner that does not take part: on core 0 with
// no thread-local core (the OS's core is read), then on core 3 inside a job of its lane (its lane byte is given up during
// the waits and is back after). The scheduler's list is the registered workers' contexts in index order; the mask and
// the count stay as registered (nothing lowers the count of a waited stage).
void stages_in_a_row() {
    Model m;
    World w(m, true);
    Thread* owner = &m.threads[8];
    const uint32_t masks[4] = {0x3f, 0x15, 0x2a, 0x08};
    for (int stage = 0; stage < 24; stage++) {
        const bool inside = stage >= 12;
        owner->core = inside ? 3 : 0;
        owner->tls = inside;
        tls_thread = owner;
        *m.at<uint64_t>(kLaneWord) = inside ? 1ull << 24 : 0;   // (lane 3: the owner's job)
        const std::vector<int> want = w.make_stage(owner, 300);
        g::WorkerSet* set = m.at<g::WorkerSet>(kSet);
        set->stage_cores = masks[stage % 4];
        w.stages.register_workers(owner, kSet, 1);
        w.stages.wait(owner, kSet);
        tls_thread = nullptr;
        w.check_ran(want);
        const int n = popcount(masks[stage % 4]);
        g::Scheduler* s = m.at<g::Scheduler>(kSched);
        CHECK(s->context_count == n && set->registered_mask == masks[stage % 4] && set->registered_count == (uint32_t)n);
        for (int i = 0, k = 0; i < kCores; i++) {
            const g::Worker* worker_i = m.at<g::Worker>(worker(i));
            if (masks[stage % 4] >> i & 1) CHECK(m.at<GuestAddr>(kList)[k++] == worker(i) + offsetof(g::Worker, context));
            CHECK((worker_i->context.flags & 1) == 0);
        }
        CHECK((s->flags & 4) == 0);
        CHECK(*m.at<uint64_t>(kLaneWord) == (inside ? 1ull << 24 : 0));
    }
    w.check_clean();
    const bridge::Stages::Stats& s = w.stages.stats();
    char line[1024];
    w.stages.report(line, sizeof line);
    std::printf("%s\n", line);
    CHECK(s.registrations == 24 && s.reg_checked == 24 && s.waits == 24 && s.owner_dispatches == 0);
    CHECK(s.started == s.done_waits && s.started == s.workers_registered && s.started_virtual == 0 && s.lane_drift == 0);
}

// The owner takes part (from inside a job of its lane): its own context first in the list, on its core, the worker of its
// core left out and told not to run; the owner's thread runs the stage on its own context. With flags bit 1 the
// scheduler's "owner polls" bit is clear.
void the_owner_takes_part() {
    Model m;
    World w(m, true);
    Thread* owner = &m.threads[10];
    owner->core = 2;
    for (int stage = 0; stage < 8; stage++) {
        tls_thread = owner;
        const std::vector<int> want = w.make_stage(owner, 300);
        g::WorkerSet* set = m.at<g::WorkerSet>(kSet);
        set->stage_cores = 0x3f;
        set->flags = stage % 2 ? 3 : 1;
        for (int i = 0; i < kCores; i++) m.at<g::Worker>(worker(i))->context.flags = 0;
        *m.at<uint64_t>(kLaneWord) = 1ull << 16;   // (lane 2: the owner's job)
        w.stages.register_workers(owner, kSet, 1);
        g::Scheduler* s = m.at<g::Scheduler>(kSched);
        CHECK(s->context_count == 6 && m.at<GuestAddr>(kList)[0] == kSet + offsetof(g::WorkerSet, own_context));
        CHECK(set->registered_mask == 0x3b && set->registered_count == 6 && set->own_context.lane == 2);
        CHECK(((s->flags & 4) != 0) == (stage % 2 == 0));
        w.stages.wait(owner, kSet);
        tls_thread = nullptr;
        w.check_ran(want);
        CHECK(m.at<g::Worker>(worker(2))->context.flags & 2);
        CHECK(*m.at<uint64_t>(kLaneWord) == 1ull << 16);
    }
    w.check_clean();
    CHECK(w.stages.stats().owner_dispatches == 8 && w.stages.stats().own_registered == 8 && w.stages.stats().lane_drift == 0);
}

// The frame's last stage: registered without the message, then each core's thread calls the last stage at once (once a
// core, as the job 0x83b001c0 does); each starts its core's worker (or, when the owner takes part, the thread of the
// owner's core runs the own context), and the one that brings the count to zero clears the manager's state byte. Every
// thread gives up the set's own lane around its wait: with the owner on core 4 taking part, lane 4 starts at 6 so that
// the five waits around it never reach the floor, and is back at 6 after.
void the_last_stage() {
    Model m;
    World w(m, true);
    uint64_t workers = 0, own = 0;
    for (int stage = 0; stage < 16; stage++) {
        Thread* owner = &m.threads[6];
        owner->core = 4;
        tls_thread = owner;
        const std::vector<int> want = w.make_stage(owner, 200);
        g::WorkerSet* set = m.at<g::WorkerSet>(kSet);
        set->stage_cores = stage % 3 == 2 ? 0x33 : 0x3f;
        set->flags = stage % 2;
        set->own_context.lane = 0;
        for (int i = 0; i < kCores; i++) m.at<g::Worker>(worker(i))->context.flags = 0;
        const uint64_t lanes = (set->flags & 1) ? 6ull << 32 : 0;
        *m.at<uint64_t>(kLaneWord) = lanes;
        m.at<g::ActorManager>(kManager)->state = 0xabcd02;
        w.stages.register_workers(owner, kSet, 0);
        tls_thread = nullptr;
        CHECK(set->own_context.lane == ((set->flags & 1) ? 4 : 0));
        const uint32_t registered = set->registered_count;
        workers += (uint64_t)popcount(set->registered_mask);
        own += set->flags & 1;
        std::vector<std::thread> threads;
        for (int i = 0; i < kCores; i++)
            threads.emplace_back([&, i] {
                Thread* t = &m.threads[9 + i];
                t->core = (uint32_t)i;
                tls_thread = t;
                w.stages.last_stage(t, kManager);
                tls_thread = nullptr;
            });
        for (std::thread& t : threads) t.join();
        w.check_ran(want);
        CHECK(set->registered_count == 0 && m.at<g::ActorManager>(kManager)->state == 0xabcd00);
        CHECK(registered == (uint32_t)popcount(set->registered_mask) + (set->flags & 1));
        CHECK(*m.at<uint64_t>(kLaneWord) == lanes);
    }
    w.check_clean();
    const bridge::Stages::Stats& s = w.stages.stats();
    CHECK(s.last_ends == 16 && s.last_workers == workers && s.last_own == own);
    CHECK(s.last_calls == 16 * 6 && s.last_none == 16 * 6 - workers - own);
}

// No stage: the wait returns at once and the last stage clears the state byte. No stage cores: only the own context is
// registered. A worker with another message function gets it called; a worker not running gets no message.
void the_edges() {
    {
        Model m;
        World w(m, true);
        Thread* t = &m.threads[7];
        tls_thread = t;
        m.at<g::ActorManager>(kManager)->state = 0x102;
        w.stages.wait(t, kSet);
        w.stages.last_stage(t, kManager);
        CHECK(m.at<g::ActorManager>(kManager)->state == 0x100);
        CHECK(w.stages.stats().waits_empty == 1 && w.stages.stats().last_empty == 1);
        g::WorkerSet* set = m.at<g::WorkerSet>(kSet);
        set->scheduler = kSched;
        set->stage_cores = 0;
        w.stages.register_workers(t, kSet, 1);
        CHECK(set->registered_count == 1 && set->registered_mask == 0 && m.at<g::Scheduler>(kSched)->context_count == 1);
        CHECK(w.stages.stats().started == 0 && w.stages.stats().reg_diff == 0);
        tls_thread = nullptr;
    }
    {
        Model m;
        World w(m, false);
        Thread* t = &m.threads[7];
        tls_thread = t;
        const std::vector<int> want = w.make_stage(t, 50);
        m.at<g::Worker>(worker(5))->vtable = kOtherVtable;
        g::WorkerSet* set = m.at<g::WorkerSet>(kSet);
        set->stage_cores = 0x30;
        w.stages.register_workers(t, kSet, 1);
        w.stages.wait(t, kSet);
        w.check_ran(want);
        CHECK(m.virtual_posts == 1 && w.stages.stats().started_virtual == 1);
        m.at<g::Worker>(worker(4))->state = 3;
        w.make_stage(t, 50);
        set->stage_cores = 0x10;
        w.stages.register_workers(t, kSet, 1);
        CHECK(w.stages.stats().not_queued == 1);
        tls_thread = nullptr;
    }
}

}   // namespace

int main() {
    stages_in_a_row();
    the_owner_takes_part();
    the_last_stage();
    the_edges();
    std::puts("native engine: stage protocol on a model of the game's worker sets: ok");
    return 0;
}
