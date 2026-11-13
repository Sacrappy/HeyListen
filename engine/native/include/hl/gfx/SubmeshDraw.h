// HeyListen native engine: the sub-mesh submission of the game's draw path (docs/native-engine/gfx-record-job.md,
// section 9).
//
// A draw of the scene pass submits its sub-meshes one by one. For one sub-mesh the game's function first applies the
// geometry processing mode the model needs (the prologue), then calls the shared preparation (program, buffers, textures,
// render state: guest code, called back), then submits the sub-mesh: a pre-recorded command list called into the
// command buffer, an indexed instanced draw, or mesh-shader tasks, and counts its triangles in the block.
//
// SubmeshDraw is that function on its direct paths (the cluster, indirect and inline-write paths stay translated; the
// bridge decides which calls are direct). Each part is decide() (pure: inputs to outputs) and emit() (the NVN calls in
// the function's order, with its arguments, into a sink). The bridge (hl/bridge/GfxBridge.h) reads the game's structures
// in place, writes the outputs back and gives the guest addresses the commands name.
#pragma once

#include <cstdint>

#include "hl/gfx/Commands.h"

namespace hl::gfx {

/// The prologue's inputs.
struct PrologueInputs {
    int32_t sub_mesh = 0;           ///< the index (signed, as the game compares it)
    uint32_t sub_meshes = 0;        ///< how many the drawable has
    uint8_t geometry_mode = 0;      ///< the mode the model needs
    uint8_t applied_mode = 0;       ///< the mode the block says was applied last
    uint32_t mode_bits = 0;         ///< the block's mode bits (bit 14 follows the geometry mode)
};
struct PrologueOutputs {
    bool in_range = false;          ///< false: the function returns at once
    bool mode_changed = false;      ///< the mode is set (and the block's two fields written)
    uint8_t applied_mode = 0;
    uint32_t mode_bits = 0;
};

/// Which way a sub-mesh is submitted.
enum class Submit : uint8_t {
    None,           ///< the preparation said no, or the model's mesh kind draws nothing: only the counter (if prepared)
    CallCommands,   ///< its pre-recorded command list
    Elements,       ///< an indexed instanced draw
    MeshTasks,      ///< mesh-shader tasks
};

/// The submission's inputs: what the preparation left (read after it returned).
struct SubmitInputs {
    uint64_t prepared = 0;          ///< the preparation's result (bit 0: go on)
    uint16_t instances = 0;         ///< the drawable's instance count
    uint8_t recorded = 0;           ///< the sub-mesh record's flags (bit 0: it has a pre-recorded command list)
    uint32_t mode_bits = 0;         ///< the block's mode bits (bit 5: pre-recorded lists allowed)
    uint8_t mesh_kind = 0;          ///< 0 indexed, 1 mesh tasks, 2 mesh-task threads (rounded up to 32s), other: none
    uint32_t mesh_tasks = 0;
    uint32_t triangles = 0;         ///< the block's counter
    uint32_t sub_mesh_triangles = 0;
};

/// The indexed draw's inputs (read only on that path): the sub-mesh descriptor's first and last index range and its
/// enums, mapped to NVN's by the game's tables, and the index buffer.
struct IndexedInputs {
    uint32_t first_offset = 0;      ///< byte offset of the first range
    uint32_t last_offset = 0, last_count = 0;
    uint32_t index_shift = 0;       ///< the index type, which is also the log2 of an index's size
    uint32_t mode = 0, type = 0;    ///< NVN's primitive and index type
    uint32_t base_vertex = 0;
    uint64_t buffer = 0;            ///< the NVN buffer holding the indices
};

/// What the submission writes and draws.
struct SubmitOutputs {
    Submit how = Submit::None;
    bool counted = false;           ///< the counter is written (every path once prepared)
    uint32_t triangles = 0;
    bool marked = false;            ///< the command buffer object's mark byte is set (the pre-recorded path)
    uint32_t instances = 0;         ///< the indexed draw's instance count (1 for 0 or 1)
    uint32_t tasks = 0;             ///< the mesh tasks' count
    uint32_t count = 0;             ///< the indexed draw's index count
};

/// Where the commands point.
struct SubmitHandles {
    uint64_t command_buffer = 0;
    uint64_t command_list = 0;      ///< the sub-mesh record's pre-recorded handle (its address: a list of one)
};

class SubmeshDraw {
public:
    static PrologueOutputs prologue(const PrologueInputs& in);
    static void emit_prologue(const PrologueOutputs& out, uint64_t command_buffer, CommandSink& sink);

    static SubmitOutputs decide(const SubmitInputs& in, const IndexedInputs* indexed);
    /// Which way decide() will go, from what it reads before the indexed draw's inputs (the bridge reads those only on
    /// that path, as the game does).
    static Submit path(const SubmitInputs& in);
    /// `address`: the index buffer's address (BufferGetAddress's answer), for the indexed draw.
    static void emit(const SubmitOutputs& out, const IndexedInputs* indexed, uint64_t address,
                     const SubmitHandles& at, CommandSink& sink);
};

}   // namespace hl::gfx
