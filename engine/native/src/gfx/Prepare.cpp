// HeyListen native engine: the shared draw preparation on its cached path (hl/gfx/Prepare.h).
//
// The order of the tests is the game function's: each is the first branch of it that leaves the cached path, so the
// route also says where a call left it (the paths' histogram counts routes). Every value is the function's, bit for bit
// (the call-by-call check compares each call's NVN calls and the bytes it writes).
#include "hl/gfx/Prepare.h"

#include "hl/gfx/Nvn.h"

namespace hl::gfx {

const char* route_name(PrepareRoute r) {
    static const char* const kNames[kPrepareRoutes] = {"no material", "cached",         "state",
                                                       "program",     "pending bits",   "drawable block",
                                                       "streams",     "inline",         "clusters"};
    return (uint32_t)r < kPrepareRoutes ? kNames[(uint32_t)r] : "?";
}

PrepareOutputs Prepare::decide(const PrepareInputs& in) {
    PrepareOutputs out;
    auto left = [&out](PrepareRoute r) {
        out.route = r;
        out.mask_written = out.marked = out.call_list = false;
        return out;
    };
    if (!in.material) return left(PrepareRoute::NoMaterial);   // (returns 0, nothing written)
    if (in.state_key != in.applied_state_key) return left(PrepareRoute::StateChanged);
    if ((in.mode_bits & 1) && in.material_state != in.applied_material_state) return left(PrepareRoute::StateChanged);
    if (in.program != in.applied_program) return left(PrepareRoute::ProgramChanged);
    if (in.pending) {
        // (the pending bits less the drawable's; any the block has not applied go to the translation, else they are the
        // block's applied bits from now on)
        const uint32_t bits = (uint32_t)in.pending & ~(uint32_t)in.drawable_mask;
        if (bits & ~(uint32_t)in.applied_mask) return left(PrepareRoute::Masked);
        out.mask_written = true;
        out.applied_mask = (uint16_t)bits;
    }
    if (in.view_blocks && in.drawable_block != in.applied_drawable_block && in.block_binding != 0xffffffffu)
        return left(PrepareRoute::DrawableBlock);
    if (!(in.mode_bits & 8) || !(in.list_flags & 1)) return left(PrepareRoute::Streams);
    if (in.list_flags & 2) {
        if (in.inline_switch) return left(PrepareRoute::Inline);
        out.marked = out.call_list = true;
    }
    if (in.cluster && ((in.drawable_kinds & 4) || in.cluster_count)) return left(PrepareRoute::Clusters);
    out.route = PrepareRoute::Cached;
    out.result = 1;   // (the material entry's first word is not 0)
    return out;
}

void Prepare::emit(const PrepareOutputs& out, const PrepareHandles& at, CommandSink& sink) {
    if (out.call_list) sink.emit(command(Proc::CommandBufferCallCommands, at.command_buffer, 1, at.command_list));
}

}   // namespace hl::gfx
