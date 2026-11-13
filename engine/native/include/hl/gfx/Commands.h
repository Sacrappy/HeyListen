// HeyListen native engine: the graphics front end's output, one seam (docs/native-engine/gfx-record-job.md).
//
// Everything the front end records leaves it as a stream of commands into one sink: a draw or a state change is one
// Command. Today the commands are the NVN procedures the game's recording pass calls, as the OS layer numbers them in its
// table of NVN entry points, with their argument registers; the only sink in use is the check's recording (PassCheck.h),
// fed from the OS layer's NVN layer while the translated pass runs. A native part of the pass will emit into the same
// sink: first one that forwards to the NVN entry points, later one that writes the Vulkan back end's records directly,
// with no change to the code that emits.
#pragma once

#include <cstdint>

namespace hl::gfx {

/// One draw or state change. `op` numbers the procedure in the OS layer's table of NVN entry points (in the order the
/// game asked for them); `x` are the integer argument registers, the command buffer or object first; `f` the low 32 bits
/// of the first float argument registers. A procedure takes fewer than these: the rest is whatever the registers held,
/// and comparisons that need exact arguments must know each procedure's count.
struct Command {
    uint32_t op = 0;
    uint64_t x[6] = {};
    uint32_t f[4] = {};
};

/// Where commands go.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual void emit(const Command& command) = 0;
};

}   // namespace hl::gfx
