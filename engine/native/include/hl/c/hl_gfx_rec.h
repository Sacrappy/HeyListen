/* HeyListen native engine: the back end's command records as native code writes them (docs/native-engine/
   direct-emission.md, sections 3.2 and 5, bricks A and B).

   The OS layer's NVN procedures do not write the game's command memory: each appends a typed record (a header {op, size}
   and a payload, 8-aligned) to a host segment kept per command buffer, which the Vulkan back end replays
   (engine/gpu/render/rx_nvn.h, its NOP_* records). Native producers write the same records themselves, into the same
   segment, through the small host surface below. This header holds the layouts native code writes: the header, the op
   numbers, the payloads and the NVN state objects a bind copies, each the same as rx_nvn.h's (engine/host/nvn_native.c
   static-asserts every size, offset, op number and constant against it, so that a change to either breaks the build and
   not the picture). */
#ifndef HL_C_GFX_REC_H
#define HL_C_GFX_REC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the records ---- */
typedef struct { uint32_t op, size; } hl_rec_hdr;   /* size: header included, a multiple of 8 */

enum {
    HL_REC_VIEWPORTS = 11,
    HL_REC_SCISSOR = 12,
    HL_REC_DEPTH_RANGE = 13,
    HL_REC_BIND_STATE = 15,
    HL_REC_BIND_VATTR = 16,
    HL_REC_BIND_VSTREAM = 17,
    HL_REC_BIND_VBUF = 18,
    HL_REC_BIND_UBO = 19,
    HL_REC_BIND_SSBO = 20,
    HL_REC_BIND_TEX = 21,
    HL_REC_BIND_SAMP = 22,
    HL_REC_STENCIL = 23,
    HL_REC_POLY_OFFSET = 24,
    HL_REC_DRAW = 25,
    HL_REC_DRAW_IDX = 26,
    HL_REC_BARRIER = 28,
    HL_REC_BLEND_COLOR = 33,
    HL_REC_BIND_IMG = 35,
    HL_REC_DEPTH_CLAMP = 39,
    HL_REC_RENDER_ENABLE = 40,
    HL_REC_RASTER_DISCARD = 41
};

/* NVN state objects as the OS layer packs them (bit fields: the same declarations as rx_nvn.h's, so the same layout
   with the same compiler) */
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } hl_rec_color_st;
typedef struct { uint32_t mask; } hl_rec_chmask_st;   /* 4 bits a target: r 1 g 2 b 4 a 8 */
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } hl_rec_blend_st;
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } hl_rec_ms_st;
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } hl_rec_poly_st;
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} hl_rec_ds_st;
/* bind_state's kinds */
enum { HL_REC_BLEND = 0, HL_REC_COLOR = 1, HL_REC_CHMASK = 2, HL_REC_MULTISAMPLE = 3, HL_REC_POLYGON = 4, HL_REC_DEPTH_STENCIL = 5 };

/* the NVN values the state objects' defaults use */
enum {
    HL_NVN_FACE_FRONT = 1, HL_NVN_FACE_BACK = 2,
    HL_NVN_BLEND_FUNC_ZERO = 1, HL_NVN_BLEND_FUNC_ONE = 2, HL_NVN_BLEND_EQUATION_ADD = 1,
    HL_NVN_LOGIC_OP_COPY = 3, HL_NVN_ALPHA_FUNC_ALWAYS = 8
};

typedef struct { uint64_t address, size; } hl_rec_range;   /* NVNbufferRange */

/* payloads */
typedef struct { int32_t first, n; float v[16][4]; } hl_rec_viewports;
typedef struct { int32_t x, y, w, h; } hl_rec_scissor;
typedef struct { int32_t value; int32_t index; } hl_rec_flag;
typedef struct { float n, f; } hl_rec_depth_range;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } hl_rec_bind_state;
typedef struct { int32_t first, n; uint32_t st[16]; } hl_rec_bind_vattr;
typedef struct { uint32_t stride, divisor; } hl_rec_vstream_st;
typedef struct { int32_t first, n; hl_rec_vstream_st st[16]; } hl_rec_bind_vstream;
typedef struct { int32_t first, n; hl_rec_range r[16]; } hl_rec_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } hl_rec_bind_buf;
typedef struct { int32_t stage, first, n, pad_; uint64_t src; uint64_t h[32]; } hl_rec_bind_handles;
typedef struct { int32_t face, value_mask, mask, ref, which; } hl_rec_stencil;   /* which: 0 value mask 1 write mask 2 reference */
typedef struct { float factor, units, clamp; } hl_rec_poly_offset;
typedef struct { int32_t prim, first, count, base_instance, instances; } hl_rec_draw;
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf; } hl_rec_draw_idx;
typedef struct { float rgba[4]; } hl_rec_blend_color;
typedef struct { int32_t bits, pad_; } hl_rec_barrier;   /* (the OS layer's record: an 8-byte payload, the bits first) */

/* ---- the OS layer's surface (engine/host/hl_gfx_rec.c over nvn_native.c) ---- */

/* where a command buffer's recording stands: the segment's length and the accounting the game reads back
   (GetCommandMemoryUsed and its kin) */
typedef struct { uint64_t offset, cmd_used, hw_used, last_rec; } hl_rec_mark;

typedef struct hl_gfx_rec_host {
    void *user;
    /* the command buffer object at guest address cmd, for appends by the calling thread (the one recording it): 0 when
       it is not a command buffer that holds its own slot of the NVN layer's table (the NVN path then does what it
       always did) */
    void *(*open)(void *user, uint64_t cmd);
    /* a record of op with payload bytes of payload, appended as the NVN layer's rec() does: its payload returned,
       zeroed; the command buffer's accounting kept (cmd_used, hw_used, last_rec). hw other than 0: what the real
       driver would have written for it (the NVN layer's hw_fix) */
    void *(*append)(void *user, void *cmd, uint32_t op, uint32_t payload, uint32_t hw);
    /* the recording's state now; the bytes from offset on (valid until the next append); back to a mark (the
       records after it dropped, the accounting restored) */
    void (*mark)(void *user, void *cmd, hl_rec_mark *out);
    const uint8_t *(*bytes)(void *user, void *cmd, uint64_t offset);
    void (*rewind)(void *user, void *cmd, const hl_rec_mark *to);
    /* n bytes of guest memory at guest, as the NVN layer reads an argument (the game stops when it is outside the
       guest window, as it does there) */
    const void *(*guest)(void *user, uint64_t guest, uint64_t n);
    /* asked once, at the first use: HL_REC_* bits (the OS layer reads the switches and logs what is on) */
    int (*mode)(void *user);
    /* one line for the log */
    void (*log)(void *user, const char *line);
} hl_gfx_rec_host;

/* HL_GFX_REC=1: the render state written as records; HL_GFX_REC_CHECK=1: compared with the NVN path's */
enum { HL_REC_WRITE = 1, HL_REC_CHECK = 2 };

/* The OS layer hands its surface over (once, before the first draw; it must outlive the process). */
void hl_gfx_rec_register(const hl_gfx_rec_host *host);

#ifdef __cplusplus
}
#endif
#endif
