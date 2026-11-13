// HeyListen native engine: the back end's command records written by native code (hl/gfx/Records.h).
//
// Every record here is the OS layer's NVN procedure's, byte for byte (engine/host/nvn_native.c: rec(), cmd_alloc(),
// hw_fix() and the handlers), which HL_GFX_REC_CHECK compares in the game: a 32-bit argument is the register's low half,
// an NVN boolean its low byte, a state object's field truncated to its bit field's width.
#include "hl/gfx/Records.h"

#include <cstring>

#include "hl/gfx/Nvn.h"

namespace hl::gfx {

// ---- the scratch segment ----

void ScratchSegment::reset(const hl_rec_mark& from) {
    bytes_.clear();
    at_ = from;
    at_.offset = 0;
}

void* ScratchSegment::append(uint32_t op, uint32_t payload, uint32_t hw) {
    const uint32_t size = record_size(payload);
    const size_t at = bytes_.size();
    bytes_.resize(at + size);
    uint8_t* p = bytes_.data() + at;
    const hl_rec_hdr h = {op, size};
    std::memcpy(p, &h, sizeof h);
    std::memset(p + sizeof h, 0, size - sizeof h);
    // (cmd_alloc: the record counted whole in both sums; then hw_fix replaces it in the driver's)
    at_.cmd_used += size;
    at_.hw_used += size;
    at_.last_rec = size;
    if (hw) {
        at_.hw_used += hw;
        at_.hw_used -= at_.last_rec;
        at_.last_rec = hw;
    }
    return p + sizeof h;
}

hl_rec_mark ScratchSegment::mark() const {
    hl_rec_mark m = at_;
    m.offset = bytes_.size();
    return m;
}

// ---- the writer ----

void RecordWriter::bind_state(int32_t kind, const void* bytes, uint32_t n) {
    hl_rec_bind_state* r = rec<hl_rec_bind_state>(HL_REC_BIND_STATE);
    r->kind = kind;
    std::memcpy(r->bytes, bytes, n < sizeof r->bytes ? n : sizeof r->bytes);
}

void RecordWriter::stencil(int32_t face, int32_t which, int32_t value) {
    hl_rec_stencil* r = rec<hl_rec_stencil>(HL_REC_STENCIL);
    r->face = face, r->which = which;
    if (which == 0) r->value_mask = value;
    else if (which == 1) r->mask = value;
    else r->ref = value;
}

void RecordWriter::poly_offset(uint32_t factor, uint32_t units, uint32_t clamp) {
    hl_rec_poly_offset* r = rec<hl_rec_poly_offset>(HL_REC_POLY_OFFSET);
    std::memcpy(&r->factor, &factor, 4);
    std::memcpy(&r->units, &units, 4);
    std::memcpy(&r->clamp, &clamp, 4);
}

void RecordWriter::blend_color(const void* rgba) {
    std::memcpy(rec<hl_rec_blend_color>(HL_REC_BLEND_COLOR)->rgba, rgba, 16);
}

void RecordWriter::flag(uint32_t op, bool on) { rec<hl_rec_flag>(op)->value = on ? 1 : 0; }

void RecordWriter::depth_range(float n, float f) {
    hl_rec_depth_range* r = rec<hl_rec_depth_range>(HL_REC_DEPTH_RANGE);
    r->n = n, r->f = f;
}

bool RecordWriter::viewports(int32_t first, int32_t n, const float (*v)[4]) {
    if (n < 0 || first < 0 || first + n > 16) return false;
    hl_rec_viewports* r = rec<hl_rec_viewports>(HL_REC_VIEWPORTS);   // (the whole record, whatever n: as the procedure)
    r->first = first, r->n = n;
    std::memcpy(r->v, v, 16 * (size_t)n);
    return true;
}

void RecordWriter::scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
    hl_rec_scissor* r = rec<hl_rec_scissor>(HL_REC_SCISSOR);
    r->x = x, r->y = y, r->w = w, r->h = h;
}

void RecordWriter::bind_buf(uint32_t op, int32_t stage, int32_t index, uint64_t addr, uint64_t size) {
    hl_rec_bind_buf* r = rec<hl_rec_bind_buf>(op, sizeof(hl_rec_bind_buf), 20);
    r->stage = stage, r->index = index, r->addr = addr, r->size = size;
}

