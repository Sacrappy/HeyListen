// HeyListen native engine: the shared draw preparation on its cached path (hl/gfx/Prepare.h).
//
// The order of the tests is the game function's: each is the first branch of it that leaves the cached path, so the
// route also says where a call left it (the paths' histogram counts routes). Every value is the function's, bit for bit
// (the call-by-call check compares each call's NVN calls and the bytes it writes).
#include "hl/gfx/Prepare.h"

#include "hl/gfx/Nvn.h"

namespace hl::gfx {

const char* route_name(PrepareRoute r) {
    static const char* const kNames[kPrepareRoutes] = {"no material",  "cached",         "program bound",
                                                       "state",        "program",        "pending bits",
                                                       "drawable block", "streams",      "inline",
                                                       "clusters"};
    return (uint32_t)r < kPrepareRoutes ? kNames[(uint32_t)r] : "?";
}

PrepareOutputs Prepare::decide(const PrepareInputs& in) {
    PrepareOutputs out;
    auto left = [](PrepareRoute r) {
        PrepareOutputs none;   // (nothing written, nothing called: the translation does the whole call)
        none.route = r;
        return none;
    };
    if (!in.material) return left(PrepareRoute::NoMaterial);   // (returns 0, nothing written)
    if (in.state_key != in.applied_state_key) return left(PrepareRoute::StateChanged);
    if ((in.mode_bits & 1) && in.material_state != in.applied_material_state) return left(PrepareRoute::StateChanged);

    // What the rest reads of the block, as the program step leaves it.
    uint32_t mode = in.mode_bits;
    uint16_t applied_mask = in.applied_mask;
    uint8_t applied_drawable_block = in.applied_drawable_block;
    uint32_t block_binding = in.block_binding;
    PrepareRoute route = PrepareRoute::Cached;
    if (in.program != in.applied_program) {
        route = PrepareRoute::ProgramBound;
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
                if (in.words[0] != in.applied_words[0] || in.words[1] != in.applied_words[1])
                    return left(PrepareRoute::ProgramChanged);
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
                if (in.words[3] != in.applied_words[3] || in.words[4] != in.applied_words[4])
                    return left(PrepareRoute::ProgramChanged);
                changed = true;
            }
        }
        if (changed) out.mask_cleared = true, applied_mask = 0;
        // (the textures: unless bit 15, a new id with any slot binds them)
        if (!(mode & 0x8000) && in.interface_id[2] != in.applied_interface_id[2]) {
            out.id_written[2] = true, out.ids[2] = in.interface_id[2];
            if (in.texture_mask & 0x3fffffffu) return left(PrepareRoute::ProgramChanged);
        }
        out.mode_bits = mode;
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

void Prepare::emit(const PrepareOutputs& out, const PrepareHandles& at, CommandSink& sink) {
    if (out.program_bound)
        sink.emit(command(Proc::CommandBufferBindProgram, out.binder, out.program_object, out.program_stages));
    if (out.call_list) sink.emit(command(Proc::CommandBufferCallCommands, at.command_buffer, 1, at.command_list));
}

}   // namespace hl::gfx
