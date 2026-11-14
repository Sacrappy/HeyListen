// HeyListen native engine: where the graphics front end (hl::gfx) meets the game's graphics module, still run from the
// user's local translation (docs/native-engine/gfx-record-job.md has the reading behind every address here).
//
// Nothing here is game code or data: guest addresses of the one supported build and the layout of the few fields read.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hl/bridge/JobBridge.h"
#include "hl/gfx/Prepare.h"
#include "hl/gfx/RecordPass.h"
#include "hl/gfx/RenderState.h"
#include "hl/gfx/SubmeshDraw.h"

namespace hl::bridge {

namespace guest {

/// The argument block the recording job's listener walk hands every listener.
struct PassArgs {
    uint8_t _0[28];
    uint32_t mode;   ///< hl::gfx::PassMode
};
static_assert(offsetof(PassArgs, mode) == 28);

/// The render-state block a draw carries (built on the stack by the draw function 0x80681320 at its sp+80, handed to the
/// per-variant draw functions and to the render-state function). Only the fields the render-state function reads or
/// writes are named.
struct RenderStateBlock {
    uint64_t flags;   ///< (bit 17: derive and write only, record nothing)
    uint8_t _8[16];
    GuestAddr material;
    uint8_t _20[8];
    GuestAddr renderer;
    GuestAddr buffer;        ///< the buffer object the preparation binds binding word 0's uniform blocks from
    uint8_t cluster;         ///< the cluster entry's index (the sub-mesh submission; the preparation's cluster calls)
    uint8_t depth_mode;      ///< (also the pass: the preparation's index into the drawable's pass slots)
    uint8_t state_bits;      ///< combined with the drawable's state key by the preparation's state step
    uint8_t geometry_mode;   ///< the geometry processing mode applied last (the sub-mesh submission)
    uint8_t _3c[20];
    uint32_t triangles;      ///< the triangles submitted (the sub-mesh submission adds each sub-mesh's)
    uint8_t _54[4];
    uint8_t offset_slot;
    uint8_t state_key_applied;   ///< the drawable state key the preparation applied last
    uint8_t key;
    uint8_t _5b;
    uint32_t mode_bits;
    uint8_t drawable_block;      ///< the per-drawable uniform block's entry bound last (the preparation)
    uint8_t blend_target_mask;
    uint8_t pass_slot;           ///< the material slot for the preparation (0xff: from the drawable's pass slots)
    uint8_t _63;
    int8_t offset_slot_base;
    uint8_t _65[11];
    uint32_t channel_masks_in;
    uint8_t _74[4];
    uint32_t words_a[2];         ///< the program interface's first part's binding words applied (the preparation)
    uint32_t block_binding;      ///< its third: the per-drawable uniform block's binding word (-1: none)
    uint32_t words_b[2];         ///< the second part's binding words applied
    uint32_t texture_id;         ///< the texture part's id applied
    uint32_t interface_id_a;     ///< the first part's id applied
    uint32_t interface_id_b;     ///< the second part's id applied
    uint32_t done;           ///< (the material state the preparation applied last: the render-state function resets it)
    uint8_t _9c[4];
    uint64_t blend_color_in[2];
    uint16_t pending;            ///< bits the preparation still has to apply
    uint16_t applied_bits;
    uint8_t _b4[28];
    uint64_t program_aux;        ///< cleared when the preparation binds a program (not read by it)
    GuestAddr program;           ///< the program the preparation bound last
    uint8_t _e0[16];
    uint8_t depth_test;
    uint8_t depth_write;
    uint8_t _f2;
    uint8_t stencil_test;
    uint32_t blend_targets;
    uint64_t blend_color[2];
    uint8_t _108[4];
    uint32_t channel_masks;
    uint32_t stencil_ref;
    uint32_t stencil_value_mask;
    uint8_t blend[8][6];
    uint32_t offset_clamp[3];
    uint8_t depth_func;
    uint8_t cull_face;
    uint8_t _156;
    uint8_t stencil_ops[3];
    uint8_t stencil_func;
    uint8_t polygon_mode;
    uint8_t polygon_mode_b;  ///< (the preparation keeps the polygon mode here too)
    uint8_t polygon_offset;
    uint8_t depth_clamp;
    uint8_t _15f;
};
static_assert(offsetof(RenderStateBlock, material) == 24);
static_assert(offsetof(RenderStateBlock, renderer) == 40);
static_assert(offsetof(RenderStateBlock, buffer) == 0x30);
static_assert(offsetof(RenderStateBlock, cluster) == 0x38);
static_assert(offsetof(RenderStateBlock, depth_mode) == 57);
static_assert(offsetof(RenderStateBlock, state_bits) == 0x3a);
static_assert(offsetof(RenderStateBlock, polygon_mode_b) == 0x15c);
static_assert(offsetof(RenderStateBlock, state_key_applied) == 0x59);
static_assert(offsetof(RenderStateBlock, drawable_block) == 0x60);
static_assert(offsetof(RenderStateBlock, pass_slot) == 0x62);
static_assert(offsetof(RenderStateBlock, words_a) == 0x78);
static_assert(offsetof(RenderStateBlock, block_binding) == 0x80);
static_assert(offsetof(RenderStateBlock, words_b) == 0x84);
static_assert(offsetof(RenderStateBlock, texture_id) == 0x8c);
static_assert(offsetof(RenderStateBlock, interface_id_a) == 0x90);
static_assert(offsetof(RenderStateBlock, interface_id_b) == 0x94);
static_assert(offsetof(RenderStateBlock, done) == 0x98);
static_assert(offsetof(RenderStateBlock, pending) == 0xb0);
static_assert(offsetof(RenderStateBlock, applied_bits) == 0xb2);
static_assert(offsetof(RenderStateBlock, program) == 0xd8);
static_assert(offsetof(RenderStateBlock, geometry_mode) == 59);
static_assert(offsetof(RenderStateBlock, triangles) == 80);
static_assert(offsetof(RenderStateBlock, offset_slot) == 88);
static_assert(offsetof(RenderStateBlock, key) == 90);
static_assert(offsetof(RenderStateBlock, mode_bits) == 92);
static_assert(offsetof(RenderStateBlock, blend_target_mask) == 97);
static_assert(offsetof(RenderStateBlock, offset_slot_base) == 100);
static_assert(offsetof(RenderStateBlock, channel_masks_in) == 112);
static_assert(offsetof(RenderStateBlock, done) == 152);
static_assert(offsetof(RenderStateBlock, blend_color_in) == 160);
static_assert(offsetof(RenderStateBlock, depth_test) == 240);
static_assert(offsetof(RenderStateBlock, depth_write) == 241);
static_assert(offsetof(RenderStateBlock, stencil_test) == 243);
static_assert(offsetof(RenderStateBlock, blend_targets) == 244);
static_assert(offsetof(RenderStateBlock, blend_color) == 248);
static_assert(offsetof(RenderStateBlock, channel_masks) == 268);
static_assert(offsetof(RenderStateBlock, stencil_ref) == 272);
static_assert(offsetof(RenderStateBlock, stencil_value_mask) == 276);
static_assert(offsetof(RenderStateBlock, blend) == 280);
static_assert(offsetof(RenderStateBlock, offset_clamp) == 328);
static_assert(offsetof(RenderStateBlock, depth_func) == 340);
static_assert(offsetof(RenderStateBlock, cull_face) == 341);
static_assert(offsetof(RenderStateBlock, stencil_ops) == 343);
static_assert(offsetof(RenderStateBlock, stencil_func) == 346);
static_assert(offsetof(RenderStateBlock, polygon_mode) == 347);
static_assert(offsetof(RenderStateBlock, polygon_offset) == 349);
static_assert(offsetof(RenderStateBlock, depth_clamp) == 350);
static_assert(sizeof(RenderStateBlock) == 352);

/// What the block's renderer field points at: the recording's context.
struct Renderer {
    uint8_t _0[216];
    GuestAddr command_buffer;
};
static_assert(offsetof(Renderer, command_buffer) == 216);

/// The draw's material (the block's material field): its polygon offsets by slot (slot s at +8s, any 32-bit slot the
/// function computes) and its state callback.
struct Material {
    uint8_t _0[1224];
    uint32_t offset_units;    ///< slot 0
    uint32_t offset_factor;   ///< slot 0
    uint8_t _4d0[0x48];
    GuestAddr drawable_blocks;   ///< the per-drawable uniform blocks' table (the preparation binds an entry: 0 none)
    uint8_t _520[0x18];
    GuestAddr buffers[2];        ///< the buffer objects of the program's binding words 3 and 4 (the preparation)
    uint8_t _548[0x20];
    GuestAddr state_callback;
};
static_assert(offsetof(Material, offset_units) == 1224);
static_assert(offsetof(Material, offset_factor) == 1228);
static_assert(offsetof(Material, drawable_blocks) == 0x518);
static_assert(offsetof(Material, buffers) == 0x538);
static_assert(offsetof(Material, state_callback) == 1384);

/// The state callback object (a function object: its invoker at +16 is the game's empty function when none is set).
struct StateCallback {
    uint8_t _0[16];
    GuestAddr invoke;
};
static_assert(offsetof(StateCallback, invoke) == 16);

/// The drawable a sub-mesh submission is called on (its x0): only the fields the function reads.
struct Drawable {
    uint8_t _0[8];
    GuestAddr lists;       ///< (the preparation: its +0x14's low two bits choose the material entry's pre-recorded list)
    uint8_t _10[4];
    uint16_t pending_mask; ///< pending bits of the block this drawable takes away (the preparation)
    uint16_t cluster_count;
    uint8_t _18[6];
    uint16_t counts;       ///< bits 4-7: the number of sub-meshes
    uint8_t _20[52];
    uint32_t flags;        ///< the draw kinds (with the call's mask: which path)
    uint8_t state_key;     ///< the render state's key (the preparation compares the block's)
    uint8_t _59;
    uint8_t block_entry;   ///< its per-drawable uniform block's entry
    uint8_t _5b;
    uint16_t instances;
    uint8_t _5e;
    uint8_t material_stride;   ///< the material entries' size
    GuestAddr materials;       ///< the material entries (by the slot the pass selects)
    GuestAddr model;
    GuestAddr material_state;  ///< its +0x154: the material state's id (the preparation compares the block's)
};
static_assert(offsetof(Drawable, lists) == 0x8);
static_assert(offsetof(Drawable, pending_mask) == 0x14);
static_assert(offsetof(Drawable, cluster_count) == 0x16);
static_assert(offsetof(Drawable, counts) == 0x1e);
static_assert(offsetof(Drawable, flags) == 0x54);
static_assert(offsetof(Drawable, state_key) == 0x58);
static_assert(offsetof(Drawable, block_entry) == 0x5a);
static_assert(offsetof(Drawable, instances) == 0x5c);
static_assert(offsetof(Drawable, material_stride) == 0x5f);
static_assert(offsetof(Drawable, materials) == 0x60);
static_assert(offsetof(Drawable, model) == 0x68);
static_assert(offsetof(Drawable, material_state) == 0x70);
/// The drawable's byte tables the preparation indexes: the pass slots (by the block's pass, unsigned) and the material
/// slots (by the pass slot, signed; -1: the game's default material entry).
inline constexpr uint64_t kDrawablePassSlots = 0x2c, kDrawableMaterialSlots = 0x3b;
/// The drawable's lists word (at its lists object's +0x14) and the material state's id (at its +0x154).
inline constexpr uint64_t kListsSelector = 0x14, kMaterialStateId = 0x154;

/// A material entry the preparation picks (its size is the drawable's material stride): the program and four
/// pre-recorded command lists with their flags.
struct MaterialEntry {
    uint64_t first;        ///< 0: no material (the preparation returns 0)
    GuestAddr program;
    struct List {
        GuestAddr handle;  ///< (CallCommands takes its address: a list of one)
        uint8_t flags;     ///< bit 0: present; bit 1: call it
        uint8_t _9[7];
    } lists[4];
};
static_assert(offsetof(MaterialEntry, program) == 8);

/// A program object (the material entry's program): its interface in three parts (each part's first word an id;
/// the first part's binding words at +4 / +8 / +0xc, the second's at +0x28 / +0x2c, the texture part's slot mask at
/// +4), its NVN program and stages, and flags (bits 7, 8: the first, second part's words are the game's defaults).
struct Program {
    GuestAddr part[3];
    uint8_t _18[8];
    GuestAddr object;
    uint32_t stages;
    uint8_t _2c[4];
    uint16_t flags;
};
static_assert(offsetof(Program, part) == 0);
static_assert(offsetof(Program, object) == 0x20);
static_assert(offsetof(Program, stages) == 0x28);
static_assert(offsetof(Program, flags) == 0x30);
/// The drawable's material state (its +0x70): the render state the preparation copies into the block.
struct MaterialStateObject {
    uint8_t _0[0xe0];
    uint8_t depth_test, depth_write;
    uint8_t _e2[2];
    uint32_t blend;              ///< not 0: blending on
    uint32_t color[4];           ///< the blend color (floats' bits)
    uint8_t _f8[0x10];
    uint8_t targets[8][6];       ///< each target's factors and equations
    uint8_t _138[0xc];
    uint8_t depth_func;
    uint8_t _145[0xf];
    uint32_t id;
    uint8_t _158;
    uint8_t kind;                ///< low three bits: 5 and 2 carry a depth state
};
static_assert(offsetof(MaterialStateObject, depth_test) == 0xe0);
static_assert(offsetof(MaterialStateObject, depth_write) == 0xe1);
static_assert(offsetof(MaterialStateObject, blend) == 0xe4);
static_assert(offsetof(MaterialStateObject, color) == 0xe8);
static_assert(offsetof(MaterialStateObject, targets) == 0x108);
static_assert(offsetof(MaterialStateObject, depth_func) == 0x144);
static_assert(offsetof(MaterialStateObject, id) == 0x154);
static_assert(offsetof(MaterialStateObject, kind) == 0x159);
/// The preparation's frame (below the caller's sp) and its five state objects there, as the render state's record names
/// them.
inline constexpr uint64_t kPrepBlend = 0x70, kPrepColor = 0x78, kPrepChannelMask = 0x74, kPrepDepthStencil = 0x80,
                          kPrepPolygon = 0x84;

/// A buffer object the preparation binds uniform blocks from: a ring of copies of one block (its methods at the vtable's
/// +0x28 (alignment) and +0x50 (bind)).
struct BufferObjectG {
    GuestAddr vtable;
    uint8_t _8[0x38];
    uint32_t size;
    uint8_t _44[4];
    uint16_t slot;
    uint8_t stride;
    uint8_t _4b[0x35];
    GuestAddr base;
    uint32_t alignment;
};
static_assert(offsetof(BufferObjectG, size) == 0x40);
static_assert(offsetof(BufferObjectG, slot) == 0x48);
static_assert(offsetof(BufferObjectG, stride) == 0x4a);
static_assert(offsetof(BufferObjectG, base) == 0x80);
static_assert(offsetof(BufferObjectG, alignment) == 0x88);
inline constexpr uint64_t kBufferAlignMethod = 0x28, kBufferBindMethod = 0x50;
/// The GOT slots of the game's known implementations: alignment 0x100, the object's alignment, a uniform bind, a storage
/// bind.
inline constexpr uint64_t kAlignFixedSlot = 0x84cb56e0, kAlignFieldSlot = 0x84cb4fa8, kBindUniformSlot = 0x84cb56e8,
                          kBindStorageSlot = 0x84cb4fb8;

/// The game's default binding words (globals): the first part's three, then the second part's two.
inline constexpr uint64_t kDefaultWordsA = 0x84ea2848, kDefaultWordsB = 0x84ea2854;
static_assert(offsetof(MaterialEntry, lists) == 0x10);
static_assert(offsetof(MaterialEntry::List, flags) == 8);
static_assert(sizeof(MaterialEntry::List) == 16);

/// The drawable's model resource.
struct Model {
    uint8_t _0[8];
    GuestAddr mesh;              ///< its +0x18: the sub-mesh descriptors (SubmeshDescriptor)
    uint8_t _10[0x628];
    GuestAddr records;           ///< the sub-mesh records (SubmeshRecord)
    uint8_t _640[0x5e];
    uint8_t geometry_mode;
    uint8_t mesh_kind;
    uint8_t _6a0[4];
    uint32_t mesh_tasks;
};
static_assert(offsetof(Model, mesh) == 0x8);
static_assert(offsetof(Model, records) == 0x638);
static_assert(offsetof(Model, geometry_mode) == 0x69e);
static_assert(offsetof(Model, mesh_kind) == 0x69f);
static_assert(offsetof(Model, mesh_tasks) == 0x6a4);

struct Mesh {
    uint8_t _0[0x18];
    GuestAddr descriptors;
};
static_assert(offsetof(Mesh, descriptors) == 0x18);

/// One sub-mesh's index ranges and enums (56 bytes; ranges: 8-byte entries, a byte offset then a count).
struct SubmeshDescriptor {
    GuestAddr ranges;
    uint8_t _8[8];
    GuestAddr index_holder;      ///< its +8: the NVN buffer of the indices
    uint8_t _18[12];
    uint32_t primitive;
    uint32_t index_type;         ///< also the log2 of an index's size
    uint8_t _2c[4];
    uint32_t base_vertex;
    uint16_t range_count;
    uint8_t _36[2];
};
static_assert(offsetof(SubmeshDescriptor, index_holder) == 0x10);
static_assert(offsetof(SubmeshDescriptor, primitive) == 0x24);
static_assert(offsetof(SubmeshDescriptor, index_type) == 0x28);
static_assert(offsetof(SubmeshDescriptor, base_vertex) == 0x30);
static_assert(offsetof(SubmeshDescriptor, range_count) == 0x34);
static_assert(sizeof(SubmeshDescriptor) == 0x38);

struct IndexHolder {
    uint8_t _0[8];
    GuestAddr buffer;
};
static_assert(offsetof(IndexHolder, buffer) == 8);

/// One sub-mesh's record (0x270 bytes).
struct SubmeshRecord {
    uint8_t _0[0x50];
    GuestAddr command_list;      ///< a pre-recorded command handle (CallCommands takes its address: a list of one)
    uint8_t flags;               ///< bit 0: the handle is there
    uint8_t _59[0x20f];
    uint32_t triangles;
    uint8_t _26c[4];
};
static_assert(offsetof(SubmeshRecord, command_list) == 0x50);
static_assert(offsetof(SubmeshRecord, flags) == 0x58);
static_assert(offsetof(SubmeshRecord, triangles) == 0x268);
static_assert(sizeof(SubmeshRecord) == 0x270);

/// The game's NVN command buffer object: the byte the submission marks before calling a pre-recorded list.
inline constexpr uint64_t kCommandBufferMark = 0x10;

}   // namespace guest

/// The pass's listener: reached only through a pointer (the listener list's walk at 0x80076cd0 jumps to it through the
/// thunk 0x80076e60, under the global lock at 0x84eb5440), so it is answered with no change to the lifted code.
inline constexpr GuestAddr kRecordPass = 0x806602e0;
inline constexpr GuestAddr kScenePass = 0x80660320;    ///< mode 0: the scene's recording (called)
inline constexpr GuestAddr kSecondPass = 0x8065fe30;   ///< mode 1 (called)

/// The render-state function (x0 the block, w1 the state key; returns nothing). Called directly (254 call sites, one a
/// function: the draw function 0x80681320 and its per-variant draw functions): answered through an entry gate.
inline constexpr GuestAddr kApplyRenderState = 0x80077d60;
/// Its stack frame, and where in it the five NVN state objects it sets up and binds are (offsets from the frame's base,
/// which is the caller's sp minus the frame).
inline constexpr uint64_t kStateFrame = 176;
inline constexpr uint64_t kStateBlend = 8, kStatePolygon = 60, kStateDepthStencil = 64, kStateColor = 72,
                          kStateChannelMask = 76;
/// A GOT slot holding the address of the game's empty function: a state callback whose invoker is that does nothing.
inline constexpr GuestAddr kEmptyFunctionSlot = 0x84cb5d98;

/// The sub-mesh submission (x0 the drawable, x1 the block, w2 the sub-mesh, w3 the mask; returns nothing): a virtual
/// method its callers call directly when the drawable's slot holds it (7 call sites in 5 functions); answered through an
/// entry gate. Its shared preparation (x0 the block, x1 the drawable, w2 0; bit 0 of the result: go on), called back, and
/// the frame the function sets up before calling it (the preparation runs at the same stack addresses natively).
inline constexpr GuestAddr kSubmitSubmesh = 0x80078fc0;
inline constexpr GuestAddr kPrepareSubmesh = 0x8008fc00;
inline constexpr GuestAddr kPrepareReturn = 0x8007903c;   ///< the link register the function's call leaves
inline constexpr uint64_t kSubmitFrame = 0x150;
/// A global switch (its GOT slot): set, the pre-recorded path writes the call into the command memory itself.
inline constexpr GuestAddr kInlineCallSwitchSlot = 0x84ca2dd8;
/// The preparation's default material entry (a GOT slot holding its address): taken when the drawable's material slot is
/// -1.
inline constexpr GuestAddr kDefaultMaterialSlot = 0x84cb5d10;
/// The game's tables from its primitive and index-type enums to NVN's (32-bit entries).
inline constexpr GuestAddr kPrimitiveTable = 0x83e79c60, kIndexTypeTable = 0x83e7a994;

/// Which way a submission goes, decided at the function's entry from the drawable's flags, the mask and the switch.
enum class SubmitRoute : uint8_t {
    Direct,     ///< the native unit's paths
    Query,      ///< mask & flags bit 3: a guest call that may stop the draw
    Callback,   ///< flags bit 5: a guest call before the draw
    Clusters,   ///< mask & flags & 0x51: the indirect and cluster (meshlet) paths
    Inline,     ///< the switch is set
};

/// The game's global that holds NVN procedure `proc` (hl/gfx/Nvn.h numbering), filled by its loader 0x812be290: the
/// procedure the translated render-state function calls. 0 past the table.
GuestAddr proc_global(uint32_t proc);

/// The pass's phases, run through the guest runtime on the calling thread's own guest context.
class GfxBridge final : public gfx::PassHost {
public:
    explicit GfxBridge(GuestRuntime& runtime) : runtime_(runtime) {}
    uint32_t mode(uint64_t args) const override;
    void run_phase(gfx::PassMode phase, uint64_t listener, uint64_t args) override;

private:
    GuestRuntime& runtime_;
};

/// The render-state function's inputs and outputs, in place in the block.
class StateBridge {
public:
    static constexpr size_t kBlockSize = sizeof(guest::RenderStateBlock);

