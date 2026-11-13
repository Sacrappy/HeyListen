// HeyListen native engine: the render-state function of the game's draw path (hl/gfx/RenderState.h).
//
// Every value here is the game function's, bit for bit (the call-by-call check in the game compares each call and the
// block's bytes after it): the depth functions are NVN's (2 less, 3 equal, 4 less or equal, 5 greater, 6 not equal,
// 7 greater or equal, 8 always; the 5 and 7 pair is the reversed depth range flag 0x40), the face 3 is front and back.
#include "hl/gfx/RenderState.h"

#include "hl/gfx/Nvn.h"

namespace hl::gfx {

StateOutputs RenderState::decide(const StateInputs& in, const PolygonOffsets& offsets) {
    StateOutputs out;
    const uint64_t f = in.flags;
    const uint32_t k = in.key;
    const bool blend_kind = (k & 6) != 0;
    const bool reversed = (f & 0x40) != 0;

    out.key = in.key;
    out.mode_bits = (in.mode_bits & ~3u) | ((f & 0xc) == 0 ? 2u : 0u) | (!(f & 0x40000) && blend_kind ? 1u : 0u);

    // the depth function and whether the key asks for depth writes
    const uint32_t keyed = (k & 0x80) == 0 ? (reversed ? 7u : 4u) : 3u;
    uint32_t write = !blend_kind && (k & 0x80) == 0 ? 1u : 0u;
    uint32_t func;
    if (in.depth_mode == 5) {
        func = in.channel_masks == 0 ? (reversed ? 5u : 2u) : (reversed ? 7u : 4u);
        write = 1;
    } else {
        const uint32_t base = in.depth_mode == 4 ? 6u : keyed;
        if (in.depth_mode == 4) write = 0;
        func = in.channel_masks != 0 ? base : (reversed ? 5u : 2u);
    }
    const uint32_t test = (f & 0x20) ? 0u : 1u;
    write = (test & write) | (uint32_t)((f >> 2) & 1);
    if (f & 0x20) func = 8;
    if (f & 8) write = 0;

    out.depth_test = (uint8_t)test;
    out.depth_write = (uint8_t)write;
    out.depth_func = (uint8_t)func;
    out.depth_clamp = (uint8_t)((f >> 4) & 1);
    out.blend_targets = (f & 0x1f000) != 0 || (k & 0x46) != 0 ? in.blend_target_mask : 0u;
    out.blend_color[0] = in.blend_color[0];
    out.blend_color[1] = in.blend_color[1];
    out.channel_masks = in.channel_masks;

    // the polygon offset slot: by the blend kind, or from the block in depth mode 8 (32-bit arithmetic, as the game's)
    uint32_t slot;
    if (in.depth_mode == 8) slot = (uint32_t)(int32_t)in.offset_slot_base + 4u;
    else slot = (k & 8) ? 3u : (blend_kind ? 2u : 1u);
    out.offset_slot = in.offset_slot;
    out.offset_clamp[0] = in.offset_clamp[0];
    out.offset_clamp[1] = in.offset_clamp[1];
    out.offset_clamp[2] = in.offset_clamp[2];
    if (slot != in.offset_slot) {
        out.slot_changed = true;
        out.offset_slot = (uint8_t)slot;
        offsets.at((int32_t)slot, out.offset_clamp[0], out.offset_clamp[1]);
        out.offset_clamp[2] = 0;
    }
    out.done = 0xffffffffu;
    return out;
}

void RenderState::emit(const StateInputs& in, const StateOutputs& out, const StateHandles& at, CommandSink& sink) {
    if ((in.flags >> 17) & 1) return;
    const uint64_t cb = at.command_buffer;
    sink.emit(command(Proc::BlendStateSetDefaults, at.blend));
    sink.emit(command(Proc::ColorStateSetDefaults, at.color));
    for (uint32_t i = 0; i < 8; i++) {
        if (!((out.blend_targets >> i) & 1)) continue;
        const uint8_t* b = in.blend[i];
        sink.emit(command(Proc::ColorStateSetBlendEnable, at.color, i, 1));
        sink.emit(command(Proc::BlendStateSetBlendTarget, at.blend, i));
        sink.emit(command(Proc::BlendStateSetBlendFunc, at.blend, b[0], b[2], b[1], b[3]));
        sink.emit(command(Proc::BlendStateSetBlendEquation, at.blend, b[4], b[5]));
        sink.emit(command(Proc::CommandBufferBindBlendState, cb, at.blend));
    }
    sink.emit(command(Proc::ColorStateSetLogicOp, at.color, 3));
    sink.emit(command(Proc::CommandBufferBindColorState, cb, at.color));
    sink.emit(command(Proc::CommandBufferSetBlendColor, cb, at.blend_color));
    const uint32_t m = out.channel_masks;
    for (uint32_t t = 0; t < 8; t++) {
        const uint32_t v = m >> (4 * t);
        sink.emit(command(Proc::ChannelMaskStateSetChannelMask, at.channel_mask, t, v & 1, (v >> 1) & 1, (v >> 2) & 1,
                          (v >> 3) & 1));
    }
    sink.emit(command(Proc::CommandBufferBindChannelMaskState, cb, at.channel_mask));
    sink.emit(command(Proc::DepthStencilStateSetDepthTestEnable, at.depth_stencil, out.depth_test));
    sink.emit(command(Proc::DepthStencilStateSetDepthWriteEnable, at.depth_stencil, out.depth_write));
    sink.emit(command(Proc::DepthStencilStateSetDepthFunc, at.depth_stencil, out.depth_func));
    sink.emit(command(Proc::DepthStencilStateSetStencilTestEnable, at.depth_stencil, in.stencil_test));
    sink.emit(command(Proc::DepthStencilStateSetStencilFunc, at.depth_stencil, 3, in.stencil_func));
    sink.emit(command(Proc::DepthStencilStateSetStencilOp, at.depth_stencil, 3, in.stencil_ops[0], in.stencil_ops[1],
                      in.stencil_ops[2]));
    sink.emit(command(Proc::CommandBufferBindDepthStencilState, cb, at.depth_stencil));
    if (in.stencil_test == 1) {
        sink.emit(command(Proc::CommandBufferSetStencilValueMask, cb, 3, in.stencil_value_mask));
        sink.emit(command(Proc::CommandBufferSetStencilMask, cb, 3, 0xff));
        sink.emit(command(Proc::CommandBufferSetStencilRef, cb, 3, in.stencil_ref));
    }
    sink.emit(command(Proc::PolygonStateSetCullFace, at.polygon, in.cull_face));
    sink.emit(command(Proc::PolygonStateSetFrontFace, at.polygon, 1));
    sink.emit(command(Proc::PolygonStateSetPolygonMode, at.polygon, in.polygon_mode));
    sink.emit(command(Proc::PolygonStateSetPolygonOffsetEnables, at.polygon, (in.polygon_offset & 1) ? 7 : 0));
    sink.emit(command(Proc::CommandBufferBindPolygonState, cb, at.polygon));
    Command clamp = command(Proc::CommandBufferSetPolygonOffsetClamp, cb);
    for (int i = 0; i < 3; i++) clamp.f[i] = out.offset_clamp[i];
    sink.emit(clamp);
    sink.emit(command(Proc::CommandBufferSetDepthClamp, cb, out.depth_clamp));
}

}   // namespace hl::gfx
