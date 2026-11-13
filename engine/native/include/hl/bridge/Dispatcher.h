// HeyListen native engine: the game's job dispatcher, natively, in place on the game's own scheduler.
//
// Written from a reading of the guest's dispatcher (docs/native-engine/jobs-boundary.md, "The dispatcher's protocol"):
// nothing here is game code. It takes the jobs of a stage from the scheduler's ring queues, runs them, finishes them and
// makes their dependents ready, wakes and sleeps dispatch contexts, and leaves when the stage is over, with the same
// atomic operations on the same words as the translated dispatcher, so that both serve one scheduler at once (the stage's
// owner and nested calls still run the translated one). What is the game's own stays the game's: job bodies, events, the
// mutex, forwarding to an executor, the timed sleep and the C++ library's table of address waiters are called as guest
// functions on the worker's guest context (guest::* in JobBridge.h has the structures).
//
// The policies a later speed study may change are each one function here, with counters: how a ring word is waited for
// (wait_change), how many contexts are woken (wake), how a context sleeps (sleep_on_event, timed_sleep).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "hl/bridge/JobBridge.h"

namespace hl::bridge {

inline constexpr uint32_t kAnyLane = 0x21;

/// Guest functions and data the native dispatcher uses (the supported build's; docs/native-engine/jobs-boundary.md).
inline constexpr GuestAddr kPush = 0x8004fa40;          ///< called for a job with a pre-push hook
inline constexpr GuestAddr kForward = 0x810cc640;       ///< a job handed to the context of a lane
inline constexpr GuestAddr kAddrNotify = 0x83c61e50;    ///< the C++ library's notify of an address's waiters
inline constexpr GuestAddr kAddrMonitor = 0x83c61e80;   ///< its monitor of an address (before a wait)
inline constexpr GuestAddr kAddrWait = 0x83c61e70;      ///< its wait on an address
inline constexpr GuestAddr kEventWait = 0x83c62cb0;
inline constexpr GuestAddr kEventSet = 0x83c62cd0;
inline constexpr GuestAddr kMutexLock = 0x83c62460;
inline constexpr GuestAddr kMutexUnlock = 0x83c62470;
inline constexpr GuestAddr kTicksToSpan = 0x83c623e0;
inline constexpr GuestAddr kSleepFor = 0x83c62de0;
inline constexpr GuestAddr kPollTicks = 0x84e9f510;       ///< the timed sleep's length, made at the first dispatch
inline constexpr GuestAddr kPollTicksGuard = 0x84e9f518;  ///< bit 0: made

class Dispatcher {
public:
    struct Options {
        bool native = false;   ///< run stages natively (else the translated dispatcher)
        bool check = false;    ///< count the invariants below
    };
    // Counters (relaxed; read for the log line). The invariants, counted with `check`:
    //   pops = runs from pops + forwards + drops   (every job popped is run, forwarded, or dropped where the guest drops it)
    //   runs = finishes + the runs going on now     (every run is finished once)
    //   bad_finish: a finish found no running runner                   (a job finished more often than started)
    //   bad_count: a dependent's waiting count went below 0             (made ready twice)
    //   double_run: a one-runner job run by two native contexts at once
    //   at the end of a stage: jobs left in the rings, contexts asleep with jobs queued (also counted after the translated
    //   dispatcher, for comparison)
    struct Stats {
        std::atomic<uint64_t> stages{0}, translated{0};   ///< native dispatches; dispatches left to the translated one
        std::atomic<uint64_t> pops_a{0}, pops_b{0}, handed{0}, runs{0}, runs_from_pops{0}, finishes{0}, completes{0};
        std::atomic<uint64_t> forwards{0}, drops{0}, requeues{0}, full{0}, readied{0}, hook_pushes{0};
        std::atomic<uint64_t> sleeps{0}, polls{0}, wakes{0}, waits{0}, stage_ends{0};
        std::atomic<uint64_t> bad_finish{0}, bad_count{0}, double_run{0}, violations_logged{0};
        std::atomic<uint64_t> end_checks{0}, end_left{0}, end_asleep{0};
    };

    Dispatcher(GuestRuntime& runtime, Options options);

    /// Runs a stage's work for dispatch context `context` of scheduler `scheduler`, as guest thread `as` (the caller holds
    /// its lane): natively, or by calling the translated dispatcher.
    void dispatch(GuestThread as, GuestAddr scheduler, GuestAddr context);
    /// The guest's push, natively (a job with a pre-push hook goes to the guest's own push).
    void push(GuestThread as, GuestAddr scheduler, GuestAddr job);

    const Options& options() const { return options_; }
    const Stats& stats() const { return stats_; }
    /// One line of counters. Returns the length written.
    int report(char* out, size_t cap) const;

private:
    template <class T>
    T* at(GuestAddr a) const { return static_cast<T*>(runtime_.ptr(a)); }
    uint64_t call(GuestThread as, GuestAddr fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0);

    void run_native(GuestThread as, GuestAddr s_at, GuestAddr c_at);
    GuestAddr pop(GuestThread as, GuestAddr ring_at, uint64_t& head);
    bool ring_push(GuestThread as, GuestAddr ring_at, GuestAddr job);
    void wait_change(GuestThread as, GuestAddr word, uint64_t seen);
    void finish(GuestThread as, GuestAddr s_at, GuestAddr job);
    void stage_over(GuestThread as, GuestAddr s_at);
    void wake(GuestThread as, GuestAddr s_at, int32_t count);
    void run_job(GuestThread as, GuestAddr s_at, GuestAddr c_at, GuestAddr job);
    void sleep_on_event(GuestThread as, GuestAddr c_at);
    void timed_sleep(GuestThread as);
    int32_t queued(GuestAddr s_at, int from) const;
    void lane_status(GuestAddr c_at, uint64_t nibble, bool up) const;
    void check_stage_end(GuestAddr s_at);
    void violation(const char* what, GuestAddr job);

    GuestRuntime& runtime_;
    Options options_;
    Stats stats_;
    std::mutex single_mu_;   // (checking only: the one-runner jobs being run natively now)
    std::unordered_set<GuestAddr> single_;
};

}   // namespace hl::bridge