    explicit StateBridge(GuestRuntime& runtime) : runtime_(runtime) {}

    /// Reads one call (x0 block, w1 key, sp the caller's stack pointer). False when the call would reach the material's
    /// state callback, which is guest code: the native version does not answer that call.
    bool read(GuestAddr block, uint64_t key, uint64_t sp, gfx::StateInputs& in, gfx::StateHandles& at) const;
    /// The polygon offsets of `block`'s material.
    class Offsets final : public gfx::PolygonOffsets {
    public:
        Offsets(const GuestRuntime& runtime, GuestAddr material) : runtime_(runtime), material_(material) {}
        void at(int32_t slot, uint32_t& factor, uint32_t& units) const override;

    private:
        const GuestRuntime& runtime_;
        GuestAddr material_;
    };
    Offsets offsets(GuestAddr block) const;
    /// Writes the outputs into the block in place (and tells the OS layer's write tracking).
    void write(GuestAddr block, const gfx::StateOutputs& out) const;
    /// The block's bytes as the call should leave them: `bytes` holds the block before the call (kBlockSize).
    static void expect(uint8_t* bytes, const gfx::StateOutputs& out);
    /// The block's bytes now.
    void copy(GuestAddr block, uint8_t* bytes) const;

private:
    GuestRuntime& runtime_;
};

/// The sub-mesh submission's inputs and outputs, in place.
class DrawBridge {
public:
    /// What the submission may write: the block's 352 bytes, then the command buffer object's mark byte.
    static constexpr size_t kSnapshotSize = sizeof(guest::RenderStateBlock) + 1;

