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
        // (read from the shared preparation)
        0x84d67c60, 0x84d67c78,               // bind program, bind uniform buffer
        0x84d66f50, 0x84d67cd0, 0x84d67ce0,   // texture handle, bind texture, bind textures deferred
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

namespace {
// (the block's bytes, then the command buffer object's mark byte: 0 without a command buffer)
void block_and_mark(const GuestRuntime& rt, GuestAddr block, GuestAddr command_buffer, uint8_t* bytes) {
    std::memcpy(bytes, rt.ptr(block), sizeof(guest::RenderStateBlock));
    bytes[sizeof(guest::RenderStateBlock)] =
        command_buffer ? *static_cast<const uint8_t*>(rt.ptr(command_buffer + guest::kCommandBufferMark)) : 0;
}
}   // namespace

void DrawBridge::snapshot(GuestAddr block, GuestAddr command_buffer, uint8_t* bytes) const {
    block_and_mark(runtime_, block, command_buffer, bytes);
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

// ---- the shared preparation ----

namespace {
uint8_t u8(const GuestRuntime& rt, GuestAddr a) { return *static_cast<const uint8_t*>(rt.ptr(a)); }
uint16_t u16(const GuestRuntime& rt, GuestAddr a) {
    uint16_t v;
    std::memcpy(&v, rt.ptr(a), 2);
    return v;
}
}   // namespace

namespace {
// (the block's render state for the preparation's state step, and back)
void read_state(const guest::RenderStateBlock& b, gfx::PrepareState& s) {
    s.applied_key = b.state_key_applied;
    s.applied_material = b.done;
    s.offset_enables = b.polygon_offset;
    s.cull_face = b.cull_face;
    s.polygon_mode[0] = b.polygon_mode, s.polygon_mode[1] = b.polygon_mode_b;
    s.depth_test = b.depth_test, s.depth_write = b.depth_write, s.depth_func = b.depth_func;
    s.blend_targets = b.blend_targets;
    std::memcpy(s.blend_color, b.blend_color, sizeof s.blend_color);
    std::memcpy(s.blend, b.blend, sizeof s.blend);
    s.channel_masks = b.channel_masks;
    s.stencil_test = b.stencil_test, s.stencil_func = b.stencil_func;
    std::memcpy(s.stencil_ops, b.stencil_ops, sizeof s.stencil_ops);
    s.stencil_ref = b.stencil_ref, s.stencil_value_mask = b.stencil_value_mask;
    std::memcpy(s.offset_clamp, b.offset_clamp, sizeof s.offset_clamp);
    s.depth_clamp = b.depth_clamp;
}
void put_state(guest::RenderStateBlock& b, const gfx::PrepareState& s) {
    b.state_key_applied = s.applied_key;
    b.done = s.applied_material;
    b.polygon_offset = s.offset_enables;
    b.cull_face = s.cull_face;
    b.polygon_mode = s.polygon_mode[0], b.polygon_mode_b = s.polygon_mode[1];
    b.depth_test = s.depth_test, b.depth_write = s.depth_write, b.depth_func = s.depth_func;
    b.blend_targets = s.blend_targets;
    std::memcpy(b.blend_color, s.blend_color, sizeof b.blend_color);
    std::memcpy(b.blend, s.blend, sizeof b.blend);
}
}   // namespace

void PrepareBridge::state_handles(GuestAddr block, uint64_t sp, gfx::PrepareHandles& h) const {
    const guest::RenderStateBlock& b = at<guest::RenderStateBlock>(runtime_, block);
    h.state_buffer = h.bind_buffer = at<guest::Renderer>(runtime_, b.renderer).command_buffer;
    h.blend = sp - guest::kPrepBlend;
    h.color = sp - guest::kPrepColor;
    h.channel_mask = sp - guest::kPrepChannelMask;
    h.depth_stencil = sp - guest::kPrepDepthStencil;
    h.polygon = sp - guest::kPrepPolygon;
    h.blend_color = block + offsetof(guest::RenderStateBlock, blend_color);
}

GuestAddr PrepareBridge::material(GuestAddr block, GuestAddr drawable) const {
    const guest::RenderStateBlock& b = at<guest::RenderStateBlock>(runtime_, block);
    // (the block's slot, or the drawable's pass slot for the block's pass: an unsigned byte index)
    uint8_t slot = b.pass_slot;
    if (slot == 0xff) slot = u8(runtime_, drawable + guest::kDrawablePassSlots + b.depth_mode);
    // (the slot is a signed byte index into the material slots, whose entry is signed too)
    const int8_t entry = (int8_t)u8(runtime_, drawable + guest::kDrawableMaterialSlots + (uint64_t)(int64_t)(int8_t)slot);
    if (entry == -1) return u64(runtime_, kDefaultMaterialSlot);
    const guest::Drawable& d = at<guest::Drawable>(runtime_, drawable);
    return d.materials + (uint64_t)((int64_t)d.material_stride * (int64_t)entry);
}

void PrepareBridge::read(GuestAddr block, GuestAddr drawable, gfx::PrepareInputs& in, gfx::PrepareHandles& h) const {
    in = gfx::PrepareInputs{};
    h = gfx::PrepareHandles{};
    const GuestAddr m = material(block, drawable);
    const guest::MaterialEntry& e = at<guest::MaterialEntry>(runtime_, m);
    in.material = e.first != 0;
    if (!in.material) return;
    const guest::Drawable& d = at<guest::Drawable>(runtime_, drawable);
    const guest::RenderStateBlock& b = at<guest::RenderStateBlock>(runtime_, block);
    in.state_key = d.state_key;
    in.applied_state_key = b.state_key_applied;
    in.mode_bits = b.mode_bits;
    // (the state step's: the block's own fields, and the material state where the function reads it)
    in.flags = b.flags;
    in.state_bits = b.state_bits;
    in.blend_target_mask = b.blend_target_mask;
    read_state(b, in.state);
    if (in.mode_bits & 1) {
        const guest::MaterialStateObject& s = at<guest::MaterialStateObject>(runtime_, d.material_state);
        gfx::MaterialState& ms = in.material_state_in;
        ms.id = s.id;
        ms.kind = s.kind;
        ms.blend = s.blend;
        std::memcpy(ms.color, s.color, sizeof ms.color);
        std::memcpy(ms.targets, s.targets, sizeof ms.targets);
        ms.depth_test = s.depth_test, ms.depth_write = s.depth_write, ms.depth_func = s.depth_func;
        in.material_state = s.id;
        in.applied_material_state = b.done;
    }
    in.program = e.program;
    in.applied_program = b.program;
    // (the global switch and the known methods: the game's own globals, read whatever the route)
    in.inline_switch = u32(runtime_, u64(runtime_, kInlineCallSwitchSlot)) != 0;
    in.known.align_fixed = u64(runtime_, guest::kAlignFixedSlot);
    in.known.align_field = u64(runtime_, guest::kAlignFieldSlot);
    in.known.bind_uniform = u64(runtime_, guest::kBindUniformSlot);
    in.known.bind_storage = u64(runtime_, guest::kBindStorageSlot);
    if (in.program != in.applied_program) read_program(block, in);
    // (the block's and the drawable's own fields whatever the route; the material's table as the function reads it at
    // every call that gets this far)
    in.pending = b.pending;
    in.drawable_mask = d.pending_mask;
    in.applied_mask = b.applied_bits;
    in.view_blocks = at<guest::Material>(runtime_, b.material).drawable_blocks != 0;
    in.drawable_block = d.block_entry;
    in.applied_drawable_block = b.drawable_block;
    in.block_binding = b.block_binding;
    if (in.view_blocks && (in.drawable_block != in.applied_drawable_block || in.program != in.applied_program)) {
        // (the drawable's entry: its signed index, -1 or past the count taking entry 0; each entry is a buffer object
        // with a made-ready word)
        const uint32_t count = u32(runtime_, b.material + guest::kDrawableBlockCount);
        const int32_t s = (int8_t)d.block_entry;
        const int32_t index = s == -1 || (uint32_t)s >= count ? 0 : s;
        const GuestAddr e = u64(runtime_, b.material + guest::kDrawableBlockTable) + (uint64_t)((int64_t)index * guest::kDrawableBlockSize);
        const guest::BufferObjectG& g = at<guest::BufferObjectG>(runtime_, e);
        gfx::BufferObject& o = in.drawable_buffer;
        o.align_method = u64(runtime_, g.vtable + guest::kBufferAlignMethod);
        o.bind_method = u64(runtime_, g.vtable + guest::kBufferBindMethod);
        o.size = g.size, o.slot = g.slot, o.stride = g.stride, o.base = g.base, o.alignment = g.alignment;
        in.drawable_ready = __atomic_load_n(static_cast<const uint32_t*>(runtime_.ptr(e + guest::kDrawableBlockReady)), __ATOMIC_ACQUIRE);
        h.drawable_entry = e;
    }
    // (the function reads the list selector before it tests the mode bit)
    const uint32_t list = u16(runtime_, d.lists + guest::kListsSelector) & 3;
    if (!(in.mode_bits & 8)) return;
    in.list_flags = e.lists[list].flags;
    if (!(in.list_flags & 1)) return;
    if (in.list_flags & 2) {
        h.command_buffer = at<guest::Renderer>(runtime_, b.renderer).command_buffer;
        if (in.inline_switch) return;
        h.command_list = m + offsetof(guest::MaterialEntry, lists) + list * sizeof(guest::MaterialEntry::List);
    }
    in.cluster = b.cluster;
    if (in.cluster) {
        in.drawable_kinds = (uint8_t)d.flags;
        if (!(in.drawable_kinds & 4)) in.cluster_count = d.cluster_count;
    }
}

void PrepareBridge::read_program(GuestAddr block, gfx::PrepareInputs& in) const {
    const guest::RenderStateBlock& b = at<guest::RenderStateBlock>(runtime_, block);
    const guest::Program& p = at<guest::Program>(runtime_, in.program);
    in.context = b.renderer;
    in.program_object = p.object;
    in.program_stages = p.stages;
    in.program_flags = p.flags;
    in.applied_interface_id[0] = b.interface_id_a;
    in.applied_interface_id[1] = b.interface_id_b;
    in.applied_interface_id[2] = b.texture_id;
    in.applied_words[0] = b.words_a[0], in.applied_words[1] = b.words_a[1], in.applied_words[2] = b.block_binding;
    in.applied_words[3] = b.words_b[0], in.applied_words[4] = b.words_b[1];
    const uint32_t mode = b.mode_bits;
    const bool defaults1 = p.flags & 0x80, defaults2 = p.flags & 0x100;
    // (a part the function skips is not read: the block already uses the defaults and the program does too)
    if (!((mode & 0x1000) && defaults1)) {
        in.interface_id[0] = u32(runtime_, p.part[0]);
        const GuestAddr w = defaults1 ? guest::kDefaultWordsA : p.part[0] + 4;
        for (int i = 0; i < 3; i++) in.words[i] = u32(runtime_, w + 4 * i);
    }
    if (!((mode & 0x2000) && defaults2)) {
        in.interface_id[1] = u32(runtime_, p.part[1]);
        const GuestAddr w = defaults2 ? guest::kDefaultWordsB : p.part[1] + 0x28;
        for (int i = 0; i < 2; i++) in.words[3 + i] = u32(runtime_, w + 4 * i);
    }
    // (the buffer objects of the words that changed: word 0's at the block's +0x30, words 3 and 4's in the material)
    const GuestAddr objects[3] = {b.buffer, 0, 0};
    const int word_of[3] = {0, 3, 4};
    for (int i = 0; i < 3; i++) {
        if (in.words[word_of[i]] == in.applied_words[word_of[i]]) continue;
        const GuestAddr o = i ? at<guest::Material>(runtime_, b.material).buffers[i - 1] : objects[0];
        const guest::BufferObjectG& g = at<guest::BufferObjectG>(runtime_, o);
        gfx::BufferObject& d = in.buffers[i];
        d.align_method = u64(runtime_, g.vtable + guest::kBufferAlignMethod);
        d.bind_method = u64(runtime_, g.vtable + guest::kBufferBindMethod);
        d.size = g.size, d.slot = g.slot, d.stride = g.stride, d.base = g.base, d.alignment = g.alignment;
    }
    if (!(mode & 0x8000)) {
        in.interface_id[2] = u32(runtime_, p.part[2]);
        in.texture_mask = u32(runtime_, p.part[2] + 4);
        // (the slots, where the function reads them: a new id with slots, without mode bit 7; the texture object only
        // for a word not -1, the device only once a handle is asked)
        if (in.interface_id[2] != in.applied_interface_id[2] && (in.texture_mask & 0x3fffffffu)) {
            bool ask = false;
            for (uint32_t i = 0; i < 30; i++) {
                if (!((in.texture_mask >> i) & 1)) continue;
                gfx::PrepareInputs::TextureSlot& s = in.textures[i];
                s.word = u32(runtime_, p.part[2] + 8 + 4 * i);
                if (s.word == 0xffffffffu) continue;
                if (mode & 0x80) {
                    // (the handle's entry: the material's per-slot index (a signed byte, 16 bytes a slot) into the
                    // block's handle table)
                    const int8_t e = (int8_t)u8(runtime_, b.material + guest::kMaterialHandleIndex + 16 * i);
                    s.entry = b.handle_table + (uint64_t)((int64_t)e * 8);
                    continue;
                }
                const GuestAddr o = u64(runtime_, b.textures + 8 * i);
                const guest::TextureObject& x = at<guest::TextureObject>(runtime_, o);
                s.ready = __atomic_load_n(&x.ready, __ATOMIC_ACQUIRE);
                if (s.ready) break;   // (the function goes no further natively)
                s.texture = x.texture, s.sampler = x.sampler;
                ask |= s.texture != 0xffffffffu && s.sampler != -1;
            }
            if (ask) in.device = u64(runtime_, u64(runtime_, u64(runtime_, guest::kDeviceHolderSlot)) + guest::kDeviceOffset);
        }
    }
}

namespace {
// (the program step's writes, in the function's order, on the block's bytes)
void put_program(guest::RenderStateBlock& b, const gfx::PrepareOutputs& out) {
    if (!out.program_bound) return;
    b.program_aux = 0;
    b.program = out.program;
    if (out.id_written[0]) b.interface_id_a = out.ids[0];
    if (out.mode_written) b.mode_bits = out.mode_bits;
    if (out.binding_written) b.block_binding = out.block_binding, b.drawable_block = 0xff;
    if (out.word_written[0]) b.words_a[0] = out.words[0];
    if (out.word_written[3]) b.words_b[0] = out.words[3];
    if (out.word_written[4]) b.words_b[1] = out.words[4];
    if (out.id_written[1]) b.interface_id_b = out.ids[1];
    if (out.mask_cleared) b.applied_bits = 0;
    if (out.id_written[2]) b.texture_id = out.ids[2];
}
}   // namespace

void PrepareBridge::write(GuestAddr block, const gfx::PrepareHandles& h, const gfx::PrepareOutputs& out) const {
    if (out.state_step) {
        put_state(*static_cast<guest::RenderStateBlock*>(runtime_.ptr(block)), out.state);
        runtime_.written(block, sizeof(guest::RenderStateBlock));
    }
    if (out.program_bound) {
        put_program(*static_cast<guest::RenderStateBlock*>(runtime_.ptr(block)), out);
        // (the fields the step may write: +0x5c to +0xdf)
        runtime_.written(block + offsetof(guest::RenderStateBlock, mode_bits),
                         offsetof(guest::RenderStateBlock, program) + 8 - offsetof(guest::RenderStateBlock, mode_bits));
    }
    if (out.drawable_bound) {
        guest::RenderStateBlock& b = *static_cast<guest::RenderStateBlock*>(runtime_.ptr(block));
        b.drawable_block = out.drawable_block;
        runtime_.written(block + offsetof(guest::RenderStateBlock, drawable_block), 1);
        // (the entry's made-ready word: the function sets its bit 0 atomically at every bind)
        __atomic_fetch_or(static_cast<uint32_t*>(runtime_.ptr(h.drawable_entry + guest::kDrawableBlockReady)), 1u, __ATOMIC_ACQ_REL);
    }
    if (out.mask_written) {
        static_cast<guest::RenderStateBlock*>(runtime_.ptr(block))->applied_bits = out.applied_mask;
        runtime_.written(block + offsetof(guest::RenderStateBlock, applied_bits), 2);
    }
    if (out.marked || out.bind_marked) {
        const GuestAddr cb = out.marked ? h.command_buffer : h.bind_buffer;
        *static_cast<uint8_t*>(runtime_.ptr(cb + guest::kCommandBufferMark)) = 1;
        runtime_.written(cb + guest::kCommandBufferMark, 1);
    }
}

void PrepareBridge::snapshot(GuestAddr block, GuestAddr command_buffer, uint8_t* bytes) const {
    block_and_mark(runtime_, block, command_buffer, bytes);
}

void PrepareBridge::expect(uint8_t* bytes, const gfx::PrepareOutputs& out) {
    if (out.state_step || out.program_bound || out.drawable_bound) {
        guest::RenderStateBlock b;
        std::memcpy(&b, bytes, sizeof b);
        if (out.state_step) put_state(b, out.state);
        put_program(b, out);
        if (out.drawable_bound) b.drawable_block = out.drawable_block;
        std::memcpy(bytes, &b, sizeof b);
    }
    if (out.mask_written) std::memcpy(bytes + offsetof(guest::RenderStateBlock, applied_bits), &out.applied_mask, 2);
    if (out.marked || out.bind_marked) bytes[sizeof(guest::RenderStateBlock)] = 1;
}

}   // namespace hl::bridge
