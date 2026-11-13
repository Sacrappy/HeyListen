// HeyListen native engine: the job system, the lanes and the main loop, run with no game (tests/test_native_engine.py).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "hl/app/MainLoop.h"
#include "hl/bridge/JobBridge.h"
#include "hl/jobs/JobSystem.h"

using namespace hl;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

// What the bridge will do for real: here it only watches that a lane is never held twice and never more than six at once.
struct WatchingHost final : jobs::GuestLaneHost {
    std::atomic<int> holders[jobs::kLaneCount]{};
    std::atomic<int> inside{0}, most{0}, entered{0};
    std::atomic<bool> clash{false};
    void enter(int lane, uint32_t) override {
        if (lane < 0 || lane >= jobs::kLaneCount || holders[lane].fetch_add(1) != 0) clash = true;
        const int n = inside.fetch_add(1) + 1;
        int m = most.load();
        while (n > m && !most.compare_exchange_weak(m, n)) {}
        entered++;
    }
    void leave(int lane, uint32_t) override {
        if (holders[lane].fetch_sub(1) != 1) clash = true;
        inside--;
    }
};

void every_job_runs_once() {
    jobs::JobSystem js({.workers = 4});
    constexpr int kJobs = 5000;
    std::vector<std::atomic<int>> ran(kJobs);
    std::vector<jobs::JobHandle> handles;
    for (int i = 0; i < kJobs; i++) {
        jobs::JobDesc d;
        d.fn = [](void* arg, const jobs::JobContext& ctx) {
            CHECK(ctx.lane == -1);
            static_cast<std::atomic<int>*>(arg)->fetch_add(1);
        };
        d.arg = &ran[(size_t)i];
        handles.push_back(js.submit(d));
    }
    js.wait_all(handles);
    for (int i = 0; i < kJobs; i++) CHECK(ran[(size_t)i].load() == 1);
    CHECK(js.stats().native_jobs == (uint64_t)kJobs);
}

void dependencies_hold() {
    jobs::JobSystem js({.workers = 6});
    // a chain of 200, then a diamond: every job writes its turn; a job never runs before what it waits for
    std::atomic<int> turn{0};
    struct Step { std::atomic<int>* turn; int at = -1; };
    std::vector<Step> steps(203);
    std::vector<jobs::JobHandle> h(203);
    for (size_t i = 0; i < steps.size(); i++) steps[i].turn = &turn;
    auto fn = [](void* arg, const jobs::JobContext&) {
        Step* s = static_cast<Step*>(arg);
        s->at = s->turn->fetch_add(1);
    };
    for (size_t i = 0; i < 200; i++) {
        jobs::JobDesc d;
        d.fn = fn, d.arg = &steps[i];
        h[i] = i ? js.submit(d, std::span(&h[i - 1], 1)) : js.submit(d);
    }
    jobs::JobDesc d;
    d.fn = fn;
    d.arg = &steps[200], h[200] = js.submit(d, std::span(&h[199], 1));   // left
    d.arg = &steps[201], h[201] = js.submit(d, std::span(&h[199], 1));   // right
    const jobs::JobHandle both[] = {h[200], h[201]};
    d.arg = &steps[202], h[202] = js.submit(d, both);                     // join
    js.wait(h[202]);
    for (size_t i = 1; i < 200; i++) CHECK(steps[i].at > steps[i - 1].at);
    CHECK(steps[200].at > steps[199].at && steps[201].at > steps[199].at);
    CHECK(steps[202].at > steps[200].at && steps[202].at > steps[201].at);
    CHECK(js.done(h[0]) && js.done(h[202]));
}

void parallel_for_covers_the_range() {
    jobs::JobSystem js({.workers = 5});
    std::vector<int> hit(100000, 0);
    js.parallel_for(hit.size(), 777, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; i++) hit[i]++;
    });
    for (const int v : hit) CHECK(v == 1);
}

