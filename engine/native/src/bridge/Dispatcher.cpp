// HeyListen native engine: the game's job dispatcher, natively (hl/bridge/Dispatcher.h). Each step below follows a step
// of docs/native-engine/jobs-boundary.md, "The dispatcher's protocol"; the guest instruction addresses in the comments are
// where the reading of that step is, in the translated dispatcher 0x8004ab40 (or the push 0x8004fa40). The loop around
// the rings (entry, handed jobs, sleeping, waking, running, leave) is the one the module system's job group shares
// (DispatchLoop.h); what is the rings' own is the policy Dispatcher::Rings below.
#include "hl/bridge/Dispatcher.h"

#include <chrono>
#include <cstdio>
#include <immintrin.h>

#include "DispatchLoop.h"

namespace hl::bridge {

using loop::cas;
using loop::ld;
using loop::ld_acquire;
using loop::st;
using loop::st_release;

namespace {

constexpr uint64_t kOwned = 1ull << 63;   // a cell's sequence word while a pop owns the cell
constexpr uint32_t kStageOver = loop::kOver;
constexpr GuestAddr kRingSize = sizeof(guest::Ring);

GuestAddr ring_at(GuestAddr s_at, int index) {   // (index: 0 level 0 A, 1 level 0 B, 2 level 1 A, ... 5 level 2 B)
    return s_at + offsetof(guest::Scheduler, levels) + (GuestAddr)index * kRingSize;
}

// The cell counter `pos` maps to: (size - 1) & pos in 32 bits, sign-extended (a ring of size 0 uses its first cell).
GuestAddr cell_at(const guest::Ring* r, uint64_t pos) {
    const int32_t size = ld(&r->size);
    const GuestAddr cells = ld(&r->cells);
    if (!size) return cells;
    return cells + (GuestAddr)(int64_t)(int32_t)((uint32_t)(size - 1) & (uint32_t)pos) * sizeof(guest::Cell);
}

// Set while this host thread calls the translated dispatcher on purpose: its entry gate lets that one call through.
thread_local bool t_bypass = false;

}   // namespace

// The rings as the loop's queue policy (DispatchLoop.h): one per native dispatch.
struct Dispatcher::Rings {
    static constexpr int32_t kIdleRounds = 9;   // (0x8004ba88: nine rounds with jobs in sight but none taken: a timed sleep)
    static constexpr GuestAddr kKindAt = offsetof(guest::Job, kind);
    static constexpr uint32_t kFrameWord = 1;

    Dispatcher& d;
    loop::Context& cx;
    GuestAddr s_at;
    guest::Scheduler* s;
    bool single = false;   // (checking: the job taken is a one-runner job, marked as running)

    GuestAddr scheduler() const { return s_at; }
    bool over() const { return ld(&s->flags) & kStageOver; }
    int32_t queued() const { return d.queued(s_at, 0); }
    uint32_t poll_lane() const { return ld(&s->poll_lane); }
    void poll() { d.timed_sleep(cx.as); }
    int32_t context_count() const { return ld(&s->context_count); }
    GuestAddr contexts() const { return ld(&s->contexts); }
    void left() {
        if (d.options_.check && over()) d.check_stage_end(s_at);
    }

    void finish(GuestAddr job) {
        d.finish(cx.as, s_at, job);
        if (single) {
            std::lock_guard lock(d.single_mu_);
            d.single_.erase(job);
            single = false;
        }
    }

