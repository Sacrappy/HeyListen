// HeyListen native engine: the module system's job group, natively, in place on the game's own group scheduler.
//
// Written from a reading of the guest's group function (docs/native-engine/jobs-boundary.md, "The module system's job
// group"): nothing here is game code. A group's ready jobs are a heap ordered by priority under the group's mutex, with a
// count of jobs that may be taken handed out by compare-and-swap; a context takes a runner of the heap's top, runs it,
// finishes it and makes its dependents ready, hands a job tied to another lane to that lane's context, sleeps when there
// is nothing to take, and leaves when the group's end job completes. The loop around the queue is the actors'
// dispatcher's (src/bridge/DispatchLoop.h); the heap is this file's queue policy. Both the main thread's call (the worker
// set's own context, in the module system's stage wait) and the five module workers' calls reach it through an entry gate
// on the guest function (run()). What is the game's own stays the game's: job bodies, a hook's push, the mutexes, the
// events, the timed sleep and its one-time constant are called as guest functions on the calling thread's guest context.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "hl/bridge/Dispatcher.h"
#include "hl/bridge/JobBridge.h"

namespace hl::bridge {

/// Guest functions and data the native group uses (the supported build's; docs/native-engine/jobs-boundary.md).
inline constexpr GuestAddr kJobGroup = 0x8007fe60;        ///< answered: run()
inline constexpr GuestAddr kGroupPush = 0x80083350;       ///< called for a job with a pre-push hook (the mutex held)
inline constexpr GuestAddr kGroupPollTicks = 0x84e9f4f0;  ///< the timed sleep's length, made on the polling path
inline constexpr GuestAddr kGroupPollGuard = 0x84e9f4f8;  ///< its guard (bit 0: made)
inline constexpr GuestAddr kGuardAcquire = 0x83c61ca0;    ///< a function static's guard, taken (1: make it now)
inline constexpr GuestAddr kGuardRelease = 0x83c61cb0;    ///< and released once made
inline constexpr GuestAddr kTickFrequency = 0x80e5a5c0;   ///< the system tick's frequency

namespace guest {

/// A job record of the module system's group (64 bytes; the actors' record is guest::Job).
struct GroupJob {
    GuestAddr object;     ///< the job's object; bit 0: tagged (cleared to use it)
    uint32_t index;
    uint16_t priority;    ///< the heap's order: largest first
    uint8_t cancel;       ///< 1: the first runner to finish cancels the runners not started yet
    uint8_t kind;         ///< 0: counted in the low four bits of the lane status, else in the high four
    uint32_t lane;        ///< the lane it must run on; kAnyLane: any
    uint32_t id;          ///< shown to an observer
    GuestAddr hook;       ///< called before it is pushed (0: none)
    GuestAddr dep_last;   ///< its list of dependents: the head node sits at +0x20 (its previous: the last node),
    GuestAddr dep_first;  ///< (its next: the first node)
    int32_t dep_count;
    int32_t dep_offset;   ///< where the node sits in a list element (whose first word is the dependent job)
    uint32_t runners;     ///< low 16 bits: runners to start; high 16 bits: runners running
    int32_t waiting;      ///< jobs it still waits for; -1 once complete
};
static_assert(offsetof(GroupJob, object) == 0x00);
static_assert(offsetof(GroupJob, priority) == 0x0c);
static_assert(offsetof(GroupJob, cancel) == 0x0e);
static_assert(offsetof(GroupJob, kind) == 0x0f);
static_assert(offsetof(GroupJob, lane) == 0x10);
static_assert(offsetof(GroupJob, id) == 0x14);
static_assert(offsetof(GroupJob, hook) == 0x18);
static_assert(offsetof(GroupJob, dep_last) == 0x20);
static_assert(offsetof(GroupJob, dep_first) == 0x28);
static_assert(offsetof(GroupJob, dep_count) == 0x30);
static_assert(offsetof(GroupJob, dep_offset) == 0x34);
static_assert(offsetof(GroupJob, runners) == 0x38);
static_assert(offsetof(GroupJob, waiting) == 0x3c);
static_assert(sizeof(GroupJob) == 0x40);

/// A node of a group job's list of dependents (circular, through the head node at the job's +0x20).
struct GroupNode {
    GuestAddr prev;
    GuestAddr next;
};
static_assert(offsetof(GroupNode, next) == 8);

/// The group's ready jobs: a binary heap of job addresses.
struct GroupHeap {
    GuestAddr items;
    int32_t room;
    int32_t size;
};
static_assert(offsetof(GroupHeap, room) == 8);
static_assert(offsetof(GroupHeap, size) == 12);

/// A group scheduler of the module system.
struct Group {
    uint8_t _0[0x38];
    int32_t context_count;
    uint32_t context_room;
    GuestAddr contexts;   ///< an array of dispatch contexts' addresses
    GroupHeap heap;       ///< changed only under `mutex`
    uint8_t mutex[0x20];
    uint8_t _78[8];
    GroupJob end_job;     ///< its completion ends the group
    uint32_t poll_lane;   ///< a context of this lane polls with timed sleeps instead of sleeping on its event
    uint32_t flags;       ///< bit 1: the group is over
    int32_t take_count;   ///< jobs that may be taken from the heap, handed out by compare-and-swap
};
static_assert(offsetof(Group, context_count) == 0x38);
static_assert(offsetof(Group, contexts) == 0x40);
static_assert(offsetof(Group, heap) == 0x48);
static_assert(offsetof(Group, mutex) == 0x58);
static_assert(offsetof(Group, end_job) == 0x80);
static_assert(offsetof(Group, poll_lane) == 0xc0);
static_assert(offsetof(Group, flags) == 0xc4);
static_assert(offsetof(Group, take_count) == 0xc8);

}   // namespace guest

class JobGroup {
public:
    struct Options {
        bool check = false;   ///< count the invariants below
    };
    // Counters (relaxed; read for the log line). The invariants, counted with `check`:
    //   takes = runs from takes + passed + drops   (every runner taken from the heap is run, handed to its lane's
    //                                               context, or dropped where the guest drops it)
    //   passed = handed                             (every job handed to a context is run by it)
    //   runs = finishes + the runs going on now     (every run is finished once)
    //   bad_finish: a finish found no runner running; bad_count: a dependent's waiting count went below 0;
    //   double_complete: a job completed when already complete; double_run: a one-runner job run by two contexts at once;
    //   heap_bad: the heap's order broken after a change (checked under the mutex);
    //   at the end of a group (under the mutex): jobs left in the heap, a take count not 0, contexts asleep with jobs left.
    // Observations beside them: the take count read below 0 (the guest allows it for a moment, when a cancel removes a job
    // that a context holds a unit for), and a context's lane byte changed between its entry and its leave (lane drift).
    struct Stats {
        std::atomic<uint64_t> groups{0};
        std::atomic<uint64_t> takes{0}, last_takes{0}, given_back{0}, empty{0}, handed{0}, runs{0}, runs_from_takes{0};
        std::atomic<uint64_t> finishes{0}, completes{0}, cancels{0}, readied{0}, hook_pushes{0}, full{0};
        std::atomic<uint64_t> passed{0}, queued_passes{0}, drops{0}, sleeps{0}, polls{0}, wakes{0}, ends{0};
        std::atomic<uint64_t> bad_finish{0}, bad_count{0}, double_complete{0}, double_run{0}, heap_bad{0};
        std::atomic<uint64_t> end_checks{0}, end_left{0}, end_count{0}, end_asleep{0}, below_zero{0}, lane_drift{0};
        std::atomic<uint64_t> violations_logged{0};
    };