void guest_jobs_keep_to_their_lanes() {
    WatchingHost host;
    jobs::JobSystem js({.workers = 12, .lane_host = &host});   // more workers than lanes
    constexpr int kJobs = 3000;
    struct G { uint8_t lanes; std::atomic<int> ran{0}; std::atomic<bool> wrong{false}; };
    std::vector<G> g(kJobs);
    std::vector<jobs::JobHandle> handles;
    std::atomic<int> native_ran{0};
    for (int i = 0; i < kJobs; i++) {
        g[(size_t)i].lanes = (uint8_t)(1u << (i % 6) | ((i % 7) == 0 ? 0x3f : 0) | ((i % 5) == 0 ? 1u << ((i + 3) % 6) : 0));
        jobs::JobDesc d;
        d.cls = jobs::JobClass::Guest;
        d.lanes = g[(size_t)i].lanes;
        d.arg = &g[(size_t)i];
        d.fn = [](void* arg, const jobs::JobContext& ctx) {
            G* s = static_cast<G*>(arg);
            if (ctx.lane < 0 || !(s->lanes >> ctx.lane & 1)) s->wrong = true;
            volatile unsigned spin = 0;
            for (unsigned k = 0; k < 2000; k++) spin = spin + k;
            s->ran++;
        };
        handles.push_back(js.submit(d));
        if (i % 3 == 0) {   // native jobs between them: no lane
            jobs::JobDesc n;
            n.arg = &native_ran;
            n.fn = [](void* arg, const jobs::JobContext& ctx) {
                CHECK(ctx.lane == -1);
                static_cast<std::atomic<int>*>(arg)->fetch_add(1);
            };
            handles.push_back(js.submit(n));
        }
    }
    js.wait_all(handles);
    for (const G& s : g) CHECK(s.ran.load() == 1 && !s.wrong.load());
    CHECK(native_ran.load() == kJobs / 3);
    CHECK(!host.clash.load());
    CHECK(host.most.load() <= jobs::kLaneCount && host.inside.load() == 0);
    CHECK(js.stats().most_lanes_at_once <= (unsigned)jobs::kLaneCount);
    CHECK(js.stats().guest_jobs == (uint64_t)kJobs);
}

void a_guest_job_waits_for_its_children_under_its_own_lane() {
    WatchingHost host;
    jobs::JobSystem js({.workers = 3, .lane_host = &host});
    struct Parent { jobs::JobSystem* js; std::atomic<int> children{0}; int lane = -1; std::atomic<bool> wrong{false}; };
    Parent parents[6];
    std::vector<jobs::JobHandle> handles;
    for (int p = 0; p < 6; p++) {
        parents[p].js = &js;
        jobs::JobDesc d;
        d.cls = jobs::JobClass::Guest;
        d.lanes = (uint8_t)(1u << p);
        d.arg = &parents[p];
        d.fn = [](void* arg, const jobs::JobContext& ctx) {
            Parent* self = static_cast<Parent*>(arg);
            self->lane = ctx.lane;
            std::vector<jobs::JobHandle> kids;
            for (int k = 0; k < 40; k++) {   // children that may run under any lane, the parent's own included
                jobs::JobDesc c;
                c.cls = jobs::JobClass::Guest;
                c.arg = self;
                c.fn = [](void* a, const jobs::JobContext& cc) {
                    Parent* s = static_cast<Parent*>(a);
                    if (cc.lane < 0) s->wrong = true;
                    s->children++;
                };
                kids.push_back(self->js->submit(c));
            }
            self->js->wait_all(kids);
        };
        handles.push_back(js.submit(d));
    }
    js.wait_all(handles);
    for (const Parent& p : parents) CHECK(p.children.load() == 40 && !p.wrong.load() && p.lane >= 0);
    CHECK(!host.clash.load() && host.inside.load() == 0);
}

void the_main_loop_runs_its_phases_in_order() {
    jobs::JobSystem js({.workers = 2});
    app::MainLoop loop(js, {.target_hz = 500});
    struct Log { std::vector<int> order; double steps = 0; };
    Log log;
    for (int phase = 0; phase < 3; phase++) {
        static int ids[3] = {0, 1, 2};
        struct User { Log* log; int id; };
        static User users[3];
        users[phase] = {&log, ids[phase]};
        loop.add_phase({"phase", [](void* user, const app::FrameTime& t, jobs::JobSystem& jobs) {
                            User* u = static_cast<User*>(user);
                            u->log->order.push_back(u->id);
                            if (u->id == 0) u->log->steps += t.step;
                            std::atomic<int> n{0};
                            jobs.parallel_for(64, 8, [&](size_t b, size_t e) { n += (int)(e - b); });
                            CHECK(n.load() == 64);
                        }, &users[phase]});
    }
    for (int i = 0; i < 20; i++) loop.step();
    CHECK(loop.stats().frames == 20 && log.order.size() == 60);
    for (size_t i = 0; i < log.order.size(); i++) CHECK(log.order[i] == (int)(i % 3));
    CHECK(log.steps > 0 && loop.stats().mean_ms >= 1.5);   // (capped at 500 Hz: no frame under 2 ms, with slack)
}

