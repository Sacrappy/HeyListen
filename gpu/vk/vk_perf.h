// The machine numbers of the full FPS overlay (F10, vk_menu.cpp): CPU, memory and GPU load, sampled twice a second on a
// thread of their own, and the frame times of the last two seconds for the graph. A value the machine does not give is negative.
// Adapted from the oot-pc port's runtime/perfmon.c (MIT, Copyright (c) 2026 Jeff Aigner and the totk-pc-recomp contributors, after
// SavageWindRecomp's perfmon, MIT, Copyright (c) 2026 Revivedx), reduced to what this back end can read.
#pragma once

#include <stdint.h>

namespace mkwvk {

constexpr int kPerfHistory = 120;   // frame times kept for the graph

struct PerfStats {
    float fps, frame_ms_avg, frame_ms_max;
    float frame_ms[kPerfHistory];   // oldest first; frames not seen yet read 0
    char cpu_name[64];
    float cpu_total_pct, cpu_game_pct;
    float ram_used_mb, ram_total_mb, ram_game_mb;
    char gpu_name[96];
    float gpu_pct, gpu_temp_c, gpu_power_w, gpu_clock_mhz, vram_used_mb, vram_total_mb;
};

/// Starts (on) or stops (off) the sampling thread. gpu_name is the Vulkan device's name, to find its NVML twin.
void perf_sampling(bool on, const char* gpu_name, float vram_total_mb);
/// A frame was shown (any thread).
void perf_note_frame();
/// The latest numbers.
void perf_snapshot(PerfStats* out);

} // namespace mkwvk
