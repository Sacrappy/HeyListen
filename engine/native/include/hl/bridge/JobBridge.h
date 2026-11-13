// HeyListen native engine: where the native job system meets the gameplay code that is still run from the user's local
// translation (docs/native-engine/jobs-boundary.md has the reading behind every number here).
//
// Nothing in this file is game code or game data. It names places: the guest addresses of the game's own job system's
// entry points in the one supported build (the build the builder accepts), and the layout of the few structures that
// gameplay code reads without calling anything. Those structures stay where the game put them, in the guest's memory:
// the bridge reads and writes them in place through the structs below, whose offsets are checked at compile time.
//
// Taking the subsystem over is done one entry point at a time. job_entries() says which are answered natively today.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hl/jobs/JobSystem.h"

namespace hl::bridge {

using GuestAddr = uint64_t;
/// A guest thread, as the layer that runs guest code knows it (opaque here).
using GuestThread = void*;

/// What the bridge needs from the layer that runs guest code (the OS layer; none of it is in the native tree).
struct GuestRuntime {
    virtual ~GuestRuntime() = default;
    /// The host address of a guest address.
    virtual void* ptr(GuestAddr at) const = 0;
    /// The guest thread the calling host thread is running as; null when it is none.
    virtual GuestThread current() const = 0;
    /// Calls the guest function `fn(a0, a1)` on the calling host thread, as guest thread `as`: on that thread's own guest
    /// context (its registers, its stack from where it stands, its thread-local values, its id and core). `as` must not be
    /// running guest code anywhere else meanwhile: the caller holds its lane. Returns the function's x0.
    virtual uint64_t call(GuestThread as, GuestAddr fn, uint64_t a0, uint64_t a1) = 0;
    /// The same with up to four arguments; when `frame_size` is not 0, a copy of `frame` is first put on the guest stack
    /// (16-byte aligned) and its guest address is passed as the argument after `args`.
    virtual uint64_t call_with(GuestThread as, GuestAddr fn, const uint64_t* args, unsigned nargs, const void* frame,
                               unsigned frame_size) = 0;
    /// One line for the game's log.
    virtual void log(const char* line) = 0;
};

// ---- the game's structures, in place -------------------------------------------------------------------------------
// Only the fields the bridge uses or that gameplay code reads are named; the rest is padding. Pointers inside them are
// guest addresses.
namespace guest {

/// A dispatch context: a worker's own, or a worker set's.
struct DispatchContext {
    uint8_t _0[8];
    uint64_t state;   ///< 0: awake, 1: asleep, else a job handed to it
    uint32_t flags;   ///< bit 0: sent its message; bit 1: not to run
    uint8_t _14[4];
    GuestAddr queue;  ///< its own queue of handed jobs (job addresses), a circle of `queue_room`
    int32_t queue_room;
    int32_t queue_first;
    int32_t queue_count;
    uint8_t _2c[68];
    uint8_t mutex[64];   ///< guards the queue
    uint8_t wake_event[16];
    GuestAddr observer;  ///< 0: none
    GuestAddr shown_object;   ///< what the observer is shown: the job's object,
    uint32_t shown_id;        ///< its identifier,
    uint8_t shown_begin;      ///< 1 before it runs, 0 after
    uint8_t _d5[3];
    int32_t lane;
    uint8_t _dc[4];
    GuestAddr lane_status;   ///< the lane status word
};
static_assert(offsetof(DispatchContext, state) == 8);
static_assert(offsetof(DispatchContext, flags) == 16);
static_assert(offsetof(DispatchContext, queue) == 0x18);
static_assert(offsetof(DispatchContext, queue_room) == 0x20);
static_assert(offsetof(DispatchContext, queue_first) == 0x24);
static_assert(offsetof(DispatchContext, queue_count) == 0x28);
static_assert(offsetof(DispatchContext, mutex) == 0x70);
static_assert(offsetof(DispatchContext, wake_event) == 176);
static_assert(offsetof(DispatchContext, observer) == 0xc0);
static_assert(offsetof(DispatchContext, shown_object) == 0xc8);
static_assert(offsetof(DispatchContext, shown_id) == 0xd0);
static_assert(offsetof(DispatchContext, shown_begin) == 0xd4);
static_assert(offsetof(DispatchContext, lane) == 216);
static_assert(offsetof(DispatchContext, lane_status) == 224);
static_assert(sizeof(DispatchContext) == 232);

/// A worker: a thread object of the game's.
struct Worker {
    uint8_t _0[40];
    uint8_t queue[132];   ///< its message queue
    uint32_t state;
    uint32_t core_mask;
    uint8_t _b4[36];
    DispatchContext context;
    uint8_t _1c0[40];
    uint8_t done_event[16];
    uint32_t core;   ///< its core, 0 to 5
    uint8_t _1fc[4];
    GuestAddr scheduler;   ///< the scheduler it serves in the stage; 0: none
};
static_assert(offsetof(Worker, queue) == 40);
static_assert(offsetof(Worker, state) == 172);
static_assert(offsetof(Worker, core_mask) == 176);
static_assert(offsetof(Worker, context) == 216);
static_assert(offsetof(Worker, done_event) == 488);
static_assert(offsetof(Worker, core) == 504);
static_assert(offsetof(Worker, scheduler) == 512);
static_assert(offsetof(Worker, context) + offsetof(DispatchContext, flags) == 232);   // (the "sent its message" word)

/// A worker set (the six executors are one).
struct WorkerSet {
    uint32_t count;
    uint8_t _4[4];
    GuestAddr workers;
    DispatchContext own_context;             ///< its lane is the owner's core
    uint32_t stage_cores;                    ///< mask
    std::atomic<uint32_t> registered_count;  ///< workers registered and not yet done
    uint32_t registered_mask;
    uint32_t flags;   ///< bit 0: the owner takes part with the set's own context
    GuestAddr scheduler;
};
static_assert(offsetof(WorkerSet, workers) == 8);
static_assert(offsetof(WorkerSet, own_context) == 16);
static_assert(offsetof(WorkerSet, own_context) + offsetof(DispatchContext, lane) == 232);   // (the owner's core)
static_assert(offsetof(WorkerSet, own_context) + offsetof(DispatchContext, lane_status) == 240);
static_assert(offsetof(WorkerSet, stage_cores) == 248);
static_assert(offsetof(WorkerSet, registered_count) == 252);
static_assert(offsetof(WorkerSet, registered_mask) == 256);
static_assert(offsetof(WorkerSet, flags) == 260);
static_assert(offsetof(WorkerSet, scheduler) == 264);

/// One cell of a ring: the job pushed there and the cell's sequence word.
struct Cell {
    GuestAddr job;
    uint64_t seq;
};
static_assert(sizeof(Cell) == 16);

/// A bounded ring of cells (many producers, many consumers). Its counters only grow; counter p is cell p & (size - 1).
struct Ring {
    int32_t size;   ///< a power of two
    uint8_t _4[4];
    GuestAddr cells;   ///< 0: the ring does not exist
    uint64_t head;
    uint64_t tail;
    uint8_t _20[8];
};
static_assert(offsetof(Ring, size) == 0);
static_assert(offsetof(Ring, cells) == 8);
static_assert(offsetof(Ring, head) == 16);
static_assert(offsetof(Ring, tail) == 24);
static_assert(sizeof(Ring) == 40);

/// One level of a scheduler's job queues: an A ring (parallel jobs put back for their next runners) and a B ring.
struct QueueLevel {
    Ring a;
    Ring b;
};
static_assert(offsetof(QueueLevel, b) == 40);
static_assert(sizeof(QueueLevel) == 80);

/// A job record: what the rings hold.
struct Job {
    GuestAddr object;   ///< the job's object; bit 0: tagged (cleared to use it)
    uint8_t _8[4];
    int16_t level;      ///< 0: level 2, -2: level 0, else level 1 (when those levels' B rings exist)
    uint8_t kind;       ///< 0: counted in the low four bits of the lane status, else in the high four
    uint8_t _f;
    uint32_t lane;      ///< the lane it must run on; kAnyLane: any
    uint32_t id;        ///< shown to an observer
    GuestAddr hook;     ///< called before it is pushed (0: none)
    GuestAddr dep_node; ///< (the head node of its list of dependents sits here, at +0x20)
    GuestAddr dep_first;
    GuestAddr dep_last;
    uint16_t runners;   ///< bits 0-6 to start, 7-13 running, 14 one finished, 15 complete at the last running one
    uint8_t _3a[2];
    int32_t waiting;    ///< jobs it still waits for
};
static_assert(offsetof(Job, object) == 0x00);
static_assert(offsetof(Job, level) == 0x0c);
static_assert(offsetof(Job, kind) == 0x0e);
static_assert(offsetof(Job, lane) == 0x10);
static_assert(offsetof(Job, id) == 0x14);
static_assert(offsetof(Job, hook) == 0x18);
static_assert(offsetof(Job, dep_node) == 0x20);
static_assert(offsetof(Job, dep_first) == 0x28);
static_assert(offsetof(Job, dep_last) == 0x30);
static_assert(offsetof(Job, runners) == 0x38);
static_assert(offsetof(Job, waiting) == 0x3c);
static_assert(sizeof(Job) == 0x40);

/// A node of a job's list of dependents.
struct DepNode {
    GuestAddr job;
    GuestAddr next;
};
static_assert(offsetof(DepNode, next) == 8);

/// A stage's scheduler.
struct Scheduler {
    uint8_t _0[48];
    int32_t context_count;
    uint32_t context_room;
    GuestAddr contexts;   ///< an array of dispatch contexts' addresses
    QueueLevel levels[3];
    uint8_t _130[16];
    Job end_job;          ///< its completion ends the stage
    uint32_t poll_lane;   ///< a context of this lane polls with timed sleeps instead of sleeping on its event
    uint32_t flags;       ///< bit 1: the stage is over
};
static_assert(offsetof(Scheduler, context_count) == 48);
static_assert(offsetof(Scheduler, context_room) == 52);
static_assert(offsetof(Scheduler, contexts) == 56);
static_assert(offsetof(Scheduler, levels) == 64);
static_assert(offsetof(Scheduler, end_job) == 0x140);
static_assert(offsetof(Scheduler, poll_lane) == 0x180);
static_assert(offsetof(Scheduler, flags) == 388);

/// The actor manager's table of who runs what, read in place by gameplay code at hundreds of sites.
struct IdentityTable {
    uint8_t _0[32];
    uint32_t thread_ids[6];
    GuestAddr current_actor[6];
    uint32_t seventh_id;
    uint8_t _6c[4];
    GuestAddr seventh_actor;
};
static_assert(offsetof(IdentityTable, thread_ids) == 32);
static_assert(offsetof(IdentityTable, current_actor) == 56);
static_assert(offsetof(IdentityTable, seventh_id) == 104);
static_assert(offsetof(IdentityTable, seventh_actor) == 112);

}   // namespace guest

enum class EntryRole : uint8_t { Setup, Stage, Worker, Dispatch, Group, Frame, Consumer };

struct GuestEntry {
    GuestAddr address;
    EntryRole role;
    bool native;   ///< answered by the native job system today
    const char* what;
};

/// The game's job system as the gameplay code reaches it (supported build only).
std::span<const GuestEntry> job_entries();

/// Guest functions the bridge calls or answers.
inline constexpr GuestAddr kWorkerMessage = 0x8004aae0;   ///< answered: on_worker_message
inline constexpr GuestAddr kDispatcher = 0x8004ab40;      ///< called: runs a scheduler's jobs on a dispatch context
inline constexpr GuestAddr kEventSignal = 0x85101620;     ///< called: signals an event object

/// How the stages' work reaches the threads.
enum class Mode : uint8_t {
    Off,       ///< the game's own threads, untouched
    Through,   ///< each stage a job of the native job system, run by the thread that waits for it
    Pool       ///< each stage a job run by a worker of the native pool, under the executor's identity
};

class Dispatcher;

class JobBridge {
public:
    struct Stats {
        std::atomic<uint64_t> messages{0};   ///< worker messages answered
        std::atomic<uint64_t> stages{0};     ///< of them, a stage's work run as a job
        std::atomic<uint64_t> moved{0};      ///< of those, run by another host thread than the worker's own
    };

    /// `dispatcher`: who runs a stage's work (null: the translated dispatcher, called).
    JobBridge(GuestRuntime& runtime, jobs::JobSystem& jobs, Mode mode, Dispatcher* dispatcher = nullptr);

    /// Answers kWorkerMessage(worker, message) on the worker's own thread: message 1 with a scheduler set runs the
    /// stage's dispatcher on the worker's dispatch context, then signals the worker's end.
    void on_worker_message(GuestAddr worker, uint64_t message);

    Mode mode() const { return mode_; }
    const Stats& stats() const { return stats_; }

private:
    struct Stage;
    static void run_stage(void* arg, const jobs::JobContext& ctx);
    void dispatch(GuestThread as, GuestAddr scheduler, GuestAddr context);

    GuestRuntime& runtime_;
    jobs::JobSystem& jobs_;
    Mode mode_;
    Dispatcher* dispatcher_;
    Stats stats_;
};

struct Readiness {
    bool ready;            ///< the stages' work goes through the native job system
    unsigned native;       ///< entry points answered natively
    unsigned total;
    const char* missing;   ///< one line per thing still to do, newline separated
};
Readiness ready(const JobBridge* bridge);

}   // namespace hl::bridge
