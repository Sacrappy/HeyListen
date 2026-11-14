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
    ProgramBuffers,   ///< another program, its changed uniform-block words bound from the known buffer objects (native)
    StateApplied,     ///< the state step taken (the render state copied, recorded again when it changed), the rest as on
                      ///< the two paths above (native)
    StateChanged,     ///< the state step (left: only with HL_GFX_PREP's state step off)
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

/// The render state as the block keeps it for the preparation's state step: what the step compares and rewrites, then
/// what its record reads.
struct PrepareState {
    uint8_t applied_key = 0;            ///< the drawable state key applied
    uint32_t applied_material = 0;      ///< the material state applied
    uint8_t offset_enables = 0;         ///< bit 0: the polygon offsets on (bits 1, 2 compared too)
    uint8_t cull_face = 0;
    uint8_t polygon_mode[2] = {};       ///< (the block keeps it twice)
    uint8_t depth_test = 0, depth_write = 0, depth_func = 0;
    uint32_t blend_targets = 0;         ///< the targets blending is on for
    uint32_t blend_color[4] = {};       ///< (four floats' bits)
    uint8_t blend[8][6] = {};           ///< a target's factors and equations
    // (read only)
    uint32_t channel_masks = 0;
    uint8_t stencil_test = 0, stencil_func = 0, stencil_ops[3] = {};
    uint32_t stencil_ref = 0, stencil_value_mask = 0;
    uint32_t offset_clamp[3] = {};      ///< (three floats' bits)
    uint8_t depth_clamp = 0;
};

/// The drawable's material state (read when the block's mode bit 0 asks for it).
struct MaterialState {
    uint32_t id = 0;
    uint8_t kind = 0;                   ///< (its low three bits: 5 and 2 carry a depth state, other kinds blending only)
    uint32_t blend = 0;                 ///< not 0: blending on (the block's blend target mask then says where)
    uint32_t color[4] = {};
    uint8_t targets[8][6] = {};
    uint8_t depth_test = 0, depth_write = 0, depth_func = 0;
};

/// A buffer object the program step binds uniform blocks from (read when its binding word differs): its two methods
/// (compared with the game's known implementations, PrepareInputs::known), its size, the slot it is at in a ring of
/// copies and the ring's stride, its base address and its own alignment.
struct BufferObject {
    uint64_t align_method = 0, bind_method = 0;
    uint32_t size = 0;
    uint16_t slot = 0;
    uint8_t stride = 0;       ///< (its high four bits count)
    uint64_t base = 0;
    uint32_t alignment = 0;
};

/// The game's implementations the function devirtualizes against (read from its GOT at run time).
struct KnownMethods {
    uint64_t align_fixed = 0;    ///< alignment 0x100
    uint64_t align_field = 0;    ///< the object's own alignment
    uint64_t bind_uniform = 0;   ///< binds a uniform buffer
    uint64_t bind_storage = 0;   ///< binds a storage buffer
};

/// What the cached path reads, in the function's order (a field past the point where the call leaves that path is not
/// read and keeps its default).
struct PrepareInputs {
    // The program step's buffer binds: the objects behind binding words 0, 3 and 4 (read when that word differs).
    BufferObject buffers[3];
    KnownMethods known;
    // The state step's.
    uint64_t flags = 0;                 ///< the block's (bit 6: reversed depth; bit 16: no blend copy; bit 17: no record)
    uint8_t state_bits = 0;             ///< the block's byte the state key is combined with
    uint8_t blend_target_mask = 0;
    PrepareState state;
    MaterialState material_state_in;
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
    // The state step: the block's render state as it leaves it, and whether it records it.
    bool state_step = false;            ///< the state key or the material state differed (the fields below written)
    bool state_recorded = false;
    PrepareState state;
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
    bool word_written[5] = {};          ///< binding words 0, 3 and 4 recorded when they changed (their buffers bound)
    uint32_t words[5] = {};
    /// The uniform buffer binds, in order: stage, slot (as the register holds it), address, size.
    struct Bind {
        uint32_t stage = 0;
        uint64_t slot = 0, address = 0, size = 0;
    };
    Bind binds[9];
    uint32_t bind_count = 0;
    bool bind_marked = false;           ///< a uniform bind sets the command buffer's mark byte
    bool mask_cleared = false;          ///< the block's applied bits cleared (before the pending step)
    bool mask_written = false;          ///< the block's applied bits are rewritten
    uint16_t applied_mask = 0;
    bool marked = false;                ///< the command buffer object's mark byte is set
    bool call_list = false;             ///< the material entry's pre-recorded list is called
};

/// Where the commands point (BindProgram's object is the recording context's +0x188: PrepareInputs::context).
inline constexpr uint64_t kProgramBinder = 0x188;
struct PrepareHandles {
    // The state record's: the command buffer, the five state objects in the function's frame, the block's blend color.
    uint64_t state_buffer = 0;
    uint64_t bind_buffer = 0;           ///< the uniform binds' command buffer
    uint64_t blend = 0, color = 0, channel_mask = 0, depth_stencil = 0, polygon = 0, blend_color = 0;
    uint64_t command_buffer = 0;
    uint64_t command_list = 0;          ///< the list's handle in the material entry (its address: a list of one)
};

class Prepare {
public:
    static bool native(PrepareRoute r) {
        return r == PrepareRoute::NoMaterial || r == PrepareRoute::Cached || r == PrepareRoute::ProgramBound ||
               r == PrepareRoute::ProgramBuffers || r == PrepareRoute::StateApplied;
    }
    /// `state_step`: take the state step natively (else a call that needs it is left, route StateChanged).
    static PrepareOutputs decide(const PrepareInputs& in, bool state_step = true);
    /// The state step alone: `s` as the step leaves it; true when it records the render state.
    static bool apply_state(PrepareState& s, uint8_t key, uint8_t state_bits, uint32_t mode, uint64_t flags,
                            uint8_t blend_target_mask, const MaterialState& m);
    static void emit_state(const PrepareState& s, const PrepareHandles& at, CommandSink& sink);
    static void emit(const PrepareOutputs& out, const PrepareHandles& at, CommandSink& sink);
};

}   // namespace hl::gfx