    explicit DrawBridge(GuestRuntime& runtime) : runtime_(runtime) {}

    SubmitRoute route(GuestAddr drawable, uint32_t mask) const;
    /// The prologue's inputs (x0 drawable, x1 block, w2 sub-mesh); `context`: the block's recording context, which the
    /// function reads once here and uses after the preparation.
    void read_prologue(GuestAddr drawable, GuestAddr block, uint32_t sub_mesh, gfx::PrologueInputs& in,
                       GuestAddr& context) const;
    GuestAddr command_buffer(GuestAddr context) const;
    void write_prologue(GuestAddr block, const gfx::PrologueOutputs& out) const;
    /// The submission's inputs once the preparation returned `prepared`.
    void read_submit(GuestAddr drawable, GuestAddr block, uint32_t sub_mesh, uint64_t prepared, GuestAddr context,
                     gfx::SubmitInputs& in, gfx::SubmitHandles& at) const;
    /// The indexed draw's inputs (read only on that path; the buffer's address is the caller's query).
    void read_indexed(GuestAddr drawable, uint32_t sub_mesh, gfx::IndexedInputs& in) const;
    void write_submit(GuestAddr block, const gfx::SubmitHandles& at, const gfx::SubmitOutputs& out) const;

    /// The bytes the submission may write, now (kSnapshotSize; the mark byte 0 without a command buffer).
    void snapshot(GuestAddr block, GuestAddr command_buffer, uint8_t* bytes) const;
    static void expect_prologue(uint8_t* bytes, const gfx::PrologueOutputs& out);
    static void expect_submit(uint8_t* bytes, const gfx::SubmitOutputs& out);

private:
    GuestRuntime& runtime_;
};

/// The shared preparation's inputs and outputs (x0 the block, x1 the drawable), in place.
class PrepareBridge {
public:
    /// What the preparation may write on its cached path: the block's 352 bytes, then the command buffer's mark byte.
    static constexpr size_t kSnapshotSize = DrawBridge::kSnapshotSize;

