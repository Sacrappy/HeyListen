// HeyListen native engine: the render-state function of the game's draw path (docs/native-engine/gfx-record-job.md,
// section 8).
//
// A draw of the scene pass carries a block of render state (built on the stack by the draw function for the draw and
// its sub-meshes). When a sub-mesh's state key differs from the last one applied, the draw path applies the block: it
// derives the depth, stencil, blend, channel-mask and polygon state from the block's flags and the key, writes what it
// derived back into the block (the next sub-mesh and the draw's own code read it), and records the state into the
// command buffer: NVN state objects set up on the stack and bound, then the dynamic values.
//
// RenderState is that function. decide() derives the values (pure: what it reads is in StateInputs, what it writes in
// StateOutputs); emit() records them as commands into a sink (Commands.h), in the order and with the arguments the game's
// function uses. The bridge (hl/bridge/GfxBridge.h) reads the block in place, writes the outputs back, and gives the
// guest addresses the commands name.
#pragma once

#include <cstdint>

#include "hl/gfx/Commands.h"

namespace hl::gfx {

/// What the function reads: the call's state key and the block's fields.
struct StateInputs {
    uint8_t key = 0;               ///< the sub-mesh's state key (bits 1-2 blend kind, bit 3, bit 7: see decide)
    uint64_t flags = 0;            ///< the draw's state flags (bit 17: derive and write only, record nothing)
    uint32_t mode_bits = 0;        ///< flags kept in the block; decide rewrites bits 0 and 1
    uint8_t depth_mode = 0;        ///< 4, 5 and 8 change the depth function and the polygon offset slot
    uint32_t channel_masks = 0;    ///< 4 bits a color target, 8 targets; 0 also changes the depth function
    uint8_t blend_target_mask = 0; ///< the targets blending is enabled on, when blending is on
    uint64_t blend_color[2] = {};  ///< RGBA, four floats
    uint8_t offset_slot = 0;       ///< the polygon offset slot applied last
    int8_t offset_slot_base = 0;   ///< depth mode 8: the slot is this + 4
    uint8_t blend[8][6] = {};      ///< a target: source and destination RGB and alpha factors' bytes, then the equations
    uint8_t stencil_test = 0;
    uint8_t stencil_func = 0;
    uint8_t stencil_ops[3] = {};   ///< fail, depth fail, depth pass
    uint32_t stencil_ref = 0;
    uint32_t stencil_value_mask = 0;
    uint8_t cull_face = 0;
    uint8_t polygon_mode = 0;
    uint8_t polygon_offset = 0;    ///< bit 0: offset on for points, lines and fills
    uint32_t offset_clamp[3] = {}; ///< factor, units, clamp (float bits) applied last
};

/// What the function writes back into the block. decide fills every field; the slot's fields only change when the slot
/// did (slot_changed).
struct StateOutputs {
    uint8_t key = 0;
    uint32_t mode_bits = 0;
    uint8_t depth_test = 0, depth_write = 0, depth_func = 0, depth_clamp = 0;
    uint32_t blend_targets = 0;    ///< the targets blending is enabled on
    uint64_t blend_color[2] = {};
    uint32_t channel_masks = 0;
    bool slot_changed = false;
    uint8_t offset_slot = 0;
    uint32_t offset_clamp[3] = {};
    uint32_t done = 0;             ///< a word the function always sets (all ones)
};

/// Where emit's commands point: the command buffer, the block's blend color (as written), and the five state objects in
/// the function's stack frame (guest addresses, passed through as values).
struct StateHandles {
    uint64_t command_buffer = 0;
    uint64_t blend_color = 0;
    uint64_t blend = 0, color = 0, channel_mask = 0, depth_stencil = 0, polygon = 0;
};

/// The material's polygon offsets by slot (factor and units, float bits), read only when the slot changes.
struct PolygonOffsets {
    virtual ~PolygonOffsets() = default;
    virtual void at(int32_t slot, uint32_t& factor, uint32_t& units) const = 0;
};

class RenderState {
public:
    static StateOutputs decide(const StateInputs& in, const PolygonOffsets& offsets);
    /// The calls the function makes once the outputs are in the block. Nothing when the flags' bit 17 is set.
    static void emit(const StateInputs& in, const StateOutputs& out, const StateHandles& at, CommandSink& sink);
    /// The most commands one call emits (one blend target per enabled bit, the stencil values when the test is on).
    static constexpr uint32_t kMostCommands = 2 + 8 * 5 + 3 + 9 + 7 + 3 + 7;
};

}   // namespace hl::gfx
