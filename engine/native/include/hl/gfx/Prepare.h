// HeyListen native engine: the shared draw preparation of the game's draw path on its cached path
// (docs/native-engine/gfx-record-job.md, section 10).
//
// Before a sub-mesh is submitted, the game's preparation (called by the sub-mesh submission, the recording job and 260
// other functions) picks the drawable's material entry for the block's pass and applies what changed since the block's
// previous draw: the render state again when the drawable's state key or its material state differs, the program and
// with it the uniform blocks, storage buffers, textures and samplers of the program's interface when the program
// differs; then, every call, a per-drawable uniform block, and either the material entry's pre-recorded command list or
// guest code that binds the vertex streams.
//
// Prepare is that function on its cached path: the material, the state and the program already applied (the block says
// so), so that what is left is the pending-bits word, the pre-recorded list and the return value. decide() is pure and
// sends every other call to the translation (PrepareRoute says which part it would have reached first); emit() makes the
// path's one NVN call. The bridge (hl/bridge/GfxBridge.h) reads the game's structures in place, in the function's order.
#pragma once

#include <cstdint>

#include "hl/gfx/Commands.h"

namespace hl::gfx {

/// Where a call goes: natively answered (NoMaterial, Cached), or the first part of the function it reaches that the
/// native version leaves to the translation.
enum class PrepareRoute : uint8_t {
    NoMaterial,       ///< the drawable's material entry is empty: the function returns 0 at once (native)
    Cached,           ///< state and program already applied (native)
    ProgramBound,     ///< another program whose interface's buffers and textures are the ones applied: bound alone (native)
    StateChanged,     ///< the drawable's state key or its material state differs: the render state is recorded again
    ProgramChanged,   ///< another program whose interface differs: its buffers or textures are bound
    Masked,           ///< pending bits the block has not applied yet
    DrawableBlock,    ///< the per-drawable uniform block is bound
    Streams,          ///< no pre-recorded list: guest code binds the vertex streams
    Inline,           ///< the inline-write switch is set: the list's call is written into the command memory
    Clusters,         ///< the cluster paths' guest calls
    Count
};
inline constexpr uint32_t kPrepareRoutes = (uint32_t)PrepareRoute::Count;
const char* route_name(PrepareRoute r);

/// What the cached path reads, in the function's order (a field past the point where the call leaves that path is not
/// read and keeps its default).
struct PrepareInputs {
    bool material = false;              ///< the material entry's first word is not 0
    uint8_t state_key = 0;              ///< the drawable's
    uint8_t applied_state_key = 0;      ///< the block's
    uint32_t mode_bits = 0;             ///< the block's (bit 0: compare the material state; bit 3: lists allowed)
    uint32_t material_state = 0;        ///< the drawable's material state's id (read when mode bit 0)
    uint32_t applied_material_state = 0;
    uint64_t program = 0;               ///< the material entry's program
    uint64_t applied_program = 0;       ///< the block's
    // The program step (read when the program differs; each part of the interface only where the function reads it).
    uint64_t context = 0;               ///< the block's recording context (BindProgram takes its +0x188)
    uint64_t program_object = 0;        ///< the program's NVN program and stages
    uint32_t program_stages = 0;
    uint16_t program_flags = 0;         ///< bit 7, bit 8: the first, second part's words are the game's defaults
    uint32_t interface_id[3] = {};      ///< the interface's three parts' ids (uniform blocks, more blocks, textures)
    uint32_t applied_interface_id[3] = {};
    uint32_t words[5] = {};             ///< the parts' binding words (three, then two)
    uint32_t applied_words[5] = {};
    uint32_t texture_mask = 0;          ///< the texture part's slots
    // The rest: the block's and the drawable's fields are read whatever the route (decide() uses what the program
    // step leaves in their place).
    uint16_t pending = 0;               ///< the block's pending bits
    uint16_t drawable_mask = 0;         ///< bits the drawable takes away from them
    uint16_t applied_mask = 0;          ///< the bits applied
    bool view_blocks = false;           ///< the block's material has a table of per-drawable uniform blocks
    uint8_t drawable_block = 0;         ///< the drawable's entry in it
    uint8_t applied_drawable_block = 0;
    uint32_t block_binding = 0;         ///< the block's binding word for it
    uint8_t list_flags = 0;             ///< the material entry's list flags (bit 0: present; bit 1: call it); mode bit 3
    bool inline_switch = false;         ///< read when the list is called
    uint8_t cluster = 0;                ///< the block's cluster index
    uint8_t drawable_kinds = 0;         ///< the drawable's draw-kind flags' low byte (read when cluster)
    uint16_t cluster_count = 0;         ///< read when cluster and kinds bit 2 is clear
};

/// What it writes and calls.
struct PrepareOutputs {
    PrepareRoute route = PrepareRoute::NoMaterial;
    uint32_t result = 0;                ///< the function's w0 (bit 0: go on)
    // The program step: the program bound and what the block records of it.
    bool program_bound = false;         ///< BindProgram, and the block's program (its +0xd0 cleared)
    uint64_t program = 0;               ///< the block's new program
    uint64_t binder = 0, program_object = 0;   ///< BindProgram's arguments
    uint32_t program_stages = 0;
    bool mode_written = false;          ///< the block's mode bits rewritten (bits 12, 13)
    uint32_t mode_bits = 0;
    bool id_written[3] = {};            ///< the interface's ids recorded
    uint32_t ids[3] = {};
    bool binding_written = false;       ///< the per-drawable block's binding word, and its entry made "none" (0xff)
    uint32_t block_binding = 0;
    bool mask_cleared = false;          ///< the block's applied bits cleared (before the pending step)
    bool mask_written = false;          ///< the block's applied bits are rewritten
    uint16_t applied_mask = 0;
    bool marked = false;                ///< the command buffer object's mark byte is set
    bool call_list = false;             ///< the material entry's pre-recorded list is called
};

/// Where the commands point (BindProgram's object is the recording context's +0x188: PrepareInputs::context).
inline constexpr uint64_t kProgramBinder = 0x188;
struct PrepareHandles {
    uint64_t command_buffer = 0;
    uint64_t command_list = 0;          ///< the list's handle in the material entry (its address: a list of one)
};

class Prepare {
public:
    static bool native(PrepareRoute r) {
        return r == PrepareRoute::NoMaterial || r == PrepareRoute::Cached || r == PrepareRoute::ProgramBound;
    }
    static PrepareOutputs decide(const PrepareInputs& in);
    static void emit(const PrepareOutputs& out, const PrepareHandles& at, CommandSink& sink);
};

}   // namespace hl::gfx