    explicit PrepareBridge(GuestRuntime& runtime) : runtime_(runtime) {}

    /// The material entry the call picks (the function's first reads).
    GuestAddr material(GuestAddr block, GuestAddr drawable) const;
    /// Reads the call in the function's order, stopping where it leaves the cached path (decide() on the result says
    /// where); `at`: the command buffer and the list, when the list is called.
    void read(GuestAddr block, GuestAddr drawable, gfx::PrepareInputs& in, gfx::PrepareHandles& at) const;
    /// The state record's handles (`sp` the caller's stack pointer: the state objects are in the function's frame);
    /// asked only when the call records the state (the function reads the command buffer only then).
    void state_handles(GuestAddr block, uint64_t sp, gfx::PrepareHandles& at) const;
    void write(GuestAddr block, const gfx::PrepareHandles& at, const gfx::PrepareOutputs& out) const;

    void snapshot(GuestAddr block, GuestAddr command_buffer, uint8_t* bytes) const;
    static void expect(uint8_t* bytes, const gfx::PrepareOutputs& out);

private:
    /// The program step's inputs (the program differs), each part read only where the function reads it.
    void read_program(GuestAddr block, gfx::PrepareInputs& in) const;
    GuestRuntime& runtime_;
};

}   // namespace hl::bridge