void the_bridge_names_each_entry_once() {
    std::set<uint64_t> seen;
    unsigned native = 0;
    for (const bridge::GuestEntry& e : bridge::job_entries()) {
        CHECK(e.address >= 0x80000000ull && e.address < 0x86000000ull && e.what && *e.what);
        CHECK(seen.insert(e.address).second);
        native += e.native;
    }
    CHECK(seen.size() >= 15);
    const bridge::Readiness none = bridge::ready(nullptr);
    CHECK(!none.ready && none.native == native && none.total == seen.size() && none.missing && *none.missing);
}

// A stand-in for the layer that runs guest code: a small guest memory, guest threads that are only names, and a record of
// every guest call (who made it, as whom, on which host thread). It checks what the bridge promises that layer: a guest
// thread never runs in two places at once.
struct FakeGuest final : bridge::GuestRuntime, jobs::ThreadHost {
    static constexpr uint64_t kBase = 0x10000000;
    std::vector<uint8_t> memory = std::vector<uint8_t>(1 << 16);
    struct Call { bridge::GuestThread as; uint64_t fn, a0, a1; std::thread::id host; };
    std::mutex mu;
    std::vector<Call> calls;
    std::set<bridge::GuestThread> running;
    std::atomic<bool> clash{false};
    std::vector<std::thread> threads;
    static inline thread_local bridge::GuestThread tls_current = nullptr;

    void* ptr(bridge::GuestAddr at) const override { return const_cast<uint8_t*>(memory.data()) + (at - kBase); }
    bridge::GuestThread current() const override { return tls_current; }
    uint64_t call_with(bridge::GuestThread as, bridge::GuestAddr fn, const uint64_t* args, unsigned nargs, const void*,
                       unsigned) override {
        return call(as, fn, nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0);
    }
    void log(const char*) override {}
    uint64_t call(bridge::GuestThread as, bridge::GuestAddr fn, uint64_t a0, uint64_t a1) override {
        const bridge::GuestThread before = tls_current;
        if (as != before) {
            std::lock_guard lock(mu);
            if (!running.insert(as).second) clash = true;
        }
        tls_current = as;
        if (fn == bridge::kDispatcher) std::this_thread::sleep_for(std::chrono::milliseconds(2));   // (a stage's work)
        {
            std::lock_guard lock(mu);
            calls.push_back({as, fn, a0, a1, std::this_thread::get_id()});
            if (as != before) running.erase(as);
        }
        tls_current = before;
        return 0;
    }
    void* start(void (*entry)(void*), void* arg, unsigned) override {
        threads.emplace_back(entry, arg);
        return &threads.back();
    }
    void join(void* handle) override { static_cast<std::thread*>(handle)->join(); }

    bridge::guest::Worker* worker(int i) { return static_cast<bridge::guest::Worker*>(ptr(worker_at(i))); }
    static uint64_t worker_at(int i) { return kBase + 0x1000 * (uint64_t)(i + 1); }
};

