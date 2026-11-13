// HeyListen native engine: the stage protocol, natively (hl/bridge/Stages.h). Each step follows docs/native-engine/
// jobs-boundary.md, "The stages' protocol"; the guest instruction addresses in the comments are where the reading of that
// step is, in the registration 0x806b0260, the wait 0x806afc40 or the last stage 0x83acfb90.
//
// Where another thread can see the words between two writes: a stage's workers read their context's words and their
// scheduler only after their message (the registration writes them between two full barriers, then sends, as the guest
// does), and the scheduler's list is read by contexts that run that scheduler, which none does before the registration
// ends. The writes are still made in the guest's order, so that nothing depends on that reasoning.
#include "hl/bridge/Stages.h"

#include <cstdio>

#include "hl/bridge/Dispatcher.h"

namespace hl::bridge {

namespace {

// The guest's plain loads and stores are relaxed; its read-modify-writes (ldset, ldclr, ldadd, cas) one sequentially
// consistent atomic operation each; its `dmb ish` a full fence.
template <class T>
T ld(const T* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
template <class T>
void st(T* p, T v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

constexpr uint32_t kSentMessage = 1;   // a context's flags
constexpr uint32_t kNotToRun = 2;
constexpr uint32_t kOwnerTakesPart = 1;   // a worker set's flags
constexpr uint32_t kKeepLane = 2;
constexpr uint32_t kOwnerPolls = 4;    // the scheduler's flags
constexpr uint32_t kStageOver = 2;
constexpr GuestAddr kOwn = offsetof(guest::WorkerSet, own_context);
constexpr GuestAddr kContext = offsetof(guest::Worker, context);
constexpr int kSnapWorkers = 32;

// One more or one fewer in the low four bits of `lane`'s byte of a lane status word, staying within 0..15, by
// compare-and-swap of the whole word (0x806afce4, 0x806afd14; 0x83acfcb0, 0x83acfd04).
void lane_step(uint64_t* word, uint32_t lane, bool up) {
    const unsigned shift = (lane << 3) & 63;
    const uint64_t mask = 0xfull << shift, unit = 1ull << shift;
    uint64_t old = ld(word);
    for (;;) {
        const uint64_t part = old & mask;
        const uint64_t next = up ? (part == mask ? part : part + unit) : (part == 0 ? 0 : part - unit);
        if (__atomic_compare_exchange_n(word, &old, next | (old & ~mask), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    }
}

// Set while this host thread calls the translated registration on purpose (the check): its entry gate lets it through.
thread_local bool t_bypass = false;

}   // namespace

// The registration's writes, read back: the scheduler's list and flags, the set's mask and count and own context, the
// workers' context words and scheduler.
struct Stages::Snapshot {
    int32_t contexts = 0;
    GuestAddr list[8] = {};
    uint32_t scheduler_flags = 0, mask = 0, count = 0, own_flags = 0;
    int32_t own_lane = 0;
    uint64_t own_state = 0;
    uint64_t state[kSnapWorkers] = {};
    uint32_t flags[kSnapWorkers] = {};
    GuestAddr scheduler[kSnapWorkers] = {};

    bool operator==(const Snapshot&) const = default;
};

Stages::Stages(GuestRuntime& runtime, Dispatcher* dispatcher, Options options)
    : runtime_(runtime), dispatcher_(dispatcher), options_(options) {}

uint64_t Stages::call(GuestThread as, GuestAddr fn, uint64_t a0, uint64_t a1, uint64_t a2) {
    const uint64_t args[3] = {a0, a1, a2};
    return runtime_.call_with(as, fn, args, 3, nullptr, 0);
}

void Stages::violation(const char* what, GuestAddr where) {
    if (stats_.violations_logged.fetch_add(1, std::memory_order_relaxed) >= 20) return;
    char line[200];
    std::snprintf(line, sizeof line, "hl stages: CHECK: %s (%#llx)", what, (unsigned long long)where);
    runtime_.log(line);
}

// The calling thread's core: the thread-local value holding the core plus one, or the OS's current core when it is 0
// (0x806b0298; the registration reads it twice, the same value both times).
uint32_t Stages::core(GuestThread as) {
    const uint64_t v = call(as, kThreadLocal, ld(at<uint32_t>(kCoreKey)));
    return v ? (uint32_t)v - 1 : (uint32_t)call(as, kCurrentCore);
}

// The worker of index `index`: the set's array holds objects whose +8 is the worker.
GuestAddr Stages::worker_at(GuestAddr set, int32_t index) const {
    const GuestAddr list = ld(&at<guest::WorkerSet>(set)->workers);
    return ld(at<GuestAddr>(ld(at<GuestAddr>(list + (GuestAddr)(int64_t)index * 8)) + 8));
}

// A context at the end of the scheduler's list, unless the list is full (0x806b0328).
void Stages::append(GuestAddr s_at, GuestAddr context) {
    guest::Scheduler* s = at<guest::Scheduler>(s_at);
    const int32_t n = ld(&s->context_count);
    if (n >= (int32_t)ld(&s->context_room)) return;
    st(at<GuestAddr>(ld(&s->contexts) + (GuestAddr)(int64_t)n * 8), context);
    st(&s->context_count, ld(&s->context_count) + 1);
}

// ---- the policy ----------------------------------------------------------------------------------------------------------

// A registered worker started: its message 1 (0x806b0418, 0x83acfc64). The workers' usual message function is the
// guest's inlined form here, read from the guest: the message is queued unless the worker is not running (state 3 or 4)
// or its last message was this one. Any other is called as the virtual function it is.
void Stages::start(GuestThread as, GuestAddr worker_at) {
    guest::Worker* w = at<guest::Worker>(worker_at);
    stats_.started.fetch_add(1, std::memory_order_relaxed);
    const GuestAddr fn = ld(at<GuestAddr>(ld(&w->vtable) + 0x10));
    if (fn != ld(at<GuestAddr>(kUsualMessageSlot))) {
        stats_.started_virtual.fetch_add(1, std::memory_order_relaxed);
        call(as, fn, worker_at, 1, 1);
        return;
    }
    if (ld(&w->state) - 3u < 2u || ld(&w->last_message) == 1) {
        stats_.not_queued.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    call(as, kQueueSend, worker_at + offsetof(guest::Worker, queue), 1);
}

// A thread waits for a worker's done event, giving up its lane meanwhile as a sleeping dispatcher does. `lane` >= 0: the
// owner's wait, on lane `lane` of the set's word (0x806afcd0: the word read once, nothing when it or the lane is 0).
// -1: the last stage, on the set's own lane, the word and the lane read again after the wait (0x83acfca0, 0x83acfcf4).
void Stages::await_done(GuestThread as, GuestAddr set, GuestAddr worker, int64_t lane) {
    guest::WorkerSet* w = at<guest::WorkerSet>(set);
    const GuestAddr done = worker + offsetof(guest::Worker, done_event);
    stats_.done_waits.fetch_add(1, std::memory_order_relaxed);
    if (lane >= 0) {
        const GuestAddr word = ld(&w->own_context.lane_status);
        const bool counted = word && lane;
        if (counted) lane_step(at<uint64_t>(word), (uint32_t)lane, false);
        call(as, kEventWait, done);
        if (counted) lane_step(at<uint64_t>(word), (uint32_t)lane, true);
    } else {
        GuestAddr word = ld(&w->own_context.lane_status);
        int32_t own = ld(&w->own_context.lane);
        if (word && own) lane_step(at<uint64_t>(word), (uint32_t)own, false);
        call(as, kEventWait, done);
        word = ld(&w->own_context.lane_status);
        own = ld(&w->own_context.lane);
        if (word && own) lane_step(at<uint64_t>(word), (uint32_t)own, true);
    }
    if (options_.check) check_done(worker);
}

// The owner's part of a stage, on the set's own context: the native dispatcher (or the translated one without it).
void Stages::dispatch(GuestThread as, GuestAddr scheduler, GuestAddr context) {
    stats_.owner_dispatches.fetch_add(1, std::memory_order_relaxed);
    if (dispatcher_) dispatcher_->dispatch(as, scheduler, context);
    else runtime_.call(as, kDispatcher, scheduler, context);
}

// ---- the registration ----------------------------------------------------------------------------------------------------

bool Stages::register_gate(GuestThread as, GuestAddr set, uint64_t send) {
    if (t_bypass) {
        t_bypass = false;
        return false;
    }
    register_workers(as, set, send);
    return true;
}

void Stages::register_workers(GuestThread as, GuestAddr set, uint64_t send) {
    stats_.registrations.fetch_add(1, std::memory_order_relaxed);
    if (!options_.check) {
        write_registration(as, set);
    } else {
        // The translated registration first, without the message, on the same set; then the native one over it. Both
        // start from the same words (each sets them all from the set, its scheduler, the workers' cores and the current
        // core), so what each leaves must be the same.
        Snapshot translated, native;
        const uint64_t lanes = lane_byte(as, set);
        t_bypass = true;
        runtime_.call(as, kStageRegister, set, 0);
        t_bypass = false;
        snapshot(set, translated);
        write_registration(as, set);
        snapshot(set, native);
        stats_.reg_checked.fetch_add(1, std::memory_order_relaxed);
        if (!(translated == native)) {
            stats_.reg_diff.fetch_add(1, std::memory_order_relaxed);
            violation("the registration's writes differ from the translated registration's", set);
        }
        note_registered(set, send, lanes);
    }
    if (send & 1) send_messages(as, set);
}

// (checking) The byte of the calling thread's lane in the set's lane status word, with the lane in its top byte; 0 when
// the word or the lane is 0 (the wait leaves it alone then).
uint64_t Stages::lane_byte(GuestThread as, GuestAddr set) {
    const GuestAddr word = ld(&at<guest::WorkerSet>(set)->own_context.lane_status);
    const uint32_t c = core(as);
    if (!word || !c) return 0;
    return (uint64_t)(c & 0xff) << 56 | ((ld(at<uint64_t>(word)) >> ((c << 3) & 63)) & 0xff);
}

void Stages::write_registration(GuestThread as, GuestAddr set) {
    guest::WorkerSet* w = at<guest::WorkerSet>(set);
    // (0x806b0278) the scheduler's list, the registered mask and count emptied
    st(&at<guest::Scheduler>(ld(&w->scheduler))->context_count, 0);
    st(&w->registered_mask, 0u);
    w->registered_count.store(0, std::memory_order_relaxed);
    const uint32_t c = core(as);
    const uint32_t cores = ld(&w->stage_cores);
    if (!cores || (ld(&w->flags) & kOwnerTakesPart)) {
        // (0x806b04a0) the set's own context: on the owner's core, awake, no message, its wake event cleared, listed
        st(&w->own_context.lane, (int32_t)c);
        st(&w->own_context.state, (uint64_t)0);
        st(&w->own_context.flags, 0u);
        call(as, kEventClear, set + kOwn + offsetof(guest::DispatchContext, wake_event));
        append(ld(&w->scheduler), set + kOwn);
        uint32_t* sflags = &at<guest::Scheduler>(ld(&w->scheduler))->flags;
        if ((ld(&w->flags) & (kOwnerTakesPart | kKeepLane)) == kOwnerTakesPart) __atomic_fetch_or(sflags, kOwnerPolls, __ATOMIC_SEQ_CST);
        else __atomic_fetch_and(sflags, ~kOwnerPolls, __ATOMIC_SEQ_CST);
        w->registered_count.fetch_add(1, std::memory_order_seq_cst);
        stats_.own_registered.fetch_add(1, std::memory_order_relaxed);
        if (!cores) return;
    } else {
        __atomic_fetch_and(&at<guest::Scheduler>(ld(&w->scheduler))->flags, ~kOwnerPolls, __ATOMIC_SEQ_CST);
    }
    // (0x806b02d8) the workers of the stage's cores, but the owner's own core's when it takes part
    const uint32_t own_core = 1u << (c & 31);
    const int32_t n = (int32_t)ld(&w->count);
    for (int32_t i = 0; i < n; i++) {
        const GuestAddr k = worker_at(set, i);
        const uint32_t core_mask = ld(&at<guest::Worker>(k)->core_mask);
        if ((ld(&w->flags) & kOwnerTakesPart) && core_mask == own_core) continue;
        if (!(ld(&w->stage_cores) & core_mask)) continue;
        append(ld(&w->scheduler), k + kContext);
        st(&w->registered_mask, ld(&w->registered_mask) | (1u << (i & 31)));
        w->registered_count.fetch_add(1, std::memory_order_seq_cst);
        stats_.workers_registered.fetch_add(1, std::memory_order_relaxed);
    }
    // (0x806b0374) each registered worker made ready, between two full barriers
    const int32_t m = (int32_t)ld(&w->count);
    for (int32_t i = 0; i < m; i++) {
        if (!((ld(&w->registered_mask) >> (i & 31)) & 1)) continue;
        const GuestAddr k = worker_at(set, i);
        guest::Worker* worker = at<guest::Worker>(k);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        st(&worker->context.state, (uint64_t)0);
        st(&worker->context.flags, 0u);
        call(as, kEventClear, k + kContext + offsetof(guest::DispatchContext, wake_event));
        call(as, kEventClear, k + offsetof(guest::Worker, done_event));
        st(&worker->scheduler, ld(&w->scheduler));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
}

// (0x806b03d0) each registered worker marked "sent its message", then sent it.
void Stages::send_messages(GuestThread as, GuestAddr set) {
    guest::WorkerSet* w = at<guest::WorkerSet>(set);
    const int32_t n = (int32_t)ld(&w->count);
    for (int32_t i = 0; i < n; i++) {
        if (!((ld(&w->registered_mask) >> (i & 31)) & 1)) continue;
        const GuestAddr k = worker_at(set, i);
        __atomic_fetch_or(&at<guest::Worker>(k)->context.flags, kSentMessage, __ATOMIC_SEQ_CST);
        if (options_.check && !note_started(set, i)) violation("a worker started twice in one stage", k);
        start(as, k);
    }
}

// ---- the wait ------------------------------------------------------------------------------------------------------------

void Stages::wait(GuestThread as, GuestAddr set) {
    guest::WorkerSet* w = at<guest::WorkerSet>(set);
    if (!ld(&w->scheduler)) {   // (0x806afc40: no stage)
        stats_.waits_empty.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    stats_.waits.fetch_add(1, std::memory_order_relaxed);
    const uint32_t c = core(as);
    if (ld(&w->flags) & kOwnerTakesPart) {
        // (0x806afdf8) the workers of the owner's core told not to run, the own context put on the owner's core, and the
        // stage run on it by this thread
        const uint32_t own_core = 1u << (c & 31);
        const int32_t n = (int32_t)ld(&w->count);
        for (int32_t i = 0; i < n; i++) {
            guest::Worker* k = at<guest::Worker>(worker_at(set, i));
            if (ld(&k->core_mask) == own_core) __atomic_fetch_or(&k->context.flags, kNotToRun, __ATOMIC_SEQ_CST);
        }
        if (!(ld(&w->flags) & kKeepLane) || (uint32_t)ld(&w->own_context.lane) != c) st(&w->own_context.lane, (int32_t)c);
        if (options_.check && !note_started(set, -1)) violation("the own context started twice in one stage", set);
        dispatch(as, ld(&w->scheduler), set + kOwn);
    }
    // (0x806afc8c) each registered worker's end waited for, in index order
    const int32_t n = (int32_t)ld(&w->count);
    for (int32_t i = 0; i < n; i++) {
        if (!((ld(&w->registered_mask) >> (i & 31)) & 1)) continue;
        await_done(as, set, worker_at(set, i), c);
    }
    if (options_.check) check_stage_end(set, false, lane_byte(as, set));
}

// ---- the last stage ------------------------------------------------------------------------------------------------------

void Stages::last_stage(GuestThread as, GuestAddr manager) {
    stats_.last_calls.fetch_add(1, std::memory_order_relaxed);
    guest::ActorManager* m = at<guest::ActorManager>(manager);
    const GuestAddr set = ld(&at<guest::StageHolder>(ld(&m->holder))->set);
    guest::WorkerSet* w = at<guest::WorkerSet>(set);
    if (!ld(&w->scheduler)) {   // (0x83acfbb0: no stage, the state byte cleared)
        stats_.last_empty.fetch_add(1, std::memory_order_relaxed);
        end_last_stage(manager);
        return;
    }
    const uint32_t c = core(as);
    // (0x83acfbd0) this core's registered worker not sent its message yet: the count, the mask and the array read once
    const int32_t n = (int32_t)ld(&w->count);
    const uint32_t mask = ld(&w->registered_mask);
    const GuestAddr list = ld(&w->workers);
    const uint32_t own_core = 1u << (c & 31);
    GuestAddr found = 0;
    int32_t index = 0;
    for (int32_t i = 0; i < n && !found; i++) {
        if (!((mask >> (i & 31)) & 1)) continue;
        const GuestAddr k = ld(at<GuestAddr>(ld(at<GuestAddr>(list + (GuestAddr)(int64_t)i * 8)) + 8));
        const guest::Worker* worker = at<guest::Worker>(k);
        if (ld(&worker->context.flags) & kSentMessage) continue;
        if (ld(&worker->core_mask) == own_core) found = k, index = i;
    }
    if (found) {   // (0x83acfc58) started, and waited for on the set's own lane
        __atomic_fetch_or(&at<guest::Worker>(found)->context.flags, kSentMessage, __ATOMIC_SEQ_CST);
        if (options_.check && !note_started(set, index)) violation("a worker started twice in one last stage", found);
        start(as, found);
        stats_.last_workers.fetch_add(1, std::memory_order_relaxed);
        await_done(as, set, found, -1);
    } else {   // (0x83acfc20) else the own context, when the owner takes part and it is this core's and not started
        if (!(ld(&w->flags) & kOwnerTakesPart) || (ld(&w->own_context.flags) & kSentMessage) ||
            (uint32_t)ld(&w->own_context.lane) != c) {
            stats_.last_none.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        __atomic_fetch_or(&w->own_context.flags, kSentMessage, __ATOMIC_SEQ_CST);
        if (options_.check && !note_started(set, -1)) violation("the own context started twice in one last stage", set);
        stats_.last_own.fetch_add(1, std::memory_order_relaxed);
        dispatch(as, ld(&w->scheduler), set + kOwn);
    }
    // (0x83acfd44) the one that brings the count to zero ends the stage
    const uint32_t was = w->registered_count.fetch_sub(1, std::memory_order_seq_cst);
    if (options_.check && (int32_t)was <= 0) {
        stats_.below_zero.fetch_add(1, std::memory_order_relaxed);
        violation("the last stage lowered the registered count below zero", set);
    }
    if (was != 1) return;
    stats_.last_ends.fetch_add(1, std::memory_order_relaxed);
    if (options_.check) check_stage_end(set, true, 0);
    end_last_stage(manager);
}

// (0x83acfd68) the low byte of the actor manager's state word set to 0.
void Stages::end_last_stage(GuestAddr manager) {
    uint32_t* state = &at<guest::ActorManager>(manager)->state;
    uint32_t old = ld(state);
    while (!__atomic_compare_exchange_n(state, &old, old & 0xffffff00u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
}

// ---- checking ------------------------------------------------------------------------------------------------------------

void Stages::snapshot(GuestAddr set, Snapshot& out) const {
    const guest::WorkerSet* w = at<guest::WorkerSet>(set);
    const guest::Scheduler* s = at<guest::Scheduler>(ld(&w->scheduler));
    out.contexts = ld(&s->context_count);
    for (int32_t i = 0; i < out.contexts && i < 8; i++) out.list[i] = ld(at<GuestAddr>(ld(&s->contexts) + (GuestAddr)i * 8));
    out.scheduler_flags = ld(&s->flags);
    out.mask = ld(&w->registered_mask);
    out.count = w->registered_count.load(std::memory_order_relaxed);
    out.own_lane = ld(&w->own_context.lane);
    out.own_state = ld(&w->own_context.state);
    out.own_flags = ld(&w->own_context.flags);
    const int32_t n = (int32_t)ld(&w->count);
    for (int32_t i = 0; i < n && i < kSnapWorkers; i++) {
        const guest::Worker* k = at<guest::Worker>(worker_at(set, i));
        out.state[i] = ld(&k->context.state);
        out.flags[i] = ld(&k->context.flags);
        out.scheduler[i] = ld(&k->scheduler);
    }
}

void Stages::note_registered(GuestAddr set, uint64_t send, uint64_t lanes) {
    const guest::WorkerSet* w = at<guest::WorkerSet>(set);
    std::lock_guard lock(records_mu_);
    Record& r = records_[set];
    if (r.open) {
        stats_.overlap.fetch_add(1, std::memory_order_relaxed);
        violation("a set registered again before its previous stage was over", set);
    }
    r.scheduler = ld(&w->scheduler);
    r.mask = ld(&w->registered_mask);
    r.count = w->registered_count.load(std::memory_order_relaxed);
    r.own = !ld(&w->stage_cores) || (ld(&w->flags) & kOwnerTakesPart);
    r.started = 0;
    r.own_started = false;
    r.last = !(send & 1);
    r.open = r.mask || r.own;
    r.lanes = lanes;
}

bool Stages::note_started(GuestAddr set, int32_t index) {
    std::lock_guard lock(records_mu_);
    const auto it = records_.find(set);
    if (it == records_.end() || !it->second.open) {
        stats_.untracked.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    Record& r = it->second;
    bool& own = r.own_started;
    const uint32_t bit = index < 0 ? 0 : 1u << (index & 31);
    if (index < 0 ? own : (r.started & bit) != 0) {
        stats_.double_start.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (index < 0) own = true;
    else r.started |= bit;
    return true;
}

// After a worker's done event: its dispatcher left (its leave clears "sent its message" before the message signals).
void Stages::check_done(GuestAddr worker) {
    if (!(ld(&at<guest::Worker>(worker)->context.flags) & kSentMessage)) return;
    stats_.still_in.fetch_add(1, std::memory_order_relaxed);
    violation("a worker's done event taken while it is still in its dispatcher", worker);
}

void Stages::check_stage_end(GuestAddr set, bool last, uint64_t lanes) {
    const guest::WorkerSet* w = at<guest::WorkerSet>(set);
    std::lock_guard lock(records_mu_);
    const auto it = records_.find(set);
    if (it == records_.end() || !it->second.open) {
        stats_.untracked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Record& r = it->second;
    r.open = false;
    if (!(ld(&at<guest::Scheduler>(r.scheduler)->flags) & kStageOver)) {
        stats_.not_over.fetch_add(1, std::memory_order_relaxed);
        violation("a stage's end with its scheduler's stage not over", set);
    }
    if (last) {
        if (r.started != r.mask || r.own_started != r.own) {
            stats_.unstarted.fetch_add(1, std::memory_order_relaxed);
            violation("a last stage ended with a registered worker not started", set);
        }
        return;
    }
    // (the wait: every registered worker was started by the registration; nothing lowers the count of a waited stage)
    const bool own_needed = r.own && (ld(&w->flags) & kOwnerTakesPart);
    if (r.started != r.mask || (own_needed && !r.own_started)) {
        stats_.unstarted.fetch_add(1, std::memory_order_relaxed);
        violation("a stage waited for with a registered worker never started", set);
    }
    if (ld(&w->registered_mask) != r.mask || w->registered_count.load(std::memory_order_relaxed) != r.count) {
        stats_.changed.fetch_add(1, std::memory_order_relaxed);
        violation("the registered mask or count changed during a waited stage", set);
    }
    if (lanes != r.lanes) stats_.lane_drift.fetch_add(1, std::memory_order_relaxed);
}

int Stages::report(char* out, size_t cap) const {
    const Stats& s = stats_;
    auto v = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    int n = std::snprintf(out, cap,
                          "stages native: registrations %llu (own context %llu, workers %llu), started %llu (virtual %llu, "
                          "not queued %llu); waits %llu (no stage %llu), owner's part run %llu, done waits %llu; last stages "
                          "%llu (no stage %llu, a worker %llu, own context %llu, nothing %llu, ends %llu)",
                          v(s.registrations), v(s.own_registered), v(s.workers_registered), v(s.started),
                          v(s.started_virtual), v(s.not_queued), v(s.waits), v(s.waits_empty), v(s.owner_dispatches),
                          v(s.done_waits), v(s.last_calls), v(s.last_empty), v(s.last_workers), v(s.last_own), v(s.last_none),
                          v(s.last_ends));
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    if (!options_.check) return n;
    const int m = std::snprintf(out + n, cap - (size_t)n,
                                "; CHECK: registrations compared %llu (differ %llu), overlaps %llu, unstarted %llu, double "
                                "starts %llu, done while in %llu, not over %llu, changed %llu, below zero %llu; untracked %llu, "
                                "lane drift %llu",
                                v(s.reg_checked), v(s.reg_diff), v(s.overlap), v(s.unstarted), v(s.double_start), v(s.still_in),
                                v(s.not_over), v(s.changed), v(s.below_zero), v(s.untracked), v(s.lane_drift));
    if (m < 0) return n;
    return (size_t)(n + m) >= cap ? (int)cap - 1 : n + m;
}

}   // namespace hl::bridge
