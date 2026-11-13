// HeyListen native engine: the job system's boundary with the gameplay code (hl/bridge/JobBridge.h).
#include "hl/bridge/JobBridge.h"

#include "hl/bridge/Dispatcher.h"

namespace hl::bridge {

namespace {
// (the supported build's addresses; docs/native-engine/jobs-boundary.md)
constexpr GuestEntry kEntries[] = {
    {0x80b5e9e0, EntryRole::Setup, false, "the job system made: its schedulers and worker sets"},
    {0x80b5ef60, EntryRole::Setup, false, "a scheduler made (its list of contexts, its queues)"},
    {0x8069b020, EntryRole::Setup, false, "a worker set made: one worker thread a core"},
    {0x806af9d0, EntryRole::Stage, false, "a stage begun on a set of cores"},
    {0x806afae0, EntryRole::Stage, false, "a stage begun on a set of cores (second form)"},
    {0x806afe70, EntryRole::Stage, false, "a stage begun on a set of cores (third form)"},
    {0x806b0260, EntryRole::Stage, false, "the stage's workers registered with its scheduler and sent their message"},
    {0x806afc40, EntryRole::Stage, false, "the stage waited for, its owner running jobs meanwhile"},
    {0x83acfb90, EntryRole::Stage, false, "the frame's last stage: each owner thread starts the executor of its core and waits"},
    {kWorkerMessage, EntryRole::Worker, true, "a worker's message: run the dispatcher, then signal the end"},
    {0x80df2680, EntryRole::Worker, false, "a job thread's loop (message to job)"},
    {0x80df25c0, EntryRole::Worker, false, "a job thread's loop (inner)"},
    {0x834a2e50, EntryRole::Worker, false, "a job thread's loop (outer)"},
    {kDispatcher, EntryRole::Dispatch, false, "the dispatcher: takes jobs from a scheduler's queues and runs them"},
    {0x8004fa40, EntryRole::Dispatch, false, "a job pushed to a scheduler"},
    {0x8007fe60, EntryRole::Group, false, "a job group run and waited for (the main thread's long wait of the frame)"},
    {0x80706be0, EntryRole::Frame, false, "the frame's time step read"},
    {0x8022e220, EntryRole::Consumer, false, "an actor link followed: reads the identity table (one of hundreds of inlined readers)"},
};
}   // namespace

std::span<const GuestEntry> job_entries() { return kEntries; }

// One stage's work for one worker, while its message is being answered.
struct JobBridge::Stage {
    JobBridge* bridge;
    GuestThread worker_thread;
    GuestAddr scheduler, context;
};

JobBridge::JobBridge(GuestRuntime& runtime, jobs::JobSystem& jobs, Mode mode, Dispatcher* dispatcher)
    : runtime_(runtime), jobs_(jobs), mode_(mode), dispatcher_(dispatcher) {}

void JobBridge::dispatch(GuestThread as, GuestAddr scheduler, GuestAddr context) {
    if (dispatcher_) dispatcher_->dispatch(as, scheduler, context);
    else runtime_.call(as, kDispatcher, scheduler, context);
}

void JobBridge::run_stage(void* arg, const jobs::JobContext&) {
    const Stage* s = static_cast<const Stage*>(arg);
    GuestRuntime& rt = s->bridge->runtime_;
    if (rt.current() != s->worker_thread) s->bridge->stats_.moved.fetch_add(1, std::memory_order_relaxed);
    s->bridge->dispatch(s->worker_thread, s->scheduler, s->context);
}

void JobBridge::on_worker_message(GuestAddr worker_at, uint64_t message) {
    stats_.messages.fetch_add(1, std::memory_order_relaxed);
    if (message != 1) return;
    const guest::Worker* worker = static_cast<const guest::Worker*>(runtime_.ptr(worker_at));
    const GuestAddr scheduler = worker->scheduler;
    if (!scheduler) return;
    const GuestThread self = runtime_.current();
    const GuestAddr context = worker_at + offsetof(guest::Worker, context);
    const uint32_t core = worker->core;
    if (mode_ == Mode::Off || core >= (uint32_t)jobs::kLaneCount) {
        dispatch(self, scheduler, context);
    } else {
        // The stage's work for this worker is one guest job under the worker's lane: its core. The worker's thread stands
        // still inside wait() until the job is done, so whichever host thread runs the job is the only one running as this
        // worker.
        Stage stage{this, self, scheduler, context};
        jobs::JobDesc desc;
        desc.fn = &JobBridge::run_stage;
        desc.arg = &stage;
        desc.cls = jobs::JobClass::Guest;
        desc.priority = jobs::Priority::High;
        desc.lanes = (uint8_t)(1u << core);
        desc.name = "stage";
        stats_.stages.fetch_add(1, std::memory_order_relaxed);
        const jobs::JobHandle job = jobs_.submit(desc);
        jobs_.wait(job, mode_ == Mode::Pool ? jobs::Help::None : jobs::Help::Awaited);
    }
    runtime_.call(self, kEventSignal, worker_at + offsetof(guest::Worker, done_event), 0);
}

Readiness ready(const JobBridge* bridge) {
    unsigned native = 0;
    for (const GuestEntry& e : kEntries) native += e.native;
    const bool on = bridge && bridge->mode() != Mode::Off;
    return {on, native, (unsigned)(sizeof kEntries / sizeof kEntries[0]),
            "the game's jobs are not native jobs one by one: the native dispatcher (HL_DISPATCH=1) runs them from the game's queues\n"
            "the stage owner's wait and nested waits run the native dispatcher only with HL_DISPATCH_ALL=1 (its entry gate)\n"
            "the stages' begin, registration and wait, and the worker sets' making, are the game's own functions\n"
            "the module system's job group (the main thread's long wait) does not go through the native job system\n"
            "the identity table and the lane status word are written by the game's functions, not by the native side\n"
            "the main loop is the game's own"};
}

}   // namespace hl::bridge
