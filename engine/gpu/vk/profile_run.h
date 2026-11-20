// (heylisten) The detailed performance profile's runtime (profile_report.h has the rules and the JSON): the CPU sampler thread and the
// GPU's per-pass timestamps, both adding into per-map counts that perf_report.cpp takes out as a batch every few minutes.
//
// CPU (Windows): a thread samples, at HL_PROFILE_HZ (250 by default), every thread of the process that ran since its last sample:
// SuspendThread, GetThreadContext, ResumeThread, and nothing else between them (no allocation, no log, no lock). The program counters
// are classified into modules once the thread runs again. A sample waits in a ring until its frame closes; the frames that closed as
// hitches count their samples into the hitch counters too. Linux has no SuspendThread: the sampler is not built there.
//
// GPU: a pair of timestamps around each render pass, compute dispatch and renderer-owned pass the recordings make, in a query pool of
// its own, read back when the recording's slot comes round again (the wait the renderer makes anyway: never an added stall).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "profile_report.h"
#include "vk_internal.h"

namespace mkwvk {

/// Starts the profile (the row's rules are perf_report.cpp's): the sampler thread and the pass timestamps. False when neither can run
bool profile_start(int hz);
/// Stops them; what was counted since the last take stays for one more take
void profile_stop();
bool profile_running();
/// The map the frames and samples belong to now (perf_report.cpp, about once a second)
void profile_map(const std::string& name);
/// A frame closed (its ms, and whether it was a hitch: perf_report.h is_hitch's rule); the frame's samples settle
void profile_frame(double ms, bool hitch);
/// The maps with anything in them, each reset to empty. `final`: the process is ending (a thread may have died holding the lock: it is only tried)
std::vector<profile_report::MapProfile> profile_take(bool final = false);
/// The calling thread's role, for threads whose names say nothing (the presenting thread: profile_report::Role)
void profile_register_thread(int role);
/// The executable's build id (the host's), 0 unknown
uint64_t profile_build_id();
/// One line of what the profile did: samples, the sampler's own time, passes timed (for the log)
std::string profile_stats();

/// The pass timestamps (vk_gprof.cpp forwards to them when RX_VK_PASS_TIMING is off)
bool tprof_on();
void tprof_bind(int slot, VkCommandBuffer cmd);
bool tprof_begin(VkCommandBuffer cmd);
void tprof_end(VkCommandBuffer cmd, int kind, uint64_t key);
void tprof_end_label(VkCommandBuffer cmd, const std::string& label);
void tprof_collect(int slot);
/// The window's pixels, for the pass kinds' heuristic
uint64_t tprof_output_pixels();

}   // namespace mkwvk
