// (HeyListen, 2026-11-14) RX_VK_LEDGER=<seconds>: the memory ledger (vk_ledger.cpp). Every <seconds> and at a normal exit, one
// table by class of game resource: what the game allocated for it (the console's bytes), and every copy this engine holds of
// it: guest memory committed and in RAM, the VRAM mirror, the Vulkan image, the imports that pin it; then what belongs to no
// game resource; reconciled with the VRAM the driver counts and the process's working set. A measurement: off by default,
// nothing changes but the counting (and RX_VK_RES_LOG's allocation counts, which it needs, are kept while it is on).
#pragma once

#include <cstdint>
#include <vector>

namespace mkwvk {

/// What the GPU used a guest page for, ever since its pool was made (bits, or'ed in by the bindings and the textures)
enum : uint16_t {
    kUseVertex = 1, kUseIndex = 2, kUseUniform = 4, kUseStorage = 8, kUseIndirect = 16, kUseGpuWrite = 32,
    kUseSampled = 64,    // a texture's storage (made for sampling)
    kUseRendered = 128,  // a texture the GPU rendered or wrote into
    kUseOther = 256,     // a binding of another kind (buffer copies, texel buffers, diagnostics)
};
bool ledger_on();
/// What the next gpu_buffer_bind on this thread binds for (kUseOther when no call site said)
extern thread_local uint16_t ledger_use;
struct LedgerUse {
    uint16_t prev;
    explicit LedgerUse(uint16_t u) : prev(ledger_use) { ledger_use = u; }
    ~LedgerUse() { ledger_use = prev; }
};
void ledger_mark_slow(uint64_t gpu, uint64_t size, uint16_t use);
/// [gpu, gpu + size) used for `use` (GPU addresses below 1 TB: the native pools, whose GPU address is their guest address)
inline void ledger_mark(uint64_t gpu, uint64_t size, uint16_t use) {
    if (ledger_on()) ledger_mark_slow(gpu, size, use);
}
/// A pool was made at [lo, lo + size): what its memory was used for before is forgotten
void ledger_pool_made(uint64_t lo, uint64_t size);

// ---- snapshots, taken on the replay's thread (texture_frame) and handed to the ledger's own thread ----
struct LedgerTex {
    uint64_t gpu = 0, guest_size = 0, vk_bytes = 0;
    uint32_t vk_format = 0, width = 0, height = 0, depth = 0, layers = 0, levels = 0;
    int block_w = 1, block_h = 1;
    bool astc = false, gpu_written = false, released = false, depth_aspect = false;
};
struct LedgerRange { uint64_t lo, hi; };
struct LedgerPoolFlags { uint64_t lo, hi; uint32_t flags; };
struct LedgerChunk {   // (an entry of the back end's pool table: an import chunk, its import and its mirror)
    uint64_t lo = 0, hi = 0;
    bool import = false, mirror = false, gpu_written = false;
    uint64_t bound_ago = 0;    // (submissions since one last bound it)
    uint64_t synced_ago = 0;   // (mirror submissions since its mirror was last brought up to date)
};
struct LedgerSnap {
    std::vector<LedgerChunk> chunks;
    uint64_t retired_chunks = 0, retired_chunk_bytes = 0, evicted_chunks = 0;
    std::vector<LedgerTex> textures;
    uint64_t spare_bytes = 0, rt_pool_bytes = 0, bcc_bytes = 0, bcc_entries = 0;
    std::vector<LedgerPoolFlags> pool_flags;   // (the flags the game made its pools with, by guest range)
    std::vector<LedgerRange> mirrors, imports;   // (live VRAM mirrors; live imports of guest memory)
    uint64_t retired_mirror_bytes = 0;
    int64_t uploads = 0, astc_gpu = 0, bytes_uploaded = 0;
};
/// (vk_vma.cpp) allocations by MemKind (11 kinds) and memory class (VRAM, VRAM mapped, system cached, system uncached), bytes and counts
void mem_kind_totals(uint64_t bytes[][4], uint64_t count[][4]);
/// (vk_vma.cpp) VMA's blocks: [0] [1] the textures' image blocks and their images (generations included), [2] [3] the render
/// targets' blocks and images, [4] [5] every device-local heap's blocks and allocations, [6] [7] what the driver says the process
/// uses there and allows it; bytes
void vma_ledger_stats(uint64_t out[8]);
/// (vk_texture.cpp, the replay's thread) the textures' part
void texture_ledger_snapshot(LedgerSnap& s);
/// (vk_memory.cpp) the pools' part
void memory_ledger_snapshot(LedgerSnap& s);
/// (vk_texture.cpp's texture_frame, every present) a snapshot every RX_VK_LEDGER seconds, counted and logged on the ledger's thread
void ledger_frame();

} // namespace mkwvk
