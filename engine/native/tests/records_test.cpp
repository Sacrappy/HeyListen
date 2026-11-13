// HeyListen native engine: the back end's records written by native code (hl/gfx/Records.h), run with no game
// (tests/test_native_engine.py).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hl/c/hl_gfx_rec.h"
#include "hl/gfx/Nvn.h"
#include "hl/gfx/Records.h"
#include "hl/gfx/RenderState.h"

using namespace hl;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

template <class T> T at(const std::vector<uint8_t>& b, size_t off) {
    T v;
    std::memcpy(&v, b.data() + off, sizeof v);
    return v;
}
hl_rec_hdr header(const std::vector<uint8_t>& b, size_t off) { return at<hl_rec_hdr>(b, off); }

// A guest memory of 4 KB at guest address 0x1000.
struct FakeGuest final : gfx::GuestReader {
    uint8_t memory[4096] = {};
    uint8_t* ptr(uint64_t a) { return memory + (a - 0x1000); }
    const void* read(uint64_t guest, uint32_t n) override {
        CHECK(guest >= 0x1000 && guest + n <= 0x1000 + sizeof memory);
        return ptr(guest);
    }
};

void every_record_kind_has_its_layout_and_accounting() {
    gfx::ScratchSegment s;
    s.reset(hl_rec_mark{999, 100, 50, 7});
    CHECK(s.mark().offset == 0 && s.mark().cmd_used == 100 && s.mark().hw_used == 50);
    gfx::RecordWriter w(s);
    size_t off = 0;
    auto next = [&](uint32_t op, uint32_t size) {
        const hl_rec_hdr h = header(s.bytes(), off);
        CHECK(h.op == op && h.size == size && s.bytes().size() == off + size);
        const size_t p = off + 8;
        off += size;
        return p;
    };

    const uint64_t blend = 0x123456789ull;
    w.bind_state(HL_REC_BLEND, &blend, 8);
    size_t p = next(HL_REC_BIND_STATE, 40);
    CHECK(at<int32_t>(s.bytes(), p) == 0 && at<uint64_t>(s.bytes(), p + 4) == blend && at<uint64_t>(s.bytes(), p + 12) == 0);
    // (no hw size: the record counted whole in both sums)
    CHECK(s.mark().cmd_used == 140 && s.mark().hw_used == 90 && s.mark().last_rec == 40);

    w.stencil(3, 2, 0x22);
    p = next(HL_REC_STENCIL, 32);
    CHECK(at<int32_t>(s.bytes(), p) == 3 && at<int32_t>(s.bytes(), p + 4) == 0 && at<int32_t>(s.bytes(), p + 12) == 0x22 &&
          at<int32_t>(s.bytes(), p + 16) == 2);
    w.stencil(3, 0, 0x33);
    p = next(HL_REC_STENCIL, 32);
    CHECK(at<int32_t>(s.bytes(), p + 4) == 0x33 && at<int32_t>(s.bytes(), p + 12) == 0);

    w.poly_offset(0x3f800000, 0x40000000, 0x80000001);
    p = next(HL_REC_POLY_OFFSET, 24);
    CHECK(at<uint32_t>(s.bytes(), p) == 0x3f800000 && at<uint32_t>(s.bytes(), p + 8) == 0x80000001);

    const float rgba[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    w.blend_color(rgba);
    p = next(HL_REC_BLEND_COLOR, 24);
    CHECK(at<float>(s.bytes(), p + 12) == 1.0f);

    w.depth_clamp(true);
    p = next(HL_REC_DEPTH_CLAMP, 16);
    CHECK(at<int32_t>(s.bytes(), p) == 1 && at<int32_t>(s.bytes(), p + 4) == 0);
    w.render_enable(false);
    next(HL_REC_RENDER_ENABLE, 16);
    w.raster_discard(true);
    next(HL_REC_RASTER_DISCARD, 16);

    w.depth_range(0.0f, 1.0f);
    p = next(HL_REC_DEPTH_RANGE, 16);
    CHECK(at<float>(s.bytes(), p + 4) == 1.0f);

    const float vp[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    CHECK(w.viewports(0, 2, vp));
    p = next(HL_REC_VIEWPORTS, 8 + 8 + 256);   // (the whole record, whatever n)
    CHECK(at<int32_t>(s.bytes(), p + 4) == 2 && at<float>(s.bytes(), p + 8 + 16 + 12) == 8.0f);
    CHECK(!w.viewports(15, 2, vp) && s.bytes().size() == off);

    w.scissor(1, 2, 3, 4);
    p = next(HL_REC_SCISSOR, 24);
    CHECK(at<int32_t>(s.bytes(), p + 12) == 4);

    // (a buffer bind: the driver's 20 bytes in the hw sum)
    const hl_rec_mark before = s.mark();
    w.bind_ubo(1, 3, 0x1000000, 0x100);
    p = next(HL_REC_BIND_UBO, 32);
    CHECK(at<int32_t>(s.bytes(), p + 4) == 3 && at<uint64_t>(s.bytes(), p + 8) == 0x1000000 && at<uint64_t>(s.bytes(), p + 16) == 0x100);
    CHECK(s.mark().cmd_used == before.cmd_used + 32 && s.mark().hw_used == before.hw_used + 20 && s.mark().last_rec == 20);
    w.bind_ssbo(0, 1, 2, 3);
    next(HL_REC_BIND_SSBO, 32);

    const uint64_t h[3] = {7, 8, 9};
    const hl_rec_mark b2 = s.mark();
    CHECK(w.bind_handles(HL_REC_BIND_TEX, 4, 2, 3, h, 0x5000, 0));
    p = next(HL_REC_BIND_TEX, 8 + 24 + 24);
    CHECK(at<int32_t>(s.bytes(), p + 8) == 3 && at<uint64_t>(s.bytes(), p + 16) == 0x5000 && at<uint64_t>(s.bytes(), p + 24 + 16) == 9);
    CHECK(s.mark().hw_used == b2.hw_used + 16 + 12 && s.mark().last_rec == 28);
    CHECK(w.bind_handles(HL_REC_BIND_SAMP, 0, 0, 1, h, 0, 24));
    next(HL_REC_BIND_SAMP, 8 + 24 + 8);
    CHECK(s.mark().last_rec == 24);
    CHECK(!w.bind_handles(HL_REC_BIND_IMG, 0, 0, 33, h, 0, 0));

    const hl_rec_range r[1] = {{0x2000, 0x40}};
    CHECK(w.bind_vertex_buffers(2, 1, r));
    p = next(HL_REC_BIND_VBUF, 8 + 8 + 16);
    CHECK(at<int32_t>(s.bytes(), p) == 2 && at<uint64_t>(s.bytes(), p + 16) == 0x40 && s.mark().last_rec == 24);
    const uint32_t va[3] = {1, 2, 3};
    CHECK(w.bind_vertex_attribs(3, va));
    p = next(HL_REC_BIND_VATTR, 8 + 8 + 16);   // (8 + 12 rounded up)
    CHECK(at<uint32_t>(s.bytes(), p + 16) == 3 && s.mark().last_rec == 68);
    const hl_rec_vstream_st vs[2] = {{16, 0}, {32, 1}};
    CHECK(w.bind_vertex_streams(2, vs));
    p = next(HL_REC_BIND_VSTREAM, 8 + 8 + 16);
    CHECK(at<uint32_t>(s.bytes(), p + 16) == 32 && s.mark().last_rec == 4 * 18);

    w.draw(4, 0, 6);
    p = next(HL_REC_DRAW, 32);
    CHECK(at<int32_t>(s.bytes(), p + 16) == 1);
    w.draw(4, 0, 6, 0, 0);   // (no instance: nothing)
    CHECK(s.bytes().size() == off);
    w.draw_indexed(4, 2, 36, 0x3000, -1, 2, 5, 44);
    p = next(HL_REC_DRAW_IDX, 40);
    CHECK(at<int32_t>(s.bytes(), p + 12) == -1 && at<int32_t>(s.bytes(), p + 20) == 5 && at<uint64_t>(s.bytes(), p + 24) == 0x3000);
    CHECK(s.mark().last_rec == 44);

    w.barrier(0x13);
    p = next(HL_REC_BARRIER, 16);
    CHECK(at<int32_t>(s.bytes(), p) == 0x13);
    CHECK(s.mark().offset == off && s.mark().cmd_used == 100 + off);
}

// The NVN layer's handlers as engine/host/nvn_native.c writes them, rewritten here with shifts on the guest bytes (not
// the shared bit fields), so that the sink's records are checked against an independent reading.
struct ReferenceNvn final : gfx::CommandSink {
    FakeGuest& g;
    gfx::ScratchSegment seg;
    explicit ReferenceNvn(FakeGuest& guest) : g(guest) {}

    static void field(uint8_t* p, unsigned size, unsigned lo, unsigned width, uint64_t v) {
        uint64_t w = 0;
        std::memcpy(&w, p, size);
        const uint64_t m = ((1ull << width) - 1) << lo;
        w = (w & ~m) | ((v << lo) & m);
        std::memcpy(p, &w, size);
    }
    uint8_t* rec(const gfx::Command& c, uint32_t op, uint32_t payload) {
        CHECK(c.x[0] == 0xcb);
        uint8_t* p = static_cast<uint8_t*>(seg.append(op, payload, 0));
        return p;
    }
    void bind(const gfx::Command& c, int32_t kind, uint32_t n) {
        uint8_t* p = rec(c, HL_REC_BIND_STATE, 28);
        std::memcpy(p, &kind, 4);
        std::memcpy(p + 4, g.ptr(c.x[1]), n);
    }
    void emit(const gfx::Command& c) override {
        using gfx::Proc;
        uint8_t* o = c.x[0] >= 0x1000 && c.x[0] < 0x2000 ? g.ptr(c.x[0]) : nullptr;
        const uint32_t v1 = (uint32_t)c.x[1], v2 = (uint32_t)c.x[2], v3 = (uint32_t)c.x[3], v4 = (uint32_t)c.x[4];
        auto b = [&](int i) { return (c.x[i] & 0xff) != 0 ? 1u : 0u; };
        switch ((Proc)c.op) {
        case Proc::BlendStateSetDefaults: {
            const uint64_t d = 2ull << 3 | 1ull << 10 | 2ull << 17 | 1ull << 24 | 1ull << 31 | 1ull << 34;
            std::memcpy(o, &d, 8);
            break;
        }
        case Proc::BlendStateSetBlendTarget: field(o, 8, 0, 3, v1); break;
        case Proc::BlendStateSetBlendFunc:
            field(o, 8, 3, 7, v1), field(o, 8, 10, 7, v2), field(o, 8, 17, 7, v3), field(o, 8, 24, 7, v4);
            break;
        case Proc::BlendStateSetBlendEquation: field(o, 8, 31, 3, v1), field(o, 8, 34, 3, v2); break;
        case Proc::ColorStateSetDefaults: {
            const uint32_t d = 3u << 8 | 8u << 12;
            std::memcpy(o, &d, 4);
            break;
        }
        case Proc::ColorStateSetBlendEnable:
            if ((int)v1 >= 0 && (int)v1 <= 7) field(o, 4, v1, 1, b(2));
            break;
        case Proc::ColorStateSetLogicOp: field(o, 4, 8, 4, v1); break;
        case Proc::ChannelMaskStateSetChannelMask:
            if ((int)v1 >= 0 && (int)v1 <= 7) field(o, 4, 4 * v1, 4, b(2) | b(3) << 1 | b(4) << 2 | b(5) << 3);
            break;
        case Proc::DepthStencilStateSetDepthTestEnable: field(o, 8, 0, 1, b(1)); break;
        case Proc::DepthStencilStateSetDepthWriteEnable: field(o, 8, 1, 1, b(1)); break;
        case Proc::DepthStencilStateSetDepthFunc: field(o, 8, 2, 4, v1); break;
        case Proc::DepthStencilStateSetStencilTestEnable: field(o, 8, 6, 1, b(1)); break;
        case Proc::DepthStencilStateSetStencilFunc:
            if (v1 & 1) field(o, 8, 7, 4, v2);
            if (v1 & 2) field(o, 8, 11, 4, v2);
            break;
        case Proc::DepthStencilStateSetStencilOp:
            if (v1 & 1) field(o, 8, 15, 4, v2), field(o, 8, 19, 4, v3), field(o, 8, 23, 4, v4);
            if (v1 & 2) field(o, 8, 27, 4, v2), field(o, 8, 31, 4, v3), field(o, 8, 35, 4, v4);
            break;
        case Proc::PolygonStateSetCullFace: field(o, 4, 0, 2, v1); break;
        case Proc::PolygonStateSetFrontFace: field(o, 4, 2, 1, v1); break;
        case Proc::PolygonStateSetPolygonMode: field(o, 4, 3, 2, v1); break;
        case Proc::PolygonStateSetPolygonOffsetEnables: field(o, 4, 5, 3, v1); break;
        case Proc::CommandBufferBindBlendState: bind(c, 0, 8); break;
        case Proc::CommandBufferBindColorState: bind(c, 1, 4); break;
        case Proc::CommandBufferBindChannelMaskState: bind(c, 2, 4); break;
        case Proc::CommandBufferBindPolygonState: bind(c, 4, 4); break;
        case Proc::CommandBufferBindDepthStencilState: bind(c, 5, 8); break;
        case Proc::CommandBufferSetBlendColor: std::memcpy(rec(c, HL_REC_BLEND_COLOR, 16), g.ptr(c.x[1]), 16); break;
        case Proc::CommandBufferSetStencilValueMask:
        case Proc::CommandBufferSetStencilMask:
        case Proc::CommandBufferSetStencilRef: {
            const int32_t which = c.op - (uint32_t)Proc::CommandBufferSetStencilValueMask;
            uint8_t* p = rec(c, HL_REC_STENCIL, 20);
            const int32_t face = (int32_t)v1, value = (int32_t)v2;
            std::memcpy(p, &face, 4);
            std::memcpy(p + 4 + 4 * which, &value, 4);
            std::memcpy(p + 16, &which, 4);
            break;
        }
        case Proc::CommandBufferSetPolygonOffsetClamp: std::memcpy(rec(c, HL_REC_POLY_OFFSET, 12), c.f, 12); break;
        case Proc::CommandBufferSetDepthClamp: {
            const int32_t v = (int32_t)b(1);
            std::memcpy(rec(c, HL_REC_DEPTH_CLAMP, 8), &v, 4);
            break;
        }
        default: CHECK(false);
        }
    }
};

struct Offsets final : gfx::PolygonOffsets {
    void at(int32_t slot, uint32_t& factor, uint32_t& units) const override {
        factor = 0x3f000000u + (uint32_t)slot, units = 0x40400000u + (uint32_t)slot;
    }
};

// The objects' bytes in the frame start as whatever the stack held: the bits no setter covers stay as they were.
void garbage(FakeGuest& g) {
    for (size_t i = 0; i < sizeof g.memory; i++) g.memory[i] = (uint8_t)(i * 37 + 11);
}

void the_sink_writes_what_the_procedures_record() {
    const gfx::StateHandles h{0xcb, 0x1100, 0x1008, 0x1048, 0x104c, 0x1040, 0x103c};
    Offsets offsets;
    uint32_t compared = 0;
    // (a spread of inputs: keys, flags, blend targets, stencil on and off, odd argument values beyond their fields)
    for (uint32_t key = 0; key < 256; key += 3)
        for (uint64_t flags : {0ull, 0x40ull, 0x20ull | 0x10ull, 0x1f000ull | 0x4ull}) {
            gfx::StateInputs in;
            in.key = (uint8_t)key;
            in.flags = flags;
            in.channel_masks = key * 0x01010101u ^ 0x5a5a5a5au;
            in.blend_target_mask = (uint8_t)(key * 7);
            in.depth_mode = (uint8_t)(key % 10);
            for (int t = 0; t < 8; t++)
                for (int i = 0; i < 6; i++) in.blend[t][i] = (uint8_t)(key + t * 13 + i * 41);
            in.stencil_test = key & 1;
            in.stencil_func = (uint8_t)(key & 0x1f);
            in.stencil_ops[0] = 1, in.stencil_ops[1] = (uint8_t)key, in.stencil_ops[2] = 0x13;
            in.stencil_ref = key * 3, in.stencil_value_mask = 0xffffff00u | key;
            in.cull_face = (uint8_t)(key & 7), in.polygon_mode = (uint8_t)(key >> 5), in.polygon_offset = (uint8_t)key;
            const gfx::StateOutputs out = gfx::RenderState::decide(in, offsets);

            for (int order = 0; order < 2; order++) {
                FakeGuest g;
                garbage(g);
                std::memcpy(g.ptr(h.blend_color), out.blend_color, 16);
                const hl_rec_mark start{0, 1000, 900, 8};
                gfx::ScratchSegment mine;
                mine.reset(start);
                gfx::RecordSink sink(mine, g, h.command_buffer);
                ReferenceNvn nvn(g);
                nvn.seg.reset(start);
                // (as the check runs them: the sink first, the procedures after; and the other way round)
                if (order == 0) {
                    gfx::RenderState::emit(in, out, h, sink);
                    gfx::RenderState::emit(in, out, h, nvn);
                } else {
                    gfx::RenderState::emit(in, out, h, nvn);
                    gfx::RenderState::emit(in, out, h, sink);
                }
                CHECK(sink.stray() == 0);
                CHECK(mine.bytes() == nvn.seg.bytes());
                const hl_rec_mark a = mine.mark(), b = nvn.seg.mark();
                CHECK(a.offset == b.offset && a.cmd_used == b.cmd_used && a.hw_used == b.hw_used && a.last_rec == b.last_rec);
                if (!((flags >> 17) & 1)) CHECK(sink.records() > 0);
                compared++;
            }
        }
    CHECK(compared > 300);
}

void the_sink_keeps_bits_no_setter_covers_and_counts_strays() {
    FakeGuest g;
    const uint64_t poly = 0x1200;
    const uint32_t old = 0xabcdef00u;
    std::memcpy(g.ptr(poly), &old, 4);
    gfx::ScratchSegment s;
    s.reset(hl_rec_mark{});
    gfx::RecordSink sink(s, g, 0xcb);
    sink.emit(gfx::command(gfx::Proc::PolygonStateSetCullFace, poly, 3));
    sink.emit(gfx::command(gfx::Proc::PolygonStateSetPolygonMode, poly, 0x12));   // (2 bits: 0x12 is 2)
    sink.emit(gfx::command(gfx::Proc::CommandBufferBindPolygonState, 0xcb, poly));
    CHECK(sink.records() == 1 && sink.stray() == 0);
    const uint32_t bound = at<uint32_t>(s.bytes(), 12);
    CHECK(bound == ((old & ~0x1bu) | 3u | 2u << 3));
    uint32_t guest;
    std::memcpy(&guest, g.ptr(poly), 4);
    CHECK(guest == old);   // (the guest object is not written)
    // (another command buffer, a procedure past the table: not written, counted)
    sink.emit(gfx::command(gfx::Proc::CommandBufferSetDepthClamp, 0xcc, 1));
    gfx::Command odd;
    odd.op = gfx::kProcCount + 3;
    sink.emit(odd);
    CHECK(sink.records() == 1 && sink.stray() == 2 && s.bytes().size() == 40);
    // (a bind of an object the sink never set: its guest bytes)
    const uint64_t ds = 0x1300, ds_bytes = 0x0123456789abcdefull;
    std::memcpy(g.ptr(ds), &ds_bytes, 8);
    sink.emit(gfx::command(gfx::Proc::CommandBufferBindDepthStencilState, 0xcb, ds));
    CHECK(at<int32_t>(s.bytes(), 48) == HL_REC_DEPTH_STENCIL && at<uint64_t>(s.bytes(), 52) == ds_bytes);
}

}   // namespace

int main() {
    every_record_kind_has_its_layout_and_accounting();
    the_sink_writes_what_the_procedures_record();
    the_sink_keeps_bits_no_setter_covers_and_counts_strays();
    std::puts("records_test: ok");
    return 0;
}
