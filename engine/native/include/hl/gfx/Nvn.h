// HeyListen native engine: the NVN procedures the graphics front end calls, and the sink that calls them
// (docs/native-engine/gfx-record-job.md, section 8).
//
// The front end's commands (Commands.h) name NVN procedures by Proc, this file's numbering. Native code reaches the
// procedures themselves only through the table of entry points the OS layer hands over (NvnEntryPoints): the same
// procedures the game calls through its own globals, called with the same argument registers. NvnSink is the sink that
// sends each command to that table: the seam's first real use. A later sink can write the Vulkan back end's records
// directly instead, with no change to the code that emits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "hl/gfx/Commands.h"

namespace hl::gfx {

/// The procedures, by their NVN names without the "nvn" prefix (proc_name gives the full name).
enum class Proc : uint32_t {
    BlendStateSetDefaults,
    BlendStateSetBlendTarget,
    BlendStateSetBlendFunc,
    BlendStateSetBlendEquation,
    ColorStateSetDefaults,
    ColorStateSetBlendEnable,
    ColorStateSetLogicOp,
    ChannelMaskStateSetChannelMask,
    DepthStencilStateSetDepthTestEnable,
    DepthStencilStateSetDepthWriteEnable,
    DepthStencilStateSetDepthFunc,
    DepthStencilStateSetStencilTestEnable,
    DepthStencilStateSetStencilFunc,
    DepthStencilStateSetStencilOp,
    PolygonStateSetCullFace,
    PolygonStateSetFrontFace,
    PolygonStateSetPolygonMode,
    PolygonStateSetPolygonOffsetEnables,
    CommandBufferBindBlendState,
    CommandBufferBindColorState,
    CommandBufferSetBlendColor,
    CommandBufferBindChannelMaskState,
    CommandBufferBindDepthStencilState,
    CommandBufferSetStencilValueMask,
    CommandBufferSetStencilMask,
    CommandBufferSetStencilRef,
    CommandBufferBindPolygonState,
    CommandBufferSetPolygonOffsetClamp,
    CommandBufferSetDepthClamp,
    // the sub-mesh submission (SubmeshDraw.h)
    CommandBufferSetGeometryProcessingMode,
    CommandBufferCallCommands,
    CommandBufferDrawMeshTasks,
    BufferGetAddress,
    CommandBufferDrawElementsInstanced,
    // the shared preparation (Prepare.h)
    CommandBufferBindProgram,
    CommandBufferBindUniformBuffer,
    Count
};
inline constexpr uint32_t kProcCount = (uint32_t)Proc::Count;

/// The full NVN name ("nvnBlendStateSetDefaults"); null past the last.
const char* proc_name(uint32_t proc);
/// How many integer and float argument registers the procedure reads (the rest are not its arguments), and whether it is
/// a query: a procedure that only returns a value (in x0) and changes nothing, so that native code asks it while it
/// derives (NvnEntryPoints::call's result) and the stream carries it only for the comparison with the translation.
struct ProcArgs {
    uint8_t ints, floats;
    bool query = false;
};
ProcArgs proc_args(uint32_t proc);

inline Command command(Proc p, uint64_t x0, uint64_t x1 = 0, uint64_t x2 = 0, uint64_t x3 = 0, uint64_t x4 = 0,
                       uint64_t x5 = 0, uint64_t x6 = 0, uint64_t x7 = 0) {
    Command c;
    c.op = (uint32_t)p;
    c.x[0] = x0, c.x[1] = x1, c.x[2] = x2, c.x[3] = x3, c.x[4] = x4, c.x[5] = x5, c.x[6] = x6, c.x[7] = x7;
    return c;
}

/// The same call: the same procedure and the same value in each of its argument registers (pointers compared as values).
bool same_call(const Command& a, const Command& b);
/// One command in words, for a log ("nvnCommandBufferSetStencilRef(0x1234, 3, 0x80)"); an op past the table is printed
/// as a number.
std::string describe(const Command& c);

/// The OS layer's NVN entry points: call(user, proc, x, nx, f, nf) calls procedure `proc` on the calling thread's guest
/// context with x[0..nx) in x0.. and f[0..nf) in the low 32 bits of s0.., and returns x0 after it (what a query returns;
/// the other procedures return nothing the front end reads).
struct NvnEntryPoints {
    void* user = nullptr;
    uint64_t (*call)(void* user, uint32_t proc, const uint64_t* x, uint32_t nx, const uint32_t* f, uint32_t nf) = nullptr;

    uint64_t ask(const Command& c) const {
        const ProcArgs a = proc_args(c.op);
        return call(user, c.op, c.x, a.ints, c.f, a.floats);
    }
};

/// Sends every command to the NVN entry points, in order. A query is not sent again: its answer was taken while deriving
/// (it changes nothing, so asking it a second time would only cost a call).
class NvnSink final : public CommandSink {
public:
    explicit NvnSink(const NvnEntryPoints& nvn) : nvn_(nvn) {}
    void emit(const Command& c) override {
        if (!proc_args(c.op).query) nvn_.ask(c);
    }

private:
    const NvnEntryPoints& nvn_;
};

}   // namespace hl::gfx
