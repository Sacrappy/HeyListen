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
    uint8_t _3a[30];
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

}   // namespace hl::bridge
