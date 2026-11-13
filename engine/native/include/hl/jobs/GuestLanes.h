// HeyListen native engine: the lanes guest jobs run under.
//
// The gameplay code that still runs from the user's local translation was built for exactly six job threads, and it
// knows them by identity: a thread id and a core number each, looked up in tables of six in hundreds of places, with
// lock-free data kept per core. It cannot be given a seventh thread, and two threads must never share an identity at
// the same time (docs/native-engine/jobs-boundary.md).
//
// A lane is one of those six identities. A worker of the native pool takes a free lane before it runs a guest job and
// gives it back after: at most six guest jobs run at once, each under an identity of its own, on whichever native
// workers are free. Native jobs need no lane, so the engine's own subsystems are not held to six.
#pragma once

#include <cstdint>

namespace hl::jobs {

inline constexpr int kLaneCount = 6;
inline constexpr uint8_t kAllLanes = 0x3f;

/// What taking a lane means on the game's side: the bridge binds the calling native thread to that executor's guest
/// context (its stack, its thread-local values, its core number) and unbinds it. Called by the worker itself, around the
/// job, outside the job system's lock.
struct GuestLaneHost {
    virtual ~GuestLaneHost() = default;
    virtual void enter(int lane, uint32_t worker) = 0;
    virtual void leave(int lane, uint32_t worker) = 0;
};

/// The six lanes' state. Not thread-safe by itself: the job system changes it under its own lock.
class GuestLanes {
public:
    /// A free lane among `allowed`, taken; -1 when none is free.
    int take(uint8_t allowed) {
        const uint8_t free = (uint8_t)(allowed & kAllLanes & ~busy_);
        if (!free) return -1;
        int lane = 0;
        while (!(free >> lane & 1)) lane++;
        busy_ = (uint8_t)(busy_ | 1u << lane);
        unsigned n = 0;
        for (int i = 0; i < kLaneCount; i++) n += busy_ >> i & 1;
        if (n > most_) most_ = n;
        return lane;
    }
    void give_back(int lane) { busy_ = (uint8_t)(busy_ & ~(1u << lane)); }
    uint8_t busy() const { return busy_; }
    unsigned most_at_once() const { return most_; }

private:
    uint8_t busy_ = 0;
    unsigned most_ = 0;
};

}   // namespace hl::jobs