bool RecordWriter::bind_handles(uint32_t op, int32_t stage, int32_t first, int32_t n, const uint64_t* handles,
                                uint64_t src, uint32_t hw) {
    if (n < 0 || n > 32) return false;
    hl_rec_bind_handles* r = rec<hl_rec_bind_handles>(
        op, (uint32_t)(offsetof(hl_rec_bind_handles, h) + 8 * (size_t)n), hw ? hw : 16 + 4 * (uint32_t)n);
    r->stage = stage, r->first = first, r->n = n, r->src = src;
    std::memcpy(r->h, handles, 8 * (size_t)n);
    return true;
}

bool RecordWriter::bind_vertex_buffers(int32_t first, int32_t n, const hl_rec_range* ranges) {
    if (n < 0 || first < 0 || first + n > 16) return false;
    hl_rec_bind_vbuf* r = rec<hl_rec_bind_vbuf>(
        HL_REC_BIND_VBUF, (uint32_t)(offsetof(hl_rec_bind_vbuf, r) + sizeof(hl_rec_range) * (size_t)n), 24 * (uint32_t)n);
    r->first = first, r->n = n;
    std::memcpy(r->r, ranges, sizeof(hl_rec_range) * (size_t)n);
    return true;
}

bool RecordWriter::bind_vertex_attribs(int32_t n, const uint32_t* states) {
    if (n < 0 || n > 16) return false;
    // (the driver writes all 16 attribute formats: 17 words)
    hl_rec_bind_vattr* r =
        rec<hl_rec_bind_vattr>(HL_REC_BIND_VATTR, (uint32_t)(offsetof(hl_rec_bind_vattr, st) + 4 * (size_t)n), 68);
    r->first = 0, r->n = n;
    std::memcpy(r->st, states, 4 * (size_t)n);
    return true;
}

bool RecordWriter::bind_vertex_streams(int32_t n, const hl_rec_vstream_st* states) {
    if (n < 0 || n > 16) return false;
    hl_rec_bind_vstream* r = rec<hl_rec_bind_vstream>(
        HL_REC_BIND_VSTREAM, (uint32_t)(offsetof(hl_rec_bind_vstream, st) + 8 * (size_t)n), 4 * (16 + (uint32_t)n));
    r->first = 0, r->n = n;
    std::memcpy(r->st, states, 8 * (size_t)n);
    return true;
}

void RecordWriter::draw(int32_t prim, int32_t first, int32_t count, int32_t base_instance, int32_t instances) {
    if (instances <= 0) return;
    hl_rec_draw* r = rec<hl_rec_draw>(HL_REC_DRAW);
    r->prim = prim, r->first = first, r->count = count, r->base_instance = base_instance, r->instances = instances;
}

void RecordWriter::draw_indexed(int32_t prim, int32_t itype, int32_t count, uint64_t ibuf, int32_t base_vertex,
                                int32_t base_instance, int32_t instances, uint32_t hw) {
    if (instances <= 0) return;
    hl_rec_draw_idx* r = rec<hl_rec_draw_idx>(HL_REC_DRAW_IDX, sizeof(hl_rec_draw_idx), hw);
    r->prim = prim, r->itype = itype, r->count = count, r->ibuf = ibuf;
    r->base_vertex = base_vertex, r->base_instance = base_instance, r->instances = instances;
}

void RecordWriter::barrier(int32_t bits) { rec<hl_rec_barrier>(HL_REC_BARRIER)->bits = bits; }

// ---- the sink ----