    JobGroup(GuestRuntime& runtime, Options options);

    /// The group function's answer: runs group `group`'s jobs on dispatch context `context` until the group is over (or
    /// the context is told not to run), as guest thread `as`, on its own guest context.
    void run(GuestThread as, GuestAddr group, GuestAddr context);
    /// The guest's push, natively; the caller holds the group's mutex (a job with a pre-push hook goes to the guest's own
    /// push).
    void push(GuestThread as, GuestAddr group, GuestAddr job);

    const Options& options() const { return options_; }
    const Stats& stats() const { return stats_; }
    /// One line of counters. Returns the length written.
    int report(char* out, size_t cap) const;

private:
    struct Heap;   // the heap as the dispatch loop's queue policy (src/bridge/DispatchLoop.h)

    template <class T>
    T* at(GuestAddr a) const { return static_cast<T*>(runtime_.ptr(a)); }
    uint64_t call(GuestThread as, GuestAddr fn, uint64_t a0, uint64_t a1 = 0);

    void lock(GuestThread as, GuestAddr g_at) { call(as, kMutexLock, g_at + offsetof(guest::Group, mutex)); }
    void unlock(GuestThread as, GuestAddr g_at) { call(as, kMutexUnlock, g_at + offsetof(guest::Group, mutex)); }
    int32_t sift_up(guest::Group* g, int32_t i, GuestAddr job);
    void sift_down(guest::Group* g, int32_t i, GuestAddr job, int32_t size);
    void take_top(guest::Group* g);
    void erase(guest::Group* g, int32_t i);
    void check_heap(guest::Group* g);
    void forward(GuestThread as, GuestAddr g_at, uint32_t lane, GuestAddr job);
    void finish(GuestThread as, GuestAddr g_at, GuestAddr job);
    void over(GuestThread as, GuestAddr g_at);
    void timed_sleep(GuestThread as);
    void violation(const char* what, GuestAddr job);

    GuestRuntime& runtime_;
    Options options_;
    Stats stats_;
    std::mutex single_mu_;   // (checking only: the one-runner jobs being run now)
    std::unordered_set<GuestAddr> single_;
};

}   // namespace hl::bridge
