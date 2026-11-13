// HeyListen native engine: the guest runtime as the OS layer hands it over (hl/c/hl_bridge.h's hl_guest_host), for every
// module's C surface. One adapter, so that a change to the runtime's interface is made in one place.
#pragma once

#include <cstdint>

#include "hl/bridge/JobBridge.h"
#include "hl/c/hl_bridge.h"

namespace hl::c {

struct HostRuntime : hl::bridge::GuestRuntime {
    hl_guest_host host{};

    void* ptr(hl::bridge::GuestAddr at) const override { return host.ptr(host.user, at); }
    hl::bridge::GuestThread current() const override { return host.current(host.user); }
    uint64_t call(hl::bridge::GuestThread as, hl::bridge::GuestAddr fn, uint64_t a0, uint64_t a1) override {
        return host.call(host.user, as, fn, a0, a1);
    }
    uint64_t call_with(hl::bridge::GuestThread as, hl::bridge::GuestAddr fn, const uint64_t* args, unsigned nargs,
                       const void* frame, unsigned frame_size) override {
        return host.call_with(host.user, as, fn, args, nargs, frame, frame_size);
    }
    void log(const char* line) override { host.log(host.user, line); }
};

/// Whether the OS layer filled in everything the adapter calls.
inline bool complete(const hl_guest_host* host) {
    return host && host->ptr && host->current && host->call && host->call_with && host->log;
}

}   // namespace hl::c
