// HeyListen native engine: the graphics front end's boundary with the game's graphics module (hl/bridge/GfxBridge.h).
#include "hl/bridge/GfxBridge.h"

#include <cstring>

#include "hl/gfx/Nvn.h"

namespace hl::bridge {

uint32_t GfxBridge::mode(uint64_t args) const { return static_cast<const guest::PassArgs*>(runtime_.ptr(args))->mode; }

void GfxBridge::run_phase(gfx::PassMode phase, uint64_t listener, uint64_t args) {
    // (the translated listener calls either phase with its own two arguments and nothing else in registers: both phases
    // read x0 and x1 only)
    runtime_.call(runtime_.current(), phase == gfx::PassMode::Scene ? kScenePass : kSecondPass, listener, args);
}

GuestAddr proc_global(uint32_t proc) {
    // (read from the lifted render-state function: the GOT slot each call site loads, and the global it points at)
    static constexpr GuestAddr kGlobals[gfx::kProcCount] = {
        0x84d678e8, 0x84d678f0, 0x84d678f8, 0x84d67900,                           // blend state
        0x84d67960, 0x84d67968, 0x84d67970,                                       // color state
        0x84d679a0,                                                               // channel mask state
        0x84d67ad0, 0x84d67ad8, 0x84d67ae0, 0x84d67ae8, 0x84d67af0, 0x84d67af8,   // depth stencil state
        0x84d67a88, 0x84d67a90, 0x84d67a98, 0x84d67aa0,                           // polygon state
        0x84d67c20, 0x84d67c30, 0x84d67ea0, 0x84d67c28, 0x84d67c48,               // command buffer: binds, blend color
        0x84d67e88, 0x84d67e90, 0x84d67e98,                                       // stencil values
        0x84d67c40, 0x84d67eb8, 0x84d67ed8,                                       // polygon state, offset clamp, depth clamp
        // (read from the sub-mesh submission and its draw helpers)
        0x84d67d90, 0x84d67c10, 0x84d67d98,   // geometry processing mode, call commands, mesh tasks
        0x84d674a8, 0x84d67d68,               // buffer address, indexed instanced draw
    };
    return proc < gfx::kProcCount ? kGlobals[proc] : 0;
}

namespace {
template <class T>
const T& at(const GuestRuntime& rt, GuestAddr a) {
    return *static_cast<const T*>(rt.ptr(a));
}
uint64_t u64(const GuestRuntime& rt, GuestAddr a) {
    uint64_t v;
    std::memcpy(&v, rt.ptr(a), 8);
    return v;
}
}   // namespace

bool StateBridge::read(GuestAddr block, uint64_t key, uint64_t sp, gfx::StateInputs& in, gfx::StateHandles& h) const {
    const guest::RenderStateBlock& b = at<guest::RenderStateBlock>(runtime_, block);
    // (the function calls the material's state callback before anything it records when the callback's invoker is not
    // the game's empty function: that call is guest code, and its result decides what follows)
    if (!b.material) return false;
    const GuestAddr callback = at<guest::Material>(runtime_, b.material).state_callback;
    if (!callback || at<guest::StateCallback>(runtime_, callback).invoke != u64(runtime_, kEmptyFunctionSlot)) return false;

    in.key = (uint8_t)key;
    in.flags = b.flags;
    in.mode_bits = b.mode_bits;
    in.depth_mode = b.depth_mode;
    in.channel_masks = b.channel_masks_in;
    in.blend_target_mask = b.blend_target_mask;
    in.blend_color[0] = b.blend_color_in[0];
    in.blend_color[1] = b.blend_color_in[1];
    in.offset_slot = b.offset_slot;
    in.offset_slot_base = b.offset_slot_base;
    std::memcpy(in.blend, b.blend, sizeof in.blend);
    in.stencil_test = b.stencil_test;
    in.stencil_func = b.stencil_func;
    std::memcpy(in.stencil_ops, b.stencil_ops, sizeof in.stencil_ops);
    in.stencil_ref = b.stencil_ref;
    in.stencil_value_mask = b.stencil_value_mask;
    in.cull_face = b.cull_face;
    in.polygon_mode = b.polygon_mode;
    in.polygon_offset = b.polygon_offset;
    std::memcpy(in.offset_clamp, b.offset_clamp, sizeof in.offset_clamp);

    // (the command buffer is read only when something is recorded, as the function does)
    h.command_buffer = (in.flags >> 17) & 1 ? 0 : at<guest::Renderer>(runtime_, b.renderer).command_buffer;
    h.blend_color = block + offsetof(guest::RenderStateBlock, blend_color);
    const uint64_t frame = sp - kStateFrame;
    h.blend = frame + kStateBlend;
    h.color = frame + kStateColor;
    h.channel_mask = frame + kStateChannelMask;
    h.depth_stencil = frame + kStateDepthStencil;
    h.polygon = frame + kStatePolygon;
    return true;
}

void StateBridge::Offsets::at(int32_t slot, uint32_t& factor, uint32_t& units) const {
    // (slot s at +8s from slot 0, s signed: 64-bit address arithmetic, as the function's)
    const GuestAddr s0 = material_ + offsetof(guest::Material, offset_units) + ((uint64_t)(int64_t)slot << 3);
    std::memcpy(&units, runtime_.ptr(s0), 4);
    std::memcpy(&factor, runtime_.ptr(s0 + 4), 4);
}

StateBridge::Offsets StateBridge::offsets(GuestAddr block) const {
    return Offsets(runtime_, at<guest::RenderStateBlock>(runtime_, block).material);
}

namespace {
void put(guest::RenderStateBlock& b, const gfx::StateOutputs& out) {
    b.key = out.key;
    b.mode_bits = out.mode_bits;
    b.blend_color[0] = out.blend_color[0];
    b.blend_color[1] = out.blend_color[1];
    b.channel_masks = out.channel_masks;
    b.depth_test = out.depth_test;
    b.depth_write = out.depth_write;
    b.depth_func = out.depth_func;
    b.depth_clamp = out.depth_clamp;
    b.blend_targets = out.blend_targets;
    if (out.slot_changed) {
        b.offset_slot = out.offset_slot;
        std::memcpy(b.offset_clamp, out.offset_clamp, sizeof b.offset_clamp);
    }
    b.done = out.done;
}
// (the span of the block the function writes: the state key at +88 to the depth clamp at +350)
constexpr uint64_t kWrittenFrom = offsetof(guest::RenderStateBlock, offset_slot);
constexpr uint64_t kWrittenTo = offsetof(guest::RenderStateBlock, depth_clamp) + 1;
}   // namespace

void StateBridge::write(GuestAddr block, const gfx::StateOutputs& out) const {
    put(*static_cast<guest::RenderStateBlock*>(runtime_.ptr(block)), out);
    runtime_.written(block + kWrittenFrom, kWrittenTo - kWrittenFrom);
}

void StateBridge::expect(uint8_t* bytes, const gfx::StateOutputs& out) {
    guest::RenderStateBlock b;
    std::memcpy(&b, bytes, kBlockSize);
    put(b, out);
    std::memcpy(bytes, &b, kBlockSize);
}

void StateBridge::copy(GuestAddr block, uint8_t* bytes) const { std::memcpy(bytes, runtime_.ptr(block), kBlockSize); }

// ---- the sub-mesh submission ----

namespace {
uint32_t u32(const GuestRuntime& rt, GuestAddr a) {
    uint32_t v;
    std::memcpy(&v, rt.ptr(a), 4);
    return v;
}
}   // namespace

SubmitRoute DrawBridge::route(GuestAddr drawable, uint32_t mask) const {
    const uint32_t flags = at<guest::Drawable>(runtime_, drawable).flags, kinds = flags & mask;
    if (kinds & 8) return SubmitRoute::Query;
    if (flags & 0x20) return SubmitRoute::Callback;
    if (kinds & 0x51) return SubmitRoute::Clusters;
    if (u32(runtime_, u64(runtime_, kInlineCallSwitchSlot))) return SubmitRoute::Inline;
    return SubmitRoute::Direct;
}

void DrawBridge::read_prologue(GuestAddr drawable, GuestAddr block, uint32_t sub_mesh, gfx::PrologueInputs& in,
                               GuestAddr& context) const {
    const guest::Drawable& d = at<guest::Drawable>(runtime_, drawable);
    const guest::RenderStateBlock& b = at<guest::RenderStateBlock>(runtime_, block);
    in.sub_mesh = (int32_t)sub_mesh;
    in.sub_meshes = (d.counts >> 4) & 0xf;
    in.applied_mode = b.geometry_mode;
    in.mode_bits = b.mode_bits;
    context = b.renderer;
    // (the model is read only past the range check, as the function does)
    in.geometry_mode = in.sub_mesh < (int32_t)in.sub_meshes ? at<guest::Model>(runtime_, d.model).geometry_mode : 0;
}

GuestAddr DrawBridge::command_buffer(GuestAddr context) const {
    return context ? at<guest::Renderer>(runtime_, context).command_buffer : 0;
}

void DrawBridge::write_prologue(GuestAddr block, const gfx::PrologueOutputs& out) const {
    if (!out.mode_changed) return;
    guest::RenderStateBlock& b = *static_cast<guest::RenderStateBlock*>(runtime_.ptr(block));
    b.geometry_mode = out.applied_mode;
    b.mode_bits = out.mode_bits;
    runtime_.written(block + offsetof(guest::RenderStateBlock, geometry_mode), 1);
    runtime_.written(block + offsetof(guest::RenderStateBlock, mode_bits), 4);
}

namespace {
GuestAddr record_of(const guest::Model& m, uint32_t sub_mesh) {
    return m.records + (uint64_t)((int64_t)(int32_t)sub_mesh * (int64_t)sizeof(guest::SubmeshRecord));
}
}   // namespace

void DrawBridge::read_submit(GuestAddr drawable, GuestAddr block, uint32_t sub_mesh, uint64_t prepared,
                             GuestAddr context, gfx::SubmitInputs& in, gfx::SubmitHandles& h) const {
    in = gfx::SubmitInputs{};
    h = gfx::SubmitHandles{};
    in.prepared = prepared;
    if (!(prepared & 1)) return;
    const guest::Drawable& d = at<guest::Drawable>(runtime_, drawable);
    const guest::RenderStateBlock& b = at<guest::RenderStateBlock>(runtime_, block);
    const guest::Model& m = at<guest::Model>(runtime_, d.model);
    const GuestAddr record = record_of(m, sub_mesh);
    const guest::SubmeshRecord& r = at<guest::SubmeshRecord>(runtime_, record);
    in.instances = d.instances;
    in.recorded = r.flags;
    in.mode_bits = b.mode_bits;
    in.mesh_kind = m.mesh_kind;
    in.mesh_tasks = m.mesh_tasks;
    in.triangles = b.triangles;
    in.sub_mesh_triangles = r.triangles;
    h.command_list = record + offsetof(guest::SubmeshRecord, command_list);
    // (the command buffer is read on every path that records; the one that records nothing reads it harmlessly)
    h.command_buffer = command_buffer(context);
}

void DrawBridge::read_indexed(GuestAddr drawable, uint32_t sub_mesh, gfx::IndexedInputs& in) const {
    const guest::Model& m = at<guest::Model>(runtime_, at<guest::Drawable>(runtime_, drawable).model);
    const GuestAddr desc = at<guest::Mesh>(runtime_, m.mesh).descriptors +
                           (uint64_t)((int64_t)(int32_t)sub_mesh * (int64_t)sizeof(guest::SubmeshDescriptor));
    const guest::SubmeshDescriptor& s = at<guest::SubmeshDescriptor>(runtime_, desc);
    // (the first range, and the last of range_count from it: 8-byte entries, a byte offset then a count)
    in.first_offset = u32(runtime_, s.ranges);
    const GuestAddr last = s.ranges + (uint64_t)((int64_t)(int32_t)s.range_count * 8) - 8;
    in.last_offset = u32(runtime_, last);
    in.last_count = u32(runtime_, last + 4);
    in.index_shift = s.index_type;
    in.mode = u32(runtime_, kPrimitiveTable + 4ull * s.primitive);
    in.type = u32(runtime_, kIndexTypeTable + 4ull * s.index_type);
    in.base_vertex = s.base_vertex;
    in.buffer = at<guest::IndexHolder>(runtime_, s.index_holder).buffer;
}

void DrawBridge::write_submit(GuestAddr block, const gfx::SubmitHandles& h, const gfx::SubmitOutputs& out) const {
    if (out.marked) {
        *static_cast<uint8_t*>(runtime_.ptr(h.command_buffer + guest::kCommandBufferMark)) = 1;
        runtime_.written(h.command_buffer + guest::kCommandBufferMark, 1);
    }
    if (out.counted) {
        static_cast<guest::RenderStateBlock*>(runtime_.ptr(block))->triangles = out.triangles;
        runtime_.written(block + offsetof(guest::RenderStateBlock, triangles), 4);
    }
}

void DrawBridge::snapshot(GuestAddr block, GuestAddr command_buffer, uint8_t* bytes) const {
    std::memcpy(bytes, runtime_.ptr(block), sizeof(guest::RenderStateBlock));
    bytes[sizeof(guest::RenderStateBlock)] =
        command_buffer ? *static_cast<const uint8_t*>(runtime_.ptr(command_buffer + guest::kCommandBufferMark)) : 0;
}

void DrawBridge::expect_prologue(uint8_t* bytes, const gfx::PrologueOutputs& out) {
    if (!out.mode_changed) return;
    bytes[offsetof(guest::RenderStateBlock, geometry_mode)] = out.applied_mode;
    std::memcpy(bytes + offsetof(guest::RenderStateBlock, mode_bits), &out.mode_bits, 4);
}

void DrawBridge::expect_submit(uint8_t* bytes, const gfx::SubmitOutputs& out) {
    if (out.marked) bytes[sizeof(guest::RenderStateBlock)] = 1;
    if (out.counted) std::memcpy(bytes + offsetof(guest::RenderStateBlock, triangles), &out.triangles, 4);
}

}   // namespace hl::bridge
