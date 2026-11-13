// HeyListen native engine: the C surface (hl/c/hl_bridge.h) over the job system and its bridge.
#include "hl/c/hl_bridge.h"

#include <cstdio>

#include "host_runtime.h"

#include "hl/bridge/Dispatcher.h"
#include "hl/bridge/JobBridge.h"
#include "hl/jobs/JobSystem.h"

namespace {

// (the shared adapter, plus the pool's threads: made by the OS layer, with the host stack guest code needs)
struct HostRuntime final : hl::c::HostRuntime, hl::jobs::ThreadHost {
    void* start(void (*entry)(void*), void* arg, unsigned index) override { return host.thread_start(host.user, entry, arg, index); }
    void join(void* handle) override { host.thread_join(host.user, handle); }
};

// (made once and never destroyed: the process ends with the pool's threads asleep, and joining them at exit would wait
// on threads the exit has already stopped)
HostRuntime* g_runtime;
hl::jobs::JobSystem* g_jobs;
hl::bridge::Dispatcher* g_dispatcher;
hl::bridge::JobBridge* g_bridge;

}   // namespace

extern "C" int hl_bridge_start(const hl_guest_host* host, int mode, unsigned workers, int dispatch) {
    if (g_bridge) return g_bridge->mode() != hl::bridge::Mode::Off;
    if (!hl::c::complete(host) || !host->thread_start || !host->thread_join) return 0;
    const hl::bridge::Mode m = mode == HL_JOBS_POOL      ? hl::bridge::Mode::Pool
                               : mode == HL_JOBS_THROUGH ? hl::bridge::Mode::Through
                                                         : hl::bridge::Mode::Off;
    g_runtime = new HostRuntime;
    g_runtime->host = *host;
    hl::jobs::JobSystem::Config config;
    config.workers = workers ? workers : (unsigned)hl::jobs::kLaneCount;
    config.thread_host = g_runtime;
    config.guest_on_workers = m == hl::bridge::Mode::Pool;
    g_jobs = new hl::jobs::JobSystem(config);
    if (dispatch)
        g_dispatcher = new hl::bridge::Dispatcher(
            *g_runtime, {.native = (dispatch & HL_DISPATCH_NATIVE) != 0, .check = (dispatch & HL_DISPATCH_CHECK) != 0});
    g_bridge = new hl::bridge::JobBridge(*g_runtime, *g_jobs, m, g_dispatcher);
    return m != hl::bridge::Mode::Off;
}

extern "C" uint64_t hl_bridge_worker_message_address(void) { return hl::bridge::kWorkerMessage; }

extern "C" void hl_bridge_worker_message(uint64_t worker, uint64_t message) { g_bridge->on_worker_message(worker, message); }

extern "C" int hl_bridge_report(char* out, size_t cap) {
    if (!g_bridge || !cap) return 0;
    const hl::bridge::JobBridge::Stats& b = g_bridge->stats();
    const hl::jobs::JobSystem::Stats j = g_jobs->stats();
    const hl::bridge::Readiness r = hl::bridge::ready(g_bridge);
    int n = std::snprintf(out, cap,
                                "mode %d, %u workers, %u of %u entry points native; messages %llu, stages %llu (moved to another "
                                "thread %llu); guest jobs %llu (on pool workers %llu), most lanes at once %u",
                                (int)g_bridge->mode(), g_jobs->worker_count(), r.native, r.total,
                                (unsigned long long)b.messages.load(), (unsigned long long)b.stages.load(),
                                (unsigned long long)b.moved.load(), (unsigned long long)j.guest_jobs,
                                (unsigned long long)j.guest_on_workers, j.most_lanes_at_once);
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    if (!g_dispatcher || (size_t)n + 2 >= cap) return n;
    out[n++] = ';';
    out[n++] = ' ';
    return n + g_dispatcher->report(out + n, cap - (size_t)n);
}