    // Take the first job of level 0 A, 0 B, 1 A, 1 B, 2 A, 2 B (0x8004acc8).
    loop::Taken take() {
        Stats& stats = d.stats_;
        guest::DispatchContext* c = cx.c;
        for (int level = 0; level < 3; level++) {
            const GuestAddr a_at = ring_at(s_at, level * 2), b_at = ring_at(s_at, level * 2 + 1);
            guest::Ring* a = cx.mem<guest::Ring>(a_at);
            guest::Ring* b = cx.mem<guest::Ring>(b_at);
            while (ld(&a->cells)) {
                uint64_t head = 0;
                const GuestAddr j = d.pop(cx.as, a_at, head);
                if (!j) break;
                stats.pops_a.fetch_add(1, std::memory_order_relaxed);
                const uint64_t tail = ld(&a->tail);   // (0x8004ad74)
                uint16_t* word = &cx.mem<guest::Job>(j)->runners;
                uint16_t h = ld(word);
                bool skip = false;
                for (;;) {
                    if (((h >> 14) & 3) > 2) { skip = true; break; }   // (not to be started: dropped)
                    if (cas(word, h, (uint16_t)(h + 0x7f))) break;
                }
                const unsigned to_start = h & 0x7f;
                if (skip || to_start == 0) {   // (pop this A ring again)
                    stats.drops.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const int32_t here = (int32_t)((uint32_t)tail - (uint32_t)head);
                int32_t n;
                if (to_start == 1) {   // (0x8004b188: its last runner)
                    n = here + d.queued(s_at, level * 2 + 1);
                } else {                // (0x8004b04c: put back for its next runners)
                    if (d.ring_push(cx.as, a_at, j)) stats.requeues.fetch_add(1, std::memory_order_relaxed);
                    else stats.full.fetch_add(1, std::memory_order_relaxed);
                    n = here + d.queued(s_at, level * 2 + 1) + 1;
                }
                return taken(j, n);
            }
            if (!ld(&b->cells)) continue;
            uint64_t head = 0;
            const GuestAddr j = d.pop(cx.as, b_at, head);   // (0x8004adc0)
            if (!j) continue;
            stats.pops_b.fetch_add(1, std::memory_order_relaxed);
            const uint64_t tail = ld(&b->tail);   // (0x8004afc0)
            uint16_t* word = &cx.mem<guest::Job>(j)->runners;
            uint16_t h = ld(word);
            bool blocked = false;
            for (;;) {
                if (((h >> 14) & 3) > 2) { blocked = true; break; }
                if (cas(word, h, (uint16_t)(h + 0x7f))) break;
            }
            const int32_t here = (int32_t)((uint32_t)tail - (uint32_t)head);
            if (blocked || (h & 0x7f) != 1) {   // (0x8004b228: into this level's A ring for its other runners, and run)
                if (d.ring_push(cx.as, a_at, j)) stats.requeues.fetch_add(1, std::memory_order_relaxed);
                else stats.full.fetch_add(1, std::memory_order_relaxed);
                return taken(j, here + 1 + d.queued(s_at, level * 2 + 2));
            }
            const uint32_t lane = ld(&cx.mem<guest::Job>(j)->lane);
            if (lane != kAnyLane && lane != (uint32_t)ld(&c->lane)) {   // (0x8004ec8c: tied to another executor)
                cx.call(kForward, s_at, lane, j);
                stats.forwards.fetch_add(1, std::memory_order_relaxed);
                return {loop::Taken::Passed, 0, 0};
            }
            if (d.options_.check && (h & 0x3f80) == 0) {   // (one runner, none running before: no other may run it now)
                std::lock_guard lock(d.single_mu_);
                if (!d.single_.insert(j).second) {
                    stats.double_run.fetch_add(1, std::memory_order_relaxed);
                    d.violation("a one-runner job taken while a native context runs it", j);
                } else {
                    single = true;
                }
            }
            return taken(j, here + d.queued(s_at, level * 2 + 2));   // (0x8004b008)
        }
        return {};   // (0x8004ae68: nothing found)
    }

    loop::Taken taken(GuestAddr job, int32_t wake) {
        d.stats_.runs_from_pops.fetch_add(1, std::memory_order_relaxed);
        return {loop::Taken::Job, job, wake};
    }
};

Dispatcher::Dispatcher(GuestRuntime& runtime, Options options) : runtime_(runtime), options_(options) {}

uint64_t Dispatcher::call(GuestThread as, GuestAddr fn, uint64_t a0, uint64_t a1, uint64_t a2) {
    const uint64_t args[3] = {a0, a1, a2};
    return runtime_.call_with(as, fn, args, 3, nullptr, 0);
}

void Dispatcher::violation(const char* what, GuestAddr job) {
    if (stats_.violations_logged.fetch_add(1, std::memory_order_relaxed) >= 20) return;
    char line[160];
    std::snprintf(line, sizeof line, "hl dispatch: CHECK: %s (job %#llx)", what, (unsigned long long)job);
    runtime_.log(line);
}

// ---- the dispatch ------------------------------------------------------------------------------------------------------

void Dispatcher::dispatch(GuestThread as, GuestAddr s_at, GuestAddr c_at) {
    // (the translated dispatcher makes its timed sleep's length the first time it runs, guarded as a function's static:
    // until then, its whole call)
    const uint8_t* guard = at<uint8_t>(kPollTicksGuard);
    if (!options_.native || !(ld_acquire(guard) & 1)) {
        if (options_.native) stats_.translated.fetch_add(1, std::memory_order_relaxed);
        t_bypass = true;   // (when the translated dispatcher has an entry gate, the call reaches the gate first)
        runtime_.call(as, kDispatcher, s_at, c_at);
        t_bypass = false;
        if (options_.check && (ld(&at<guest::Scheduler>(s_at)->flags) & kStageOver)) check_stage_end(s_at);
        return;
    }
    stats_.stages.fetch_add(1, std::memory_order_relaxed);
    loop::Context cx(runtime_, as, c_at);
    Rings rings{*this, cx, s_at, at<guest::Scheduler>(s_at)};
    loop::run(cx, rings, stats_);
}

bool Dispatcher::gated(GuestThread as, GuestAddr s_at, GuestAddr c_at) {
    stats_.gated.fetch_add(1, std::memory_order_relaxed);
    if (t_bypass) {
        t_bypass = false;
        stats_.bypassed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    dispatch(as, s_at, c_at);
    return true;
}

// ---- the rings ---------------------------------------------------------------------------------------------------------

GuestAddr Dispatcher::pop(GuestThread as, GuestAddr r_at, uint64_t& head) {
    guest::Ring* r = at<guest::Ring>(r_at);
    uint64_t pos = ld(&r->head);
    for (;;) {
        const GuestAddr cell_addr = cell_at(r, pos);
        guest::Cell* cell = at<guest::Cell>(cell_addr);
        const GuestAddr seq_addr = cell_addr + offsetof(guest::Cell, seq);
        const uint64_t seq = ld_acquire(&cell->seq);
        if (seq == pos + 1) {
            uint64_t e = pos;
            if (!cas(&r->head, e, seq)) {   // (another took it: from the head it read)
                pos = e;
                continue;
            }
            uint64_t was = __atomic_fetch_or(&cell->seq, kOwned, __ATOMIC_SEQ_CST);
            while (was & kOwned) {   // (0x8004edc8)
                uint64_t v = ld(&cell->seq);
                while (v & kOwned) {
                    wait_change(as, seq_addr, v);
                    v = ld(&cell->seq);
                }
                was = __atomic_fetch_or(&cell->seq, kOwned, __ATOMIC_SEQ_CST);
            }
            const GuestAddr job = __atomic_exchange_n(&cell->job, (GuestAddr)0, __ATOMIC_SEQ_CST);
            st_release(&cell->seq, pos + (uint64_t)(int64_t)ld(&r->size));
            call(as, kAddrNotify, seq_addr);
            if (job) {
                head = pos;
                return job;
            }
            continue;   // (a job of 0 is skipped: again, from the same head)
        }
        if (seq > pos + 1) {   // (0x8004bc50: a pop still owns it, or the head read is stale)
            if (seq & kOwned) wait_change(as, seq_addr, seq);
            pos = ld(&r->head);
            continue;
        }
        if (ld(&r->tail) == pos) return 0;   // (empty)
        wait_change(as, seq_addr, seq);       // (0x8004d5b8: a push is between its tail and its store)
        pos = ld(&r->head);
    }
}

bool Dispatcher::ring_push(GuestThread as, GuestAddr r_at, GuestAddr job) {
    guest::Ring* r = at<guest::Ring>(r_at);
    uint64_t pos = ld(&r->tail);
    for (;;) {
        const GuestAddr cell_addr = cell_at(r, pos);
        guest::Cell* cell = at<guest::Cell>(cell_addr);
        const GuestAddr seq_addr = cell_addr + offsetof(guest::Cell, seq);
        const uint64_t seq = ld_acquire(&cell->seq);
        if (seq == pos) {
            uint64_t e = pos;
            if (!cas(&r->tail, e, pos + 1)) {
                pos = e;
                continue;
            }
            st(&cell->job, job);
            st_release(&cell->seq, pos + 1);
            call(as, kAddrNotify, seq_addr);
            return true;
        }
        if (seq < pos) {
            // (full: the guest returns without pushing; else the last lap's pop has not finished)
            if (pos - ld(&r->head) >= (uint64_t)(int64_t)ld(&r->size)) return false;
            wait_change(as, seq_addr, seq);
        } else if (seq & kOwned) {
            wait_change(as, seq_addr, seq);
        }
        pos = ld(&r->tail);
    }
}

void Dispatcher::push(GuestThread as, GuestAddr s_at, GuestAddr job) {
    guest::Job* j = at<guest::Job>(job);
    if (ld(&j->hook)) {   // (0x8004fafc: the hook decides the runners, and may complete it at once: the guest's push)
        call(as, kPush, s_at, job);
        stats_.hook_pushes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    guest::Scheduler* s = at<guest::Scheduler>(s_at);
    int index = 3;   // (level 1 B)
    if (ld(&s->levels[2].b.cells) && (uint16_t)ld(&j->level) == 0) index = 5;
    else if (ld(&s->levels[0].b.cells) && ld(&j->level) == -2) index = 1;
    if (ring_push(as, ring_at(s_at, index), job)) stats_.readied.fetch_add(1, std::memory_order_relaxed);
    else stats_.full.fetch_add(1, std::memory_order_relaxed);
}

// The C++ library's atomic wait (0x8012f790, and its copies in the dispatcher): polls, then polls with a clock, then after
// 4 microseconds waits on the address through the guest's own table of waiters, so that a translated push or pop wakes it.
void Dispatcher::wait_change(GuestThread as, GuestAddr word, uint64_t seen) {
    const uint64_t* p = at<uint64_t>(word);
    for (int i = 0; i < 64; i++) {
        if (ld_acquire(p) != seen) return;
        _mm_pause();
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (ld_acquire(p) != seen) return;
        if (std::chrono::steady_clock::now() - t0 < std::chrono::nanoseconds(4001)) {
            _mm_pause();
            continue;
        }
        const uint64_t monitor = call(as, kAddrMonitor, word);
        if (ld_acquire(p) != seen) return;
        stats_.waits.fetch_add(1, std::memory_order_relaxed);
        call(as, kAddrWait, word, monitor);
    }
}

int32_t Dispatcher::queued(GuestAddr s_at, int from) const {
    int32_t n = 0;
    for (int i = from; i < 6; i++) {
        const guest::Ring* r = at<guest::Ring>(ring_at(s_at, i));
        n += (int32_t)((uint32_t)ld(&r->tail) - (uint32_t)ld(&r->head));
    }
    return n;
}

// ---- jobs --------------------------------------------------------------------------------------------------------------

void Dispatcher::finish(GuestThread as, GuestAddr s_at, GuestAddr job) {
    guest::Job* j = at<guest::Job>(job);
    uint16_t h = ld(&j->runners);
    while (!cas(&j->runners, h, (uint16_t)((h - 0x80) | 0x4000))) {}
    stats_.finishes.fetch_add(1, std::memory_order_relaxed);
    if (options_.check && (h & 0x3f80) == 0) {
        stats_.bad_finish.fetch_add(1, std::memory_order_relaxed);
        violation("a job finished with no runner running", job);
    }
    if ((h & 0x3f80) != 0x80) return;               // (other runners still run it)
    if (!(h & 0x8000) && (h & 0x7f) != 0) return;   // (runners still to start)
    // Complete (0x8004ac28): each dependent waits for one job fewer; those that wait for none are pushed.
    stats_.completes.fetch_add(1, std::memory_order_relaxed);
    const GuestAddr list = job + offsetof(guest::Job, dep_node);
    for (GuestAddr node = ld(&j->dep_first); node != list; node = ld(&at<guest::DepNode>(node)->next)) {
        const GuestAddr dep = ld(&at<guest::DepNode>(node)->job);
        const int32_t was = __atomic_fetch_add(&at<guest::Job>(dep)->waiting, -1, __ATOMIC_SEQ_CST);
        if (options_.check && was <= 0) {
            stats_.bad_count.fetch_add(1, std::memory_order_relaxed);
            violation("a dependent's waiting count went below 0", dep);
        }
        if (was == 1) push(as, s_at, dep);
    }
    st(&j->dep_first, list);
    st(&j->dep_last, list);
    if (job == s_at + offsetof(guest::Scheduler, end_job)) stage_over(as, s_at);
}

// The end job completed (0x8004bc58): the stage is over; every context is woken to leave.
void Dispatcher::stage_over(GuestThread as, GuestAddr s_at) {
    guest::Scheduler* s = at<guest::Scheduler>(s_at);
    __atomic_fetch_or(&s->flags, kStageOver, __ATOMIC_SEQ_CST);
    stats_.stage_ends.fetch_add(1, std::memory_order_relaxed);
    const int32_t count = ld(&s->context_count);
    const GuestAddr list = ld(&s->contexts);
    for (int32_t i = 0; i < count; i++) {
        const GuestAddr ctx = ld(at<GuestAddr>(list + (GuestAddr)i * 8));
        uint64_t* state = &at<guest::DispatchContext>(ctx)->state;
        for (;;) {
            uint64_t v = ld(state);
            if (v != 1 || cas(state, v, (uint64_t)0)) break;
        }
        call(as, kEventSet, ctx + offsetof(guest::DispatchContext, wake_event));
    }
}

void Dispatcher::timed_sleep(GuestThread as) {
    stats_.polls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t span = call(as, kTicksToSpan, ld(at<uint64_t>(kPollTicks)));
    call(as, kSleepFor, span);
}

void Dispatcher::check_stage_end(GuestAddr s_at) {
    stats_.end_checks.fetch_add(1, std::memory_order_relaxed);
    const int32_t left = queued(s_at, 0);
    if (!left) return;
    stats_.end_left.fetch_add(1, std::memory_order_relaxed);
    const guest::Scheduler* s = at<guest::Scheduler>(s_at);
    const int32_t n = ld(&s->context_count);
    const GuestAddr list = ld(&s->contexts);
    for (int32_t i = 0; i < n; i++)
        if (ld(&at<guest::DispatchContext>(ld(at<GuestAddr>(list + (GuestAddr)i * 8)))->state) == 1)
            stats_.end_asleep.fetch_add(1, std::memory_order_relaxed);
}

int Dispatcher::report(char* out, size_t cap) const {
    const Stats& s = stats_;
    auto v = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    int n = std::snprintf(out, cap,
                          "dispatch %s: stages %llu (translated %llu; through the gate %llu, let through %llu); pops A %llu B %llu, "
                          "handed %llu, runs %llu (from pops %llu), finishes %llu, completes %llu, readied %llu (hook %llu), "
                          "forwards %llu, drops %llu, requeues %llu, full %llu; sleeps %llu, polls %llu, wakes %llu, address "
                          "waits %llu, stage ends %llu",
                          options_.native ? "native" : "translated", v(s.stages), v(s.translated), v(s.gated), v(s.bypassed),
                          v(s.pops_a), v(s.pops_b),
                          v(s.handed), v(s.runs), v(s.runs_from_pops), v(s.finishes), v(s.completes), v(s.readied),
                          v(s.hook_pushes), v(s.forwards), v(s.drops), v(s.requeues), v(s.full), v(s.sleeps), v(s.polls),
                          v(s.wakes), v(s.waits), v(s.stage_ends));
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    if (!options_.check) return n;
    const long long unaccounted = (long long)(v(s.pops_a) + v(s.pops_b)) - (long long)(v(s.runs_from_pops) + v(s.forwards) + v(s.drops));
    const long long unfinished = (long long)v(s.runs) - (long long)v(s.finishes);
    const int m = std::snprintf(out + n, cap - (size_t)n,
                                "; CHECK: pops not run %lld, runs not finished %lld, bad finishes %llu, bad counts %llu, "
                                "double runs %llu; stage ends checked %llu: with jobs left %llu, contexts asleep then %llu",
                                unaccounted, unfinished, v(s.bad_finish), v(s.bad_count), v(s.double_run), v(s.end_checks),
                                v(s.end_left), v(s.end_asleep));
    if (m < 0) return n;
    return (size_t)(n + m) >= cap ? (int)cap - 1 : n + m;
}

}   // namespace hl::bridge
