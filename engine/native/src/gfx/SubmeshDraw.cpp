// HeyListen native engine: the sub-mesh submission of the game's draw path (hl/gfx/SubmeshDraw.h).
//
// Every value here is the game function's, bit for bit (the call-by-call check in the game compares each call's NVN calls
// and the bytes it writes): 32-bit arithmetic where the game's is, the index count's offset difference in 64 bits.
#include "hl/gfx/SubmeshDraw.h"

#include "hl/gfx/Nvn.h"

namespace hl::gfx {

PrologueOutputs SubmeshDraw::prologue(const PrologueInputs& in) {
    PrologueOutputs out;
    out.applied_mode = in.applied_mode;
    out.mode_bits = in.mode_bits;
    out.in_range = in.sub_mesh < (int32_t)in.sub_meshes;
    if (!out.in_range || in.geometry_mode == in.applied_mode) return out;
    out.mode_changed = true;
    out.applied_mode = in.geometry_mode;
    // (bit 14 says whether the mode is 1; the game leaves the word alone when it already says so)
    out.mode_bits = (in.mode_bits & ~0x4000u) | (in.geometry_mode == 1 ? 0x4000u : 0u);
    return out;
}

void SubmeshDraw::emit_prologue(const PrologueOutputs& out, uint64_t command_buffer, CommandSink& sink) {
    if (out.mode_changed)
        sink.emit(command(Proc::CommandBufferSetGeometryProcessingMode, command_buffer, out.applied_mode));
}

Submit SubmeshDraw::path(const SubmitInputs& in) {
    if (!(in.prepared & 1)) return Submit::None;
    if (in.instances <= 1 && (in.recorded & 1) && ((in.mode_bits >> 5) & 1)) return Submit::CallCommands;
    switch (in.mesh_kind) {
    case 0: return Submit::Elements;
    case 1:
    case 2: return Submit::MeshTasks;
    default: return Submit::None;
    }
}

SubmitOutputs SubmeshDraw::decide(const SubmitInputs& in, const IndexedInputs* indexed) {
    SubmitOutputs out;
    out.how = path(in);
    if (!(in.prepared & 1)) return out;
    out.counted = true;
    out.triangles = in.triangles + in.sub_mesh_triangles;
    out.marked = out.how == Submit::CallCommands;
    out.instances = in.instances > 1 ? in.instances : 1u;
    out.tasks = in.mesh_kind == 2 ? (in.mesh_tasks + 31u) >> 5 : in.mesh_tasks;
    if (out.how == Submit::Elements && indexed) {
        const int64_t span = (int64_t)((uint64_t)indexed->last_offset - (uint64_t)indexed->first_offset);
        out.count = indexed->last_count + (uint32_t)(span >> (indexed->index_shift & 63));
    }
    return out;
}

void SubmeshDraw::emit(const SubmitOutputs& out, const IndexedInputs* indexed, uint64_t address, const SubmitHandles& at,
                       CommandSink& sink) {
    switch (out.how) {
    case Submit::CallCommands:
        sink.emit(command(Proc::CommandBufferCallCommands, at.command_buffer, 1, at.command_list));
        break;
    case Submit::Elements:
        sink.emit(command(Proc::BufferGetAddress, indexed->buffer));
        sink.emit(command(Proc::CommandBufferDrawElementsInstanced, at.command_buffer, indexed->mode, indexed->type,
                          out.count, address + indexed->first_offset, indexed->base_vertex, 0, out.instances));
        break;
    case Submit::MeshTasks:
        sink.emit(command(Proc::CommandBufferDrawMeshTasks, at.command_buffer, 0, out.tasks));
        break;
    case Submit::None: break;
    }
}

}   // namespace hl::gfx
