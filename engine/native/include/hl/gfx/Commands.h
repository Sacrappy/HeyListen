// HeyListen native engine: the graphics front end's output, one seam (docs/native-engine/gfx-record-job.md).
//
// Everything the front end records leaves it as a stream of commands into one sink: a draw or a state change is one
// Command, an NVN procedure with its argument registers. A native part of the pass emits into a sink: NvnSink (Nvn.h)
// forwards each command to the OS layer's NVN entry points, the checks' recordings keep them to compare (PassCheck.h,
// StateCheck.h); later a sink can write the Vulkan back end's records directly, with no change to the code that emits.
#pragma once

#include <cstdint>

namespace hl::gfx {

/// One draw or state change. `op` numbers the procedure: Nvn.h's Proc for what native code emits and the call-by-call
/// check compares; the OS layer's order of procedures (the order the game asked for them) in the pass check's
/// recordings. `x` are the integer argument registers x0..x7, the command buffer or object first (an indexed instanced
/// draw takes all eight); `f` the low 32 bits of the first float argument registers. A procedure takes fewer than these:
/// the rest is whatever the registers held, and comparisons that need exact arguments must know each procedure's count
/// (proc_args).
struct Command {
    uint32_t op = 0;
    uint64_t x[8] = {};
    uint32_t f[4] = {};
};

/// Where commands go.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual void emit(const Command& command) = 0;
};

}   // namespace hl::gfx