// Six workers get their stage message at once, each on its own thread, as in the game.
void a_stage_through(bridge::Mode mode) {
    FakeGuest guest;
    guest.threads.reserve(16);
    jobs::JobSystem js({.workers = 6, .thread_host = &guest, .guest_on_workers = mode == bridge::Mode::Pool});
    bridge::JobBridge br(guest, js, mode);
    constexpr uint64_t kScheduler = FakeGuest::kBase + 0x9000;
    for (int i = 0; i < 6; i++) {
        guest.worker(i)->core = (uint32_t)i;
        guest.worker(i)->scheduler = kScheduler;
    }
    static int names[6];
    std::vector<std::thread> workers;
    std::thread::id ids[6];
    for (int round = 0; round < 3; round++) {
        workers.clear();
        for (int i = 0; i < 6; i++)
            workers.emplace_back([&, i] {
                FakeGuest::tls_current = &names[i];
                ids[i] = std::this_thread::get_id();
                br.on_worker_message(FakeGuest::worker_at(i), 1);
                br.on_worker_message(FakeGuest::worker_at(i), 2);   // (another message: nothing to run)
            });
        for (std::thread& t : workers) t.join();
        CHECK(!guest.clash);
        CHECK(guest.calls.size() == 12);
        for (int i = 0; i < 6; i++) {
            int dispatched = -1, signalled = -1;
            for (size_t k = 0; k < guest.calls.size(); k++) {
                const FakeGuest::Call& c = guest.calls[k];
                if (c.as != &names[i]) continue;
                if (c.fn == bridge::kDispatcher) {
                    CHECK(dispatched < 0 && c.a0 == kScheduler);
                    CHECK(c.a1 == FakeGuest::worker_at(i) + offsetof(bridge::guest::Worker, context));
                    // (through: on the worker's own thread; pool: on a worker of the pool, never the worker's thread)
                    CHECK(mode == bridge::Mode::Pool ? c.host != ids[i] : c.host == ids[i]);
                    dispatched = (int)k;
                } else {
                    CHECK(signalled < 0 && c.fn == bridge::kEventSignal && c.host == ids[i]);
                    CHECK(c.a0 == FakeGuest::worker_at(i) + offsetof(bridge::guest::Worker, done_event));
                    signalled = (int)k;
                }
            }
            CHECK(dispatched >= 0 && signalled > dispatched);   // (the end is signalled after the stage's work, not before)
        }
        guest.calls.clear();
    }
    const bridge::Readiness r = bridge::ready(&br);
    CHECK(r.ready == (mode != bridge::Mode::Off) && r.native >= 1);
    CHECK(br.stats().messages == 36);
    CHECK(br.stats().stages == (mode == bridge::Mode::Off ? 0u : 18u));
    CHECK(br.stats().moved == (mode == bridge::Mode::Pool ? 18u : 0u));
    CHECK(js.stats().guest_on_workers == (mode == bridge::Mode::Pool ? 18u : 0u));
    if (mode != bridge::Mode::Off) CHECK(js.stats().most_lanes_at_once >= 2);   // (the six stages overlap: 2 ms each)
    // a worker with no scheduler set: no work, no signal
    guest.worker(0)->scheduler = 0;
    FakeGuest::tls_current = &names[0];
    br.on_worker_message(FakeGuest::worker_at(0), 1);
    FakeGuest::tls_current = nullptr;
    CHECK(guest.calls.empty());
}

void a_waiting_thread_takes_only_what_it_was_told() {
    jobs::JobSystem js({.workers = 1, .guest_on_workers = false});
    // the pool's only worker is kept busy; a guest job and a native job wait
    std::atomic<bool> release{false}, guest_ran{false}, native_ran{false};
    jobs::JobDesc hold;
    hold.fn = [](void* arg, const jobs::JobContext&) { while (!static_cast<std::atomic<bool>*>(arg)->load()) std::this_thread::yield(); };
    hold.arg = &release;
    const jobs::JobHandle held = js.submit(hold);
    jobs::JobDesc g;
    g.fn = [](void* arg, const jobs::JobContext& ctx) { CHECK(ctx.lane == 3 && ctx.worker == jobs::kExternalWorker); static_cast<std::atomic<bool>*>(arg)->store(true); };
    g.arg = &guest_ran;
    g.cls = jobs::JobClass::Guest;
    g.lanes = 1u << 3;
    const jobs::JobHandle gh = js.submit(g);
    jobs::JobDesc n;
    n.fn = [](void* arg, const jobs::JobContext&) { static_cast<std::atomic<bool>*>(arg)->store(true); };
    n.arg = &native_ran;
    const jobs::JobHandle nh = js.submit(n);
    js.wait(gh, jobs::Help::Awaited);   // (runs the guest job itself: the pool does not take guest jobs)
    CHECK(guest_ran && !native_ran);     // (and nothing else)
    release = true;
    js.wait(held);
    js.wait(nh);
    CHECK(native_ran && js.stats().guest_on_workers == 0);
}

}   // namespace

int main() {
    every_job_runs_once();
    dependencies_hold();
    parallel_for_covers_the_range();
    guest_jobs_keep_to_their_lanes();
    a_guest_job_waits_for_its_children_under_its_own_lane();
    the_main_loop_runs_its_phases_in_order();
    the_bridge_names_each_entry_once();
    a_waiting_thread_takes_only_what_it_was_told();
    a_stage_through(bridge::Mode::Off);
    a_stage_through(bridge::Mode::Through);
    a_stage_through(bridge::Mode::Pool);
    std::puts("native engine: jobs, lanes, main loop, job bridge: ok");
    return 0;
}
