// HeyListen native engine: where the native job system meets the gameplay code that is still run from the user's local
// translation (docs/native-engine/jobs-boundary.md has the reading behind every number here).
//
// Nothing in this file is game code or game data. It names places: the guest addresses of the game's own job system's
// entry points in the one supported build (the build the builder accepts), and the offsets of the few structures that
// gameplay code reads without calling anything. Taking the subsystem over means two things: each entry point answered by
// the native job system instead of the translated function, and those structures kept as the gameplay code expects to
// find them. NOT switched on: ready() says what is missing.
#pragma once

#include <cstdint>
#include <span>

#include "hl/jobs/GuestLanes.h"

namespace hl::bridge {

using GuestAddr = uint64_t;

/// What the bridge needs from the layer that runs guest code (the OS layer; none of it is in the native tree).
struct GuestRuntime {
    virtual ~GuestRuntime() = default;
    virtual uint64_t read64(GuestAddr at) const = 0;
    virtual uint32_t read32(GuestAddr at) const = 0;
    virtual void write64(GuestAddr at, uint64_t value) = 0;
    virtual void write32(GuestAddr at, uint32_t value) = 0;
    /// Binds the calling native thread to executor `lane`'s guest context (stack, thread-local values, core number) and
    /// gives it back.
    virtual void bind_lane(int lane) = 0;
    virtual void unbind_lane(int lane) = 0;
    /// Calls the guest function at `fn` with one argument on the calling thread's bound guest context.
    virtual void call(GuestAddr fn, uint64_t arg) = 0;
};

enum class EntryRole : uint8_t { Setup, Stage, Worker, Dispatch, Group, Frame, Consumer };

struct GuestEntry {
    GuestAddr address;
    EntryRole role;
    const char* what;
};

/// The game's job system as the gameplay code reaches it (supported build only).
std::span<const GuestEntry> job_entries();

/// Offsets inside the structures gameplay code reads directly. Bytes from the start of each object.
namespace layout {
// a worker set (the six executors are one)
namespace worker_set {
inline constexpr uint32_t count = 0, workers = 8, own_context = 16, owner_core = 232, lane_status = 240, stage_cores = 248,
                          registered_count = 252, registered_mask = 256, flags = 260, scheduler = 264;
}
// a worker's thread object
namespace worker {
inline constexpr uint32_t queue = 40, queue_count = 160, state = 172, core_mask = 176, context = 216, done_event = 488,
                          core = 504, scheduler = 512;
}
// a dispatch context (a worker's at worker::context, a set's own at worker_set::own_context)
namespace context {
inline constexpr uint32_t state = 8, flags = 16, wake_event = 176, lane = 216, lane_status = 224;
}
// a stage's scheduler
namespace scheduler {
inline constexpr uint32_t context_count = 48, context_room = 52, contexts = 56, queues = 64, flags = 388;
inline constexpr uint32_t queue_stride = 80, queue_levels = 3;
}
// the actor manager's table of who runs what: the six executors' thread ids, each one's current actor, a seventh thread
namespace identity {
inline constexpr uint32_t table = 128;   // (pointer, from the manager)
inline constexpr uint32_t thread_ids = 32, current_actor = 56, seventh_id = 104, seventh_actor = 112;
inline constexpr uint32_t manager_state = 1100, manager_thread = 1104;
}
}   // namespace layout

/// Takes a lane for the job system by binding the worker to that executor's guest context.
class LaneBinder final : public jobs::GuestLaneHost {
public:
    explicit LaneBinder(GuestRuntime& runtime) : runtime_(runtime) {}
    void enter(int lane, uint32_t) override { runtime_.bind_lane(lane); }
    void leave(int lane, uint32_t) override { runtime_.unbind_lane(lane); }

private:
    GuestRuntime& runtime_;
};

struct Readiness {
    bool ready;
    const char* missing;   ///< one line per thing still to do, newline separated
};
/// Whether the native job system can take the game's over. It cannot yet.
Readiness ready();

}   // namespace hl::bridge
