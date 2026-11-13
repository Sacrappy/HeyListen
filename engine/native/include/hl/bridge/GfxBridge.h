// HeyListen native engine: where the graphics front end (hl::gfx) meets the game's graphics module, still run from the
// user's local translation (docs/native-engine/gfx-record-job.md has the reading behind every address here).
//
// Nothing here is game code or data: guest addresses of the one supported build and the layout of the few fields read.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hl/bridge/JobBridge.h"
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
    uint8_t _30[9];
    uint8_t depth_mode;
    uint8_t _3a;
    uint8_t geometry_mode;   ///< the geometry processing mode applied last (the sub-mesh submission)
    uint8_t _3c[20];
    uint32_t triangles;      ///< the triangles submitted (the sub-mesh submission adds each sub-mesh's)
    uint8_t _54[4];
    uint8_t offset_slot;
    uint8_t _59;
    uint8_t key;
    uint8_t _5b;
    uint32_t mode_bits;
    uint8_t _60;
    uint8_t blend_target_mask;
    uint8_t _62[2];
    int8_t offset_slot_base;
    uint8_t _65[11];
    uint32_t channel_masks_in;
    uint8_t _74[36];
    uint32_t done;
    uint8_t _9c[4];
    uint64_t blend_color_in[2];
    uint8_t _b0[64];
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
    uint8_t _15c;
    uint8_t polygon_offset;
    uint8_t depth_clamp;
    uint8_t _15f;
};
static_assert(offsetof(RenderStateBlock, material) == 24);
static_assert(offsetof(RenderStateBlock, renderer) == 40);
static_assert(offsetof(RenderStateBlock, depth_mode) == 57);
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
    uint8_t _4d0[152];
    GuestAddr state_callback;
};
static_assert(offsetof(Material, offset_units) == 1224);
static_assert(offsetof(Material, offset_factor) == 1228);
static_assert(offsetof(Material, state_callback) == 1384);

/// The state callback object (a function object: its invoker at +16 is the game's empty function when none is set).
struct StateCallback {
    uint8_t _0[16];
    GuestAddr invoke;
};
static_assert(offsetof(StateCallback, invoke) == 16);

/// The drawable a sub-mesh submission is called on (its x0): only the fields the function reads.
struct Drawable {
    uint8_t _0[30];
    uint16_t counts;       ///< bits 4-7: the number of sub-meshes
    uint8_t _20[52];
    uint32_t flags;        ///< the draw kinds (with the call's mask: which path)
    uint8_t _58[4];
    uint16_t instances;
    uint8_t _5e[10];
    GuestAddr model;
};
static_assert(offsetof(Drawable, counts) == 0x1e);
static_assert(offsetof(Drawable, flags) == 0x54);
static_assert(offsetof(Drawable, instances) == 0x5c);
static_assert(offsetof(Drawable, model) == 0x68);

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

}   // namespace hl::bridge
