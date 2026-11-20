// (heylisten) What the performance summary (perf_report.h) takes of a frame, measured on the thread that presents
// (perf_report.cpp perf_frame_shown, the parts' scopes in perf_parts.h): the frame's ms, its four parts, the GPU's own
// time and the replay's draws left out (a program or pipeline not ready: skipped rather than waited for).
#pragma once

#include <cstdint>

namespace mkwvk::fps_stats {

enum Part { kGame, kRender, kGpuWait, kPace, kParts };

struct Frame {
    float ms = 0;               // (present to present)
    float part[kParts] = {};    // (ms: the game's step, the renderer's work, the waits for the GPU, the pacing sleep)
    float gpu_ms = 0;           // (the GPU's own time since the frame before, 0 when not timed)
    uint16_t left_out = 0;      // (the replay's draws left out this frame)
};

}   // namespace mkwvk::fps_stats
