// HeyListen native engine: the game's stage protocol around the dispatcher, natively, in place on the game's own worker
// sets: the registration of a stage's workers, the owner's wait for them, and the frame's last stage.
//
// Written from a reading of the guest's three functions (docs/native-engine/jobs-boundary.md, "The stages' protocol"):
// nothing here is game code. They are answered together, through entry gates on the translated functions, because they
// share state that cannot be half native (the registered mask and count, the "sent its message" bit, the done events).
// Every word the translated code still reads is written in place, with the same atomic operations in the same order:
// the worker set's fields, the scheduler's list of contexts, the workers' context words and scheduler, the lane status
// word, the actor manager's state byte. The guest's event objects stay the medium of every hand-off (the forward, the
// pushers and the module system's twins of these functions signal and wait on them too), and the guest's own message
// queue starts a worker.
//
// The hand-off policy is each one function here, with counters: how a registered worker is started (start), how a thread
// waits for a worker's end (await_done), what a stage's owner runs meanwhile (dispatch). Check mode counts the protocol's
// invariants (Stats) and compares the registration's writes with the translated registration's.
//
// The module system has twins of the registration and the wait (jobs-boundary.md, "The module system's twins"): the same
// worker set and worker words, its group's list of contexts at other offsets, the group's heap seeded inside the
// registration, the message always sent, and the owner's part run by the module system's job group. They are answered
// here by the same code, with what differs as data.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "hl/bridge/JobBridge.h"

namespace hl::bridge {

class Dispatcher;
class JobGroup;

/// Guest functions and data the stage protocol uses (the supported build's; docs/native-engine/jobs-boundary.md).
inline constexpr GuestAddr kStageRegister = 0x806b0260;   ///< answered: register_workers (worker set, send)
inline constexpr GuestAddr kStageWait = 0x806afc40;       ///< answered: wait (worker set)
inline constexpr GuestAddr kLastStage = 0x83acfb90;       ///< answered: last_stage (actor manager)
inline constexpr GuestAddr kModuleRegister = 0x800711c0;  ///< answered: register_module (worker set, group)
inline constexpr GuestAddr kModuleWait = 0x80070b20;      ///< answered: wait_module (worker set)
inline constexpr GuestAddr kThreadLocal = 0x83c61cf0;     ///< a thread-local value by key
inline constexpr GuestAddr kCurrentCore = 0x83c61f60;     ///< the OS's current core
inline constexpr GuestAddr kCoreKey = 0x84e9f7ac;         ///< the key of the thread-local "core plus one" (32 bits)
inline constexpr GuestAddr kEventClear = 0x83c62ce0;
inline constexpr GuestAddr kQueueSend = 0x83c62d20;       ///< a message into a thread's message queue
inline constexpr GuestAddr kUsualMessageSlot = 0x84cab408;   ///< holds the workers' usual message function

namespace guest {

/// A stage's holder (the actor manager's, at its +0x80).
struct StageHolder {
    uint8_t _0[8];
    GuestAddr scheduler;
    GuestAddr set;   ///< the worker set
};
static_assert(offsetof(StageHolder, scheduler) == 8);
static_assert(offsetof(StageHolder, set) == 0x10);

/// The actor manager, as the last stage uses it.
struct ActorManager {
    uint8_t _0[0x80];
    GuestAddr holder;
    uint8_t _88[0x44c - 0x88];
    uint32_t state;   ///< low byte 2 while a stage begun by the second and third begin forms runs
};
static_assert(offsetof(ActorManager, holder) == 0x80);
static_assert(offsetof(ActorManager, state) == 0x44c);

}   // namespace guest

class Stages {
public:
    struct Options {
        bool check = false;   ///< count the invariants below and compare the registration with the translated one
    };
    // Counters (relaxed). The invariants, counted with `check` (each should stay 0):
    //   overlap: a set registered again while its previous stage was not over (not waited for, or its last stage not ended)
    //   unstarted: a registered worker waited for that was never sent its message; or a last stage ended with one unsent
    //   double_start: a worker (or the set's own context) started twice in one stage
    //   still_in: a worker's done event taken while its context still says it is inside its dispatcher
    //   not_over: a stage's wait returned (or its last stage ended) with the scheduler's stage not over
    //   changed: the registered mask or count changed between the registration and the end of the wait
    //   below_zero: the last stage lowered the registered count below zero
    //   reg_diff: the registration's writes differ from the translated registration's on the same set
    // and, as observations: untracked (a wait or last stage of a set registered before the gates were on), lane_drift (the
    // owner's lane byte differs after its wait from before its registration: the guest's floor at 0 and ceiling at 15
    // around the owner's waits can do that, when the owner is not counted in its lane).
    //
    // The module system's twins (register_module, wait_module) share the invariants and the records (their sets are other
    // sets). Their own counters: mod_* beside the actors' ones; mod_checked / mod_diff the twin registration compared with
    // the translated twin, mod_skipped the registrations the check could not compare without side effects (a root job
    // with a pre-push hook, whose hook the translated twin would call a second time; a worker whose message function is
    // not the usual one, which the translated twin would start; a heap larger than the check's copy).
    struct Stats {
        std::atomic<uint64_t> registrations{0}, own_registered{0}, workers_registered{0};
        std::atomic<uint64_t> started{0}, started_virtual{0}, not_queued{0};
        std::atomic<uint64_t> waits{0}, waits_empty{0}, owner_dispatches{0}, done_waits{0};
        std::atomic<uint64_t> last_calls{0}, last_empty{0}, last_workers{0}, last_own{0}, last_none{0}, last_ends{0};
        std::atomic<uint64_t> mod_registrations{0}, mod_empty{0}, mod_seeded{0}, mod_hooks{0};
        std::atomic<uint64_t> mod_waits{0}, mod_waits_empty{0}, mod_owner_runs{0};
        std::atomic<uint64_t> overlap{0}, unstarted{0}, double_start{0}, still_in{0}, not_over{0}, changed{0};
        std::atomic<uint64_t> below_zero{0}, reg_checked{0}, reg_diff{0}, untracked{0}, lane_drift{0}, violations_logged{0};
        std::atomic<uint64_t> mod_checked{0}, mod_diff{0}, mod_skipped{0};
    };

