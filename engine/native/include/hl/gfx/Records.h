// HeyListen native engine: the graphics front end writing the back end's command records itself
// (docs/native-engine/direct-emission.md, sections 3.2 and 5, bricks A and B).
//
// An NVN procedure of the OS layer reads its arguments from guest registers and appends one typed record (hl/c/
// hl_gfx_rec.h's layouts) to the command buffer's host segment. RecordWriter appends the same records directly, one
// typed method a record kind, each writing the bytes and the accounting the procedure would have. RecordSink is a
// CommandSink (Commands.h) that turns the commands native code already emits into those records: the setters of NVN
// state objects are applied to native copies of the objects (keyed by their guest address, never written back), and a
// bind appends the copy's bytes. The code that emits does not change.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hl/c/hl_gfx_rec.h"
#include "hl/gfx/Commands.h"

namespace hl::gfx {

/// Where records go: a command buffer's segment (the OS layer's, through hl_gfx_rec_host) or a scratch one.
class RecordTarget {
public:
    virtual ~RecordTarget() = default;
    /// A record of `op` with `payload` bytes: its header written, its payload returned zeroed. `hw` other than 0 is
    /// what the real driver would have written for it (the accounting the game reads back), as the NVN layer's hw_fix.
    virtual void* append(uint32_t op, uint32_t payload, uint32_t hw) = 0;
};

/// A segment in host memory that keeps the NVN layer's accounting as a command buffer does: for the check and tests.
class ScratchSegment final : public RecordTarget {
public:
    /// Empty, with the accounting of `from` (offset 0).
    void reset(const hl_rec_mark& from);
    void* append(uint32_t op, uint32_t payload, uint32_t hw) override;
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    /// The accounting now, the offset being the bytes written.
    hl_rec_mark mark() const;

private:
    std::vector<uint8_t> bytes_;
    hl_rec_mark at_{};
};

/// The size of a record of `payload` bytes: header included, 8-aligned (the NVN layer's rec()).
constexpr uint32_t record_size(uint32_t payload) { return (8 + payload + 7) & ~7u; }

/// One typed method a record kind, each writing what the NVN procedure named would have.
class RecordWriter {
public:
    explicit RecordWriter(RecordTarget& target) : t_(target) {}

    /// Bind{Blend,Color,ChannelMask,Multisample,Polygon,DepthStencil}State: the object's bytes (n at most 24).
    void bind_state(int32_t kind, const void* bytes, uint32_t n);
    /// SetStencilValueMask (which 0), SetStencilMask (1), SetStencilRef (2).
    void stencil(int32_t face, int32_t which, int32_t value);
    /// SetPolygonOffsetClamp: the three floats' bits.
    void poly_offset(uint32_t factor, uint32_t units, uint32_t clamp);
    /// SetBlendColor: four floats (16 bytes).
    void blend_color(const void* rgba);
    void depth_clamp(bool on) { flag(HL_REC_DEPTH_CLAMP, on); }
    void render_enable(bool on) { flag(HL_REC_RENDER_ENABLE, on); }
    void raster_discard(bool on) { flag(HL_REC_RASTER_DISCARD, on); }
    /// SetDepthRange.
    void depth_range(float n, float f);
    /// SetViewports(first, n, v): x, y, w, h a viewport. False (nothing written) past the 16 the record holds.
    bool viewports(int32_t first, int32_t n, const float (*v)[4]);
    /// SetScissor.
    void scissor(int32_t x, int32_t y, int32_t w, int32_t h);
    /// BindUniformBuffer / BindStorageBuffer.
    void bind_ubo(int32_t stage, int32_t index, uint64_t addr, uint64_t size) { bind_buf(HL_REC_BIND_UBO, stage, index, addr, size); }
    void bind_ssbo(int32_t stage, int32_t index, uint64_t addr, uint64_t size) { bind_buf(HL_REC_BIND_SSBO, stage, index, addr, size); }
    /// Texture, sampler and image handles (op HL_REC_BIND_TEX / _SAMP / _IMG): src the game's array for a deferred bind
    /// (read again at the submit), 0 otherwise; hw as the procedure: 0 for BindTexture(s) and BindTexturesDeferred
    /// (16 + 4 a handle), 24 for one separate texture, sampler or image, 16 + 8 a handle for their arrays. False past 32.
    bool bind_handles(uint32_t op, int32_t stage, int32_t first, int32_t n, const uint64_t* handles, uint64_t src,
                      uint32_t hw);
    /// BindVertexBuffers(first, n, ranges). False past 16.
    bool bind_vertex_buffers(int32_t first, int32_t n, const hl_rec_range* ranges);
    /// BindVertexAttribState(n, states), BindVertexStreamState(n, states). False past 16.
    bool bind_vertex_attribs(int32_t n, const uint32_t* states);
    bool bind_vertex_streams(int32_t n, const hl_rec_vstream_st* states);
    /// DrawArrays (instances 1, base instance 0) and DrawArraysInstanced (nothing for no instance: as the procedure).
    void draw(int32_t prim, int32_t first, int32_t count, int32_t base_instance = 0, int32_t instances = 1);
    /// DrawElements (hw 48), DrawElementsBaseVertex and DrawElementsInstanced (hw 44; nothing for no instance).
    void draw_indexed(int32_t prim, int32_t itype, int32_t count, uint64_t ibuf, int32_t base_vertex,
                      int32_t base_instance, int32_t instances, uint32_t hw);
    /// Barrier(bits).
    void barrier(int32_t bits);

private:
    template <class T> T* rec(uint32_t op, uint32_t payload = sizeof(T), uint32_t hw = 0) {
        return static_cast<T*>(t_.append(op, payload, hw));
    }
    void flag(uint32_t op, bool on);
    void bind_buf(uint32_t op, int32_t stage, int32_t index, uint64_t addr, uint64_t size);
    RecordTarget& t_;
};

/// Guest memory as the NVN procedures read their pointer arguments.
class GuestReader {
public:
    virtual ~GuestReader() = default;
    /// n bytes at a guest address (the OS layer stops the game when they are outside the guest window, as the NVN
    /// procedures do).
    virtual const void* read(uint64_t guest, uint32_t n) = 0;
};

/// The commands of Nvn.h's table turned into records for one command buffer (a native unit of work: one call of the
/// render-state function). State objects are native copies for the sink's life, each seeded from its guest bytes at
/// its first use (an object the setters do not cover whole keeps the bits it had, as with the procedures).
class RecordSink final : public CommandSink {
public:
    RecordSink(RecordTarget& target, GuestReader& guest, uint64_t command_buffer)
        : w_(target), guest_(guest), cb_(command_buffer) {}
    void emit(const Command& c) override;
    /// Records appended.
    uint32_t records() const { return records_; }
    /// Commands not written: for another command buffer, a procedure this sink does not know, or more state objects
    /// than it keeps. Zero for the render-state function; the caller counts anything else as a failure.
    uint32_t stray() const { return stray_; }

private:
    struct Object {
        uint64_t at;
        uint8_t bytes[8];
    };
    static constexpr uint32_t kObjects = 8;
    uint8_t* object(uint64_t at, uint32_t size);
    void bind(int32_t kind, uint64_t at, uint32_t size);

    RecordWriter w_;
    GuestReader& guest_;
    uint64_t cb_;
    Object objects_[kObjects];
    uint32_t n_objects_ = 0, records_ = 0, stray_ = 0;
};

}   // namespace hl::gfx