namespace {
// (the procedures' argument reads: a 32-bit value is the register's low half, an NVN boolean its low byte)
inline int32_t i32(const Command& c, int i) { return (int32_t)c.x[i]; }
inline uint32_t u32(const Command& c, int i) { return (uint32_t)c.x[i]; }
inline bool boolean(const Command& c, int i) { return (c.x[i] & 0xff) != 0; }

template <class T> T load(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <class T> void store(uint8_t* p, const T& v) { std::memcpy(p, &v, sizeof v); }
}   // namespace

uint8_t* RecordSink::object(uint64_t at, uint32_t size) {
    for (uint32_t i = 0; i < n_objects_; i++)
        if (objects_[i].at == at) return objects_[i].bytes;
    if (n_objects_ == kObjects) return nullptr;
    Object& o = objects_[n_objects_++];
    o.at = at;
    std::memset(o.bytes, 0, sizeof o.bytes);
    std::memcpy(o.bytes, guest_.read(at, size), size);
    return o.bytes;
}

void RecordSink::bind(int32_t kind, uint64_t at, uint32_t size) {
    const uint8_t* bytes = nullptr;
    for (uint32_t i = 0; i < n_objects_ && !bytes; i++)
        if (objects_[i].at == at) bytes = objects_[i].bytes;
    if (!bytes) bytes = static_cast<const uint8_t*>(guest_.read(at, size));
    w_.bind_state(kind, bytes, size);
}

void RecordSink::emit(const Command& c) {
    const uint64_t o = c.x[0];
    // (the state objects: set in the native copy, as the procedure does in place)
    auto blend = [&](auto&& set) {
        uint8_t* p = object(o, sizeof(hl_rec_blend_st));
        if (!p) return (void)stray_++;
        hl_rec_blend_st s = load<hl_rec_blend_st>(p);
        set(s);
        store(p, s);
    };
    auto color = [&](auto&& set) {
        uint8_t* p = object(o, sizeof(hl_rec_color_st));
        if (!p) return (void)stray_++;
        hl_rec_color_st s = load<hl_rec_color_st>(p);
        set(s);
        store(p, s);
    };
    auto ds = [&](auto&& set) {
        uint8_t* p = object(o, sizeof(hl_rec_ds_st));
        if (!p) return (void)stray_++;
        hl_rec_ds_st s = load<hl_rec_ds_st>(p);
        set(s);
        store(p, s);
    };
    auto poly = [&](auto&& set) {
        uint8_t* p = object(o, sizeof(hl_rec_poly_st));
        if (!p) return (void)stray_++;
        hl_rec_poly_st s = load<hl_rec_poly_st>(p);
        set(s);
        store(p, s);
    };
    // (the command buffer's procedures: one record each, into this sink's command buffer only)
    auto record = [&]() {
        if (o != cb_) {
            stray_++;
            return false;
        }
        records_++;
        return true;
    };

    switch ((Proc)c.op) {
    case Proc::BlendStateSetDefaults:
        blend([](hl_rec_blend_st& s) {
            std::memset(&s, 0, sizeof s);
            s.src_rgb = HL_NVN_BLEND_FUNC_ONE, s.dst_rgb = HL_NVN_BLEND_FUNC_ZERO;
            s.src_a = HL_NVN_BLEND_FUNC_ONE, s.dst_a = HL_NVN_BLEND_FUNC_ZERO;
            s.eq_rgb = HL_NVN_BLEND_EQUATION_ADD, s.eq_a = HL_NVN_BLEND_EQUATION_ADD;
        });
        break;
    case Proc::BlendStateSetBlendTarget: blend([&](hl_rec_blend_st& s) { s.target = u32(c, 1); }); break;
    case Proc::BlendStateSetBlendFunc:
        blend([&](hl_rec_blend_st& s) {
            s.src_rgb = u32(c, 1), s.dst_rgb = u32(c, 2), s.src_a = u32(c, 3), s.dst_a = u32(c, 4);
        });
        break;
    case Proc::BlendStateSetBlendEquation:
        blend([&](hl_rec_blend_st& s) { s.eq_rgb = u32(c, 1), s.eq_a = u32(c, 2); });
        break;
    case Proc::ColorStateSetDefaults:
        color([](hl_rec_color_st& s) {
            std::memset(&s, 0, sizeof s);
            s.logic_op = HL_NVN_LOGIC_OP_COPY;
            s.alpha_func = HL_NVN_ALPHA_FUNC_ALWAYS;
        });
        break;
    case Proc::ColorStateSetBlendEnable:
        color([&](hl_rec_color_st& s) {
            const int32_t i = i32(c, 1);
            if (i < 0 || i > 7) return;
            s.blend_en = boolean(c, 2) ? (s.blend_en | 1u << i) : (s.blend_en & ~(1u << i));
        });
        break;
    case Proc::ColorStateSetLogicOp: color([&](hl_rec_color_st& s) { s.logic_op = u32(c, 1); }); break;
    case Proc::ChannelMaskStateSetChannelMask: {
        const int32_t i = i32(c, 1);
        uint8_t* p = object(o, sizeof(hl_rec_chmask_st));
        if (!p) {
            stray_++;
            break;
        }
        if (i < 0 || i > 7) break;
        const uint32_t m = (boolean(c, 2) ? 1u : 0u) | (boolean(c, 3) ? 2u : 0u) | (boolean(c, 4) ? 4u : 0u) |
                           (boolean(c, 5) ? 8u : 0u);
        hl_rec_chmask_st s = load<hl_rec_chmask_st>(p);
        s.mask = (s.mask & ~(0xfu << (4 * i))) | m << (4 * i);
        store(p, s);
        break;
    }
    case Proc::DepthStencilStateSetDepthTestEnable: ds([&](hl_rec_ds_st& s) { s.dt = boolean(c, 1); }); break;
    case Proc::DepthStencilStateSetDepthWriteEnable: ds([&](hl_rec_ds_st& s) { s.dw = boolean(c, 1); }); break;
    case Proc::DepthStencilStateSetDepthFunc: ds([&](hl_rec_ds_st& s) { s.df = u32(c, 1); }); break;
    case Proc::DepthStencilStateSetStencilTestEnable: ds([&](hl_rec_ds_st& s) { s.st = boolean(c, 1); }); break;
    case Proc::DepthStencilStateSetStencilFunc:
        ds([&](hl_rec_ds_st& s) {
            if (u32(c, 1) & HL_NVN_FACE_FRONT) s.sf_f = u32(c, 2);
            if (u32(c, 1) & HL_NVN_FACE_BACK) s.sf_b = u32(c, 2);
        });
        break;
    case Proc::DepthStencilStateSetStencilOp:
        ds([&](hl_rec_ds_st& s) {
            if (u32(c, 1) & HL_NVN_FACE_FRONT) s.fail_f = u32(c, 2), s.zfail_f = u32(c, 3), s.zpass_f = u32(c, 4);
            if (u32(c, 1) & HL_NVN_FACE_BACK) s.fail_b = u32(c, 2), s.zfail_b = u32(c, 3), s.zpass_b = u32(c, 4);
        });
        break;
    case Proc::PolygonStateSetCullFace: poly([&](hl_rec_poly_st& s) { s.cull = u32(c, 1); }); break;
    case Proc::PolygonStateSetFrontFace: poly([&](hl_rec_poly_st& s) { s.front = u32(c, 1); }); break;
    case Proc::PolygonStateSetPolygonMode: poly([&](hl_rec_poly_st& s) { s.mode = u32(c, 1); }); break;
    case Proc::PolygonStateSetPolygonOffsetEnables: poly([&](hl_rec_poly_st& s) { s.offset_en = u32(c, 1); }); break;

    case Proc::CommandBufferBindBlendState:
        if (record()) bind(HL_REC_BLEND, c.x[1], sizeof(hl_rec_blend_st));
        break;
    case Proc::CommandBufferBindColorState:
        if (record()) bind(HL_REC_COLOR, c.x[1], sizeof(hl_rec_color_st));
        break;
    case Proc::CommandBufferBindChannelMaskState:
        if (record()) bind(HL_REC_CHMASK, c.x[1], sizeof(hl_rec_chmask_st));
        break;
    case Proc::CommandBufferBindDepthStencilState:
        if (record()) bind(HL_REC_DEPTH_STENCIL, c.x[1], sizeof(hl_rec_ds_st));
        break;
    case Proc::CommandBufferBindPolygonState:
        if (record()) bind(HL_REC_POLYGON, c.x[1], sizeof(hl_rec_poly_st));
        break;
    case Proc::CommandBufferSetBlendColor:
        if (record()) w_.blend_color(guest_.read(c.x[1], 16));
        break;
    case Proc::CommandBufferSetStencilValueMask:
        if (record()) w_.stencil(i32(c, 1), 0, i32(c, 2));
        break;
    case Proc::CommandBufferSetStencilMask:
        if (record()) w_.stencil(i32(c, 1), 1, i32(c, 2));
        break;
    case Proc::CommandBufferSetStencilRef:
        if (record()) w_.stencil(i32(c, 1), 2, i32(c, 2));
        break;
    case Proc::CommandBufferSetPolygonOffsetClamp:
        if (record()) w_.poly_offset(c.f[0], c.f[1], c.f[2]);
        break;
    case Proc::CommandBufferSetDepthClamp:
        if (record()) w_.depth_clamp(boolean(c, 1));
        break;
    default: stray_++; break;
    }
}

}   // namespace hl::gfx