    /// `dispatcher`: who runs the owner's part of a stage (null: the translated dispatcher, called). `group`: the native
    /// module system's group, which seeds a twin registration's heap and runs the owner's part of a twin wait (null: the
    /// twins are not answered).
    Stages(GuestRuntime& runtime, Dispatcher* dispatcher, JobGroup* group, Options options);

    /// The registration's entry gate, with the translated function's arguments, on the calling thread (`as`, its own
    /// guest context): answers and returns true; or returns false, to let the translated body run, when this host
    /// thread's bypass is set (the check calls the translated registration through its own gate). The wait and the last
    /// stage have nothing to let through: their gates call wait() and last_stage().
    bool register_gate(GuestThread as, GuestAddr set, uint64_t send);

    /// 0x806b0260: registers the stage's workers with the set's scheduler; with `send` bit 0, sends them their message.
    void register_workers(GuestThread as, GuestAddr set, uint64_t send);
    /// 0x806afc40: the owner's wait for the set's registered workers (running the stage itself first when it takes part).
    void wait(GuestThread as, GuestAddr set) { wait(as, set, false); }
    /// 0x83acfb90: the frame's last stage, on one core's thread: starts and waits for that core's registered worker (or
    /// runs the set's own context), and the thread that brings the registered count to zero ends the stage.
    void last_stage(GuestThread as, GuestAddr manager);

    /// The twin registration's entry gate (as register_gate: false lets the translated twin run for the check).
    bool register_module_gate(GuestThread as, GuestAddr set, GuestAddr group);
    /// 0x800711c0: the module system's registration: the set's stage put on `group` (none when the group's graph is made
    /// and has no job), the group's heap seeded with its roots, the workers registered as register_workers does on the
    /// group's own list, and always sent their message.
    void register_module(GuestThread as, GuestAddr set, GuestAddr group);
    /// 0x80070b20: the module system's wait, as wait() with the owner's part run by the job group.
    void wait_module(GuestThread as, GuestAddr set) { wait(as, set, true); }

    const Options& options() const { return options_; }
    const Stats& stats() const { return stats_; }
    /// One line of counters. Returns the length written.
    int report(char* out, size_t cap) const;

private:
    template <class T>
    T* at(GuestAddr a) const { return static_cast<T*>(runtime_.ptr(a)); }
    uint64_t call(GuestThread as, GuestAddr fn, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0);

    // Where a stage's scheduler keeps its list of contexts and its flags: the actors' scheduler (guest::Scheduler) and the
    // module system's group (guest::Group) have the same words at other offsets.
    struct ListAt {
        GuestAddr count, room, array, flags;
    };
    static ListAt list_at(GuestAddr scheduler, bool module);

    void wait(GuestThread as, GuestAddr set, bool module);

    // the policy
    void start(GuestThread as, GuestAddr worker);
    void await_done(GuestThread as, GuestAddr set, GuestAddr worker, int64_t lane);
    void dispatch(GuestThread as, GuestAddr scheduler, GuestAddr context, bool module);

    uint32_t core(GuestThread as);
    GuestAddr worker_at(GuestAddr set, int32_t index) const;
    void append(const ListAt& list, GuestAddr context);
    void write_registration(GuestThread as, GuestAddr set, bool module);
    void send_messages(GuestThread as, GuestAddr set);
    void end_last_stage(GuestAddr manager);

    // checking
    struct Record {   // a set's stage, as its registration made it
        GuestAddr scheduler = 0;
        uint32_t mask = 0, count = 0, started = 0;
        bool own = false, own_started = false, last = false, open = false, module = false;
        uint64_t lanes = 0;   // the owner's lane and its byte of the lane status word before the registration
    };
    struct Snapshot;   // the registration's writes, read back
    void snapshot(GuestAddr set, bool module, Snapshot& out) const;
    bool compare_module(GuestThread as, GuestAddr set, GuestAddr group, Snapshot& translated);
    uint64_t lane_byte(GuestThread as, GuestAddr set);
    void note_registered(GuestAddr set, uint64_t send, uint64_t lanes, bool module);
    bool note_started(GuestAddr set, int32_t index);   // index -1: the own context; false when started before
    void check_done(GuestAddr worker);
    void check_stage_end(GuestAddr set, bool last, uint64_t lanes);
    void violation(const char* what, GuestAddr at);

    GuestRuntime& runtime_;
    Dispatcher* dispatcher_;
    JobGroup* group_;
    Options options_;
    Stats stats_;
    std::mutex records_mu_;   // (checking only)
    std::unordered_map<GuestAddr, Record> records_;
};

}   // namespace hl::bridge
