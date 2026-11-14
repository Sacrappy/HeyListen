// HeyListen native engine: the shared draw preparation on its cached path (hl/gfx/Prepare.h).
//
// The order of the tests is the game function's: each is the first branch of it that leaves the cached path, so the
// route also says where a call left it (the paths' histogram counts routes). Every value is the function's, bit for bit
// (the call-by-call check compares each call's NVN calls and the bytes it writes).
#include "hl/gfx/Prepare.h"

#include <cstring>

#include "hl/gfx/Nvn.h"

namespace hl::gfx {

const char* route_name(PrepareRoute r) {
    static const char* const kNames[kPrepareRoutes] = {"no material",  "cached",         "program bound", "program buffers",
                                                       "state applied", "state",         "program",        "pending bits",
                                                       "drawable block", "streams",      "inline",
                                                       "clusters"};
    return (uint32_t)r < kPrepareRoutes ? kNames[(uint32_t)r] : "?";
}

namespace {
bool same_float(uint32_t a, uint32_t b) {
    float x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x == y;   // (the function compares the blend color as floats: +0 and -0 equal, a NaN never)
}
}   // namespace

bool Prepare::apply_state(PrepareState& s, uint8_t key, uint8_t state_bits, uint32_t mode, uint64_t flags,
                          uint8_t blend_target_mask, const MaterialState& m) {
    bool changed = false, depth_func_changed = false;
    if (key != s.applied_key) {
        s.applied_key = key;
        // (the key, with its two low bits forced by mode bit 9, over the block's own state bits)
        const uint32_t bits = (uint32_t)state_bits | ((mode & 0x200) ? (key | 3u) : key);
        const uint32_t on = (bits >> 2) & 1, e = s.offset_enables;
        if ((e & 1) != on || ((e >> 1) & 1) != on || ((e >> 2) & 1) != on) {
            s.offset_enables = (uint8_t)((e & 0xfe) | on);   // (bit 0 only: the record turns it into all three)
            changed = true;
        }
        uint8_t cull;
        switch (bits & 3) {
        case 0: cull = 3; break;
        case 1: cull = (bits & 8) ? 1 : 2; break;
        case 2: cull = (bits & 8) ? 2 : 1; break;
        default: cull = 0; break;
        }
        if (cull != s.cull_face) s.cull_face = cull, changed = true;
        const uint32_t p = (bits >> 4) & 3;
        const uint8_t polygon = p == 1 ? 1 : p == 2 ? 0 : 2;
        if (polygon != s.polygon_mode[0] || polygon != s.polygon_mode[1])
            s.polygon_mode[0] = s.polygon_mode[1] = polygon, changed = true;
    }
    if ((mode & 1) && m.id != s.applied_material) {
        s.applied_material = m.id;
        const uint32_t kind = m.kind & 7;
        if (kind == 5 || kind == 2) {
            uint32_t enabled = 0;
            if (m.blend) {
                enabled = blend_target_mask;
                if (!(same_float(s.blend_color[0], m.color[0]) && same_float(s.blend_color[1], m.color[1]) &&
                      same_float(s.blend_color[2], m.color[2]) && same_float(s.blend_color[3], m.color[3]))) {
                    std::memcpy(s.blend_color, m.color, sizeof s.blend_color);
                    changed = true;
                }
                if (!(flags & 0x10000))
                    for (int t = 0; t < 8; t++)
                        if (std::memcmp(s.blend[t], m.targets[t], 6)) {
                            std::memcpy(s.blend[t], m.targets[t], 6);
                            changed = true;
                        }
            }
            if (s.blend_targets != enabled) s.blend_targets = enabled, changed = true;
            if (mode & 2) {
                uint8_t func;
                if (kind == 5) {
                    if (s.depth_test != m.depth_test || s.depth_write != m.depth_write)
                        s.depth_test = m.depth_test, s.depth_write = m.depth_write, changed = true;
                    func = m.depth_func;
                    // (reversed depth: less <-> greater, less-or-equal <-> greater-or-equal)
                    const uint32_t v = (uint32_t)func - 2;
                    if ((flags & 0x40) && v <= 5 && ((0x2du >> v) & 1)) func = (uint8_t)(0x0000040002070005ull >> (v * 8));
                } else {
                    if (!(s.depth_test == 1 && !(s.depth_write & 1))) s.depth_test = 1, s.depth_write = 0, changed = true;
                    func = (flags & 0x40) ? 7 : 4;
                }
                // (a new depth function records the state whatever else changed)
                if (s.depth_func != func) s.depth_func = func, depth_func_changed = true;
            }
        }
    }
    return changed || depth_func_changed;
}

void Prepare::emit_state(const PrepareState& s, const PrepareHandles& at, CommandSink& sink) {
    const uint64_t cb = at.state_buffer;
    sink.emit(command(Proc::BlendStateSetDefaults, at.blend));
    sink.emit(command(Proc::ColorStateSetDefaults, at.color));
    for (uint32_t t = 0; t < 8; t++) {
        if (!((s.blend_targets >> t) & 1)) continue;
        const uint8_t* b = s.blend[t];
        sink.emit(command(Proc::ColorStateSetBlendEnable, at.color, t, 1));
        sink.emit(command(Proc::BlendStateSetBlendTarget, at.blend, t));
        sink.emit(command(Proc::BlendStateSetBlendFunc, at.blend, b[0], b[2], b[1], b[3]));
        sink.emit(command(Proc::BlendStateSetBlendEquation, at.blend, b[4], b[5]));
        sink.emit(command(Proc::CommandBufferBindBlendState, cb, at.blend));
    }
    sink.emit(command(Proc::ColorStateSetLogicOp, at.color, 3));
    sink.emit(command(Proc::CommandBufferBindColorState, cb, at.color));
    sink.emit(command(Proc::CommandBufferSetBlendColor, cb, at.blend_color));
    for (uint32_t t = 0; t < 8; t++) {
        const uint32_t m = s.channel_masks >> (4 * t);
        sink.emit(command(Proc::ChannelMaskStateSetChannelMask, at.channel_mask, t, m & 1, (m >> 1) & 1, (m >> 2) & 1,
                          (m >> 3) & 1));
    }
    sink.emit(command(Proc::CommandBufferBindChannelMaskState, cb, at.channel_mask));
    sink.emit(command(Proc::DepthStencilStateSetDepthTestEnable, at.depth_stencil, s.depth_test));
    sink.emit(command(Proc::DepthStencilStateSetDepthWriteEnable, at.depth_stencil, s.depth_write));
    sink.emit(command(Proc::DepthStencilStateSetDepthFunc, at.depth_stencil, s.depth_func));
    sink.emit(command(Proc::DepthStencilStateSetStencilTestEnable, at.depth_stencil, s.stencil_test));
    sink.emit(command(Proc::DepthStencilStateSetStencilFunc, at.depth_stencil, 3, s.stencil_func));
    sink.emit(command(Proc::DepthStencilStateSetStencilOp, at.depth_stencil, 3, s.stencil_ops[0], s.stencil_ops[1],
                      s.stencil_ops[2]));
    sink.emit(command(Proc::CommandBufferBindDepthStencilState, cb, at.depth_stencil));
    if (s.stencil_test == 1) {
        sink.emit(command(Proc::CommandBufferSetStencilValueMask, cb, 3, s.stencil_value_mask));
        sink.emit(command(Proc::CommandBufferSetStencilMask, cb, 3, 0xff));
        sink.emit(command(Proc::CommandBufferSetStencilRef, cb, 3, s.stencil_ref));
    }
    sink.emit(command(Proc::PolygonStateSetCullFace, at.polygon, s.cull_face));
    sink.emit(command(Proc::PolygonStateSetFrontFace, at.polygon, 1));
    sink.emit(command(Proc::PolygonStateSetPolygonMode, at.polygon, s.polygon_mode[0]));
    sink.emit(command(Proc::PolygonStateSetPolygonOffsetEnables, at.polygon, (s.offset_enables & 1) ? 7 : 0));
    sink.emit(command(Proc::CommandBufferBindPolygonState, cb, at.polygon));
    Command clamp = command(Proc::CommandBufferSetPolygonOffsetClamp, cb);
    for (int i = 0; i < 3; i++) clamp.f[i] = s.offset_clamp[i];
    sink.emit(clamp);
    sink.emit(command(Proc::CommandBufferSetDepthClamp, cb, s.depth_clamp));
}

namespace {
// (a stage and the byte of a binding word that holds its slot, 0xff none: tag 0xff stages 0, 1, 2 from bytes 0, 2, 1; tag
// 0xfe stages 1, 6, 7 from bytes 2, 0, 1)
constexpr uint32_t kStages[2][3][2] = {{{0, 0}, {1, 2}, {2, 1}}, {{1, 2}, {6, 0}, {7, 1}}};

// One binding word's uniform buffer binds (a changed word): false when the call must go to the translation (an object
// whose methods are not the game's known ones, a storage buffer, a tag other than 0xff and 0xfe, the inline switch).
bool bind_word(const PrepareInputs& in, const BufferObject& b, uint32_t word, PrepareOutputs& out) {
    // (the alignment method is asked before anything else, the word -1 included)
    uint32_t align;
    if (b.align_method == in.known.align_fixed) align = 0x100;
    else if (b.align_method == in.known.align_field) align = b.alignment;
    else return false;
    if (word == 0xffffffffu) return true;
    const uint32_t tag = word >> 24;
    if (tag != 0xff && tag != 0xfe) return false;
    // (the ring's slot times the size rounded up to the alignment, in 32 bits, from the base)
    const uint32_t step = (uint32_t)b.slot * (uint32_t)(b.stride >> 4);
    const uint32_t rounded = (b.size + align - 1) & (0u - align);
    const uint64_t address = b.base + (uint64_t)step * rounded;
    // (a stage and the byte of the word that holds its slot, 0xff none: tag 0xff stages 0, 1, 2 from bytes 0, 2, 1; tag 0xfe
    // stages 1, 6, 7 from bytes 2, 0, 1)
    for (const auto& [stage, at] : kStages[tag == 0xff ? 0 : 1]) {
        const uint32_t byte = (word >> (8 * at)) & 0xff;
        if (byte == 0xff) continue;
        if (b.bind_method != in.known.bind_uniform || in.inline_switch) return false;
        PrepareOutputs::Bind& d = out.binds[out.bind_count++];
        d.stage = stage;
        d.slot = (uint32_t)(int32_t)(int8_t)byte;
        d.address = address;
        d.size = b.size;
        out.bind_marked = true;
    }
    return true;
}

// The texture part's binds (a new id with slots): false when the call must go to the translation (a texture object being
// made ready, a tag other than 0xff and 0xfe, the inline switch for a deferred bind). With mode bit 7 each slot binds the
// handle the block's table holds for it (BindTexturesDeferred, the command buffer marked); without, a handle is asked of
// the device for the slot's texture object and bound (BindTexture).
bool bind_textures(const PrepareInputs& in, uint32_t mode, PrepareOutputs& out) {
    const bool deferred = mode & 0x80;
    for (uint32_t i = 0; i < 30; i++) {
        if (!((in.texture_mask >> i) & 1)) continue;
        const PrepareInputs::TextureSlot& s = in.textures[i];
        if (s.word == 0xffffffffu) continue;
        const uint32_t tag = s.word >> 24;
        if (!deferred) {
            if (s.ready) return false;   // (the function waits for the texture or registers it: guest code)
            if (s.texture == 0xffffffffu || s.sampler == -1) continue;
        }
        // (without bit 7 the handle is asked whatever the tag; the binds by the tag, as the uniform blocks')
        if (tag != 0xff && tag != 0xfe) return false;
        PrepareOutputs::Texture& d = out.textures[out.texture_count++];
        d.deferred = deferred;
        d.entry = s.entry;
        d.texture = s.texture;
        d.sampler = (uint32_t)(int32_t)s.sampler;
        for (const auto& [stage, at] : kStages[tag == 0xff ? 0 : 1]) {
            const uint32_t byte = (s.word >> (8 * at)) & 0xff;
            if (byte == 0xff) continue;
            if (deferred && in.inline_switch) return false;
            d.stage[d.stages] = stage;
            d.slot[d.stages++] = (uint32_t)(int32_t)(int8_t)byte;
            if (deferred) out.bind_marked = true;
        }
    }
    return true;
}
}   // namespace

PrepareOutputs Prepare::decide(const PrepareInputs& in, bool state_step) {
    PrepareOutputs out;
    auto left = [](PrepareRoute r) {
        PrepareOutputs none;   // (nothing written, nothing called: the translation does the whole call)
        none.route = r;
        return none;
    };
    if (!in.material) return left(PrepareRoute::NoMaterial);   // (returns 0, nothing written)
    PrepareRoute route = PrepareRoute::Cached;
    if (in.state_key != in.applied_state_key ||
        ((in.mode_bits & 1) && in.material_state != in.applied_material_state)) {
        if (!state_step) return left(PrepareRoute::StateChanged);
        route = PrepareRoute::StateApplied;
        out.state_step = true;
        out.state = in.state;
        const bool record = apply_state(out.state, in.state_key, in.state_bits, in.mode_bits, in.flags,
                                        in.blend_target_mask, in.material_state_in);
        out.state_recorded = record && !(in.flags & 0x20000);   // (bit 17: derive and write only)
    }

    // What the rest reads of the block, as the program step leaves it.
    uint32_t mode = in.mode_bits;
    uint16_t applied_mask = in.applied_mask;
    uint8_t applied_drawable_block = in.applied_drawable_block;
    uint32_t block_binding = in.block_binding;
    if (in.program != in.applied_program) {
        if (route == PrepareRoute::Cached) route = PrepareRoute::ProgramBound;
        out.program_bound = true;
        out.program = in.program;
        out.binder = in.context + kProgramBinder;
        out.program_object = in.program_object;
        out.program_stages = in.program_stages;
        const bool defaults1 = in.program_flags & 0x80, defaults2 = in.program_flags & 0x100;
        // (the first part: skipped when the block already uses its defaults and the program does too)
        bool changed = false;
        if (!((mode & 0x1000) && defaults1)) {
            changed = in.interface_id[0] != in.applied_interface_id[0];
            if (changed) out.id_written[0] = true, out.ids[0] = in.interface_id[0];
            mode = defaults1 ? (mode | 0x1000) : (mode & ~0x1000u);
            out.mode_written = true;
            if (changed) {
                if (in.words[0] != in.applied_words[0]) {
                    if (!bind_word(in, in.buffers[0], in.words[0], out)) return left(PrepareRoute::ProgramChanged);
                    out.word_written[0] = true, out.words[0] = in.words[0];
                }
                if (in.words[1] != in.applied_words[1]) return left(PrepareRoute::ProgramChanged);
                if (in.words[2] != in.applied_words[2]) {
                    // (no bind: the per-drawable block's binding word, and its entry marked to be bound again)
                    out.binding_written = true;
                    out.block_binding = block_binding = in.words[2];
                    applied_drawable_block = 0xff;
                }
            }
        }
        // (the second part: likewise with bit 13 and the program's bit 8)
        if (!((mode & 0x2000) && defaults2)) {
            mode = defaults2 ? (mode | 0x2000) : (mode & ~0x2000u);
            out.mode_written = true;
            if (in.interface_id[1] != in.applied_interface_id[1]) {
                out.id_written[1] = true, out.ids[1] = in.interface_id[1];
                for (int w = 3; w < 5; w++)
                    if (in.words[w] != in.applied_words[w]) {
                        if (!bind_word(in, in.buffers[w - 2], in.words[w], out)) return left(PrepareRoute::ProgramChanged);
                        out.word_written[w] = true, out.words[w] = in.words[w];
                    }
                changed = true;
            }
        }
        if (changed) out.mask_cleared = true, applied_mask = 0;
        // (the textures: unless bit 15, a new id with any slot binds them)
        if (!(mode & 0x8000) && in.interface_id[2] != in.applied_interface_id[2]) {
            out.id_written[2] = true, out.ids[2] = in.interface_id[2];
            if ((in.texture_mask & 0x3fffffffu) && !bind_textures(in, mode, out)) return left(PrepareRoute::ProgramChanged);
        }
        out.mode_bits = mode;
        if ((out.bind_count || out.texture_count) && route == PrepareRoute::ProgramBound) route = PrepareRoute::ProgramBuffers;
    }
    if (in.pending) {
        // (the pending bits less the drawable's; any the block has not applied go to the translation, else they are the
        // block's applied bits from now on)
        const uint32_t bits = (uint32_t)in.pending & ~(uint32_t)in.drawable_mask;
        if (bits & ~(uint32_t)applied_mask) return left(PrepareRoute::Masked);
        out.mask_written = true;
        out.applied_mask = (uint16_t)bits;
    }
    if (in.view_blocks && in.drawable_block != applied_drawable_block && block_binding != 0xffffffffu)
        return left(PrepareRoute::DrawableBlock);
    if (!(mode & 8) || !(in.list_flags & 1)) return left(PrepareRoute::Streams);
    if (in.list_flags & 2) {
        if (in.inline_switch) return left(PrepareRoute::Inline);
        out.marked = out.call_list = true;
    }
    if (in.cluster && ((in.drawable_kinds & 4) || in.cluster_count)) return left(PrepareRoute::Clusters);
    out.route = route;
    out.result = 1;   // (the material entry's first word is not 0)
    return out;
}

void Prepare::emit(const PrepareOutputs& out, const PrepareHandles& at, CommandSink& sink, bool with_state) {
    if (out.state_recorded && with_state) emit_state(out.state, at, sink);
    if (out.program_bound)
        sink.emit(command(Proc::CommandBufferBindProgram, out.binder, out.program_object, out.program_stages));
    for (uint32_t i = 0; i < out.bind_count; i++) {
        const PrepareOutputs::Bind& b = out.binds[i];
        sink.emit(command(Proc::CommandBufferBindUniformBuffer, at.bind_buffer, b.stage, b.slot, b.address, b.size));
    }
    for (uint32_t i = 0; i < out.texture_count; i++) {
        const PrepareOutputs::Texture& x = out.textures[i];
        if (x.deferred) {
            for (uint32_t k = 0; k < x.stages; k++)
                sink.emit(command(Proc::CommandBufferBindTexturesDeferred, at.bind_buffer, x.stage[k], x.slot[k], 1, x.entry));
            continue;
        }
        sink.emit(command(Proc::DeviceGetTextureHandle, at.device, x.texture, x.sampler));
        for (uint32_t k = 0; k < x.stages; k++)
            sink.emit(command(Proc::CommandBufferBindTexture, at.bind_buffer, x.stage[k], x.slot[k], at.handles[i]));
    }
    if (out.call_list) sink.emit(command(Proc::CommandBufferCallCommands, at.command_buffer, 1, at.command_list));
}

}   // namespace hl::gfx
