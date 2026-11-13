// Guest memory for the GPU: each NVN memory pool imported as host memory (VK_EXT_external_memory_host) with one
// buffer over it. The GPU reads the game's memory where it is and its writes land there: no mirrored copies, no
// write-backs, no upload streams. A GPU address (a guest address for the native pools) maps to (buffer, offset).
//
// Pools are imported the first time something on the GPU touches them, not all at once: MKW has ~2,400 pools at the
// title and importing each is a buffer, a memory object and a device address (tens of seconds taken at one submit,
// on the render thread, in the first frame). The table of pools is cheap to keep current (host->gpu_pools: two
// numbers each) and is refreshed when its generation changes, at a submit.
#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <vector>
#include <condition_variable>
#include "vk_menu.h"
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#if defined(__AVX2__)
#include <immintrin.h>   // (TICK-037: the capture's streaming copy)
#endif

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "vk_internal.h"
#include "vk_texture.h"   // (upload_command_buffer)
#include "vk_gprof.h"      // (RX_VK_PASS_TIMING)
#include "import_hold.h"
#include "capture_merge.h"    // (TICK-043: the capture copied ahead)
#include "mirror_inherit.h"   // (TICK-036 / #1127: what a new mirror takes from the retired pools' mirrors)
#include "msync_diff.h"       // (TICK-048: the mirror syncs' diff)
#include "gone_ring.h"        // (TICK-052: the recording threads' pool table)
#include "shaders/msync_scatter_comp_spv.h"   // (TICK-021: the mirror syncs' scatter copy)
#ifdef __linux__
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "shaders/wb_diff_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

struct Pool {
    uint64_t lo = 0, hi = 0;   // guest range imported (aligned outwards)
    uint64_t range_lo = 0, range_hi = 0;   // (the game's pool range it was found for: pool_of's last hit)
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> pieces;   // (a pool imported as several dma-bufs: the ones after `memory`)
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    // its VRAM mirror (see "the VRAM mirror" below)
    enum : uint8_t { MIRROR_NONE, MIRROR_WANTED, MIRROR_ON, MIRROR_REFUSED } mstate = MIRROR_NONE;
    VkBuffer mirror = VK_NULL_HANDLE;
    VmaAllocation mirror_alloc = nullptr;
    uint32_t synced_gen = 0;     // every page stamped up to this generation is in the mirror
    uint64_t synced_epoch = 0;   // the submission that last brought it up to date
    uint64_t sync_seg = 0;       // (and the parallel replay's segment whose uploads did)
    uint64_t made_epoch = 0, made_seg = 0;   // (TICK-036: the submission and segment whose uploads filled its mirror)
    uint64_t last_value = 0;     // (the newest submission that bound it: its mirror is in use until that one is done)
    uint64_t direct_write_epoch = 0;   // (the part that last wrote it in guest memory, before it had a mirror)
    uint64_t retired_value = 0;  // (retired: the submission that may still use it)
    bool was_mirrored = false;   // (its mirror was given back: its pages stay in the captures, for the next one)
    // (EvilOOT) retired with its mirror on: the pool that took it in copies the GPU's contents from it when its own mirror is
    // made (mirror_make), and until then the mirror stays (mirrors_give_back)
    bool inherit_pending = false;
    uint64_t retired_epoch = 0;
    bool prefetched = false;     // (amd-fix) imported ahead by the background thread, not bound yet
    bool lazy_held = false;      // (TICK-015) its mirror holds GPU writes not written back: kept (see "write-backs left out")
    // (hookshot) the write-back's diff: what guest memory holds of the pool, in VRAM, and the pages of it known to be so
    VkBuffer shadow = VK_NULL_HANDLE;
    VmaAllocation shadow_alloc = nullptr;
    VkDeviceAddress shadow_address = 0;
    std::vector<uint64_t> shadow_known;   // (a bit a page)
    uint32_t mirror_id = 0;      // (TICK-048: a new number at each mirror_make; the sync diff's page hashes name their mirror)
    uint64_t idle_freed_epoch = 0;   // (TICK-057: its mirror last given back as idle, at this submission; 0: never)
    uint32_t remade = 0;             // (TICK-057: its mirror made again within kLongIdle of being given back as idle)
};
void diff_gpu_wrote(uint64_t guest, uint64_t size);   // (TICK-048, below: the GPU may have written there)
uint64_t mem_rec_value = 0, mem_completed = 0;   // (the recording being made, and the GPU's progress)

struct GuestPool {
    uint64_t lo = 0, hi = 0;   // (aligned outwards the same way, so an import covers what the lookup found)
};

std::vector<GuestPool> guest_pools;   // by lo, from the host
std::atomic<uint64_t> guest_pools_gen{1};   // (bumped when that table is replaced: a span cached without the lock goes stale)
std::map<uint64_t, Pool> imported;    // by lo, made on first use
Pool* last_pool = nullptr;            // (the last pool_of hit: draws in a row bind buffers of the same pools)
extern Pool* pool_cache[4096];
void retire_pool(std::map<uint64_t, Pool>::iterator it);
Pool* pool_lookup(uint64_t guest, uint64_t size);
struct SrwMutex {   // (a slim reader/writer lock: half std::mutex's cost, taken for every buffer a draw binds)
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
};
SrwMutex pools_mu;
uint32_t generation = ~0u;
uint64_t align = 4096;
uint64_t max_import_size = UINT64_MAX;
int failures = 0;
ImportHolds import_holds;          // (issue 218: a failed import is not tried again for ImportHolds::kGapMs)
std::mutex import_holds_mu;
thread_local bool import_cooling = false;
thread_local VkResult import_result = VK_SUCCESS;

int64_t import_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool import_on_hold(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_cooling = !import_holds.allow(lo, hi, import_now_ms());
    return import_cooling;
}
void import_remember_failure(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_holds.deny_for(lo, hi, import_now_ms());
}

constexpr VkBufferUsageFlags kUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

#ifdef __linux__
// Guest memory handed to the GPU as a dma-buf over the game's own pages: the window is a memfd's (host/hos_main.c),
// and /dev/udmabuf makes a dma-buf of a range of it, which the device imports like any other. The pages are the same ones
// the game reads and writes (nothing is copied), pinned once when it is made. A host-pointer import (VK_EXT_external_memory_host)
// is a userptr buffer object to amdgpu, whose pages the kernel looks up again at every command submission, and RADV puts every
// buffer object in every submission: with ~1.5 GB of pools imported that was 15-45 ms a submission in Kokiri Forest,
// 3-8 fps with 11 ms of GPU work. A pool larger than udmabuf's size limit (/sys/module/udmabuf/parameters/size_limit_mb,
// 64 MB unless raised) is made of several dma-bufs of at most that size, bound one after another into a sparse buffer, so the
// pool is still one buffer with one device address and nobody has to raise the limit. RX_VK_NO_DMABUF=1: host pointers only,
// as before; RX_VK_DMABUF_PIECE_MB=<n>: pieces of at most n MB (to try the sparse path with the limit raised)
struct UdmabufCreate { uint32_t memfd, flags; uint64_t offset, size; };
constexpr unsigned long kUdmabufCreate = (1ul << 30) | (sizeof(UdmabufCreate) << 16) | ('u' << 8) | 0x42;   // (_IOW('u', 0x42, ...))
int dmabuf_memfd = -2, dmabuf_dev = -1, dmabuf_failures = 0;
uint64_t dmabuf_limit = 0;

/// The memory of guest [lo, lo + size) as an imported dma-buf; VK_NULL_HANDLE when udmabuf or the import refused it
VkDeviceMemory dma_buf_memory(uint64_t lo, uint64_t size) {
    UdmabufCreate c{(uint32_t)dmabuf_memfd, 1 /* UDMABUF_FLAGS_CLOEXEC */, lo, size};
    const int fd = ioctl(dmabuf_dev, kUdmabufCreate, &c);
    if (fd < 0) {
        if (dmabuf_failures++ < 8)
            logf("vk: guest %#llx..%#llx: no udmabuf (errno %d)", (unsigned long long)lo, (unsigned long long)(lo + size), errno);
        return VK_NULL_HANDLE;
    }
    static uint32_t type = UINT32_MAX;   // (the same for every dma-buf of the window: asked once)
    if (type == UINT32_MAX) {
        VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vkGetMemoryFdPropertiesKHR(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) == VK_SUCCESS) {
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent, first)
                if ((fp.memoryTypeBits >> i & 1) &&
                    (dev.memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                    type = i;
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)
                if (fp.memoryTypeBits >> i & 1) type = i;
        }
        if (type == UINT32_MAX) {
            close(fd);
            if (dmabuf_failures++ < 8) logf("vk: a dma-buf of guest memory has no memory type");
            return VK_NULL_HANDLE;
        }
    }
    VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    im.fd = fd;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult r = vkAllocateMemory(dev.device, &ai, nullptr, &memory);
    if (r != VK_SUCCESS) {   // (the fd is the caller's again when the import fails)
        close(fd);
        if (dmabuf_failures++ < 8)
            logf("vk: guest %#llx..%#llx: its dma-buf not imported (VkResult %d)", (unsigned long long)lo,
                 (unsigned long long)(lo + size), (int)r);
        return VK_NULL_HANDLE;
    }
    return memory;
}

/// A pool larger than a dma-buf may be: a sparse buffer, each `piece` bytes of it bound to its own dma-buf
bool import_dma_buf_pieces(uint64_t lo, uint64_t hi, uint64_t piece, Pool& p) {
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    bi.size = hi - lo;
    bi.usage = kUsage;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) {
        if (dmabuf_failures++ < 8) logf("vk: pool %#llx..%#llx: no sparse buffer for its dma-bufs", (unsigned long long)lo, (unsigned long long)hi);
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    // (each piece starts at a multiple of the sparse block size; the last runs to the end of what the buffer needs, past hi
    // when the size rounds up: the window's next pages, which the buffer never reaches)
    const uint64_t align = std::max<uint64_t>(req.alignment, 4096);
    piece = piece / align * align;
    const uint64_t end = lo + req.size;
    if (!piece || (req.size & 4095) || end > host->guest_size()) {
        vkDestroyBuffer(dev.device, buffer, nullptr);
        if (dmabuf_failures++ < 8)
            logf("vk: pool %#llx..%#llx: sparse block %llu, size %llu: not in dma-bufs of %llu", (unsigned long long)lo,
                 (unsigned long long)hi, (unsigned long long)req.alignment, (unsigned long long)req.size, (unsigned long long)piece);
        return false;
    }
    std::vector<VkDeviceMemory> memories;
    std::vector<VkSparseMemoryBind> binds;
    for (uint64_t off = 0; off < req.size; off += piece) {
        const uint64_t n = std::min(piece, req.size - off);
        const VkDeviceMemory m = dma_buf_memory(lo + off, n);
        if (!m) break;
        memories.push_back(m);
        binds.push_back({off, n, m, 0, 0});
    }
    VkResult res = VK_ERROR_UNKNOWN;
    if (binds.size() == (req.size + piece - 1) / piece) {
        VkSparseBufferMemoryBindInfo bb{buffer, (uint32_t)binds.size(), binds.data()};
        VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        si.bufferBindCount = 1;
        si.pBufferBinds = &bb;
        static VkFence fence = VK_NULL_HANDLE;   // (the background importer and the recording thread can both import)
        static std::mutex fence_mu;
        std::lock_guard fk(fence_mu);
        if (!fence) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(dev.device, &fi, nullptr, &fence);
        }
        {   // (on the queue, as the sparse textures' binds: nothing uses the new buffer yet, so no wait for earlier work)
            late_fix_wait_unlocked();   // (TICK-060m: a pending late fix waited for outside the queue's lock, as the other senders)
            std::lock_guard qk(dev.present_mu);
            queue_flush_locked();
            res = vkQueueBindSparse(dev.queue, 1, &si, fence);
        }
        if (res == VK_SUCCESS) {
            res = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(dev.device, 1, &fence);
        }
    }
    if (res != VK_SUCCESS) {
        vkDestroyBuffer(dev.device, buffer, nullptr);
        for (VkDeviceMemory m : memories) vkFreeMemory(dev.device, m, nullptr);
        if (dmabuf_failures++ < 8)
            logf("vk: pool %#llx..%#llx: its %zu dma-bufs not bound (VkResult %d)", (unsigned long long)lo, (unsigned long long)hi,
                 memories.size(), (int)res);
        return false;
    }
    p.lo = lo, p.hi = hi, p.buffer = buffer, p.memory = memories[0];
    p.pieces.assign(memories.begin() + 1, memories.end());
    return true;
}

bool import_dma_buf(uint64_t lo, uint64_t hi, Pool& p) {
    static uint64_t piece = 0;
    if (dmabuf_memfd == -2) {
        dmabuf_memfd = -1;
        typedef int (*MemfdFn)(void);
        const MemfdFn fn = getenv("RX_VK_NO_DMABUF") ? nullptr : (MemfdFn)dlsym(RTLD_DEFAULT, "hos_posix_guest_memfd");
        const int fd = fn ? fn() : -1;
        if (fd >= 0 && dev.dma_buf_import && vkGetMemoryFdPropertiesKHR) dmabuf_dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (fd >= 0 && dmabuf_dev >= 0) {
            dmabuf_memfd = fd;
            dmabuf_limit = 64ull << 20;
            if (FILE* f = fopen("/sys/module/udmabuf/parameters/size_limit_mb", "r")) {
                unsigned long long mb = 0;
                if (fscanf(f, "%llu", &mb) == 1 && mb) dmabuf_limit = mb << 20;
                fclose(f);
            }
            piece = dmabuf_limit;
            if (const char* e = getenv("RX_VK_DMABUF_PIECE_MB"))
                if (atoll(e) > 0) piece = std::min<uint64_t>(piece, (uint64_t)atoll(e) << 20);
        }
        logf("vk: guest memory as dma-bufs: %s", dmabuf_memfd >= 0 ? "yes" : !fn ? "no (no memfd window, or RX_VK_NO_DMABUF)"
                 : fd < 0 ? "no (the window is anonymous memory)" : !dev.dma_buf_import ? "no (VK_EXT_external_memory_dma_buf)"
                 : "no (/dev/udmabuf cannot be opened)");
        if (dmabuf_memfd >= 0)
            logf("vk: dma-bufs of at most %llu MB (udmabuf's limit %llu MB); larger pools %s", (unsigned long long)(piece >> 20),
                 (unsigned long long)(dmabuf_limit >> 20),
                 dev.sparse_buffers ? "are several, in a sparse buffer" : "are host-pointer imports (no sparse binding)");
    }
    if (dmabuf_memfd < 0 || (lo & 4095) || (hi & 4095)) return false;
    if (hi - lo > piece) {
        if (!dev.sparse_buffers || !import_dma_buf_pieces(lo, hi, piece, p)) return false;
    } else {
        VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.pNext = &eb;
        bi.size = hi - lo;
        bi.usage = kUsage;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
        const VkDeviceMemory memory = dma_buf_memory(lo, hi - lo);
        if (!memory) {
            vkDestroyBuffer(dev.device, buffer, nullptr);
            return false;
        }
        vkBindBufferMemory(dev.device, buffer, memory, 0);
        p.lo = lo, p.hi = hi, p.buffer = buffer, p.memory = memory;
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.buffer;
    p.address = vkGetBufferDeviceAddress(dev.device, &da);
    if (getenv("RX_VK_LOG_ADDR"))   // (as import_pool's: which buffer a device fault's address is in)
        logf("vk: addr dma-buf %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)p.address, (unsigned long long)(p.address + (hi - lo)));
    static int64_t n = 0, bytes = 0, sparse = 0;
    n++, bytes += (int64_t)(hi - lo), sparse += !p.pieces.empty();
    if (n <= 4 || n % 64 == 0 || (!p.pieces.empty() && sparse <= 4))
        logf("vk: pool %#llx..%#llx a dma-buf%s (%lld so, %lld in pieces, %lld MB)", (unsigned long long)lo, (unsigned long long)hi,
             p.pieces.empty() ? "" : "s in a sparse buffer", (long long)n, (long long)sparse, (long long)(bytes >> 20));
    return true;
}
#endif

bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {
    import_cooling = false;
    import_result = VK_SUCCESS;
    if (import_on_hold(lo, hi)) return false;   // (tried recently and failed: not again until the hold ends)
    if (hi <= lo || hi - lo > max_import_size) {
        logf("vk: import %#llx..%#llx exceeds the device's allocation/buffer limit", (unsigned long long)lo,
             (unsigned long long)hi);
        import_remember_failure(lo, hi);
        return false;
    }
    p.lo = lo;
    p.hi = hi;
    uint8_t* ptr = host->guest_base() + lo;
    const uint64_t bytes = hi - lo;
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    clk::time_point t1, t2, t3, t4;
    struct Report {   // (where an import's time goes: logged for slow ones, 12 times)
        const clk::time_point &a, &b, &c, &d, &e;
        ~Report() {
            if (e.time_since_epoch().count() == 0) return;
            const double total = std::chrono::duration<double, std::milli>(e - a).count();
            static int logged;
            if (total < 2.0 || logged++ >= 12) return;
            auto ms = [](clk::time_point x, clk::time_point y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
            logf("vk: import %.1f ms: commit+properties %.1f, buffer %.1f, allocate %.1f, bind+address %.1f", total, ms(a, b),
                 ms(b, c), ms(c, d), ms(d, e));
        }
    } report{t0, t1, t2, t3, t4};
    {   // (HOS_POOL_TRAP, host/hos_memsplit.c: pages a measurement made NOACCESS get their access back before the driver
        // locks them, which does not come through the host's handler. An exe without the export: nothing)
        using Release = void (*)(uint64_t, uint64_t, const char*);
        static const Release release = (Release)(void*)GetProcAddress(GetModuleHandleA(nullptr), "hos_pool_trap_release");
        if (release) release(lo, hi - lo, "import");
        // (HOS_NOACCESS_RAM: the host never gives back pages of a range imported, from before the commit below on: the driver
        // locks them, and a page decommitted under the import would leave the GPU on another page than the CPU)
        using Note = void (*)(uint64_t, uint64_t);
        static const Note note = (Note)(void*)GetProcAddress(GetModuleHandleA(nullptr), "hos_noaccess_import");
        if (note) note(lo, hi);
    }
    // (pages the game has not touched yet may be only reserved: committed now, as the pool's memory is usable)
    // (hookshot) One commit avoids the slow query walk, but resets protection on committed pages too. Keep the walk
    // for CPU write watches: commit only reserved pages, leaving the watched pages read-only.
    // ponytail: add new watch switches here; query the host if watches stop requiring a startup switch.
    static const bool page_watches = getenv("RX_NVN_WATCH_TEX") || getenv("RX_NVN_WATCH_TEXELS_SRC") ||
                                     getenv("RX_VK_WATCH_CPU") || getenv("RX_VK_VERIFY_DIM");
    if (page_watches || VirtualAlloc(ptr, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) != ptr)
        for (uint8_t* q = ptr; q < ptr + bytes;) {
            MEMORY_BASIC_INFORMATION mi{};
            if (VirtualQuery(q, &mi, sizeof mi) != sizeof mi) break;
            uint8_t* end = std::min((uint8_t*)mi.BaseAddress + mi.RegionSize, ptr + bytes);
            if (mi.State == MEM_RESERVE) VirtualAlloc(q, (size_t)(end - q), MEM_COMMIT, PAGE_READWRITE);
            q = end;
        }
#ifdef __linux__
    if (import_dma_buf(lo, hi, p)) {
        t1 = t2 = t3 = t4 = clk::now();
        return true;
    }
#endif
    // (the host allocations' memory type: the same for every pointer, asked once)
    static uint32_t type = UINT32_MAX;
    if (type == UINT32_MAX) {
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr,
                                                &hp) != VK_SUCCESS) {
            import_remember_failure(lo, hi);
            return false;
        }
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent)
            if ((hp.memoryTypeBits >> i & 1) &&
                (dev.memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                type = i;
        if (type == UINT32_MAX) {
            import_remember_failure(lo, hi);
            return false;
        }
    }
    t1 = clk::now();
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.size = bytes;
    bi.usage = kUsage;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &p.buffer) != VK_SUCCESS) {
        import_remember_failure(lo, hi);
        return false;
    }
    t2 = clk::now();
    VkImportMemoryHostPointerInfoEXT im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    im.pHostPointer = ptr;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = bytes;
    ai.memoryTypeIndex = type;
    const VkResult allocated = vkAllocateMemory(dev.device, &ai, nullptr, &p.memory);
    if (allocated != VK_SUCCESS) {
        import_result = allocated;
        vkDestroyBuffer(dev.device, p.buffer, nullptr);
        p.buffer = VK_NULL_HANDLE;
        import_remember_failure(lo, hi);
        return false;
    }
    t3 = clk::now();
    vkBindBufferMemory(dev.device, p.buffer, p.memory, 0);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.buffer;
    p.address = vkGetBufferDeviceAddress(dev.device, &da);
    if (getenv("RX_VK_LOG_ADDR"))   // (diagnostics: which buffer a device fault's address is in)
        logf("vk: addr import %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)p.address, (unsigned long long)(p.address + (hi - lo)));
    t4 = clk::now();
    return true;
}

} // namespace

// (hookshot) guest memory's page table, for the pointers shaders follow (spirv_emit_context.cpp): a page's device address, 0 for none. Written as
// pools come and go, before the submissions that can reach them
uint64_t* guest_pages = nullptr;
uint64_t guest_pages_address = 0;
VkBuffer guest_pages_buffer = VK_NULL_HANDLE;
// (hookshot, 11-07) A change to the table made while a submission is recorded goes into that submission's upload commands, after
// the mirror copies it records there, instead of into the table at once: the table is read by the GPU, and work submitted
// before (still running, or queued) read the new entries early. A pool's range zeroed at its merge read as unmapped (zero
// counts: the culling's foliage gone for a frame), and a mirror made but not yet filled read as garbage: the foliage
// culling 53caac5f looped on a count from it until Windows reset the driver (the device lost of ~1 start in 5, always
// after pool merges, 2 s after the GPU stopped)
struct TableRun { uint64_t first, count, address; };
thread_local std::vector<TableRun> table_pending;
thread_local bool table_defer = false;
void guest_pages_set(uint64_t lo, uint64_t hi, uint64_t address) {
    if (!guest_pages) return;
    // (on by default since 11-08: 14 of 14 starts without the device lost, against ~1 in 7 written at once; one-frame flashes
    // 0-1 bright / 4 dark in two 40 s Kokiri runs, as many as without it, and the same fps. One earlier run with 21 dark ones was
    // the runs' spread, not this. RX_VK_TABLE_NOW=1: written at once, as before)
    static const bool now = getenv("RX_VK_TABLE_NOW") != nullptr;
    // (two windows, as spirv_emit_context.cpp reads them: 8 GB from 0x1000000000, then 4 GB from 0x7f00000000)
    const struct { uint64_t base, bytes, first; } windows[] = {{0x1000000000ull, 8ull << 30, 0}, {0x7f00000000ull, 4ull << 30, (8ull << 30) >> 12}};
    for (const auto& w : windows) {
        const uint64_t a0 = std::max(lo, w.base) & ~0xFFFull, a1 = std::min(hi, w.base + w.bytes);
        if (a0 >= a1) continue;
        const uint64_t first = w.first + ((a0 - w.base) >> 12), count = (a1 - a0 + 0xFFF) >> 12;
        const uint64_t at = address ? address + (a0 - lo) : 0;
        if (table_defer && !now) {
            table_pending.push_back({first, count, at});
            continue;
        }
        for (uint64_t i = 0; i < count; i++) guest_pages[first + i] = at ? at + (i << 12) : 0;
    }
}
void memory_table_begin() {
    if (!table_pending.empty()) {   // (left by a recording that never ended: written now)
        table_defer = false;
        for (const TableRun& r : table_pending)
            for (uint64_t i = 0; i < r.count; i++) guest_pages[r.first + i] = r.address ? r.address + (i << 12) : 0;
        table_pending.clear();
    }
    table_defer = guest_pages != nullptr;
}
// (at the end of the recording's uploads: the changes, the last one for each page, as transfers into the table; the
// upload command buffer's barriers order them after the work submitted before and before this submission's)
bool memory_table_flush() {
    table_defer = false;
    if (table_pending.empty()) return false;
    std::vector<std::pair<uint64_t, uint64_t>> pages;   // (page, value), in the order they were set
    for (const TableRun& r : table_pending)
        for (uint64_t i = 0; i < r.count; i++) pages.push_back({r.first + i, r.address ? r.address + (i << 12) : 0});
    table_pending.clear();
    std::stable_sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> run;
    VkCommandBuffer cmd = upload_command_buffer();
    GprofSpan gp(cmd, "ptable update (guest page table)");
    gp.items = 0;
    uint64_t run_first = 0;
    const auto emit = [&] {
        if (!run.empty()) vkCmdUpdateBuffer(cmd, guest_pages_buffer, run_first * 8, run.size() * 8, run.data()), gp.items++, gp.bytes += run.size() * 8;
        run.clear();
    };
    for (size_t i = 0; i < pages.size(); i++) {
        if (i + 1 < pages.size() && pages[i + 1].first == pages[i].first) continue;   // (a later change of the same page wins)
        if (!run.empty() && (pages[i].first != run_first + run.size() || run.size() == 8192)) emit();
        if (run.empty()) run_first = pages[i].first;
        run.push_back(pages[i].second);
    }
    emit();
    return true;
}

uint64_t memory_guest_pages() {
    static std::mutex mu;
    std::lock_guard lk(mu);
    static bool tried = false;
    if (tried) return guest_pages_address;
    tried = true;
    if (getenv("RX_VK_NO_FOLLOW")) return 0;   // (on: Kokiri's lighting. The menus' blank text was the rule in graphics stages, now compute only)
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    void* mapped = nullptr;
    const uint64_t bytes = ((8ull << 30 >> 12) + (4ull << 30 >> 12)) * 8;
    if (!create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       true, b, a, &mapped, true) || !mapped) {
        logf("vk: no page table of guest memory: pointers in no bound buffer read as 0");
        return 0;
    }
    std::memset(mapped, 0, bytes);
    log_alloc_where("page table of guest memory (read by shaders following pointers)", a);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    guest_pages_address = vkGetBufferDeviceAddress(dev.device, &da);
    guest_pages_buffer = b;
    std::lock_guard pk(pools_mu);
    guest_pages = (uint64_t*)mapped;
    for (auto& kv : imported) guest_pages_set(kv.second.lo, kv.second.hi, kv.second.address);
    logf("vk: page table of guest memory at %#llx (%zu pools)", (unsigned long long)guest_pages_address, imported.size());
    return guest_pages_address;
}

bool memory_init() {
    if (!dev.external_memory_host) {
        logf("vk: no VK_EXT_external_memory_host: the guest memory cannot be imported (needed for now)");
        return false;
    }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    hp.pNext = &p11;
    p11.pNext = &p13;
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    align = std::max<uint64_t>(hp.minImportedHostPointerAlignment, 4096);
    max_import_size = std::min(p11.maxMemoryAllocationSize, p13.maxBufferSize);
    logf("vk: guest memory imported in place (alignment %llu)", (unsigned long long)align);
    return true;
}

/// Refresh the guest's pool table (no Vulkan work: pools are imported as they are used)
void memory_sync() {
    static std::mutex sync_mu;   // (called from every thread that touches memory)
    std::lock_guard<std::mutex> sync_lk(sync_mu);
    const uint32_t g = host->maps_generation();
    if (g == generation) return;
    generation = g;
    static thread_local std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<GuestPool> next;
    next.reserve(n > 0 ? (size_t)n : 0);
    for (int i = 0; i < n; i++) {
        if (!list[i][1]) continue;
        GuestPool p;
        p.lo = list[i][0] & ~(align - 1);
        p.hi = (list[i][0] + list[i][1] + align - 1) & ~(align - 1);
        next.push_back(p);
    }
    std::sort(next.begin(), next.end(), [](const GuestPool& a, const GuestPool& b) { return a.lo < b.lo; });
    std::lock_guard lk(pools_mu);
    guest_pools.swap(next);
    guest_pools_gen.fetch_add(1, std::memory_order_relaxed);
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8) logf("vk: %zu pools mapped", guest_pools.size());
}

// ---- page write generations ----
// Two levels over the guest's 512 GB (2^27 pages of 4 KB): chunks of 64K page generations made when a page of the
// chunk is first stamped, and the newest generation of each 2 MB region (512 pages), which lets a check skip a big
// texture's clean regions without looking at their pages. 0: never written.
namespace {
constexpr uint64_t kPageShift = 12, kChunkPages = 1u << 16, kRegionPages = 512;
constexpr uint64_t kPages = 1ull << 27;
std::vector<std::unique_ptr<uint32_t[]>> gen_chunks(kPages / kChunkPages);
std::vector<uint32_t> region_gen(kPages / kRegionPages);
// (and the generation every page of a region was written at, at least: a large stamp (a storage buffer a dispatch may
// write, hundreds of MB) marks whole regions, not 512 pages each; it took dispatches 8-150 ms)
std::vector<uint32_t> region_full(kPages / kRegionPages);
inline uint32_t load_u32(uint32_t& x) {
    return std::atomic_ref<uint32_t>(x).load(std::memory_order_acquire);
}
inline void store_u32(uint32_t& x, uint32_t v) {
    std::atomic_ref<uint32_t>(x).store(v, std::memory_order_release);
}
inline uint32_t page_gen(uint64_t p) {
    const auto& c = gen_chunks[p / kChunkPages];
    return std::max(c ? load_u32(c[p % kChunkPages]) : 0u, load_u32(region_full[p / kRegionPages]));
}
uint32_t gen_now = 1;
uint64_t pages_collected = 0;

// (TICK-036, RX_VK_LOG_INHERIT=<hex guest address>: that page's stamps and the retired mirrors a new mirror takes over it)
uint64_t inherit_watch_page() {
    static const uint64_t v = getenv("RX_VK_LOG_INHERIT") ? strtoull(getenv("RX_VK_LOG_INHERIT"), nullptr, 16) >> kPageShift : ~0ull;
    return v;
}
void stamp_page(uint64_t p, uint32_t g) {
    if (p >= kPages) return;
    if (p == inherit_watch_page()) logf("vk: inherit watch: page %#llx stamped, gen %u", (unsigned long long)(p << kPageShift), g);
    auto& c = gen_chunks[p / kChunkPages];
    if (!c) {
        c.reset(new uint32_t[kChunkPages]);
        std::fill(c.get(), c.get() + kChunkPages, 0u);
    }
    store_u32(c[p % kChunkPages], g);
    region_gen[p / kRegionPages] = g;
}
} // namespace

namespace { void cap_view_bump(); }   // (defined with cap_view_gen below, in this file's anonymous namespace)
const std::vector<std::pair<uint64_t, uint64_t>>& capture_ranges_merged();
void memory_collect_written() {
    uint32_t* pages = nullptr;
    const int64_t n = host->collect_written(&pages);
    if (n > 0) {
        const uint32_t g = ++gen_now;
        // (the captured ranges once a call: a lock, a pool-list refresh and three searches for every dirty page cost the
        // replay thousands of lock cycles a submit, the cost that retired gpu_pages' lock below)
        const auto& owned = capture_ranges_merged();
        const auto in_capture = [&owned](uint64_t guest) {
            auto it = std::upper_bound(owned.begin(), owned.end(), std::make_pair(guest, ~uint64_t(0)));
            return it != owned.begin() && guest < std::prev(it)->second;
        };
        for (int64_t i = 0; i < n; i++) {
            const uint64_t guest = (uint64_t)pages[i] << kPageShift;
            if (!owned.empty() && in_capture(guest)) {   // (written after its submit's capture: the next capture takes it, as it is then)
                uint64_t avail = 0;
                if (const uint8_t* q = host->gpu_ptr(guest, &avail)) host->mark_written(q, 4096);
                continue;
            }
            stamp_page(pages[i], g);
        }
        pages_collected += (uint64_t)n;
        cap_view_bump();
    }
    if (pages) host->free_pages(pages);
}
uint32_t memory_generation() {
    return gen_now;
}
bool memory_written_since(uint64_t guest, uint64_t size, uint32_t gen) {
    if (!size) return false;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        if (load_u32(region_full[r]) > gen) return true;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++)
            if (page_gen(p) > gen) return true;
    }
    return false;
}
void memory_written_runs(uint64_t guest, uint64_t size, uint32_t gen, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    if (!size) return;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++) {
            if (page_gen(p) <= gen) continue;
            const uint64_t lo = std::max(p << kPageShift, guest) - guest;
            const uint64_t hi = std::min((p + 1) << kPageShift, guest + size) - guest;
            if (!out.empty() && out.back().second == lo) out.back().second = hi;
            else out.push_back({lo, hi});
        }
    }
}
void memory_drop_shadow(uint64_t guest, uint64_t size);
// (a GPU write: RX_VK_GPU_PAGES=1 notes the pages, and the mirror syncs that copy the CPU's version of one over the
// mirror are counted (the GPU's bytes in that page replaced by what guest memory or a capture held: a page the CPU
// and the GPU both write). ~1M a race, no effect seen; off by default: its lock on every dirty page cost the replay)
std::mutex gpu_pages_mu;
std::unordered_map<uint32_t, uint32_t> gpu_pages;   // page -> the generation of the GPU's last write
int64_t st_sync_gpu_pages, st_sync_gpu_live;
bool gpu_pages_on() {
    static const bool v = getenv("RX_VK_GPU_PAGES") != nullptr;
    return v;
}
void memory_stamp_gpu(uint64_t guest, uint64_t size) {
    if (!size) return;
    diff_gpu_wrote(guest, size);
    memory_stamp(guest, size);
    if (!gpu_pages_on()) return;
    std::lock_guard lk(gpu_pages_mu);
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    if (p1 - p0 > 4096) return;   // (big copies: not tracked)
    for (uint64_t p = p0; p <= p1; p++) gpu_pages[(uint32_t)p] = gen_now;
    if (gpu_pages.size() > 2000000) gpu_pages.clear();
}
bool gpu_page(uint64_t page) {
    if (!gpu_pages_on()) return false;
    std::lock_guard lk(gpu_pages_mu);
    return gpu_pages.count((uint32_t)page) != 0;
}

// (RX_VK_PROTECT_BARE=1, an experiment for the one-frame foliage flicker and the floating trees) The ranges a shader writes
// through a table pointer (vk_draw.cpp: a bare entry, only its first 64 bytes marked written) are GPU-written pages the
// mirror syncs know nothing of: a CPU write anywhere on such a page copied the CPU's whole page over the GPU's records
// (NX2navi found the same: "trees drawn from records lost that way stretched into columns reaching the sky", and culled
// foliage drew garbage for a frame). Pages noted here keep the GPU's version when the CPU also wrote them since the
// pool's last sync. Nothing is copied back to guest memory (that overwrote the CPU's own data beside them once).
std::mutex prot_mu;
std::unordered_map<uint32_t, uint32_t> prot_pages;   // page -> gen_now when a shader last wrote through it
std::atomic<int64_t> st_prot_kept{0};
bool protect_bare_on() {
    static const bool v = getenv("RX_VK_PROTECT_BARE") && getenv("RX_VK_PROTECT_BARE")[0] == '1';
    return v;
}
bool prot_page_since(uint64_t page, uint32_t synced) {
    std::lock_guard lk(prot_mu);
    const auto it = prot_pages.find((uint32_t)page);
    return it != prot_pages.end() && it->second >= synced;
}
void memory_protect_gpu(uint64_t guest, uint64_t size) {
    diff_gpu_wrote(guest, size);   // (TICK-048: a bare entry's guessed range)
    if (!size || !protect_bare_on()) return;
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    if (p1 - p0 > 16384) return;   // (64 MB at most: a guess past that is not the GPU's records)
    std::lock_guard lk(prot_mu);
    for (uint64_t p = p0; p <= p1; p++) prot_pages[(uint32_t)p] = gen_now;
    if (prot_pages.size() > 4000000) prot_pages.clear();
}

void memory_stamp(uint64_t guest, uint64_t size) {
    if (!size) return;
    {   // (RX_VK_LOG_STAMP=<lo>:<hi> (hex guest addresses): the stamps there, 300 lines, with their callers)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_LOG_STAMP")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && guest < range.second && guest + size > range.first && logged++ < 300)
            logf("vk: stamp %#llx +%#llx (gen %u) from %p", (unsigned long long)guest, (unsigned long long)size, gen_now + 1,
                 __builtin_return_address(0));
    }
    memory_drop_shadow(guest, size);   // (a write the capture did not take: guest memory is newer than its copy)
    const uint32_t g = ++gen_now;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t p = p0; p <= p1;) {
        if (p % kRegionPages == 0 && p + kRegionPages - 1 <= p1) {   // (a whole region)
            if (inherit_watch_page() / kRegionPages == p / kRegionPages)
                logf("vk: inherit watch: region of page %#llx stamped whole, gen %u", (unsigned long long)(inherit_watch_page() << kPageShift), g);
            store_u32(region_full[p / kRegionPages], g);
            region_gen[p / kRegionPages] = g;
            p += kRegionPages;
            continue;
        }
        stamp_page(p++, g);
    }
    cap_view_bump();   // (a replay read's cached page is this generation's answer, not the one from before the write)
}
uint64_t memory_pages_collected() {
    return pages_collected;
}

// ---- submit-time capture ----
// MKW writes some of a frame's data in place right after its submit (the console's GPU has read it by then). The
// replay records a submit later, and the mirrors were filled from guest memory as it was then: a frame's terrain
// tiles read the next one's constants and flashed as walls. The pages are now copied as they are at the submit.
namespace {
struct Capture {
    std::vector<uint32_t> pages;     // (written since the last submit)
    std::vector<uint32_t> copied;    // (those copied, in order)
    std::vector<uint32_t> slots;     // (and where each is in the buffer: 4 KB a slot, the page's index in pages)
    // (copied by the submitting thread straight into the buffer the GPU copies the mirrors' pages from, in cached host
    // memory: the replay's own copy of them into one was ~0.6 ms a submit once the game's small pools were captured)
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t bsize = 0;
    size_t nbytes = 0;
    std::vector<uint32_t> deferred;   // (TICK-017: pages left to the GPU's copy from guest memory, capture_make)
    std::vector<uint32_t> late;      // (indices into pages: copied again by the replay once late_value is done)
    uint64_t late_value = 0;
    // (TICK-060, RX_VK_CAPTURE_HASH) the chunk hashes of each slot's bytes, made by the copy itself (16 a slot), and which slots have
    // them (0: none, the mirror sync hashes the slot as before)
    std::vector<uint64_t> hashes;
    std::vector<uint8_t> hok;
    // (sprint 5, RX_VK_CAPTURE_GRAN) slot i's written granules, bit k = its k-th 512 bytes hold the page's (0xff: all of it);
    // empty: every slot whole
    std::vector<uint8_t> gmask;
};
std::mutex cap_mu;
std::deque<std::unique_ptr<Capture>> captures;
std::vector<std::unique_ptr<Capture>> spare;   // (taken ones, their buffers kept)
// ---- the captures' pages, for the mirror syncs ----
// A capture takes the pages written since the last submit; a pool its submission does not bind is synced at a later
// one, whose capture does not have those pages: copied from guest memory as it was at that replay, by when MKW had
// rewritten them for a later frame (it writes several submits ahead of the replay), a frame's objects drew with
// another one's transforms and instance data (the one-frame flashes). Each page a capture took is in a table by page:
// the capture (the generation its take stamped the pages with) and the page's slot in that capture's buffer. The
// entry holds while the page's generation is still that one (a later capture re-takes the page, a write the captures
// did not take (memory_stamp) makes guest memory newer) and the capture's buffer is kept (the last kKeep captures, and
// until the work that copies from it is done); a sync copies a dirty page from there, else from guest memory.
// (A flat table and generations: the hash maps' node allocations took ~0.7 ms a submit, the replay's.)
// (RX_VK_SYNC_LIVE=1: dirty pages not in their submission's capture read from guest memory, as before)
struct CapEntry { uint32_t gen, slot; };
constexpr uint64_t kCapChunkPages = 1u << 16;   // (a chunk of the table: 256 MB of guest memory, made when first used)
// (TICK-053) the slot's top bit: a late page (a write-back still on the GPU when the capture copied it) merged at its sync,
// see "the late pages merged" below; cap_entry gives the slot without it, cap_entry_late the bit
constexpr uint32_t kCapLateBit = 0x80000000u;   // (a slot is an index into a capture's pages: under 65536)
std::vector<std::unique_ptr<CapEntry[]>> cap_table;
inline CapEntry cap_entry(uint64_t page) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size() || !cap_table[c]) return {0, 0};
    CapEntry e = cap_table[c][page % kCapChunkPages];
    e.slot &= ~kCapLateBit;
    return e;
}
inline bool cap_entry_late(uint64_t page) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size() || !cap_table[c]) return false;
    return (cap_table[c][page % kCapChunkPages].slot & kCapLateBit) != 0;
}
inline void cap_entry_set(uint64_t page, CapEntry e) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size()) cap_table.resize(c + 1);
    if (!cap_table[c]) {
        cap_table[c].reset(new CapEntry[kCapChunkPages]);
        std::memset(cap_table[c].get(), 0, kCapChunkPages * sizeof(CapEntry));
    }
    cap_table[c][page % kCapChunkPages] = e;
}
// (each kept capture's copy on the GPU, by its generation)
struct CapHash { std::vector<uint64_t> h; std::vector<uint8_t> ok; };   // (TICK-060: a kept capture's slot hashes, see Capture)
struct CapBuf { uint64_t value; VkBuffer buffer; VmaAllocation alloc; uint64_t size; void* map; uint64_t last_use = 0;
                std::shared_ptr<CapHash> hash = {};
                std::shared_ptr<std::vector<uint8_t>> gmask = {};   // (sprint 5: see Capture::gmask; null: every slot whole)
                uint64_t used = 0; };   // (the bytes its capture filled, for RX_VK_RES_LOG)
// ---- (HeyListen, sprint 5) RX_VK_CAPTURE_GRAN=1, with a game built with A64_GRAN=1 (off): the capture by written granules ----
// A capture copied every page the game wrote since the last submit, whole: ~11,900 pages a frame at the Market (46 MB, at the
// memory bus's speed: 1.3-1.9 ms on the frame's loop), of which ~1 MB of words differs from their last copy. The lifted stores
// now mark the 512 bytes they write too (nx2/rtgen.py A64_MARK, host/hos_mem.c hos_gran_take), and a capture copies only the
// granules written, into the page's slot at their place: the rest of the slot is not the page's. What reads a slot follows:
// - a mirror sync copies the slot's granules only (the chunk mask TICK-048's diff already has);
// - a granule set holds what was written since the page's last capture THAT A SYNC COPIED: a pool synced later than the next
//   capture of the page would miss the first one's granules, so a capture adds the granules of the page's entry while no sync
//   has copied that entry (PageX::syn < the entry's generation), read from guest memory, where they are unchanged since (no
//   mark came). That is what the whole page gave, for those granules;
// - a read of guest memory as submitted takes the slot's granules over what guest memory holds; a page read that way is
//   captured whole from then on (PageX::full), as are the pages of a texture's storage (their shadow is a whole page), a page
//   whose mark came without granules (given back by the host), and every page when the chunk diff, the pre-copy or the
//   replay's own thread is on;
// - a late page's copies (the take's wait, the late fix) stay whole pages: more than the mask says is the page's, never less.
struct PageX { uint32_t syn; uint8_t gm; uint8_t full; uint16_t pad; };   // (the generation a sync copied it at; its entry's mask)
std::atomic<PageX*> pagex_chunks[(1ull << 27) / 65536];
inline PageX* pagex(uint64_t page, bool make) {
    if (page >= (1ull << 27)) return nullptr;
    std::atomic<PageX*>& c = pagex_chunks[page >> 16];
    PageX* p = c.load(std::memory_order_acquire);
    if (!p) {
        if (!make) return nullptr;
        PageX* n = (PageX*)calloc(65536, sizeof(PageX));
        if (c.compare_exchange_strong(p, n, std::memory_order_acq_rel)) p = n;
        else free(n);
    }
    return p + (page & 65535);
}
typedef void (*GranTakeFn)(const uint32_t*, int64_t, uint8_t*);
GranTakeFn gran_take_fn();
bool capture_gran_on();
std::atomic<int64_t> st_gran_pages{0}, st_gran_bytes{0}, st_gran_full{0}, st_gran_union{0}, st_gran_made_full{0};
inline uint32_t gran_chunk_mask(uint8_t m8) {   // (a granule is two of the diff's 256-byte chunks)
    uint32_t m = 0;
    for (int k = 0; k < 8; k++)
        if (m8 >> k & 1) m |= 3u << (2 * k);
    return m;
}
std::vector<std::shared_ptr<CapHash>> cap_hash_free;   // (pools_mu: retired captures' hash arrays, their capacity kept)
std::map<uint32_t, CapBuf> cap_bufs;
uint64_t cap_bytes = 0, cap_used = 0;   // (pools_mu: the kept buffers' sizes, and what their captures filled of them)
constexpr size_t kKeep = 24;
constexpr uint64_t kCapBytesMax = 768ull << 20;   // (and no more than this in them: loads capture hundreds of MB)
// (retired buffers, for the next captures: making and freeing a 5 MB buffer every submit cost the replay too)
struct FreeBuf { VkBuffer buffer; VmaAllocation alloc; void* map; uint64_t size; uint64_t at = 0; uint64_t ms = 0; };
std::vector<FreeBuf> cap_free;
uint64_t cur_cap_value = 0;   // (the submission being recorded: what its syncs' copies wait for)
uint32_t cur_cap_gen = 0;     // (its capture's generation)
bool sync_live() {
    static const bool v = getenv("RX_VK_SYNC_LIVE") != nullptr;
    return v;
}
std::mutex cap_free_mu;   // (the submitting threads take buffers from cap_free, the replay gives them back)
uint64_t cap_free_bytes = 0;   // (cap_free_mu)
// (the spare buffers: 12 and, on a card of 6 GB or less, 32 MB of them; RX_VK_CAP_SPARE_MB=<n>: that many, 0: as before.
// After a load the retired ones, 40-60 MB each, stayed as spares for good)
uint64_t cap_spare_max() {
    static const uint64_t v = [] {
        if (const char* e = getenv("RX_VK_CAP_SPARE_MB")) return e[0] && atoll(e) ? (uint64_t)atoll(e) << 20 : ~0ull;
        uint64_t used = 0, allowed = 0;
        return vram_usage(&used, &allowed) && allowed <= (6ull << 30) + (512ull << 20) ? 32ull << 20 : ~0ull;
    }();
    return v;
}
// (TICK-038, off by default) RX_VK_CAP_SPARE_AGE=<n>: a spare no capture took for n captures is freed. The 12 spares held 195-385 MB of
// host memory in the Market; with 600 they held 89-411 MB and the submits made 1.5-2x as many buffers (each on the game's thread):
// the spares are taken again, not left over from the load. 0 (default): kept, as before
uint64_t cap_gets = 0;   // (cap_free_mu: buffers asked for)
uint64_t cap_spare_age() {
    static const uint64_t v = getenv("RX_VK_CAP_SPARE_AGE") ? (uint64_t)atoll(getenv("RX_VK_CAP_SPARE_AGE")) : 0;
    return v;
}
uint64_t cap_spares_aged = 0;   // (cap_free_mu)
// (HeyListen, 2026-11-14) RX_VK_CAP_SPARE_SECS=<s> (20 by default; 0: kept, as before): a spare no capture took for that long is
// freed. The spares are what a load's captures left (40-64 MB each): after the scene had loaded, 10 of the 11 were never taken
// again (the fewest spares left after a take, RX_VK_RES_LOG), 276-400 MB of host-cached memory in the three scenes on the RTX
// 5070. Unlike RX_VK_CAP_SPARE_AGE (counted in takes, ~1000 a second: 600 freed spares the next frames took again) it is time:
// a spare left unused for 20 s is not part of the scene's steady need
uint64_t cap_spare_ms() {
    static const uint64_t v = (getenv("RX_VK_CAP_SPARE_SECS") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_CAP_SPARE_SECS"))) : 20) * 1000;
    return v;
}
// (HeyListen, 2026-11-14) RX_VK_CAP_FIT=1 (default; 0: as before): a capture takes a spare at most twice its need's size class
// (cap_buf_size), else a buffer of its size is made; and a spare returned when 12 are kept takes the largest one's place if it is
// smaller. Best fit alone gave the load's 40-64 MB spares to captures of a few MB: 68-886 such takes per 600 frames on the RTX
// 5070, and the 25 kept buffers (each held for 24 captures, for the mirror syncs) were 283-309 MB of which their captures filled
// 150-171 MB
bool cap_fit() {
    static const bool v = !getenv("RX_VK_CAP_FIT") || atoi(getenv("RX_VK_CAP_FIT")) != 0;
    return v;
}
uint64_t cap_now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void cap_buf_free(VkBuffer buffer, VmaAllocation alloc, void* map, uint64_t size) {
    FreeBuf old[12];
    int n_old = 0;
    const uint64_t now = cap_now_ms();
    {
        std::lock_guard lk(cap_free_mu);
        if (const uint64_t ms = cap_spare_ms())
            for (size_t i = 0; i < cap_free.size();)
                if (now - cap_free[i].ms > ms && n_old < 12) {
                    old[n_old++] = cap_free[i];
                    cap_free_bytes -= cap_free[i].size;
                    cap_free.erase(cap_free.begin() + (ptrdiff_t)i);
                    cap_spares_aged++;
                } else {
                    i++;
                }
        if (const uint64_t age = cap_spare_age())
            for (size_t i = 0; i < cap_free.size();)
                if (cap_gets - cap_free[i].at > age && n_old < 12) {
                    old[n_old++] = cap_free[i];
                    cap_free_bytes -= cap_free[i].size;
                    cap_free.erase(cap_free.begin() + (ptrdiff_t)i);
                    cap_spares_aged++;
                } else {
                    i++;
                }
        // (RX_VK_CAP_FIT: a smaller one takes the place of the largest spare left untaken for 2 s. Not any largest: a castle walk's
        // ~50 MB capture a frame lost its spare to the small ones and made a buffer every frame, ~1000 in a run)
        if (cap_fit() && cap_free.size() >= 12 && size <= (64ull << 20)) {
            size_t big = cap_free.size();
            for (size_t i = 0; i < cap_free.size(); i++)
                if (now - cap_free[i].ms > 2000 && (big == cap_free.size() || cap_free[i].size > cap_free[big].size)) big = i;
            if (big < cap_free.size() && cap_free[big].size > size && n_old < 12) {
                old[n_old++] = cap_free[big];
                cap_free_bytes -= cap_free[big].size;
                cap_free.erase(cap_free.begin() + (ptrdiff_t)big);
            }
        }
        if (cap_free.size() < 12 && size <= (64ull << 20) && cap_free_bytes + size <= cap_spare_max()) {
            cap_free.push_back({buffer, alloc, map, size, cap_gets, now});
            cap_free_bytes += size;
            buffer = VK_NULL_HANDLE;
        }
    }
    if (buffer) destroy_buffer(buffer, alloc);
    for (int i = 0; i < n_old; i++) destroy_buffer(old[i].buffer, old[i].alloc);
}
void cap_buf_retire(std::map<uint32_t, CapBuf>::iterator it) {
    if (it->second.hash && cap_hash_free.size() < 32) cap_hash_free.push_back(std::move(it->second.hash));
    cap_bytes -= it->second.size;
    cap_used -= it->second.used;
    cap_buf_free(it->second.buffer, it->second.alloc, it->second.map, it->second.size);
    cap_bufs.erase(it);
}
// (a capture's buffer by its size class: 2, 4, 8, then 16 MB and up to the MB. It was never under 16 MB, for captures of ~1 MB:
// 25 kept ones were 413 MB of host-cached system memory, all resident, and three to a 64 MB block left 14 MB of each unused.
// RX_VK_CAP_MIN_MB=<n>: the smallest (2), 16: as before. From 16 MB each has a memory object of its own: RX_VK_CAP_BLOCKS=1: not)
uint64_t cap_buf_size(uint64_t need) {
    static const uint64_t min = (getenv("RX_VK_CAP_MIN_MB") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_CAP_MIN_MB"))) : 2u) << 20;
    uint64_t s = min;
    while (s < need && s < (16ull << 20)) s <<= 1;
    return s >= need ? s : (need + (1u << 20) - 1) & ~((1ull << 20) - 1);
}
uint64_t cap_bufs_made = 0;   // (TICK-038: capture buffers made, for the log: a spare that was missed)
// (cap_free_mu; RX_VK_RES_LOG, since its last line) the largest need, the takes of a spare over twice the need's size class,
// and the fewest spares left after a take
uint64_t cap_need_max = 0, cap_taken_over = 0, cap_free_low = ~0ull;
bool cap_buf_get(uint64_t need, VkBuffer& buffer, VmaAllocation& alloc, void*& map, uint64_t& size) {
    std::lock_guard lk(cap_free_mu);
    cap_gets++;
    cap_need_max = std::max(cap_need_max, need);
    size_t best = cap_free.size();
    for (size_t i = 0; i < cap_free.size(); i++)
        if (cap_free[i].size >= need && (best == cap_free.size() || cap_free[i].size < cap_free[best].size)) best = i;
    if (best < cap_free.size() && cap_fit() && cap_free[best].size > 2 * cap_buf_size(need))   // (RX_VK_CAP_FIT: counted as over)
        best = cap_free.size(), cap_taken_over++;
    if (best < cap_free.size()) {
        buffer = cap_free[best].buffer, alloc = cap_free[best].alloc, map = cap_free[best].map, size = cap_free[best].size;
        cap_free_bytes -= size;
        cap_free.erase(cap_free.begin() + (ptrdiff_t)best);
        if (size > 2 * cap_buf_size(need)) cap_taken_over++;
        cap_free_low = std::min<uint64_t>(cap_free_low, cap_free.size());
        return map != nullptr;
    }
    cap_free_low = 0;
    size = cap_buf_size(need);
    map = nullptr;
    cap_bufs_made++;
    MemKindScope mem_kind_(MemKind::Capture);   // (TICK-038: RX_VK_RES_LOG)
    const bool ok = create_buffer_cached(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, buffer, alloc, &map,
                                         size >= (16ull << 20) && !getenv("RX_VK_CAP_BLOCKS")) && map;
    static int logged;   // (TICK-019: the mirror syncs copy from these)
    if (ok && logged++ < 1) log_alloc_where("a capture buffer (the mirror syncs' source)", alloc);
    return ok;
}
std::vector<std::pair<uint64_t, uint64_t>> cap_ranges;   // (the mirrored pools the captures take, as of the last one)
std::mutex cap_ranges_mu;
uint64_t cap_ranges_ver = 1;   // (any of the captures' range lists changed: cap_ranges_mu)
// (and the game's pools up to RX_VK_CAPTURE_POOL_MB (default 64; 0: none) from when they are made, mirrored or not:
// pools are imported, and mirrored, at the replay's first binding of them, two presents behind the game, and the
// captures taken before that left them out. MK World moves its per-frame data to a new pool every so many frames:
// the new pool's mirror was filled and synced from the game's memory as it was at the replay, its next frames' data
// in it already, until a mirror made the slow way published the list again (seconds): shadows dark below a line,
// the picture cyan below one, in bursts every 30 or 60 frames. The bigger pools hold textures, whose storage the
// capture takes anyway.)
// (RX_VK_CAPTURE_EAGER=<n>: only for their first n captures. Off: pools made at the load are bound by the menus much
// later, and they flashed (30 in the main menu); and the pages the captures copy are almost all the mirrored pools'
// anyway)
std::vector<std::pair<uint64_t, uint64_t>> pool_cap_ranges;   // (merged; cap_ranges_mu)
uint32_t pool_cap_gen = 0;
uint64_t captures_made = 0, pool_cap_built = 0;              // (cap_ranges_mu)
std::unordered_map<uint64_t, uint64_t> pool_first_seen;     // (a pool's start -> the capture it was first seen at)
uint64_t capture_pool_max() {
    static const uint64_t v = (getenv("RX_VK_CAPTURE_POOL_MB") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_POOL_MB")) : 64) << 20;
    return v;
}
uint64_t eager_captures() {
    static const uint64_t v = getenv("RX_VK_CAPTURE_EAGER") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_EAGER")) : 0;
    return v;
}
void refresh_pool_ranges_locked() {   // (cap_ranges_mu held)
    if (!capture_pool_max()) return;
    const uint32_t g = host->maps_generation();
    if (g == pool_cap_gen && captures_made - pool_cap_built < 64) return;   // (and every 64: the old ones out)
    pool_cap_gen = g, pool_cap_built = captures_made;
    static std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<std::pair<uint64_t, uint64_t>> v;
    std::unordered_map<uint64_t, uint64_t> seen;
    seen.reserve((size_t)std::max(n, 0));
    for (int i = 0; i < n; i++) {
        if (!list[i][1] || list[i][1] > capture_pool_max() || list[i][0] >= (1ull << 40)) continue;
        auto it = pool_first_seen.find(list[i][0]);
        const uint64_t first = it != pool_first_seen.end() ? it->second : captures_made;
        seen[list[i][0]] = first;
        if (!eager_captures() || captures_made - first < eager_captures())
            v.push_back({list[i][0] & ~4095ull, (list[i][0] + list[i][1] + 4095) & ~4095ull});
    }
    pool_first_seen.swap(seen);
    cap_ranges_ver++;
    std::sort(v.begin(), v.end());
    std::vector<std::pair<uint64_t, uint64_t>> m;
    for (const auto& r : v) {
        if (!m.empty() && r.first <= m.back().second) m.back().second = std::max(m.back().second, r.second);
        else m.push_back(r);
    }
    pool_cap_ranges.swap(m);
}
bool capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_CAPTURE");
        const bool v = e ? atoi(e) != 0 : true;
        logf("vk: submit-time capture of written pages %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::atomic<bool> captures_seen{false};
std::atomic<uint64_t> cap_view_gen{1};   // (bumped when a capture is published or a page is stamped: cached reads go stale)
void cap_view_bump() { cap_view_gen.fetch_add(1, std::memory_order_release); }
double st_tk_all, st_tk_stamp, st_tk_copy, st_tk_late, st_tk_lock;   // (memory_take_capture's parts, ms)
double st_tk_stamps, st_tk_shadow, st_tk_entries;   // (TICK-061: inside "retires and stamps": the page stamps, the texture shadow, the table entries)
int64_t st_tk_shadow_fast;   // (TICK-061: takes whose shadow block took RX_VK_SHADOW_FAST's path)
std::atomic<int64_t> st_tr_late_ns{0}, st_tr_late_n{0};   // (TICK-051: the same, never reset, for the frame trace)
int64_t st_tk_late_n;   // (the takes that waited for the GPU for their late pages)   // (a capture was ever made: the replay thread's; without it nothing owns pages)

// ---- texture storage in the capture ----
// The textures' storage the replay uploads from joins the capture, and what the capture took of it is kept (the
// shadow) for the uploads to read instead of guest memory: the storage as it was at its submit. MKW writes its terrain's
// height tiles (68 x 68 RGBA16, at new addresses every frame) for a frame and then rewrites them right after the
// submit; uploaded when the replay recorded the submit, from guest memory as it was by then, a frame's tiles could take
// another's heights. Measured at a snowy spot it is rare: of 107,000 pages the uploads read from the copies, 13 had
// been rewritten since their submit. In a Grand Prix it is what flashed: ~100 one-frame flashes a minute (giant
// stretched shapes over most of the picture, black or fog-coloured bands, a 30-frame rhythm), 9-18 rewritten pages of
// 800,000, and none with it on. On by default now that the guarded copy holds (a call, not inlined). (A page the game
// wrote and then freed is still marked: the copy commits it again, zeros, on first touch (commit_on_touch), which is
// what the mirrors must see of it; clearing the marks at the free lost that, and a Grand Prix loaded to a black
// screen.) RX_VK_TEX_CAPTURE=0: off.
bool tex_capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_TEX_CAPTURE");
        const bool v = (e ? atoi(e) != 0 : true) && capture_on();
        logf("vk: texture storage in the submit-time capture %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::vector<std::pair<uint64_t, uint64_t>> tex_cap_ranges;   // (texture storage the capture takes, merged; cap_ranges_mu)
std::mutex shadow_mu;   // (after pools_mu, before cap_ranges_mu)
struct ShadowPage {
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;   // (the capture that took it)
};
std::unordered_map<uint32_t, ShadowPage> shadow;   // guest page -> its 4 KB as the last capture taken had it
std::unordered_map<uint32_t, uint32_t> shadow_regions;   // (2 MB region -> its pages in the shadow)
std::atomic<size_t> shadow_n{0};
uint64_t shadow_seq = 0;   // (captures taken)
// (256 MB: past it the pages no capture took in the last 64 go, the oldest first: those the game rewrites every frame
// stay; if none can go, a page is read from guest memory as before)
constexpr size_t kShadowMax = 65536;
std::map<uint64_t, std::pair<uint64_t, uint64_t>> tex_spans;   // lo -> (hi, the present a texture was last uploaded from it)
std::vector<std::pair<uint64_t, uint64_t>> tex_merged;          // (tex_spans merged; shadow_mu)
bool tex_spans_changed = false;
uint64_t tex_merged_bytes = 0;

bool in_tex_merged(uint64_t guest) {   // (shadow_mu held)
    auto it = std::upper_bound(tex_merged.begin(), tex_merged.end(), std::make_pair(guest, ~uint64_t(0)));
    return it != tex_merged.begin() && guest < std::prev(it)->second;
}
void shadow_erase(uint32_t page) {   // (shadow_mu held)
    if (!shadow.erase(page)) return;
    auto r = shadow_regions.find(page >> 9);
    if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
    shadow_n--;
}
void shadow_drop_range(uint64_t lo, uint64_t hi) {   // (shadow_mu held)
    if (hi <= lo) return;
    for (uint64_t r = lo >> 21; r <= (hi - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(lo >> 12, r << 9), p1 = std::min((hi + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) shadow_erase((uint32_t)p);
    }
}
// ---- (TICK-061, C3 = E2) RX_VK_SHADOW_FAST=1 (off by default): take_capture's texture-shadow block with less work a page ----
// The block (shadow_mu and pools_mu held, on the replay thread or the held flush) did for every page of a capture two binary
// searches of tex_merged (one in the erase pass, one in the insert pass), a hash erase, then for every copied texture page a hash
// insert, a fresh `new uint8_t[4096]` and a 4 KB copy: each page the capture just copied was first erased and then made again.
// Here (shadow_take_fast):
// - whether a page is texture storage comes from one merge walk of the sorted page list against the sorted, disjoint tex_merged
//   (the same answer as in_tex_merged); a page list not in ascending order (the TICK-043 pre-copy's merged list) takes the old code;
// - a copied texture page keeps its entry and its 4 KB block, which the copy overwrites; only texture pages written but not copied
//   (a failed copy, a deferred page) are erased, as before;
// - a page new to the shadow takes a block left by an erased one when there is one (shadow_slab, up to 1024 blocks), else new[].
// The shadow after the block is the same: the old code erased every texture page of c->pages and then inserted each copied one,
// so a copied page ends with an entry holding the capture's bytes and seq = shadow_seq, a written but not copied one with none,
// every other entry as it was; shadow_n and shadow_regions (each region's count of entries; a region at 0 leaves the map) follow
// from that membership alone, whichever way the counts moved on the way. The sweeps decide per entry by seq (no early stop), so
// the map's iteration order does not change what they keep. The only path where the order of the old code mattered is the cap
// (kShadowMax reached inside the insert loop, a sweep or a stop part way): the fast path runs only when it cannot be reached,
// shadow_n + (copied texture pages) <= kShadowMax before anything is erased (the old count after its erase pass is at most
// shadow_n, and each insert adds one), else the old code runs as it was. The readers (memory_as_submitted, memory_has_shadow)
// take shadow_mu, held throughout, so none sees a state in between.
bool shadow_fast_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_SHADOW_FAST");
        const bool v = e && atoi(e) != 0;
        logf("vk: a capture's texture shadow: %s (RX_VK_SHADOW_FAST)", v ? "one walk, blocks kept" : "searched, made again, as before");
        return v;
    }();
    return on;
}
std::vector<std::unique_ptr<uint8_t[]>> shadow_slab;   // (4 KB blocks of pages that left the shadow, for new ones; shadow_mu)
constexpr size_t kShadowSlab = 1024;
void shadow_erase_keep(uint32_t page) {   // (shadow_mu held: shadow_erase, the block kept for the next page)
    auto it = shadow.find(page);
    if (it == shadow.end()) return;
    if (it->second.b && shadow_slab.size() < kShadowSlab) shadow_slab.push_back(std::move(it->second.b));
    shadow.erase(it);
    auto r = shadow_regions.find(page >> 9);
    if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
    shadow_n--;
}
/// The block for a capture: pages (c->pages), the copied ones and their slots (c->copied, c->slots: slot = index in pages), the
/// capture's mapped copy. False: not taken (the old code runs). shadow_mu held, shadow_seq already advanced
bool shadow_take_fast(const uint32_t* pages, size_t n, const uint32_t* copied, const uint32_t* slots, size_t m, const uint8_t* map) {
    if (!map || !std::is_sorted(pages, pages + n)) return false;
    static thread_local std::vector<uint8_t> flag;   // (bit 0: texture storage, bit 1: copied)
    flag.assign(n, 0);
    size_t r = 0;
    for (size_t i = 0; i < n; i++) {
        const uint64_t g = (uint64_t)pages[i] << 12;
        while (r < tex_merged.size() && tex_merged[r].second <= g) r++;
        if (r < tex_merged.size() && g >= tex_merged[r].first) flag[i] = 1;
    }
    size_t copied_tex = 0;
    for (size_t j = 0; j < m; j++) {
        if (slots[j] >= n || pages[slots[j]] != copied[j]) return false;   // (not the layout described: the old code)
        if (flag[slots[j]] & 1) flag[slots[j]] |= 2, copied_tex++;
    }
    if (shadow_n.load(std::memory_order_relaxed) + copied_tex > kShadowMax) return false;   // (the cap could be reached: the old code)
    for (size_t i = 0; i < n; i++)
        if (flag[i] == 1) shadow_erase_keep(pages[i]);   // (written, not copied: its old copy is stale)
    for (size_t j = 0; j < m; j++) {
        if (flag[slots[j]] != 3) continue;
        const uint32_t p = copied[j];
        auto [it, made] = shadow.try_emplace(p);
        ShadowPage& slot = it->second;
        if (!slot.b) {
            if (!shadow_slab.empty()) slot.b = std::move(shadow_slab.back()), shadow_slab.pop_back();
            else slot.b.reset(new uint8_t[4096]);
        }
        if (made) {
            shadow_regions[p >> 9]++;
            shadow_n++;
        }
        slot.seq = shadow_seq;
        std::memcpy(slot.b.get(), map + (uint64_t)slots[j] * 4096, 4096);
    }
    return true;
}
/// (the spans unused for 1800 presents dropped with their shadow pages; the rest published for the capture)
void publish_tex_spans(uint64_t frame) {
    std::lock_guard lk(shadow_mu);
    for (auto it = tex_spans.begin(); it != tex_spans.end();) {
        if (frame > it->second.second + 1800) {
            shadow_drop_range(it->first, it->second.first);
            it = tex_spans.erase(it);
            tex_spans_changed = true;
        } else {
            ++it;
        }
    }
    if (!tex_spans_changed) return;
    tex_spans_changed = false;
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, e] : tex_spans) {
        if (!v.empty() && lo <= v.back().second) v.back().second = std::max(v.back().second, e.first);
        else v.push_back({lo, e.first});
    }
    tex_merged = v;
    tex_merged_bytes = 0;
    for (const auto& r : v) tex_merged_bytes += r.second - r.first;
    std::lock_guard lk2(cap_ranges_mu);
    tex_cap_ranges.swap(v);
    cap_ranges_ver++;
}
} // namespace

void memory_note_texture_upload(uint64_t guest, uint64_t size) {
    if (!size || guest >= (1ull << 40) || size > (1ull << 40) || !tex_capture_on()) return;   // (hookshot: no wrap below)
    uint64_t lo, hi;
    {   // (the pool it is in, when that is at most 256 MB; else the 2 MB regions it spans)
        std::lock_guard lk(pools_mu);
        auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                                   [](uint64_t g, const GuestPool& p) { return g < p.lo; });
        if (it != guest_pools.begin() && guest >= std::prev(it)->lo && guest + size <= std::prev(it)->hi &&
            std::prev(it)->hi - std::prev(it)->lo <= (256ull << 20)) {
            lo = std::prev(it)->lo, hi = std::prev(it)->hi;
        } else {
            lo = guest & ~((2ull << 20) - 1), hi = (guest + size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
        }
    }
    const uint64_t frame = texture_frame_no();
    std::lock_guard lk(shadow_mu);
    auto [it, fresh] = tex_spans.try_emplace(lo, hi, frame);
    if (!fresh) {
        it->second.second = frame;
        if (it->second.first >= hi) return;
        it->second.first = hi;
    }
    tex_spans_changed = true;
}

/// A replay read of one page can reuse what the last read of that page decided, while the page's write generation and
/// the capture table are the ones that decision was made from.
bool submitted_page_reuse(uint64_t page, uint32_t page_gen, uint64_t view, uint64_t cached_page, uint32_t cached_gen,
                          uint64_t cached_view) {
    return cached_view != 0 && page == cached_page && page_gen == cached_gen && view == cached_view;
}
struct SubmittedPage {
    uint64_t page = ~0ull;
    uint32_t pgen = 0;
    uint64_t view = 0;
    bool overlay = false;   // (bytes is the capture's copy of the page; otherwise guest memory stands)
    uint8_t bytes[4096]{};
};
thread_local SubmittedPage submitted_page;
bool late_fix_slot_pending(uint32_t gen, uint32_t slot, uint64_t* value);   // (TICK-060, below)
bool guarded_copy(void* dst, const void* src, size_t n);   // (below)
std::atomic<int64_t> st_late_def_full{0}, st_late_reads{0}, st_late_syncs{0};   // (TICK-060)

bool memory_read_as_submitted(uint64_t gpu, void* dst, uint64_t n) {
    // (once a capture exists, every uniform word took pools_mu to ask whether its page was in one. The next words are
    // the same page: the answer is kept until a stamp or a new capture)
    if (n && capture_on() && captures_seen.load(std::memory_order_relaxed)) {
        const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
        if (guest) {
            const uint64_t page0 = guest >> kPageShift, page1 = (guest + n - 1) >> kPageShift;
            const uint64_t view = cap_view_gen.load(std::memory_order_acquire);
            if (page0 == page1 && submitted_page_reuse(page0, page_gen(page0), view, submitted_page.page,
                                                       submitted_page.pgen, submitted_page.view) &&
                submitted_page.overlay) {
                std::memcpy(dst, submitted_page.bytes + (guest & 4095), (size_t)n);
                return true;
            }
        }
    }
    if (!host->gpu_read(gpu, dst, n)) return false;
    if (!n || !capture_on() || !captures_seen.load(std::memory_order_relaxed)) return true;
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return true;
    const uint64_t page0 = guest >> kPageShift, page1 = (guest + n - 1) >> kPageShift;
    const uint64_t view = cap_view_gen.load(std::memory_order_acquire);
    if (page0 == page1 && submitted_page_reuse(page0, page_gen(page0), view, submitted_page.page, submitted_page.pgen,
                                               submitted_page.view))
        return true;
    static std::atomic<int64_t> reads{0}, pages_from{0};
    int64_t from = 0;
    {
        std::lock_guard lk(pools_mu);
        bool late_read = false;
        for (uint64_t page = guest >> kPageShift; page < (guest + n + 4095) >> kPageShift; page++) {
            const CapEntry e = cap_entry(page);
            if (!e.gen || e.gen != page_gen(page)) continue;
            auto b = cap_bufs.find(e.gen);
            if (b == cap_bufs.end() || !b->second.map) continue;
            const uint64_t a = std::max(page << kPageShift, guest), z = std::min((page + 1) << kPageShift, guest + n);
            // (TICK-060, RX_VK_LATE_DEFER) a late slot whose fix is pending: its value waited for and the fixes reached made here
            // (TICK-060m), so this read and the GPU take the same bytes, the slot's, as with the take's wait. Only past the wait's
            // timeouts (the value not reached in 2 s) guest memory as it is, not kept for the next reads
            if (uint64_t lv = 0; late_fix_slot_pending(e.gen, e.slot, &lv)) {
                vk_timeline_wait_sent(lv);
                memory_late_fix_apply(vk_timeline_completed());
                st_late_reads.fetch_add(1, std::memory_order_relaxed);
                if (late_fix_slot_pending(e.gen, e.slot, nullptr)) {
                    guarded_copy((uint8_t*)dst + (a - guest), (const uint8_t*)host->guest_base() + a, z - a);
                    late_read = true;
                    from++;
                    continue;
                }
            }
            if (const uint8_t gm = b->second.gmask && e.slot < b->second.gmask->size() ? (*b->second.gmask)[e.slot] : 0xff; gm != 0xff) {
                // (sprint 5: its granules only, over what guest memory holds; the page whole from its next capture on)
                for (unsigned k = 0; k < 8; k++) {
                    if (!(gm >> k & 1)) continue;
                    const uint64_t g0 = (page << kPageShift) + (uint64_t)k * 512, a2 = std::max(a, g0), z2 = std::min(z, g0 + 512);
                    if (a2 < z2)
                        std::memcpy((uint8_t*)dst + (a2 - guest), (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096 + (a2 - (page << kPageShift)), z2 - a2);
                }
                if (PageX* x = pagex(page, true); x && !x->full) x->full = 1, st_gran_made_full.fetch_add(1, std::memory_order_relaxed);
                late_read = true;   // (not kept as the page's whole copy below)
                from++;
                continue;
            }
            std::memcpy((uint8_t*)dst + (a - guest), (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096 + (a - (page << kPageShift)), z - a);
            from++;
        }
        if (page0 == page1 && !late_read) {
            submitted_page.page = page0;
            submitted_page.pgen = page_gen(page0);
            submitted_page.view = cap_view_gen.load(std::memory_order_relaxed);
            submitted_page.overlay = false;
            const CapEntry e = cap_entry(page0);
            if (e.gen && e.gen == submitted_page.pgen) {
                auto b = cap_bufs.find(e.gen);
                if (b != cap_bufs.end() && b->second.map &&
                    !(b->second.gmask && e.slot < b->second.gmask->size() && (*b->second.gmask)[e.slot] != 0xff)) {
                    std::memcpy(submitted_page.bytes, (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096, 4096);
                    submitted_page.overlay = true;
                }
            }
        }
    }
    pages_from += from;
    if (++reads % 20000 == 0)
        logf("vk: replay reads of guest memory as submitted: %lld (%lld pages from the captures)", (long long)reads.load(),
             (long long)pages_from.load());
    return true;
}

uint64_t memory_capture_view() { return cap_view_gen.load(std::memory_order_acquire); }
uint32_t memory_page_stamp(uint64_t gpu) {
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return 0;
    return page_gen(guest >> kPageShift);
}

bool memory_has_shadow(uint64_t guest, uint64_t n) {
    if (!n || !shadow_n.load(std::memory_order_relaxed)) return false;
    std::lock_guard lk(shadow_mu);
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++)
            if (shadow.count((uint32_t)p)) return true;
    }
    return false;
}

/// (RX_VK_TEX_CAPTURE_OFF_FILE=<path> (A/B): while that file exists the uploads read guest memory as before)
bool overlay_off() {
    static const char* path = getenv("RX_VK_TEX_CAPTURE_OFF_FILE");
    if (!path) return false;
    static std::atomic<int64_t> next{0};
    static std::atomic<bool> off{false};
    const int64_t now = GetTickCount64();
    if (now >= next.load(std::memory_order_relaxed)) {
        next.store(now + 200, std::memory_order_relaxed);
        const bool v = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        if (v != off.exchange(v)) logf("vk: texture storage as submitted: %s (A/B file)", v ? "OFF" : "on");
    }
    return off.load(std::memory_order_relaxed);
}
std::atomic<int64_t> st_overlaid{0}, st_differed{0};   // (pages read from the shadow; of them, ones guest memory had changed)

const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, std::vector<uint8_t>& scratch) {
    if (!n || !shadow_n.load(std::memory_order_relaxed) || overlay_off()) return live;
    std::lock_guard lk(shadow_mu);
    bool any = false;
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) {
            auto it = shadow.find((uint32_t)p);
            if (it == shadow.end()) continue;
            if (!any) {
                any = true;
                if (scratch.data() != live) scratch.assign(live, live + n);
            }
            const uint64_t a = std::max(p << 12, guest), b = std::min((p + 1) << 12, guest + n);
            st_overlaid++;
            if (std::memcmp(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a)) st_differed++;
            std::memcpy(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a);
        }
    }
    static std::atomic<int64_t> served{0};
    if (any && served.fetch_add(1) % 2000 == 0)
        logf("vk: texture storage read as submitted (%lld times; %lld pages from the copies, %lld of them rewritten in "
             "guest memory since their submit; %zu pages kept, %zu spans, %llu MB)", (long long)served.load(),
             (long long)st_overlaid.load(), (long long)st_differed.load(), shadow.size(), tex_spans.size(),
             (unsigned long long)(tex_merged_bytes >> 20));
    return any ? scratch.data() : live;
}

void memory_drop_shadow(uint64_t guest, uint64_t size) {
    if (!size || !shadow_n.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(shadow_mu);
    shadow_drop_range(guest, guest + size);
}

// ---- (EvilBoyce) GPU results by page: the submission that last wrote a page into guest memory ----
// For invalidate (vk_replay.cpp): a CPU read of GPU results waits until the submission that writes them there is done, as
// the console's coherent memory has them once that work is. Guest page number -> the recording's timeline value (write-backs
// from a mirror, and small bindings written in place). RX_VK_NO_READBACK_WAIT=1: none
namespace {
std::mutex wb_pages_mu;
// (each page's value: the timeline value of its last write-back, 0 for never, in chunks of kChunkPages made when a page of
// the chunk is first written back, as the page generations above. Write-backs are of imported pools only, which lie in the
// GPU's windows, so few chunks are ever made. Values are never lowered or dropped: the readers (memory_writeback_value's
// "ever written back" and its value against the GPU's progress, and the capture's wait, the same compare) read one the GPU
// is past as "written back, nothing to wait for", whatever it is. tests/test_wb_pages.py)
struct WbPages {
    std::vector<std::unique_ptr<uint64_t[]>> chunks = std::vector<std::unique_ptr<uint64_t[]>>(kPages / kChunkPages);
    bool any = false;
    void set(uint64_t page, uint64_t value) {
        if (page >= kPages) return;
        auto& c = chunks[page / kChunkPages];
        if (!c) {
            c.reset(new uint64_t[kChunkPages]);
            std::fill(c.get(), c.get() + kChunkPages, 0ull);
        }
        c[page % kChunkPages] = value;
        any = true;
    }
    uint64_t get(uint64_t page) const {
        if (page >= kPages) return 0;
        const auto& c = chunks[page / kChunkPages];
        return c ? c[page % kChunkPages] : 0;
    }
    /// the largest value of pages first..last (0: none), and whether any had one; a chunk never made is skipped whole
    uint64_t max_in(uint64_t first, uint64_t last, bool* ever) const {
        uint64_t v = 0;
        for (uint64_t p = first; p <= last && p < kPages;) {
            const uint64_t end = (p / kChunkPages + 1) * kChunkPages;   // (the first page past p's chunk)
            const auto& c = chunks[p / kChunkPages];
            if (!c) {
                p = end;
                continue;
            }
            for (; p <= last && p < end; p++)
                if (const uint64_t pv = c[p % kChunkPages]) {
                    if (ever) *ever = true;
                    v = std::max(v, pv);
                }
        }
        return v;
    }
    bool empty() const { return !any; }
} wb_pages;
void wb_pages_note(uint64_t guest_lo, uint64_t guest_hi, uint64_t value) {
    static const bool off = getenv("RX_VK_NO_READBACK_WAIT") != nullptr;
    if (off || !value || guest_hi <= guest_lo) return;
    std::lock_guard lk(wb_pages_mu);
    for (uint64_t p = guest_lo >> kPageShift; p <= (guest_hi - 1) >> kPageShift; p++) wb_pages.set(p, value);
}
} // namespace
uint64_t memory_writeback_value(uint64_t guest, uint64_t size, bool* ever) {
    uint64_t v = 0;
    if (ever) *ever = false;
    if (!size) return 0;
    std::lock_guard lk(wb_pages_mu);
    if (wb_pages.empty()) return 0;
    v = wb_pages.max_in(guest >> kPageShift, (guest + size - 1) >> kPageShift, ever);
    return v;
}

/// A copy that fails instead of faulting on a page unmapped meanwhile (structured exception handling: the common case
/// costs a memcpy; ReadProcessMemory per page was ~2.3 us each). Not inlined, and the copy a call: without
/// -fasync-exceptions clang catches a fault in a __try block only across a call, and a 4 KB memcpy inlined into the
/// block (the per-page retry) faulted past the handler: the crash that kept the texture storage out of the capture.
void* (*volatile guarded_memcpy)(void*, const void*, size_t) = std::memcpy;
#ifdef _WIN32
__declspec(noinline) bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        guarded_memcpy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
#else
// (linux: the guest window is one mapping that stays readable, decommitted pages read as zero: nothing to fault on)
bool guarded_copy(void* dst, const void* src, size_t n) {
    guarded_memcpy(dst, src, n);
    return true;
}
#endif
// (TICK-037) The capture's copies with streaming stores. The capture buffers (25 kept and the spares, 5-10 MB each) are never in
// the CPU's cache when a capture fills them, so a plain copy read every line of them from DRAM before writing it (~40 MB a frame of
// it, on the game's main thread, the other game threads mostly idle meanwhile): bandwidth, not threads, held the copy (four helpers
// or eight, the same time). Streaming stores skip that read: ~0.43 -> ~0.31 ms for a 10 MB capture on this machine (5 threads,
// the source just written by 6 others). What the buffers hold is the same; the stores are fenced before the capture is handed on.
// RX_VK_CAPTURE_STREAM=0: memcpy, as before
#if defined(__AVX2__)
__attribute__((noinline)) void* stream_memcpy(void* dst, const void* src, size_t n) {
    if (((uintptr_t)dst & 31) || (n & 127)) return std::memcpy(dst, src, n);
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t o = 0; o < n; o += 128) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + o));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + o + 32));
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + o + 64));
        const __m256i e = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + o + 96));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + o), a);
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + o + 32), b);
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + o + 64), c);
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + o + 96), e);
    }
    return dst;
}
void stream_fence() { _mm_sfence(); }
bool stream_on() {
    static const bool v = !getenv("RX_VK_CAPTURE_STREAM") || atoi(getenv("RX_VK_CAPTURE_STREAM")) != 0;
    return v;
}
#else
void* stream_memcpy(void* dst, const void* src, size_t n) { return std::memcpy(dst, src, n); }
void stream_fence() {}
bool stream_on() { return false; }
#endif
void* (*volatile guarded_stream_memcpy)(void*, const void*, size_t) = stream_memcpy;
#ifdef _WIN32
__declspec(noinline) bool guarded_copy_stream(void* dst, const void* src, size_t n) {   // (as guarded_copy)
    __try {
        guarded_stream_memcpy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
#else
bool guarded_copy_stream(void* dst, const void* src, size_t n) {
    guarded_stream_memcpy(dst, src, n);
    return true;
}
#endif
// ---- a capture's copies: on the submitting thread and two helpers, four from 12 CPU threads (RX_VK_CAPTURE_THREADS=<n>, 0-4): ~9 MB of the
// game's dynamic data a submit, four submits a frame, ~1.4 ms on the game's thread each with one ----
// (a page written and then unmapped before the submit faulted a plain copy: ReadProcessMemory fails on it instead.
// VirtualQuery per page took ~1 ms each in this address space: 22 ms a submit for 18 pages) (runs of adjacent pages
// in one copy each, page by page only where a run fails; page i into slot i)
struct ClaimJob;
struct CopyPart {
    const uint32_t* pages; int64_t i0, i1; uint8_t* out; uint8_t* ok;
    // (TICK-043, RX_VK_CAPTURE_SCAN_SPLIT: a part of the submit's scan of the write marks instead: these ranges, the pages into *res)
    const uint64_t (*ranges)[2] = nullptr; int nr = 0; uint32_t** res = nullptr; int64_t* res_n = nullptr;
    ClaimJob* claim = nullptr;   // (TICK-061, RX_VK_CAPTURE_CLAIM: a token for a copy whose chunks are claimed, see copy_pages)
    uint64_t* hash = nullptr; uint8_t* hok = nullptr;   // (TICK-060, RX_VK_CAPTURE_HASH: page i's 16 chunk hashes at hash + 16 i)
};
// (HeyListen TICK-060) RX_VK_CAPTURE_HASH: the capture's copy also makes the mirror syncs' chunk hashes (TICK-048's diff) of the
// bytes it copies, page by page in one pass (msync_diff::copy_hash_page: each 32-byte load stored and hashed from one register,
// so the hashes are those of the slot's bytes even with the game writing the page meanwhile). The diff hashed each synced page
// again on the Presentation Thread from the capture buffer, which the streaming stores had left in DRAM: on PC 3050 (i5-10400F)
// ~4 ms of the frame's ~5.6 ms mirror pre-sync, on the thread the frame waits for once RX_VK_LATE_DEFER takes the late-page wait
// off it (RX_VK_MSYNC_DIFF=0 there: 1.5 ms, with 30 MB more over the x4 link). The sync takes a slot's hashes when the copy made
// them (hok) and hashes it as before otherwise: a page the copy could not read whole, the pre-copy's (RX_VK_CAPTURE_PRECOPY), a
// late page copied again after the take (its slot's hashes dropped). Same hashes, same copies, same mirror. Only with the diff on
// (RX_VK_MSYNC_DIFF, by default on links of 8 GB/s or less) and AVX2 builds. On by default since the PC 3050 confirm (TICK-060; =0: off). tests/test_msync_diff.cpp
namespace { int diff_mode(); }   // (below, in this file's anonymous namespace)
bool capture_hash_on() {
#if defined(__AVX2__)
    static const bool v = [] {
        // (TICK-060: on by default after the PC 3050 confirm, wherever the diff is on; =0 off)
        const bool on = (!getenv("RX_VK_CAPTURE_HASH") || atoi(getenv("RX_VK_CAPTURE_HASH")) != 0) && diff_mode() != 0;
        if (on) logf("vk: the capture's copy makes the mirror syncs' chunk hashes (RX_VK_CAPTURE_HASH)");
        return on;
    }();
    return v;
#else
    return false;
#endif
}
#ifdef _WIN32
__declspec(noinline) bool guarded_copy_hash(uint8_t* dst, const uint8_t* src, uint64_t* h, bool nt) {   // (as guarded_copy)
    __try {
        msync_diff::copy_hash_page(dst, src, h, nt);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
#else
bool guarded_copy_hash(uint8_t* dst, const uint8_t* src, uint64_t* h, bool nt) {
    msync_diff::copy_hash_page(dst, src, h, nt);
    return true;
}
#endif
std::atomic<int64_t> st_hash_made{0}, st_hash_used{0}, st_hash_again{0};   // (TICK-060: slots hashed by the copy; syncs that used them, that hashed)
void copy_part(const CopyPart& w) {
    uint8_t* const base = (uint8_t*)host->guest_base();   // (guest addresses: no GPU translation per page)
    const uint64_t gsize = host->guest_size();
    const bool stream = stream_on();   // (TICK-037: streaming stores, fenced below)
    if (w.hash) {   // (TICK-060: page by page, each copied and hashed in one pass)
        int64_t made = 0;
        for (int64_t i = w.i0; i < w.i1; i++) {
            const uint64_t ga = (uint64_t)w.pages[i] << kPageShift;
            if (ga + 4096 > gsize) continue;
            if (guarded_copy_hash(w.out + (uint64_t)i * 4096, base + ga, w.hash + (uint64_t)i * msync_diff::kChunks, stream))
                w.ok[i] = 1, w.hok[i] = 1, made++;
            else
                w.ok[i] = guarded_copy(w.out + (uint64_t)i * 4096, base + ga, 4096);
        }
        if (stream) stream_fence();
        st_hash_made.fetch_add(made, std::memory_order_relaxed);
        return;
    }
    for (int64_t i = w.i0; i < w.i1;) {
        const uint64_t ga = (uint64_t)w.pages[i] << kPageShift;
        if (ga + 4096 > gsize) { i++; continue; }
        int64_t j = i + 1;
        while (j < w.i1 && w.pages[j] == w.pages[j - 1] + 1 && ((uint64_t)w.pages[j] << kPageShift) + 4096 <= gsize) j++;
        if (stream ? guarded_copy_stream(w.out + (uint64_t)i * 4096, base + ga, (SIZE_T)(j - i) * 4096)
                   : guarded_copy(w.out + (uint64_t)i * 4096, base + ga, (SIZE_T)(j - i) * 4096)) {
            std::memset(w.ok + i, 1, (size_t)(j - i));
        } else {
            for (int64_t q = i; q < j; q++)
                w.ok[q] = guarded_copy(w.out + (uint64_t)q * 4096, base + ((uint64_t)w.pages[q] << kPageShift), 4096);
        }
        i = j;
    }
    if (stream) stream_fence();   // (the streaming stores visible before the part is counted done)
}
struct CopyHelpers {
    std::mutex mu;
    std::condition_variable cv, done;
    std::deque<CopyPart> parts;
    int pending = 0;
    bool started = false;
} copy_helpers;
// ---- (TICK-061, C1) RX_VK_CAPTURE_CLAIM=1 (off by default): the capture's copy in chunks claimed by whoever is free ----
// The copy was cut into k = helpers + 1 equal parts; the submitter copied part 0 and then slept until the slowest helper was
// through. With the game's job group on every core (HOS_NVN_HOLD=2's held flush runs beside it) a helper was often scheduled
// late, and the copy ended with the last part to start (1.36 ms held against 0.6 ms on an idle machine, TICK-054). Here the
// pages are cut into chunks of `chunk` pages, claimed from one atomic counter by the submitter and every helper that wakes in
// time; the submitter claims until none is left, so a helper that never got a core leaves nothing undone.
// Same output, by construction: chunk [i0, i1) is copied by exactly one thread with copy_part, which writes page i into slot i
// (out + i * 4096) and ok[i], as before; a run of adjacent pages cut at a chunk's edge is two copies of the same bytes into the
// same slots (and a failing run falls back page by page within each chunk, so ok[] gets the same per-page answer). The copy reads
// guest memory as before: the order the threads read pages in changes, and the capture always allowed any order (it ran on 5
// threads already). Visibility: copy_part fences its streaming stores; a helper drops its use count under copy_helpers.mu after
// its last chunk, and the submitter waits on that mutex for the count to reach 0, so every store is visible to it when
// copy_pages returns, as with the pending count before.
// Lifetime: the job lives on the submitter's stack. A token not popped yet is removed by the submitter (under the mutex) before
// it waits; a token popped holds `users`, which the submitter waits for. No helper touches the job after copy_pages returns.
struct ClaimJob {
    const uint32_t* pages;
    int64_t n;
    uint8_t* out;
    uint8_t* ok;
    int64_t chunk;
    std::atomic<int64_t> next{0};
    int users = 0;   // (helpers that popped a token and have not finished: copy_helpers.mu)
    // (TICK-061, E3: other work cut the same way: fn(ctx, i0, i1) for each chunk instead of copy_part; null: the copy)
    void (*fn)(void* ctx, int64_t i0, int64_t i1) = nullptr;
    void* ctx = nullptr;
};
bool capture_claim_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_CAPTURE_CLAIM");
        const bool v = e && atoi(e) != 0;
        logf("vk: the capture's copy: %s (RX_VK_CAPTURE_CLAIM)", v ? "chunks claimed by the submitter and the helpers" : "equal parts, as before");
        return v;
    }();
    return on;
}
int64_t claim_chunk_pages(int64_t n, int participants) {
    // (RX_VK_CAPTURE_CLAIM=<n> >= 8: n pages a chunk; 1: about four chunks a participant, 16 to 256 pages (64 KB to 1 MB))
    static const int64_t fixed = getenv("RX_VK_CAPTURE_CLAIM") ? atoll(getenv("RX_VK_CAPTURE_CLAIM")) : 0;
    if (fixed >= 8) return fixed;
    return std::clamp<int64_t>(n / (4 * std::max(participants, 1)), 16, 256);
}
void claim_run(ClaimJob& j) {
    for (;;) {
        const int64_t i0 = j.next.fetch_add(j.chunk, std::memory_order_relaxed);
        if (i0 >= j.n) return;
        if (j.fn) j.fn(j.ctx, i0, std::min(i0 + j.chunk, j.n));
        else copy_part({j.pages, i0, std::min(i0 + j.chunk, j.n), j.out, j.ok});
    }
}
void copy_helpers_start_locked();
/// The job's chunks on this thread and `helpers` copy helpers (those that wake in time), until every chunk is done (see ClaimJob:
/// the tokens no helper took are removed, the ones taken waited for). One at a time: the captures' submit_order_mu, or pools_mu
void claim_all(ClaimJob& job, int helpers) {
    {
        std::lock_guard lk(copy_helpers.mu);
        copy_helpers_start_locked();
        for (int i = 0; i < helpers; i++) {
            CopyPart t{job.pages, 0, 0, job.out, job.ok};
            t.claim = &job;
            copy_helpers.parts.push_back(t);
        }
        copy_helpers.pending += helpers;
    }
    copy_helpers.cv.notify_all();
    claim_run(job);   // (returns once every chunk is claimed: by it or by a helper still on one)
    std::unique_lock lk(copy_helpers.mu);
    int left = 0;   // (tokens no helper took in time: nobody will, they go)
    for (auto it = copy_helpers.parts.begin(); it != copy_helpers.parts.end();)
        if (it->claim == &job) it = copy_helpers.parts.erase(it), left++;
        else ++it;
    copy_helpers.pending -= left;
    if (copy_helpers.pending == 0) copy_helpers.done.notify_all();
    copy_helpers.done.wait(lk, [&job] { return job.users == 0; });
}
std::atomic<int64_t> st_claim_copies{0}, st_claim_helped{0};   // (copies made with claiming; tokens a helper took in time)
void copy_helper_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk capture copy");
    // (TICK-054) RX_VK_CAPTURE_PRIO=1: above normal, as the replay's workers (a capture made while the game's job threads run)
    if (getenv("RX_VK_CAPTURE_PRIO") && atoi(getenv("RX_VK_CAPTURE_PRIO")) != 0) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        CopyPart w;
        {
            std::unique_lock lk(copy_helpers.mu);
            copy_helpers.cv.wait(lk, [] { return !copy_helpers.parts.empty(); });
            w = copy_helpers.parts.front();
            copy_helpers.parts.pop_front();
            if (w.claim) w.claim->users++;   // (TICK-061: the job stays alive until this helper is through)
        }
        if (w.claim) {
            st_claim_helped.fetch_add(1, std::memory_order_relaxed);
            claim_run(*w.claim);
        } else if (w.ranges) {
            *w.res_n = host->collect_written_in(w.ranges, w.nr, w.res);
        } else {
            copy_part(w);
        }
        std::lock_guard lk(copy_helpers.mu);
        if (w.claim && --w.claim->users == 0) copy_helpers.done.notify_all();
        if (--copy_helpers.pending == 0) copy_helpers.done.notify_all();
    }
}
int copy_helper_count() {
    static const int helpers = getenv("RX_VK_CAPTURE_THREADS")
        ? std::clamp(atoi(getenv("RX_VK_CAPTURE_THREADS")), 0, 4)
        : (getenv("RX_VK_LOW_SPEC") ? 0 : std::thread::hardware_concurrency() >= 12 ? 4 : 2);   // (four where the threads are spare)
    return helpers;
}
void copy_helpers_start_locked() {   // (copy_helpers.mu held)
    if (copy_helpers.started) return;
    copy_helpers.started = true;
    for (int i = 0; i < copy_helper_count(); i++) std::thread(copy_helper_main).detach();
}
void copy_pages(const uint32_t* pages, int64_t n, uint8_t* out, uint8_t* ok, uint64_t* hash = nullptr, uint8_t* hok = nullptr) {   // (one capture at a time: submit_order_mu)
    const auto part = [&](int64_t i0, int64_t i1) {
        CopyPart w{pages, i0, i1, out, ok};
        w.hash = hash, w.hok = hok;
        return w;
    };
    const int helpers = copy_helper_count();
    if (!helpers || n < 128) return copy_part(part(0, n));
    const int k = helpers + 1;
    if (capture_claim_on()) {   // (TICK-061, C1: see ClaimJob)
        ClaimJob job{pages, n, out, ok, claim_chunk_pages(n, k)};
        claim_all(job, helpers);
        st_claim_copies.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard lk(copy_helpers.mu);
        copy_helpers_start_locked();
        for (int i = 1; i < k; i++) copy_helpers.parts.push_back(part(n * i / k, n * (i + 1) / k));
        copy_helpers.pending += k - 1;
    }
    copy_helpers.cv.notify_all();
    copy_part(part(0, n / k));
    std::unique_lock lk(copy_helpers.mu);
    copy_helpers.done.wait(lk, [] { return copy_helpers.pending == 0; });
}

// (sprint 5, RX_VK_CAPTURE_GRAN) pages [i0, i1) copied by their granule masks: whole pages in runs as copy_part, the others
// run of granules by run, each into its place in the page's slot
struct GranCopy { const uint32_t* pages; uint8_t* masks; uint8_t* out; uint8_t* ok; };
void gran_copy_chunk(void* ctx, int64_t i0, int64_t i1) {
    const GranCopy& w = *(const GranCopy*)ctx;
    {   // (the chunk's granule marks taken and made the slots' masks here, on whichever thread copies it: ~9,000 cold marks a frame)
        gran_take_fn()(w.pages + i0, i1 - i0, w.masks + i0);
        int64_t full = 0, uni = 0;
        for (int64_t i = i0; i < i1; i++) {
            uint8_t m = w.masks[i];
            PageX* x = pagex(w.pages[i], true);
            if (!m || !x || x->full) m = 0xff;
            else if (const CapEntry e = cap_entry(w.pages[i]); e.gen && x->syn < e.gen) {   // (its last capture not yet synced)
                if ((m | x->gm) != m) uni++;
                m |= x->gm;
            }
            w.masks[i] = m, full += m == 0xff;
        }
        st_gran_pages.fetch_add(i1 - i0, std::memory_order_relaxed), st_gran_full.fetch_add(full, std::memory_order_relaxed);
        st_gran_union.fetch_add(uni, std::memory_order_relaxed);
    }
    uint8_t* const base = (uint8_t*)host->guest_base();
    const uint64_t gsize = host->guest_size();
    const bool stream = stream_on();
    int64_t bytes = 0;
    for (int64_t i = i0; i < i1;) {
        const uint64_t ga = (uint64_t)w.pages[i] << kPageShift;
        if (ga + 4096 > gsize) { i++; continue; }
        if (w.masks[i] == 0xff) {
            int64_t j = i + 1;
            while (j < i1 && w.masks[j] == 0xff && w.pages[j] == w.pages[j - 1] + 1 && ((uint64_t)w.pages[j] << kPageShift) + 4096 <= gsize) j++;
            if (stream ? guarded_copy_stream(w.out + (uint64_t)i * 4096, base + ga, (SIZE_T)(j - i) * 4096)
                       : guarded_copy(w.out + (uint64_t)i * 4096, base + ga, (SIZE_T)(j - i) * 4096)) {
                std::memset(w.ok + i, 1, (size_t)(j - i));
            } else {
                for (int64_t q = i; q < j; q++)
                    w.ok[q] = guarded_copy(w.out + (uint64_t)q * 4096, base + ((uint64_t)w.pages[q] << kPageShift), 4096);
            }
            bytes += (j - i) * 4096;
            i = j;
            continue;
        }
        bool ok = true;
        const unsigned m = w.masks[i];
        for (unsigned k = 0; k < 8;) {
            if (!(m >> k & 1)) { k++; continue; }
            unsigned e = k + 1;
            while (e < 8 && (m >> e & 1)) e++;
            const uint64_t off = (uint64_t)k * 512, len = (uint64_t)(e - k) * 512;
            ok &= stream ? guarded_copy_stream(w.out + (uint64_t)i * 4096 + off, base + ga + off, (SIZE_T)len)
                         : guarded_copy(w.out + (uint64_t)i * 4096 + off, base + ga + off, (SIZE_T)len);
            bytes += (int64_t)len;
            k = e;
        }
        w.ok[i] = ok ? 1 : 0;
        i++;
    }
    if (stream) stream_fence();
    st_gran_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
std::unique_ptr<Capture> capture_make();
void shadow_written(const uint32_t* pages, int64_t n);   // (the write-back's diff, below: pages the CPU wrote)
void shadow_free(Pool& p);
void memory_capture_submit() {
    std::unique_ptr<Capture> c = capture_make();
    if (!c) return;
    std::lock_guard lk(cap_mu);
    captures.push_back(std::move(c));
}
// ---- (TICK-043) the submit's scan of the write marks on the copy helpers: RX_VK_CAPTURE_SCAN_SPLIT=1 ----
// The scan (collect_in over ~1,500 ranges, ~3.4 GB of guest memory: ~830 KB of marks) is ~0.07 ms a submit on the submitting
// thread, six submits a frame. Split by pages between it and the copy helpers; the parts' pages joined in range order (the
// same list, in the same order, as one scan).
bool scan_split_on() {
    static const bool v = getenv("RX_VK_CAPTURE_SCAN_SPLIT") && atoi(getenv("RX_VK_CAPTURE_SCAN_SPLIT")) != 0;
    return v;
}
std::vector<uint32_t>& collect_split(const std::vector<std::pair<uint64_t, uint64_t>>& r) {
    static thread_local std::vector<uint32_t> all;
    all.clear();
    const int helpers = copy_helper_count();
    const auto* rr = reinterpret_cast<const uint64_t(*)[2]>(r.data());
    if (!helpers || r.size() < 32) {
        uint32_t* p = nullptr;
        const int64_t n = host->collect_written_in(rr, (int)r.size(), &p);
        if (n > 0) all.assign(p, p + n);
        if (p) host->free_pages(p);
        return all;
    }
    const int k = std::min(helpers + 1, 16);
    uint64_t total = 0;
    for (const auto& x : r) total += x.second - x.first;
    int cut[17];
    cut[0] = 0, cut[k] = (int)r.size();
    {
        uint64_t acc = 0;
        int part = 1;
        for (int i = 0; i < (int)r.size() && part < k; i++) {
            acc += r[i].second - r[i].first;
            while (part < k && acc >= total * (uint64_t)part / (uint64_t)k) cut[part++] = i + 1;
        }
        while (part < k) cut[part++] = (int)r.size();
    }
    uint32_t* res[16] = {};
    int64_t res_n[16] = {};
    {
        std::lock_guard lk(copy_helpers.mu);
        copy_helpers_start_locked();
        for (int i = 1; i < k; i++) {
            CopyPart w{nullptr, 0, 0, nullptr, nullptr};
            w.ranges = rr + cut[i], w.nr = cut[i + 1] - cut[i], w.res = &res[i], w.res_n = &res_n[i];
            copy_helpers.parts.push_back(w);
        }
        copy_helpers.pending += k - 1;
    }
    copy_helpers.cv.notify_all();
    res_n[0] = host->collect_written_in(rr, cut[1], &res[0]);
    {
        std::unique_lock lk(copy_helpers.mu);
        copy_helpers.done.wait(lk, [] { return copy_helpers.pending == 0; });
    }
    for (int i = 0; i < k; i++) {
        if (res_n[i] > 0) all.insert(all.end(), res[i], res[i] + res_n[i]);
        if (res[i]) host->free_pages(res[i]);
    }
    return all;
}
// ---- (TICK-043) the capture copied ahead: RX_VK_CAPTURE_PRECOPY=1 ----
// The capture copies the pages the game wrote since the last submit, on the submitting thread (the game's main thread, alone
// then: TICK-042). Most of them are written well before the submit, while the main thread waits for the game's job threads.
// A thread of its own (every RX_VK_PRECOPY_US, default 250) takes the pages written so far in the last capture's ranges (it
// clears their marks: collect_in) and copies them into the next capture's buffer, while the game runs. The submit then copies
// only what was written since, and again what was written again (capture_merge.h has the rule and why the result is the
// pages as they are at the submit). The thread stops at its next page when a capture is made.
bool precopy_on() {
    static const bool v = [] {
        const char* e = getenv("RX_VK_CAPTURE_PRECOPY");
        // (sprint 5: on by default from the host's environment, host/hos_main.c; not where the mirror syncs' chunk diff is on (slow
        // links: PC 3050), whose hashes the capture's own copy makes: the pre-copy's slots have none)
        const bool on = e && atoi(e) != 0 && capture_on() && !(getenv("RX_VK_CAPTURE_DEFER") && atoi(getenv("RX_VK_CAPTURE_DEFER")) != 0) &&
                        !diff_mode();
        if (on) logf("vk: the submit-time capture is copied ahead on a thread of its own (RX_VK_CAPTURE_PRECOPY)");
        else if (e && atoi(e) != 0 && diff_mode()) logf("vk: the capture is not copied ahead: the mirror syncs' chunk diff is on (RX_VK_CAPTURE_PRECOPY)");
        return on;
    }();
    return v;
}
int precopy_period_us() {
    static const int v = getenv("RX_VK_PRECOPY_US") ? std::clamp(atoi(getenv("RX_VK_PRECOPY_US")), 50, 100000) : 250;
    return v;
}
struct PreCopy {
    std::mutex mu;                   // (the copy below; the thread holds it while it copies, a capture takes it)
    std::atomic<bool> stop{false};   // (a capture is waiting for mu: the thread stops at its next page)
    std::vector<uint32_t> pages;     // (copied, page i in slot i)
    std::vector<uint64_t> done;      // (what the GPU had done when each one's copy began)
    std::vector<uint8_t> ok;
    std::unordered_map<uint32_t, uint32_t> slot_of;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t bsize = 0;
    uint64_t want = 4ull << 20;      // (the next buffer's size: the recent captures', and room)
    bool full = false;
    int64_t st_again = 0, st_left = 0, st_runs = 0;   // (pages copied again by the thread, given back as the buffer was full; its rounds)
    std::mutex ranges_mu;            // (the last capture's ranges, for the thread)
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    uint64_t ranges_ver = 0;
    bool started = false;            // (mu)
    uint64_t gen = 0;                // (mu: the captures that took the copies)
    int64_t st_grown = 0;
} pre;
struct PreTaken {
    std::vector<uint32_t> pages;
    std::vector<uint64_t> done;
    std::vector<uint8_t> ok;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t bsize = 0;
};
/// (mu held, the thread's) The buffer twice as large, what it holds copied over: false when it cannot be, or a capture took the
/// copies while the new one was made (mu let go meanwhile)
bool precopy_grow(std::unique_lock<std::mutex>& lk) {
    const uint64_t gen = pre.gen, want = std::min<uint64_t>(pre.bsize * 2, 64ull << 20);
    if (want <= pre.bsize) return false;
    lk.unlock();
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    void* m = nullptr;
    uint64_t sz = 0;
    const bool got = cap_buf_get(want, b, a, m, sz);
    lk.lock();
    if (!got) {
        if (b) destroy_buffer(b, a);
        return false;
    }
    if (pre.gen != gen || !pre.map || sz <= pre.bsize) {
        cap_buf_free(b, a, m, sz);
        return false;
    }
    for (size_t o = 0; o < pre.pages.size(); o += 256) {   // (a capture waiting: given up)
        if (pre.stop.load(std::memory_order_relaxed)) {
            cap_buf_free(b, a, m, sz);
            return false;
        }
        const size_t k = std::min<size_t>(256, pre.pages.size() - o);
        stream_memcpy((uint8_t*)m + o * 4096, (const uint8_t*)pre.map + o * 4096, k * 4096);
    }
    stream_fence();
    cap_buf_free(pre.buffer, pre.alloc, pre.map, pre.bsize);
    pre.buffer = b, pre.alloc = a, pre.map = m, pre.bsize = sz;
    pre.st_grown++;
    return true;
}
// (HeyListen, TICK-054) RX_VK_PRECOPY_KICK=1 (with RX_VK_CAPTURE_PRECOPY=1): the thread copies once per kick from the host
// (mkwvk_precopy_kick, called when the game's main thread leaves its end-of-frame wait: the job threads' writes of the
// frame are final then and the main thread's draw code and submits are still 2-4 ms away), not on the 250 us timer whose
// rounds during the job threads' work cost more than they saved (TICK-043). The copies then run while the job threads idle.
bool precopy_kick_on() {
    static const bool v = getenv("RX_VK_PRECOPY_KICK") && atoi(getenv("RX_VK_PRECOPY_KICK")) != 0;
    return v;
}
// (HeyListen, sprint 5) RX_VK_PRECOPY_KICK=2 (with RX_VK_CAPTURE_PRECOPY=1): one round a frame, kicked by the renderer when a
// present begins (memory_precopy_present), copied by the capture's own helpers (copy_pages: runs of pages, every helper), into a
// buffer sized for what was collected. Why there: with the held flush early (HOS_NVN_HOLD_LINGER_US) the frame's first capture
// (the pages the game wrote during the whole frame: 30-40 MB, 1.6-2 ms) is made on the Presentation Thread at the start of the
// render chain, which the frame waits for; when the previous present begins, the game's job group and module phase of that next
// frame are nearly over, the replay's workers and the copy helpers idle, and the present takes 0.6-1.5 ms. The capture at the
// submit then copies what was written since and what was written again (capture_merge.h: the same pages as at the submit).
// kick 1's single-thread page-by-page rounds copied 100-300 pages ahead of ~7,500 (TICK-054).
int precopy_kick_mode() {
    static const int v = getenv("RX_VK_PRECOPY_KICK") ? atoi(getenv("RX_VK_PRECOPY_KICK")) : 0;
    return v;
}
std::atomic<int64_t> st_pre2_rounds{0}, st_pre2_pages{0}, st_pre2_skipped{0}, st_pre2_ns{0};
HANDLE precopy_kick_event = nullptr;
std::atomic<int64_t> st_precopy_kicks{0};
void precopy_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk capture pre-copy");
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    uint64_t ver = 0;
    uint8_t* const base = (uint8_t*)host->guest_base();
    const uint64_t gsize = host->guest_size();
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)precopy_period_us() * 10;
        if (precopy_kick_on() && precopy_kick_event) WaitForSingleObject(precopy_kick_event, INFINITE);   // (TICK-054: a kick a frame)
        else if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        if (pre.stop.load(std::memory_order_relaxed)) continue;
        {
            std::lock_guard rk(pre.ranges_mu);
            if (pre.ranges_ver != ver) ranges = pre.ranges, ver = pre.ranges_ver;
        }
        if (ranges.empty()) continue;
        std::unique_lock lk(pre.mu);
        if (pre.full || pre.stop.load(std::memory_order_relaxed)) continue;
        if (precopy_kick_mode() >= 2) {   // (sprint 5: one round on the capture's helpers; see precopy_kick_mode)
            if (!pre.pages.empty()) { st_pre2_skipped++; continue; }   // (the last round's copies not taken yet)
            const auto r0 = std::chrono::steady_clock::now();
            const uint64_t done = vk_timeline_completed();   // (before the marks are cleared, as below)
            uint32_t* pages = nullptr;
            const int64_t n = host->collect_written_in(reinterpret_cast<const uint64_t(*)[2]>(ranges.data()), (int)ranges.size(), &pages);
            bool taken = false;
            if (n > 0 && (uint64_t)n * 4096 <= (96ull << 20)) {
                const uint64_t need = (uint64_t)n * 4096 * 5 / 4 + (4ull << 20);   // (room for the pages written after)
                if (pre.map && pre.bsize < need) cap_buf_free(pre.buffer, pre.alloc, pre.map, pre.bsize), pre.buffer = VK_NULL_HANDLE, pre.map = nullptr, pre.bsize = 0;
                if (!pre.map && !cap_buf_get(need, pre.buffer, pre.alloc, pre.map, pre.bsize)) {
                    if (pre.buffer) destroy_buffer(pre.buffer, pre.alloc);
                    pre.buffer = VK_NULL_HANDLE, pre.map = nullptr, pre.bsize = 0;
                }
                if (pre.map) {
                    pre.ok.assign((size_t)n, 0);
                    copy_pages(pages, n, (uint8_t*)pre.map, pre.ok.data(), nullptr, nullptr);
                    pre.pages.assign(pages, pages + n);
                    pre.done.assign((size_t)n, done);
                    taken = true;
                    st_pre2_rounds++, st_pre2_pages += n;
                }
            }
            if (!taken)
                for (int64_t i = 0; i < n; i++) host->mark_written(base + ((uint64_t)pages[i] << kPageShift), 4096), pre.st_left++;
            if (pages) host->free_pages(pages);
            st_pre2_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - r0).count();
            continue;
        }
        if (!pre.map) {   // (the next capture's buffer, made here: not on the submitting thread)
            const uint64_t want = pre.want;
            lk.unlock();
            VkBuffer b = VK_NULL_HANDLE;
            VmaAllocation a = nullptr;
            void* m = nullptr;
            uint64_t sz = 0;
            const bool got = cap_buf_get(want, b, a, m, sz);
            lk.lock();
            if (!got) {
                if (b) destroy_buffer(b, a);
                continue;
            }
            pre.buffer = b, pre.alloc = a, pre.map = m, pre.bsize = sz;
            if (pre.stop.load(std::memory_order_relaxed)) continue;
        }
        const uint64_t done = vk_timeline_completed();   // (before the marks are cleared: a write-back after it is copied again)
        uint32_t* pages = nullptr;
        const int64_t n = host->collect_written_in(reinterpret_cast<const uint64_t(*)[2]>(ranges.data()), (int)ranges.size(), &pages);
        pre.st_runs++;
        uint64_t slots = pre.bsize / 4096 * 3 / 4;   // (a quarter left for the pages written after, at the submit)
        const bool stream = stream_on();
        int64_t i = 0;
        for (; i < n; i++) {
            if (pre.stop.load(std::memory_order_relaxed)) break;
            const uint32_t p = pages[i];
            uint32_t slot;
            if (const auto it = pre.slot_of.find(p); it != pre.slot_of.end()) {
                slot = it->second;
                pre.st_again++;
            } else {
                if (pre.pages.size() >= slots) {
                    const uint64_t gen = pre.gen;
                    if (!precopy_grow(lk)) {   // (the rest given back below, this page with them)
                        if (pre.gen == gen) pre.full = true;
                        break;
                    }
                    slots = pre.bsize / 4096 * 3 / 4;
                }
                slot = (uint32_t)pre.pages.size();
                pre.pages.push_back(p), pre.done.push_back(0), pre.ok.push_back(0);
                pre.slot_of.emplace(p, slot);
            }
            const uint64_t ga = (uint64_t)p << kPageShift;
            uint8_t* dst = (uint8_t*)pre.map + (uint64_t)slot * 4096;
            bool ok = false;
            if (ga + 4096 <= gsize) {
                ok = stream ? guarded_copy_stream(dst, base + ga, 4096) : guarded_copy(dst, base + ga, 4096);
                if (!ok && stream) ok = guarded_copy(dst, base + ga, 4096);
            }
            pre.done[slot] = done, pre.ok[slot] = ok ? 1 : 0;
        }
        for (; i < n; i++) {   // (not copied: given back to the marks, for the submit's scan)
            host->mark_written(base + ((uint64_t)pages[i] << kPageShift), 4096);
            pre.st_left++;
        }
        if (stream) stream_fence();   // (the copies visible before the capture that takes them)
        if (pages) host->free_pages(pages);
    }
}
/// The pages copied so far and their buffer, for the capture being made (the thread stopped at its next page)
void precopy_take(PreTaken& t) {
    pre.stop.store(true, std::memory_order_relaxed);
    std::lock_guard lk(pre.mu);
    if (!pre.started) {
        pre.started = true;
        if (precopy_kick_on()) precopy_kick_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread(precopy_main).detach();
    }
    t.pages.swap(pre.pages), t.done.swap(pre.done), t.ok.swap(pre.ok);
    pre.pages.clear(), pre.done.clear(), pre.ok.clear(), pre.slot_of.clear();
    t.buffer = pre.buffer, t.alloc = pre.alloc, t.map = pre.map, t.bsize = pre.bsize;
    pre.buffer = VK_NULL_HANDLE, pre.alloc = nullptr, pre.map = nullptr, pre.bsize = 0;
    pre.full = false;
    pre.gen++;
    pre.stop.store(false, std::memory_order_relaxed);
}
/// (the next buffer: 1.5x the mean of the last 16 captures, 2-32 MB. Not their largest: one large capture in 16 made every buffer
/// 40-60 MB, and the 25 kept ones went past the captures' byte budget (821 MB in 17). A capture larger than the room is copied at
/// its submit, as without the thread)
void precopy_size(int64_t n) {
    static int64_t last[16];
    static int at;
    last[at++ & 15] = n;
    int64_t m = 0;
    for (int64_t x : last) m += x;
    m /= 16;
    const uint64_t want = std::min<uint64_t>(std::max<uint64_t>(2ull << 20, (uint64_t)m * 4096 * 3 / 2), 32ull << 20);
    std::lock_guard lk(pre.mu);
    pre.want = want;
}
void precopy_ranges(const std::vector<std::pair<uint64_t, uint64_t>>& r) {
    std::lock_guard lk(pre.ranges_mu);
    pre.ranges = r;
    pre.ranges_ver++;
}
/// (the copied-ahead and the collected pages into one list, each once, in the order of the pages: the capture as without the thread)
void precopy_union(const PreTaken& t, const uint32_t* fresh, int64_t n, std::vector<uint32_t>& out) {
    out.assign(t.pages.begin(), t.pages.end());
    out.insert(out.end(), fresh, fresh + std::max<int64_t>(n, 0));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}
/// Copies pages[i] into slot slots[i] of out, ok[slots[i]] set (runs of adjacent pages and slots in one copy)
void copy_slots(const uint32_t* pages, const uint32_t* slots, size_t n, uint8_t* out, uint8_t* ok) {
    uint8_t* const base = (uint8_t*)host->guest_base();
    const uint64_t gsize = host->guest_size();
    const bool stream = stream_on();
    for (size_t i = 0; i < n;) {
        const uint64_t ga = (uint64_t)pages[i] << kPageShift;
        if (ga + 4096 > gsize) { ok[slots[i]] = 0; i++; continue; }
        size_t j = i + 1;
        while (j < n && pages[j] == pages[j - 1] + 1 && slots[j] == slots[j - 1] + 1 && ((uint64_t)pages[j] << kPageShift) + 4096 <= gsize) j++;
        uint8_t* dst = out + (uint64_t)slots[i] * 4096;
        if (stream ? guarded_copy_stream(dst, base + ga, (SIZE_T)(j - i) * 4096) : guarded_copy(dst, base + ga, (SIZE_T)(j - i) * 4096)) {
            for (size_t q = i; q < j; q++) ok[slots[q]] = 1;
        } else {
            for (size_t q = i; q < j; q++)
                ok[slots[q]] = guarded_copy(out + (uint64_t)slots[q] * 4096, base + ((uint64_t)pages[q] << kPageShift), 4096);
        }
        i = j;
    }
    if (stream) stream_fence();
}
/// (the pages the CPU wrote since the last capture, copied now: a submit's capture, or memory_capture_now's)
std::unique_ptr<Capture> capture_make() {
    if (!capture_on()) return nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    static thread_local PreTaken pt;   // (TICK-043: the pages copied ahead, taken first)
    const bool pre_on = precopy_on();
    if (pre_on) precopy_take(pt);
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (!spare.empty()) c = std::move(spare.back()), spare.pop_back();
    }
    if (!c) c = std::make_unique<Capture>();
    c->pages.clear(), c->copied.clear(), c->slots.clear(), c->nbytes = 0, c->deferred.clear(), c->late.clear(), c->late_value = 0;
    c->hok.clear();   // (TICK-060: no slot hashes unless the copy below makes them)
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    // (only the pools mirrored in VRAM, the ones the mirrors are filled from, and only their part of the write marks:
    // every written page was taken at first (hundreds of MB a submit where the game streams its world), and the whole
    // collection, over every GPU range, is ~20 ms: 4 of them a frame on the game's thread made it crawl. The replay's
    // collection takes the rest, as before)
    // (the list the replay publishes when it makes mirrors: pools_mu here waited ~40 ms a submit for the replay)
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> mirrored;
    bool ranges_new = false;
    {   // (and the texture storage the replay uploads from: see "texture storage in the capture"; and the small pools)
        std::lock_guard lk(cap_ranges_mu);
        captures_made++;
        refresh_pool_ranges_locked();
        static thread_local uint64_t ver = 0;
        if (ver != cap_ranges_ver) {   // (the three lists overlap: their union, sorted, once each time one changes)
            ver = cap_ranges_ver;
            ranges_new = true;
            std::vector<std::pair<uint64_t, uint64_t>> all = cap_ranges;
            all.insert(all.end(), tex_cap_ranges.begin(), tex_cap_ranges.end());
            all.insert(all.end(), pool_cap_ranges.begin(), pool_cap_ranges.end());
            std::sort(all.begin(), all.end());
            mirrored.clear();
            for (const auto& r : all) {
                if (!mirrored.empty() && r.first <= mirrored.back().second)
                    mirrored.back().second = std::max(mirrored.back().second, r.second);
                else
                    mirrored.push_back(r);
            }
        }
    }
    if (pre_on && ranges_new) precopy_ranges(mirrored);
    captures_seen.store(true, std::memory_order_relaxed);
    const auto t_lock = std::chrono::steady_clock::now();
    uint32_t* host_pages = nullptr;   // (the host's list, freed below)
    const uint32_t* pages = nullptr;
    int64_t n = 0;
    if (!mirrored.empty() && host->collect_written_in) {
        if (scan_split_on()) {
            const std::vector<uint32_t>& v = collect_split(mirrored);
            pages = v.data(), n = (int64_t)v.size();
        } else {
            n = host->collect_written_in(reinterpret_cast<const uint64_t(*)[2]>(mirrored.data()), (int)mirrored.size(), &host_pages);
            pages = host_pages;
        }
    }
    const auto t_scan = std::chrono::steady_clock::now();
    // (TICK-043) the copied-ahead pages and these into one list (capture_merge.h): the new ones after the copied ones; or, when a
    // copied page is outside the ranges now or the buffer has no room, every page copied now as without the thread
    static thread_local capture_merge::Merged mg;
    static thread_local std::vector<uint32_t> all_pages;
    static int64_t st_pre_k, st_pre_new, st_pre_again, st_pre_fallback, st_pre_out;
    bool use_pre = false;
    if (pre_on) {
        if (!pt.pages.empty()) {
            const auto inside = [](uint64_t ga) {
                auto it = std::upper_bound(mirrored.begin(), mirrored.end(), std::make_pair(ga, ~uint64_t(0)));
                return it != mirrored.begin() && ga < std::prev(it)->second;
            };
            bool all_inside = true;
            for (uint32_t p : pt.pages)
                if (!inside((uint64_t)p << kPageShift)) { all_inside = false; break; }
            if (all_inside) {
                {
                    std::lock_guard lk(wb_pages_mu);
                    capture_merge::merge(pt.pages.data(), pt.done.data(), pt.pages.size(), pages, (size_t)std::max<int64_t>(n, 0),
                                         [](uint32_t p) { return wb_pages.get(p); }, mg);
                }
                use_pre = pt.map && (uint64_t)mg.pages.size() * 4096 <= pt.bsize;
            }
            if (!use_pre) {
                st_pre_fallback++, st_pre_out += !all_inside;
                std::vector<uint32_t> keep;   // (a copied page outside the ranges now: back to the marks, as if never taken)
                for (uint32_t p : pt.pages) {
                    const uint64_t ga = (uint64_t)p << kPageShift;
                    if (!inside(ga)) host->mark_written((uint8_t*)host->guest_base() + ga, 4096);
                    else keep.push_back(p);
                }
                pt.pages.swap(keep);
                precopy_union(pt, pages, n, all_pages);
                pages = all_pages.data(), n = (int64_t)all_pages.size();
            } else {
                pages = mg.pages.data(), n = (int64_t)mg.pages.size();
                st_pre_k += (int64_t)mg.first_new, st_pre_new += n - (int64_t)mg.first_new, st_pre_again += (int64_t)mg.recopy.size();
            }
        }
        if (!use_pre && pt.map) cap_buf_free(pt.buffer, pt.alloc, pt.map, pt.bsize), pt.buffer = VK_NULL_HANDLE, pt.map = nullptr, pt.bsize = 0;
    }
    // (hookshot) A page the CPU wrote that a submission still on the GPU writes back: the copy waits for that write-back.
    // OoT's exposure meter page is written by both: the GPU adds the frame's light into it and the CPU sets the sums back to
    // 0 and fills in the rest. The CPU runs a frame ahead, and a copy taken before the write-back had landed had the GPU's
    // words from the frame before; the mirror sync of the page put them back over the GPU's newer ones, and the exposure
    // stepped for a frame (one-frame flashes, 40-120 a minute in Kokiri Forest). Only the copies of such pages wait, for the
    // last submission writing them back: not every frame's present. That wait held the game's thread (~8% of its time at a
    // ~2150-draw scene), so those pages are copied again by the replay, before it uses this capture, once the submission is
    // done: the replay waits instead, while the game goes on. RX_VK_CAPTURE_WAIT=1: the game's thread waits, 0: no wait
    // (HeyListen, TICK-017) RX_VK_CAPTURE_DEFER=1: such a page is left out of the capture and copied by the submission's mirror
    // sync from guest memory on the GPU instead (an experiment, off by default: slower, TICK-017 in docs/perf-audit.md);
    // RX_VK_CAPTURE_SLACK=<k>: with it, the game's thread still waits for the submission k before (pacing only)
    std::vector<uint32_t>& deferred = c->deferred;
    if (n > 0) {
        static const int cap_wait = getenv("RX_VK_CAPTURE_WAIT") ? atoi(getenv("RX_VK_CAPTURE_WAIT")) : 2;
        static const bool defer = getenv("RX_VK_CAPTURE_DEFER") && atoi(getenv("RX_VK_CAPTURE_DEFER")) != 0;   // (off: see TICK-017)
        uint64_t v = 0;
        if (cap_wait) {
            const uint64_t done = vk_timeline_completed();
            std::lock_guard lk(wb_pages_mu);
            if (!wb_pages.empty())
                for (int64_t i = 0; i < n; i++) {
                    const uint64_t pv = wb_pages.get(pages[i]);
                    if (pv > done) {
                        v = std::max(v, pv);
                        if (defer) deferred.push_back(pages[i]);
                        else if (cap_wait == 2) c->late.push_back((uint32_t)i);
                    }
                }
        }
        static const uint64_t slack = getenv("RX_VK_CAPTURE_SLACK") ? atoll(getenv("RX_VK_CAPTURE_SLACK")) : 2;
        if (v && defer) {
            if (v > slack) vk_timeline_wait(v - slack);   // (pacing only: see TICK-017 in docs/perf-audit.md)
        } else if (v && cap_wait == 2) c->late_value = v;
        else if (v) vk_timeline_wait(v);
    }
    if (n > 0) shadow_written(pages, n);
    {   // (sprint 5, RX_VK_GRAN_DIAG=1, with a game built with A64_GRAN=1: the 512-byte granules written of each captured page)
        static const bool on = getenv("RX_VK_GRAN_DIAG") != nullptr;
        typedef void (*GranTake)(const uint32_t*, int64_t, uint8_t*);
        static const GranTake take = on ? (GranTake)(void*)GetProcAddress(GetModuleHandleA(nullptr), "hos_gran_take") : nullptr;
        if (take && n > 0) {
            static std::mutex mu;
            static int64_t caps, pages_n, gran_n, hist[9];
            static std::vector<uint8_t> masks;
            std::lock_guard lk(mu);
            masks.assign((size_t)n, 0);
            take(pages, n, masks.data());
            for (int64_t i = 0; i < n; i++) {
                const int k = __builtin_popcount(masks[i]);
                hist[k]++, pages_n++, gran_n += k ? k : 8;
            }
            if (++caps % 600 == 0) {
                char line[256];
                int m = 0;
                for (int k = 0; k <= 8; k++) m += snprintf(line + m, sizeof line - m, " %d:%.0f", k, hist[k] / 100.0), hist[k] = 0;
                logf("vk: granules (a frame of 6 captures): %.0f pages, %.0f granules written of %.0f (%.1f%%; a page with none counts whole); "
                     "pages by granules written:%s", pages_n / 100.0, gran_n / 100.0, pages_n * 8 / 100.0, 100.0 * gran_n / (pages_n * 8.0), line);
                pages_n = gran_n = 0;
            }
        }
    }
    {   // (sprint 5, RX_VK_CAPTURE_WHERE=1: the captures' pages by 16 MB of guest addresses, every 600 captures)
        static const bool on = getenv("RX_VK_CAPTURE_WHERE") != nullptr;
        if (on) {
            static std::mutex mu;
            static std::unordered_map<uint64_t, int64_t> by;
            static int64_t caps, total;
            std::lock_guard lk(mu);
            for (int64_t i = 0; i < n; i++) by[(uint64_t)pages[i] >> 12]++;
            total += n;
            if (++caps % 600 == 0) {
                std::vector<std::pair<int64_t, uint64_t>> v;
                for (const auto& kv : by) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                std::string line;
                char t[64];
                for (size_t k = 0; k < v.size() && k < 24; k++)
                    snprintf(t, sizeof t, " %#llx:%.0f", (unsigned long long)v[k].second << 24, v[k].first / 100.0), line += t;
                logf("vk: capture where (pages a frame of 6 captures, by 16 MB; %.0f in all, %zu chunks):%s", total / 100.0, v.size(), line.c_str());
                by.clear(), total = 0;
            }
        }
    }
    if (n > 0 && ubo_check_on()) ubo_check_captured(pages, n);   // (HL_UBO_CHECK: the frame's captured pages, by what reads them)
    const auto t_copy = std::chrono::steady_clock::now();
    if (use_pre) {   // (TICK-043: the buffer copied ahead; the new pages after its pages, the ones written again into their slots)
        c->pages.assign(pages, pages + n);
        c->buffer = pt.buffer, c->alloc = pt.alloc, c->map = pt.map, c->bsize = pt.bsize;
        pt.buffer = VK_NULL_HANDLE, pt.alloc = nullptr, pt.map = nullptr, pt.bsize = 0;
        static thread_local std::vector<uint8_t> ok;
        ok.assign((size_t)n, 0);
        std::memcpy(ok.data(), pt.ok.data(), mg.first_new);
        const int64_t fresh_n = n - (int64_t)mg.first_new;
        if (fresh_n > 0) copy_pages(pages + mg.first_new, fresh_n, (uint8_t*)c->map + (uint64_t)mg.first_new * 4096, ok.data() + mg.first_new);
        if (!mg.recopy.empty()) {
            static thread_local std::vector<uint32_t> rp;
            rp.clear();
            for (uint32_t s : mg.recopy) rp.push_back(pages[s]);
            copy_slots(rp.data(), mg.recopy.data(), rp.size(), (uint8_t*)c->map, ok.data());
        }
        for (int64_t i = 0; i < n; i++)
            if (ok[i]) c->copied.push_back(pages[i]), c->slots.push_back((uint32_t)i);
        c->nbytes = (size_t)n * 4096;
        flush_mapped(c->alloc, c->nbytes);
    } else if (n > 0) {
        c->pages.assign(pages, pages + n);
        c->gmask.clear();
        const bool gran = capture_gran_on();
        if (gran) c->gmask.assign((size_t)n, 0);   // (sprint 5: made by the copy's chunks, gran_copy_chunk)
        if (gran && (uint64_t)n * 4096 <= (256ull << 20) && cap_buf_get((uint64_t)n * 4096, c->buffer, c->alloc, c->map, c->bsize)) {
            static thread_local std::vector<uint8_t> ok;
            ok.assign((size_t)n, 0);
            GranCopy w{pages, c->gmask.data(), (uint8_t*)c->map, ok.data()};
            const int helpers = copy_helper_count();
            if (!helpers || n < 128) gran_copy_chunk(&w, 0, n);
            else {
                ClaimJob job{nullptr, n, nullptr, nullptr, claim_chunk_pages(n, helpers + 1)};
                job.fn = gran_copy_chunk, job.ctx = &w;
                claim_all(job, helpers);
            }
            for (int64_t i = 0; i < n; i++)
                if (ok[i] && (deferred.empty() || std::find(deferred.begin(), deferred.end(), pages[i]) == deferred.end()))
                    c->copied.push_back(pages[i]), c->slots.push_back((uint32_t)i);
            c->nbytes = (size_t)n * 4096;
            flush_mapped(c->alloc, c->nbytes);
        } else if (gran) {
            c->gmask.clear();   // (no buffer: no slots)
        } else if ((uint64_t)n * 4096 <= (256ull << 20) &&   // (a load writes more: those are read later, as before)
            cap_buf_get((uint64_t)n * 4096, c->buffer, c->alloc, c->map, c->bsize)) {
            static thread_local std::vector<uint8_t> ok;
            ok.assign((size_t)n, 0);
            if (capture_hash_on()) {   // (TICK-060)
                c->hashes.resize((size_t)n * msync_diff::kChunks);
                c->hok.assign((size_t)n, 0);
                copy_pages(pages, n, (uint8_t*)c->map, ok.data(), c->hashes.data(), c->hok.data());
            } else {
                copy_pages(pages, n, (uint8_t*)c->map, ok.data());
            }
            for (int64_t i = 0; i < n; i++)
                if (ok[i] && (deferred.empty() || std::find(deferred.begin(), deferred.end(), pages[i]) == deferred.end()))
                    c->copied.push_back(pages[i]), c->slots.push_back((uint32_t)i);
            c->nbytes = (size_t)n * 4096;
            flush_mapped(c->alloc, c->nbytes);
        }
    }
    {   // (sprint 5, RX_VK_CAPTURE_DIFF=1, a diagnostic: of each copied page, the 256-byte chunks that differ from its last copy)
        static const bool on = getenv("RX_VK_CAPTURE_DIFF") != nullptr;
        if (on && c->map) {
            static std::mutex mu;
            static std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> last;
            static int64_t caps, pages_n, first_n, same_n, hist[17], bytes_changed;
            std::lock_guard lk(mu);
            for (size_t i = 0; i < c->copied.size(); i++) {
                const uint8_t* cur = (const uint8_t*)c->map + (uint64_t)c->slots[i] * 4096;
                auto& old = last[c->copied[i]];
                pages_n++;
                if (!old) {
                    old.reset(new uint8_t[4096]);
                    first_n++;
                } else {
                    int ch = 0;
                    for (int k = 0; k < 16; k++) ch += std::memcmp(cur + k * 256, old.get() + k * 256, 256) != 0;
                    hist[ch]++, same_n += ch == 0;
                    for (int b8 = 0; b8 < 512; b8++) bytes_changed += std::memcmp(cur + b8 * 8, old.get() + b8 * 8, 8) != 0 ? 8 : 0;
                }
                std::memcpy(old.get(), cur, 4096);
            }
            if (++caps % 600 == 0) {
                char line[512];
                int m = 0;
                for (int k = 0; k <= 16; k++) m += snprintf(line + m, sizeof line - m, " %d:%.0f", k, hist[k] / 100.0), hist[k] = 0;
                logf("vk: capture diff (a frame of 6 captures): %.0f pages copied, %.0f first seen, %.0f identical to their last copy, "
                     "%.0f KB of 8-byte words changed; pages by changed 256-byte chunks:%s", pages_n / 100.0, first_n / 100.0, same_n / 100.0,
                     bytes_changed / 100.0 / 1024, line);
                pages_n = first_n = same_n = bytes_changed = 0;
            }
        }
    }
    if (pre_on) precopy_size(n);
    if (host_pages) host->free_pages(host_pages);
    {   // (what it cost, every 600 submits)
        static int64_t subs, written, copied;
        static double ms, ms_lock, ms_scan, ms_copy;
        static uint64_t span;
        subs++, written += n > 0 ? n : 0, copied += (int64_t)c->copied.size();
        const auto now = std::chrono::steady_clock::now();
        ms += std::chrono::duration<double, std::milli>(now - t0).count();
        ms_lock += std::chrono::duration<double, std::milli>(t_lock - t0).count();
        ms_scan += std::chrono::duration<double, std::milli>(t_scan - t_lock).count();
        ms_copy += std::chrono::duration<double, std::milli>(now - t_copy).count();
        if (subs % 600 == 0) {
            span = 0;
            for (const auto& r : mirrored) span += r.second - r.first;
            logf("vk: capture: a submit %.2f ms (lock %.2f, scan %.2f over %zu ranges, %llu MB, copy %.2f), %.0f pages written, %.0f "
                 "copied (%.1f MB)", ms / 600, ms_lock / 600, ms_scan / 600, mirrored.size(),
                 (unsigned long long)(span >> 20), ms_copy / 600, written / 600.0, copied / 600.0, copied * 4096.0 / 600 / 1048576);
            ms = ms_lock = ms_scan = ms_copy = 0, written = copied = 0;
            if (capture_gran_on()) {
                const int64_t pg = st_gran_pages.exchange(0), by = st_gran_bytes.exchange(0), fu = st_gran_full.exchange(0),
                              un = st_gran_union.exchange(0);
                logf("vk: capture by granules (RX_VK_CAPTURE_GRAN): a submit %.0f pages, %.2f MB copied (%.0f%% of their bytes), %.0f whole, "
                     "%.0f with granules of an entry no sync had copied; %lld pages made whole so far (read as submitted, a texture's storage)",
                     pg / 600.0, by / 600.0 / 1048576, pg ? 100.0 * by / (pg * 4096.0) : 0.0, fu / 600.0, un / 600.0,
                     (long long)st_gran_made_full.load());
            }
            if (pre_on) {
                int64_t again, left, runs, grown;
                {
                    std::lock_guard lk(pre.mu);
                    again = pre.st_again, left = pre.st_left, runs = pre.st_runs, grown = pre.st_grown;
                    pre.st_again = pre.st_left = pre.st_runs = pre.st_grown = 0;
                }
                logf("vk: capture copied ahead (TICK-043): a submit %.0f pages copied ahead, %.0f new at the submit, %.0f copied again "
                     "there (written again or written back since), %lld of 600 every page at the submit (%lld: ranges changed); the thread: %.1f rounds, "
                     "%.0f pages copied again, %.0f given back (no room) a submit, %lld buffers grown",
                     st_pre_k / 600.0, st_pre_new / 600.0, st_pre_again / 600.0, (long long)st_pre_fallback, (long long)st_pre_out, runs / 600.0,
                     again / 600.0, left / 600.0, (long long)grown);
                st_pre_k = st_pre_new = st_pre_again = st_pre_fallback = st_pre_out = 0;
                if (precopy_kick_mode() >= 2) {
                    const int64_t r = st_pre2_rounds.exchange(0), pg = st_pre2_pages.exchange(0), sk = st_pre2_skipped.exchange(0), ns = st_pre2_ns.exchange(0);
                    logf("vk: capture copied ahead at the presents (RX_VK_PRECOPY_KICK=2): %lld rounds, %.0f pages and %.2f ms each, %lld kicks "
                         "skipped (the copies before not taken)", (long long)r, r ? (double)pg / (double)r : 0.0, r ? ns / 1e6 / (double)r : 0.0, (long long)sk);
                }
            }
        }
    }
    return c;
}

/// (HL_UBO_CHECK) the texture storage the captures take, as of now (sorted, merged)
std::vector<std::pair<uint64_t, uint64_t>> memory_tex_capture_ranges() {
    std::lock_guard lk(cap_ranges_mu);
    return tex_cap_ranges;
}
/// The ranges the captures take (the mirrored pools, the texture storage, the small pools): their union, sorted and
/// merged, as of now; empty while no capture was made (memory_collect_written: a page in them written after its
/// submit's capture is left for the next capture). Built again only when one of the three lists changed, as
/// capture_make keeps its own union; the reference is the calling thread's until its next call
const std::vector<std::pair<uint64_t, uint64_t>>& capture_ranges_merged() {
    static const std::vector<std::pair<uint64_t, uint64_t>> none;
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> merged;
    static thread_local uint64_t ver = ~uint64_t(0);
    // (no capture is ever made without the replay thread (RX_VK_ASYNC=0): the mirrored pools' writes were left for one
    // that never came, and the pools kept their first contents: a grey world)
    if (!capture_on() || !captures_seen.load(std::memory_order_relaxed)) return none;
    std::lock_guard lk(cap_ranges_mu);
    refresh_pool_ranges_locked();   // (a pool made since the last capture: the next one takes its pages, not this)
    if (ver != cap_ranges_ver) {
        ver = cap_ranges_ver;
        std::vector<std::pair<uint64_t, uint64_t>> all = cap_ranges;
        all.insert(all.end(), tex_cap_ranges.begin(), tex_cap_ranges.end());
        all.insert(all.end(), pool_cap_ranges.begin(), pool_cap_ranges.end());
        std::sort(all.begin(), all.end());
        merged.clear();
        for (const auto& r : all) {
            if (!merged.empty() && r.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, r.second);
            else
                merged.push_back(r);
        }
    }
    return merged;
}
// (TICK-036, RX_VK_LOG_INHERIT) 64 bytes at the watched address: the retired mirror, guest memory and the new mirror, read back
VkBuffer iw_buf = VK_NULL_HANDLE;
VmaAllocation iw_alloc = nullptr;
void* iw_map = nullptr;
uint64_t iw_value = 0;   // (the submission whose uploads copied them: printed once it is done)
void inherit_watch_print() {
    if (!iw_value || mem_completed < iw_value || !iw_map) return;
    iw_value = 0;
    for (int k = 0; k < 3; k++) {
        const uint32_t* w = (const uint32_t*)iw_map + k * 16;
        logf("vk: inherit watch: %s: %08x %08x %08x %08x %08x %08x %08x %08x", k == 0 ? "retired mirror" : k == 1 ? "guest memory  " : "new mirror    ",
             w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}
void memory_recording_value(uint64_t value, uint64_t completed) {
    mem_rec_value = value;
    if (completed > mem_completed) mem_completed = completed;
    inherit_watch_print();
}
// ---- (HeyListen TICK-053) the late pages merged: the capture's wait for the GPU ended ----
// A late page is one a submission still on the GPU writes back when the capture copies it (capture_make): its copy has the
// GPU's words as they were a frame before, and synced whole over the mirror it would put them back over the GPU's newer ones
// (hookshot's reason for the wait: one-frame flashes of OoT's exposure). The wait is for the previous frame's last write-back,
// i.e. for the GPU to finish the previous frame, once a frame on the game's thread: ~9 ms of a 35 ms frame on PC 3050 (RTX
// 3050, i5-10400F), the GPU idle meanwhile (63% busy). Copying the page live from guest memory on the GPU instead (TICK-017's
// RX_VK_CAPTURE_DEFER) reads the CPU's words of a later frame (the capture is there to stop that) and read the imported pages the
// CPU is writing: 15 fps plateaus on PC 3050.
// Here the page's copy from the capture is merged into the mirror word by word: only the words the CPU changed since the page
// was last synced to this mirror are copied; the GPU's words stay the mirror's. For that the bytes the mirror was last synced
// with are kept by page (a shadow, 4 KB in system memory), set at every sync of the page from a capture, and void when the
// mirror takes the page another way (live from guest memory, a readback refresh, a parallel segment's sync, a new mirror).
// A late page with no shadow yet is waited for as before (once; then it has one). The words the GPU wrote and the write-back
// landed in guest memory since the shadow was taken read as changed: copied again with the mirror's own value, harmless.
// RX_VK_LATE_MERGE=1: on; off by default: without the wait the game's thread runs ahead of the GPU, and on PC 3050 (over SSH, in
// session 0) the queue then idles 25-50 ms a frame between our own command buffers while both game threads wait for the GPU
// (TICK-053 in docs/perf-audit.md: not placed; the presentation, VRAM headroom and clocks ruled out), 23 fps against 28.5. The
// late bit rides in the capture table's slot (kCapLateBit, above).
bool late_merge_on() {
    static const bool v = getenv("RX_VK_LATE_MERGE") && atoi(getenv("RX_VK_LATE_MERGE")) != 0;
    return v;
}
struct LateShadow {
    uint32_t mid = 0;                   // (the mirror whose page holds these bytes; 0: none, void)
    uint64_t used = 0;                  // (gen_now at the last sync: the oldest go past kLateShadowMax)
    std::unique_ptr<uint8_t[]> bytes;   // (4 KB)
};
constexpr size_t kLateShadowMax = 4096;   // (16 MB)
std::unordered_map<uint32_t, LateShadow> late_shadows;   // (guest page -> its shadow; pools_mu)
std::atomic<int64_t> st_late_merged{0}, st_late_merged_bytes{0}, st_late_waited{0}, st_late_no_shadow{0}, st_late_seg{0};
void late_shadow_void(uint64_t page) {   // (pools_mu held: the mirror's page no longer holds what the shadow says)
    if (late_shadows.empty()) return;
    if (auto it = late_shadows.find((uint32_t)page); it != late_shadows.end()) it->second.mid = 0;
}
void late_shadow_void_range(uint64_t lo, uint64_t hi) {   // (pools_mu held)
    if (late_shadows.empty() || hi <= lo) return;
    for (uint64_t p = lo >> kPageShift; p <= (hi - 1) >> kPageShift; p++) late_shadow_void(p);
}
/// A shadow wanted for the page: an entry with no bytes yet, filled at the page's next sync from a capture (pools_mu held)
void late_shadow_want(uint64_t page) {
    if (late_shadows.size() >= kLateShadowMax && !late_shadows.count((uint32_t)page)) return;   // (full: the page waits as before)
    late_shadows.try_emplace((uint32_t)page);
}
/// Whether the page's shadow holds what its pool's mirror has for the page now (pools_mu held)
std::atomic<int64_t> st_late_why[4];   // (why a late page had no shadow: no entry, bytes not yet taken, void, another mirror)
bool late_shadow_valid_locked(uint64_t page) {
    const auto it = late_shadows.find((uint32_t)page);
    if (it == late_shadows.end()) return st_late_why[0]++, false;
    if (!it->second.bytes) return st_late_why[1]++, false;
    if (!it->second.mid) return st_late_why[2]++, false;
    const uint64_t g = page << kPageShift;
    auto pit = imported.upper_bound(g);
    if (pit == imported.begin()) return false;
    --pit;
    const Pool& p = pit->second;
    if (g >= p.lo && g < p.hi && p.mstate == Pool::MIRROR_ON && p.mirror && p.mirror_id == it->second.mid) return true;
    return st_late_why[3]++, false;
}
/// The page's shadow made or refreshed with the capture's bytes: the mirror holds them from this sync on (pools_mu held)
void late_shadow_set(uint64_t page, uint32_t mid, const uint8_t* bytes) {
    if (late_shadows.size() >= kLateShadowMax && !late_shadows.count((uint32_t)page)) {   // (the least recently synced go)
        uint64_t oldest = ~0ull;
        for (const auto& [p, s] : late_shadows) oldest = std::min(oldest, s.used);
        for (auto it = late_shadows.begin(); it != late_shadows.end();)
            it = it->second.used <= oldest + 64 ? late_shadows.erase(it) : std::next(it);
    }
    LateShadow& s = late_shadows[(uint32_t)page];
    if (!s.bytes) s.bytes.reset(new uint8_t[4096]);
    std::memcpy(s.bytes.get(), bytes, 4096);
    s.mid = mid, s.used = gen_now;
}
/// The words of now (the capture's page) that differ from the shadow, as runs of bytes (offset, length), 4-byte granular,
/// into out; the count. tests/test_late_merge.py
int late_diff_runs(const uint8_t* now, const uint8_t* shadow, uint32_t* off, uint32_t* len, int max_runs) {
    const uint64_t* a = (const uint64_t*)now;
    const uint64_t* b = (const uint64_t*)shadow;
    int n = 0;
    uint32_t run_lo = 0, run_hi = 0;
    for (uint32_t i = 0; i < 512; i++) {
        if (a[i] == b[i]) continue;
        const uint32_t* a4 = (const uint32_t*)(a + i);
        const uint32_t* b4 = (const uint32_t*)(b + i);
        for (uint32_t w = 0; w < 2; w++) {
            if (a4[w] == b4[w]) continue;
            const uint32_t at = i * 8 + w * 4;
            if (run_hi == at && run_hi) { run_hi += 4; continue; }
            if (run_hi) {
                if (n == max_runs) return -1;   // (cannot happen with 512: a page's words alternating is 512 runs)
                off[n] = run_lo, len[n] = run_hi - run_lo, n++;
            }
            run_lo = at, run_hi = at + 4;
        }
    }
    if (run_hi) {
        if (n == max_runs) return -1;
        off[n] = run_lo, len[n] = run_hi - run_lo, n++;
    }
    return n;
}

// ---- (HeyListen TICK-060) RX_VK_LATE_DEFER: the late pages' wait moved from the capture's take to the queue ----
// On PC 3050 (RTX 3050 on PCIe 3.0 x4, i5-10400F) the late-page wait above is ~8 ms of every ~36 ms frame: the held submits'
// thread waits there for the previous frame's last write-back while holding the replay, the Presentation Thread waits behind
// it, and the GPU, done with that frame's tail, then idles until the next recording arrives. With the switch on, a capture
// whose late value the queue already has (a signal of it or of a later value went out: the GPU reaches it with nothing more
// submitted) is taken without the wait: its late slots are noted (a LateFix: the capture buffer, the slots and their pages)
// and the replay goes on recording. Nothing that reads the capture buffer reaches the GPU before the fix: until it is applied
// every recording is held back (vk_replay.cpp submit_recording, the batched path), and every submit and flush applies the
// pending fixes first (queue_flush_locked: a wait for the value, then the same copies the take made after its wait: the late
// pages from guest memory into their slots); a thread that has to send something waits for the value before it takes the
// queue's lock (late_fix_wait_unlocked), and a poll of a fence neither waits nor flushes (what is held is not done anyway: a
// poll of the main thread's that waited under the lock held up the Presentation Thread 7.5 ms a frame in the first try). The
// CPU's own readers of a capture buffer (a replay read as submitted, the mirror sync's chunk hashes) ask late_fix_slot_pending:
// a read waits for the value and reads guest memory, a sync copies the page whole and keeps no hash of it. A thread of its own
// (vk_replay.cpp late_fix_main) waits for the value and applies
// the fix and sends the held recordings as soon as it is reached, so the GPU gets the new frame when the old one ends, as it
// did, with the CPU's recording done meanwhile. What the GPU reads is what the take's copies after the wait gave: the same
// bytes, copied at the same moment (the value's end), by another thread. Not deferred, as before: a late value not yet sent
// (held recordings, a queue stopped at an event), RX_VK_LATE_MERGE, a late page in a live texture's storage (uploads read it as
// submitted from the shadow the take copies: the take waits as before); a deferred late page in a pool with textures gets no
// shadow copy (an upload over it would read guest memory). At most kLateFixMax pending. On by default since the PC 3050 confirm (=0: off).
// tests/test_late_defer.py
bool late_defer_on() {   // (TICK-060: on by default after the PC 3050 confirm; =0 off)
    static const bool v = [] {
        const bool on = !getenv("RX_VK_LATE_DEFER") || atoi(getenv("RX_VK_LATE_DEFER")) != 0;
        if (on) logf("vk: late pages' wait deferred to the queue (RX_VK_LATE_DEFER)");
        return on;
    }();
    return v;
}
struct LateFix {
    uint64_t value = 0;                                  // (the late pages' write-back: their slots copied again once it is done)
    uint32_t gen = 0;                                    // (the capture's generation: its slots in the capture table)
    uint8_t* map = nullptr;                              // (the capture's buffer, kept in cap_bufs until its submission is done)
    VmaAllocation alloc = nullptr;
    size_t nbytes = 0;
    std::vector<std::pair<uint32_t, uint32_t>> slots;    // (slot, guest page)
};
std::mutex late_fix_mu;
std::deque<LateFix> late_fixes;          // (late_fix_mu; in take order, values NOT always rising: see memory_late_fix_apply)
std::atomic<uint64_t> late_fix_top{0};   // (the highest value a pending fix waits for; 0: none pending)
constexpr int kLateFixMax = 4;           // (pending at once; a take past it waits as before)
std::atomic<uint32_t> late_fix_gens[kLateFixMax];   // (the pending fixes' generations, 0: free: the readers' quick test)
/// Whether a capture's slot is a late one whose fix is pending (its bytes not yet the write-back's): *value, the value to wait for.
/// The CPU's readers of the capture buffers ask it: a replay read as submitted (memory_read_as_submitted) and the mirror sync's
/// chunk hashes (msync_diff), which must not take the slot's bytes before the fix
bool late_fix_slot_pending(uint32_t gen, uint32_t slot, uint64_t* value) {
    if (!gen || !late_fix_top.load(std::memory_order_acquire)) return false;
    bool any = false;
    for (int k = 0; k < kLateFixMax; k++) any |= late_fix_gens[k].load(std::memory_order_acquire) == gen;
    if (!any) return false;
    std::lock_guard lk(late_fix_mu);
    for (const LateFix& f : late_fixes) {
        if (f.gen != gen) continue;
        const auto it = std::lower_bound(f.slots.begin(), f.slots.end(), std::make_pair(slot, 0u));
        if (it != f.slots.end() && it->first == slot) {
            if (value) *value = f.value;
            return true;
        }
    }
    return false;
}
std::atomic<int64_t> st_late_deferred{0}, st_late_def_pages{0}, st_late_def_tex{0}, st_late_def_unsent{0};
uint64_t memory_late_fix_pending() { return late_fix_top.load(std::memory_order_acquire); }
size_t late_fix_count() {
    std::lock_guard lk(late_fix_mu);
    return late_fixes.size();
}
/// The pending fixes whose value done has reached: their late pages copied again from guest memory into their slots (as the
/// take did after its wait). Called before anything goes to the queue (vk_replay.cpp, the queue's lock held), and by the CPU's
/// readers of a pending slot once its value is reached (late_fix_mu alone orders them: late_fix_top drops only after the copies).
/// (TICK-060m) Any fix whose value is reached, not only the front ones, and late_fix_top the highest pending value: the values are
/// not rising in take order (a capture's late value is the newest write-back of its own late pages: the next capture's late pages
/// can be older submissions' write-backs). With the front-only loop and the back's value as the top, a fix behind a lower one
/// left the top below its own value: a flush waited for the lower value and sent the recordings reading the unfixed slots, and
/// its buffer could be retired and reused before its fix
void memory_late_fix_apply(uint64_t done) {
    std::lock_guard lk(late_fix_mu);
    if (late_fixes.empty()) return;
    uint8_t* const base = (uint8_t*)host->guest_base();
    uint64_t top = 0;
    for (auto it = late_fixes.begin(); it != late_fixes.end();) {
        LateFix& f = *it;
        if (f.value > done) {
            top = std::max(top, f.value);
            ++it;
            continue;
        }
        for (const auto& [slot, page] : f.slots)
            guarded_copy(f.map + (uint64_t)slot * 4096, base + ((uint64_t)page << kPageShift), 4096);
        flush_mapped(f.alloc, f.nbytes);
        for (int k = 0; k < kLateFixMax; k++)
            if (late_fix_gens[k].load(std::memory_order_relaxed) == f.gen) {
                late_fix_gens[k].store(0, std::memory_order_release);
                break;
            }
        it = late_fixes.erase(it);
    }
    late_fix_top.store(top, std::memory_order_release);
}
std::atomic<int64_t> st_late_def_lower{0};   // (TICK-060m: fixes registered with a value below a pending one's)
/// (TICK-060m) A deferred take's fix registered: its generation for the readers' quick test, late_fix_top raised to its value
/// if higher (never lowered: see memory_late_fix_apply), the fix thread woken. late_fix_count() < kLateFixMax (one taker)
void late_fix_push(LateFix&& f) {
    const uint64_t v = f.value;
    {
        std::lock_guard fl(late_fix_mu);
        for (int k = 0; k < kLateFixMax; k++)
            if (!late_fix_gens[k].load(std::memory_order_relaxed)) {
                late_fix_gens[k].store(f.gen, std::memory_order_release);
                break;
            }
        const uint64_t top = late_fix_top.load(std::memory_order_relaxed);
        if (top > v) st_late_def_lower.fetch_add(1, std::memory_order_relaxed);
        late_fixes.push_back(std::move(f));
        late_fix_top.store(std::max(top, v), std::memory_order_release);
    }
    late_fix_wake(v);
}
/// (TICK-060m) The highest value of the pending fixes with a late page in [guest, guest + n) (0: none): a reader of guest memory
/// as submitted that has no capture slot to read (a texture's storage the CPU uploads from) waits for it
uint64_t late_fix_range_value(uint64_t guest, uint64_t n) {
    if (!n || !late_fix_top.load(std::memory_order_acquire)) return 0;
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + n - 1) >> kPageShift;
    uint64_t v = 0;
    std::lock_guard lk(late_fix_mu);
    for (const LateFix& f : late_fixes)
        for (const auto& sp : f.slots)
            if (sp.second >= p0 && sp.second <= p1) {
                v = std::max(v, f.value);
                break;
            }
    return v;
}
std::atomic<int64_t> st_late_tex_waits{0};   // (TICK-060m: texture storage reads that waited for a pending late page's value)
/// (TICK-060m) Before the CPU reads [guest, guest + n) of guest memory for an upload (vk_texture.cpp guest_bytes): a late page
/// there whose fix is pending is not the write-back's yet in guest memory (the take did not wait, and gave it no shadow): its
/// value waited for (it is sent: no flush needed) and the fixes reached applied, as the take's wait had it before the upload
void memory_late_fix_wait_range(uint64_t guest, uint64_t n) {
    const uint64_t v = late_fix_range_value(guest, n);
    if (!v) return;
    st_late_tex_waits.fetch_add(1, std::memory_order_relaxed);
    vk_timeline_wait_sent(v);
    memory_late_fix_apply(vk_timeline_completed());
}
bool memory_is_rb_page_locked(uint32_t page);
// ---- (TICK-061, E3) RX_VK_STAMP_SPLIT=1 (off by default): a take's page stamps and table entries on the copy helpers ----
// take_capture stamps each page of the capture with its generation (stamp_page) and points each copied page's table entry at its
// slot (cap_entry_set), ~1,000-2,000 pages a take, serially under pools_mu. Here the two loops become one job of chunks claimed by
// the taking thread and the copy helpers (claim_all, as RX_VK_CAPTURE_CLAIM's copy), for takes of 1,024 pages or more.
// The same stores, at the same point of the critical section: the job runs and ends while the taking thread holds pools_mu (and
// waits for every chunk, claim_all), so no thread that reads the stamps or the table under that lock sees the take half done.
// The tables' chunks are made first, on the taking thread (the only allocation either store makes: stamp_page and cap_entry_set
// allocate a chunk on its first use, which two helpers must not race on); then each store is to its own page's word, by one
// thread. region_gen[r] = g is stored by every page of the region: the same value from each thread (g is the newest generation,
// so the result is g whichever store lands last), done with an atomic store so the threads do not race on the plain word.
// The stamps move after the texture-shadow block, which reads no stamp (both under the same locks).
bool stamp_split_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_STAMP_SPLIT");
        const bool v = e && atoi(e) != 0;
        logf("vk: a capture's page stamps: %s (RX_VK_STAMP_SPLIT)", v ? "on the copy helpers from 1,024 pages" : "on the taking thread, as before");
        return v;
    }();
    return on;
}
struct StampSplit {
    const uint32_t *pages, *copied, *slots;
    size_t n, m;
    uint32_t g;
    const uint8_t* late;
};
void stamp_split_chunk(void* ctx, int64_t i0, int64_t i1) {
    const StampSplit& w = *(const StampSplit*)ctx;
    for (int64_t i = i0; i < i1; i++) {
        if ((size_t)i < w.n) {
            const uint64_t p = w.pages[i];
            if (p >= kPages) continue;
            if (p == inherit_watch_page()) logf("vk: inherit watch: page %#llx stamped, gen %u", (unsigned long long)(p << kPageShift), w.g);
            store_u32(gen_chunks[p / kChunkPages][p % kChunkPages], w.g);
            std::atomic_ref<uint32_t>(region_gen[p / kRegionPages]).store(w.g, std::memory_order_relaxed);
        } else {
            const size_t j = (size_t)i - w.n;
            const uint64_t page = w.copied[j];
            cap_table[page / kCapChunkPages][page % kCapChunkPages] = CapEntry{w.g, w.slots[j] | (w.late[w.slots[j]] ? kCapLateBit : 0u)};
        }
    }
}
void stamp_split_run(const uint32_t* pages, size_t n, const uint32_t* copied, const uint32_t* slots, size_t m, uint32_t g,
                     const uint8_t* late) {
    for (size_t i = 0; i < n; i++) {   // (the chunks first: see above)
        const uint64_t p = pages[i];
        if (p >= kPages) continue;
        auto& c = gen_chunks[p / kChunkPages];
        if (!c) {
            c.reset(new uint32_t[kChunkPages]);
            std::fill(c.get(), c.get() + kChunkPages, 0u);
        }
    }
    for (size_t j = 0; j < m; j++) {
        const uint64_t c = copied[j] / kCapChunkPages;
        if (c >= cap_table.size()) cap_table.resize(c + 1);
        if (!cap_table[c]) {
            cap_table[c].reset(new CapEntry[kCapChunkPages]);
            std::memset(cap_table[c].get(), 0, kCapChunkPages * sizeof(CapEntry));
        }
    }
    StampSplit w{pages, copied, slots, n, m, g, late};
    ClaimJob job{nullptr, (int64_t)(n + m), nullptr, nullptr, 256};
    job.fn = stamp_split_chunk, job.ctx = &w;
    claim_all(job, copy_helper_count());
}
bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {
    const auto tk0 = std::chrono::steady_clock::now();
    // (TICK-053) the late pages merged at their syncs instead (RX_VK_LATE_MERGE): no wait when every one of them has a shadow
    // of what its mirror holds; else the wait below, and a shadow wanted for each, made at its sync from this fresh copy
    bool late_merge = false, late_defer = false;
    if (c->late_value && c->map) {   // (the pages a submission still on the GPU writes back: see capture_make)
        if (late_merge_on()) {
            std::lock_guard sl(pools_mu);
            late_merge = true;
            for (uint32_t i : c->late)
                if (!late_shadow_valid_locked(c->pages[i])) {
                    late_merge = false;
                    break;
                }
            if (late_merge) st_late_merged.fetch_add(1, std::memory_order_relaxed);
            else {
                st_late_no_shadow.fetch_add(1, std::memory_order_relaxed);
                for (uint32_t i : c->late) late_shadow_want(c->pages[i]);
            }
        }
        // (TICK-060) RX_VK_LATE_DEFER: no wait here when the queue has the late value already and no late page is a live texture's
        // storage (the uploads read such pages as submitted, from the shadow the take copies below)
        if (!late_merge_on() && late_defer_on() && c->late_value > vk_timeline_completed()) {
            // (HeyListen, sprint 5) RX_VK_LATE_DEFER_TEX (on; =0: such a take waits, as before): a late page in a live texture's
            // storage no longer keeps the take waiting. In OoT it is one page, 0x11371e7000: two 1x1 textures of 128 bytes made at
            // boot (0x...d80, 0x...e00) beside bytes the CPU writes and a submission writes back every frame; that page alone made
            // 59-89 of ~106 takes wait for the GPU (~1.3 ms a frame once the held flush comes early). What an upload reads of such
            // a page is the texture's own bytes, which a write-back does not write (write-backs are of buffers: "texture storage
            // 0.00" in the write-backs' log): they are in the slot as copied at the submit, so the page's shadow is made from that
            // copy below, at the take, as for any other page of a texture's storage; the write-back's bytes reach the slot with
            // the fix, as for the other late pages. An upload over the page while its fix is pending still waits for the value
            // first (memory_late_fix_wait_range).
            static const bool defer_tex = !getenv("RX_VK_LATE_DEFER_TEX") || atoi(getenv("RX_VK_LATE_DEFER_TEX")) != 0;
            bool tex = false;
            if (!defer_tex)
                for (uint32_t i : c->late)
                    if (replay_tex_storage_in((uint64_t)c->pages[i] << kPageShift, 4096)) { tex = true; break; }
            {   // (sprint 5, RX_VK_LATE_TEX_LOG=1: which late pages lie in a live texture's storage, 40 takes)
                static const bool lg = getenv("RX_VK_LATE_TEX_LOG") != nullptr;
                static int logged;
                if (lg && tex && logged < 40) {
                    logged++;
                    char line[1024];
                    int n = 0, k = 0;
                    for (uint32_t i : c->late)
                        if (replay_tex_storage_in((uint64_t)c->pages[i] << kPageShift, 4096)) {
                            k++;
                            if (n < 900) n += snprintf(line + n, sizeof line - n, " %#llx", (unsigned long long)c->pages[i] << kPageShift);
                        }
                    logf("vk: late tex: %d of %zu late pages in a live texture's storage:%s", k, c->late.size(), line);
                }
            }
            if (tex) st_late_def_tex.fetch_add(1, std::memory_order_relaxed);
            else if (c->late_value > vk_timeline_signal_sent()) st_late_def_unsent.fetch_add(1, std::memory_order_relaxed);
            else if (late_fix_count() >= kLateFixMax) st_late_def_full.fetch_add(1, std::memory_order_relaxed);
            else late_defer = true, gpu_tail_note_late(c->late_value);   // (TICK-057's tail: the value the queue now waits for)
        }
        if (!late_merge && !late_defer) {
            st_late_waited.fetch_add(1, std::memory_order_relaxed);
            // (timed apart: the replay thread waits here for the GPU, the more often the fewer pools have a mirror. With
            // RX_VK_MIRROR_MB=1024 on an 8 GB card it was most of a capture's 2.4-3 ms, six submits a frame: Hyrule Field at
            // 35-45 fps instead of 55, issue 995)
            const auto tw = std::chrono::steady_clock::now();
            if (c->late_value > vk_timeline_completed()) gpu_tail_note_late(c->late_value);   // (TICK-057: a real wait)
            vk_timeline_wait(c->late_value);
            st_tk_late += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw).count();
            st_tk_late_n++;
            st_tr_late_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tw).count(),
                                    std::memory_order_relaxed);
            st_tr_late_n.fetch_add(1, std::memory_order_relaxed);
            uint8_t* const base = (uint8_t*)host->guest_base();
            for (uint32_t i : c->late) {
                guarded_copy((uint8_t*)c->map + (uint64_t)i * 4096, base + ((uint64_t)c->pages[i] << kPageShift), 4096);
                if (i < c->hok.size()) c->hok[i] = 0;   // (TICK-060: other bytes than the copy hashed)
            }
            flush_mapped(c->alloc, c->nbytes);
        }
    }
    const auto tl = std::chrono::steady_clock::now();
    std::lock_guard lk(pools_mu);
    const auto tk_locked = std::chrono::steady_clock::now();
    st_tk_lock += std::chrono::duration<double, std::milli>(tk_locked - tl).count();
    cur_cap_value = value;
    cur_cap_gen = 0;
    // (captures past the last kKeep, or past the byte budget, retired once the work that read them is done)
    while (!cap_bufs.empty()) {
        auto oldest = cap_bufs.begin();
        const bool over = cap_bufs.size() > kKeep || cap_bytes > kCapBytesMax;
        if (!over || std::max(oldest->second.value, oldest->second.last_use) > completed) break;
        cap_buf_retire(oldest);
    }
    uint32_t g = 0;
    // (TICK-061, E3: RX_VK_STAMP_SPLIT=1 stamps the pages and sets the table entries on the copy helpers, below)
    const bool split = stamp_split_on() && copy_helper_count() > 0 && c->pages.size() >= 1024;
    if (!c->pages.empty()) {
        g = ++gen_now;
        if (!split) {
            const auto ts0 = std::chrono::steady_clock::now();
            for (uint32_t p : c->pages) stamp_page(p, g);
            st_tk_stamps += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts0).count();
        }
        pages_collected += c->pages.size();
    }
    if (!c->gmask.empty() && c->map && tex_capture_on()) {   // (sprint 5, RX_VK_CAPTURE_GRAN: a texture's storage is taken whole)
        // (a page of it copied by granules: the rest of its slot from guest memory now, a few microseconds after the capture's
        // copy (a late page's copy does the same later), and the page whole from its next capture on: the shadow below is a page)
        std::lock_guard sk(shadow_mu);
        if (!tex_merged.empty()) {
            uint8_t* const base = (uint8_t*)host->guest_base();
            for (size_t i = 0; i < c->copied.size(); i++) {
                const uint32_t s = c->slots[i];
                if (c->gmask[s] == 0xff || !in_tex_merged((uint64_t)c->copied[i] << 12)) continue;
                for (unsigned k = 0; k < 8; k++)
                    if (!(c->gmask[s] >> k & 1))
                        guarded_copy((uint8_t*)c->map + (uint64_t)s * 4096 + k * 512, base + ((uint64_t)c->copied[i] << kPageShift) + k * 512, 512);
                c->gmask[s] = 0xff;
                if (PageX* x = pagex(c->copied[i], true)) x->full = 1;
                st_gran_made_full.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!c->pages.empty() && tex_capture_on()) {   // (the texture storage it took: the shadow, as of this submit)
        std::lock_guard sk(shadow_mu);
        shadow_seq++;
        // (the copies of pages no capture took in the last 256 submits go, over 32 MB of them, every 256 captures: such a page is what guest
        // memory holds (a write drops its copy), and the cap's sweep below came only at 256 MB. RX_VK_SHADOW_SWEEP_OFF=1: the cap's alone)
        static const bool sweep_off = getenv("RX_VK_SHADOW_SWEEP_OFF") != nullptr;
        if (!sweep_off && shadow_seq % 256 == 0 && shadow_n.load(std::memory_order_relaxed) > 8192)
            for (auto it = shadow.begin(); it != shadow.end();) {
                if (it->second.seq + 256 >= shadow_seq) { ++it; continue; }
                auto r = shadow_regions.find(it->first >> 9);
                if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
                it = shadow.erase(it);
                shadow_n--;
            }
        const auto tsh0 = std::chrono::steady_clock::now();
        if (!tex_merged.empty() && shadow_fast_on() &&   // (TICK-061: see shadow_take_fast; not with deferred late slots, TICK-060)
            shadow_take_fast(c->pages.data(), c->pages.size(), c->copied.data(), c->slots.data(), c->copied.size(),
                             (const uint8_t*)c->map)) {
            st_tk_shadow_fast++;
        } else if (!tex_merged.empty()) {
            // (every page written since the last capture: its old copy is stale; the ones copied now replace it)
            for (uint32_t p : c->pages)
                if (in_tex_merged((uint64_t)p << 12)) shadow_erase(p);
            bool swept = false;
            for (size_t i = 0; i < c->copied.size(); i++) {
                const uint32_t p = c->copied[i];
                if (!in_tex_merged((uint64_t)p << 12)) continue;
                // (TICK-060: a deferred late page's slot is not the write-back's yet. Sprint 5: its shadow is made all the same,
                // from the slot as copied at the submit: a texture's own bytes there are not what the write-back writes; see
                // RX_VK_LATE_DEFER_TEX in take_capture. Pages with no texture's storage never come here: in_tex_merged above)
                if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax) {
                    if (swept) break;
                    swept = true;
                    for (auto it = shadow.begin(); it != shadow.end();) {
                        if (it->second.seq + 64 >= shadow_seq) { ++it; continue; }
                        auto r = shadow_regions.find(it->first >> 9);
                        if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
                        it = shadow.erase(it);
                        shadow_n--;
                    }
                    if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax) break;
                }
                ShadowPage& slot = shadow[p];
                if (!slot.b) {
                    slot.b.reset(new uint8_t[4096]);
                    shadow_regions[p >> 9]++;
                    shadow_n++;
                }
                slot.seq = shadow_seq;
                std::memcpy(slot.b.get(), (const uint8_t*)c->map + (uint64_t)c->slots[i] * 4096, 4096);
            }
        }
        st_tk_shadow += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tsh0).count();
    }
    if (!c->deferred.empty()) {   // (TICK-017: the pages the wait was for, logged: the evidence of which reads need the GPU's results)
        static int64_t caps, pages_n, logged;
        caps++, pages_n += (int64_t)c->deferred.size();
        for (uint32_t p : c->deferred)
            if (logged < 24) {
                logged++;
                const uint64_t ga = (uint64_t)p << kPageShift;
                auto pit = imported.upper_bound(ga);
                const Pool* pool = pit != imported.begin() && ga < std::prev(pit)->second.hi ? &std::prev(pit)->second : nullptr;
                logf("vk: capture defer: page %#llx (pool %#llx+%#llx, readback page %d) left to the GPU's copy at %llu",
                     (unsigned long long)ga, (unsigned long long)(pool ? pool->lo : 0), (unsigned long long)(pool ? ga - pool->lo : 0),
                     memory_is_rb_page_locked(p) ? 1 : 0, (unsigned long long)value);
            }
        if (caps % 600 == 0)
            logf("vk: capture defer: %lld captures had pages a write-back in flight writes (%.2f pages each)", (long long)caps,
                 pages_n / 600.0), pages_n = 0;
    }
    const auto tk1 = std::chrono::steady_clock::now();
    bool split_done = false;
    if (!c->copied.empty() && g && c->map) {   // (its buffer kept with the captures, for the mirror syncs)
        CapBuf b{value, c->buffer, c->alloc, c->bsize, c->map};
        b.used = std::min<uint64_t>(c->nbytes, c->bsize);
        if (c->hok.size() == c->pages.size() && !c->hok.empty()) {   // (TICK-060: the copy's slot hashes kept with it)
            if (late_defer)
                for (uint32_t i : c->late) c->hok[i] = 0;   // (copied again at its fix: hashed by the sync as before)
            std::shared_ptr<CapHash> ch;
            if (!cap_hash_free.empty()) ch = std::move(cap_hash_free.back()), cap_hash_free.pop_back();
            else ch = std::make_shared<CapHash>();
            ch->h.swap(c->hashes);
            ch->ok.swap(c->hok);
            b.hash = std::move(ch);
        }
        if (!c->gmask.empty()) {   // (sprint 5: the slots' granules kept with the buffer, and each page's with its entry)
            for (size_t i = 0; i < c->copied.size(); i++)
                if (PageX* x = pagex(c->copied[i], true)) x->gm = c->gmask[c->slots[i]];
            b.gmask = std::make_shared<std::vector<uint8_t>>();
            b.gmask->swap(c->gmask);
        }
        c->buffer = VK_NULL_HANDLE, c->alloc = nullptr, c->map = nullptr, c->bsize = 0;
        cap_bufs.emplace(g, b);
        cap_bytes += b.size, cap_used += b.used;
        cur_cap_gen = g;
        // (TICK-053: a late page merged at its sync carries kCapLateBit; a slot is the page's index in c->pages, as c->late)
        static thread_local std::vector<uint8_t> late_mark;
        late_mark.assign(c->pages.size(), 0);
        if (late_merge)
            for (uint32_t i : c->late) late_mark[i] = 1;
        const auto te0 = std::chrono::steady_clock::now();
        if (split) {
            stamp_split_run(c->pages.data(), c->pages.size(), c->copied.data(), c->slots.data(), c->copied.size(), g, late_mark.data());
            split_done = true;
        } else {
            for (size_t i = 0; i < c->copied.size(); i++)
                cap_entry_set(c->copied[i], CapEntry{g, c->slots[i] | (late_mark[c->slots[i]] ? kCapLateBit : 0u)});
        }
        st_tk_entries += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - te0).count();
        if (late_defer) {   // (TICK-060: the late slots copied again once their write-back is done, before anything reads them)
            LateFix f;
            f.value = c->late_value, f.gen = g, f.map = (uint8_t*)b.map, f.alloc = b.alloc, f.nbytes = c->nbytes;
            for (uint32_t i : c->late) f.slots.push_back({i, c->pages[i]});
            static int logged;
            for (uint32_t i : c->late)
                if (logged < 24) {
                    logged++;
                    const uint64_t ga = (uint64_t)c->pages[i] << kPageShift;
                    auto pit = imported.upper_bound(ga);
                    const Pool* pool = pit != imported.begin() && ga < std::prev(pit)->second.hi ? &std::prev(pit)->second : nullptr;
                    logf("vk: late page deferred: %#llx (pool %#llx+%#llx, readback page %d), write-back %llu, submission %llu",
                         (unsigned long long)ga, (unsigned long long)(pool ? pool->lo : 0), (unsigned long long)(pool ? ga - pool->lo : 0),
                         memory_is_rb_page_locked(c->pages[i]) ? 1 : 0, (unsigned long long)c->late_value, (unsigned long long)value);
                }
            st_late_deferred.fetch_add(1, std::memory_order_relaxed);
            st_late_def_pages.fetch_add((int64_t)c->late.size(), std::memory_order_relaxed);
            late_fix_push(std::move(f));   // (slots in rising order: late_fix_slot_pending's lower_bound)
        }
    }
    if (split && !split_done) {   // (no buffer kept: the stamps alone)
        const auto te0 = std::chrono::steady_clock::now();
        stamp_split_run(c->pages.data(), c->pages.size(), nullptr, nullptr, 0, g, nullptr);
        st_tk_entries += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - te0).count();
    }
    cap_view_bump();
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    {
        std::lock_guard lk(cap_mu);
        if (spare.size() < 8) spare.push_back(std::move(c));
    }
    {   // (every 600 takes: where a take's time went)
        const auto tk2 = std::chrono::steady_clock::now();
        static int64_t n;
        st_tk_stamp += std::chrono::duration<double, std::milli>(tk1 - tk_locked).count();
        st_tk_all += std::chrono::duration<double, std::milli>(tk2 - tk0).count();
        if (++n % 600 == 0) {
            logf("vk: a capture taken (ms): all %.3f; GPU wait for late pages %.3f (%lld of 600 takes), pools lock %.3f, retires "
                 "and stamps %.3f, bytes copied %.3f; %zu kept (%llu MB), %zu spare buffers, %zu texture pages copied (%llu MB)",
                 st_tk_all / 600, st_tk_late / 600, (long long)st_tk_late_n, st_tk_lock / 600, st_tk_stamp / 600, st_tk_copy / 600,
                 cap_bufs.size(), (unsigned long long)(cap_bytes >> 20), cap_free.size(), shadow_n.load(std::memory_order_relaxed),
                 (unsigned long long)(shadow_n.load(std::memory_order_relaxed) >> 8));
            st_tk_all = st_tk_stamp = st_tk_copy = st_tk_late = st_tk_lock = 0, st_tk_late_n = 0;
            // (TICK-061) inside it: the page stamps, the texture shadow block, the table entries (with RX_VK_STAMP_SPLIT the stamps
            // are in the entries' part); and the capture copies made by claiming (RX_VK_CAPTURE_CLAIM) and the helpers' tokens taken
            logf("vk: a capture taken, in parts (ms): page stamps %.3f, texture shadow %.3f (%lld of 600 by RX_VK_SHADOW_FAST), table "
                 "entries %.3f; copies by claiming %lld, helpers in time %lld", st_tk_stamps / 600, st_tk_shadow / 600,
                 (long long)st_tk_shadow_fast, st_tk_entries / 600, (long long)st_claim_copies.exchange(0),
                 (long long)st_claim_helped.exchange(0));
            st_tk_stamps = st_tk_shadow = st_tk_entries = 0, st_tk_shadow_fast = 0;
            if (capture_hash_on()) {   // (TICK-060)
                static int64_t l_m, l_u, l_a;
                const int64_t m = st_hash_made.load(), u = st_hash_used.load(), a = st_hash_again.load();
                logf("vk: capture hashes (RX_VK_CAPTURE_HASH): %.0f slots hashed by the copy a submit; the syncs took %.0f, hashed %.0f again",
                     (m - l_m) / 600.0, (u - l_u) / 600.0, (a - l_a) / 600.0);
                l_m = m, l_u = u, l_a = a;
            }
            if (late_defer_on()) {   // (TICK-060)
                static int64_t l_d, l_p, l_t, l_u, l_f, l_r, l_s, l_x, l_lo;
                const int64_t d = st_late_deferred.load(), pg = st_late_def_pages.load(), t = st_late_def_tex.load(), u = st_late_def_unsent.load();
                logf("vk: late pages deferred (RX_VK_LATE_DEFER): %lld takes not waited (%.1f late pages each), waited as before: %lld "
                     "(a late page in a live texture's storage), %lld (the value not sent yet), %lld (%d fixes pending); before their fix, %lld replay "
                     "reads of a late slot (waited), %lld syncs of one (copied whole), %lld texture storage reads (waited); %lld fixes below a "
                     "pending one's value", (long long)(d - l_d),
                     d - l_d ? (double)(pg - l_p) / (double)(d - l_d) : 0.0, (long long)(t - l_t), (long long)(u - l_u),
                     (long long)(st_late_def_full.load() - l_f), kLateFixMax, (long long)(st_late_reads.load() - l_r),
                     (long long)(st_late_syncs.load() - l_s), (long long)(st_late_tex_waits.load() - l_x),
                     (long long)(st_late_def_lower.load() - l_lo));
                l_d = d, l_p = pg, l_t = t, l_u = u, l_f = st_late_def_full.load(), l_r = st_late_reads.load(), l_s = st_late_syncs.load();
                l_x = st_late_tex_waits.load(), l_lo = st_late_def_lower.load();
            }
            if (late_merge_on()) {   // (TICK-053)
                static int64_t l_m, l_w, l_ns, l_b, l_seg;
                const int64_t m = st_late_merged.load(), w = st_late_waited.load(), ns = st_late_no_shadow.load();
                const int64_t b = st_late_merged_bytes.load(), seg = st_late_seg.load();
                static int64_t l_why[4];
                int64_t why[4];
                for (int k = 0; k < 4; k++) why[k] = st_late_why[k].load();
                logf("vk: late pages merged (RX_VK_LATE_MERGE): %lld takes merged (no wait), %lld waited (%lld for a page with no "
                     "shadow yet: %lld never late, %lld not synced since, %lld void, %lld another mirror), %.1f KB of changed words a "
                     "take, %lld pages in a segment; %zu shadows", (long long)(m - l_m), (long long)(w - l_w), (long long)(ns - l_ns),
                     (long long)(why[0] - l_why[0]), (long long)(why[1] - l_why[1]), (long long)(why[2] - l_why[2]), (long long)(why[3] - l_why[3]),
                     m - l_m ? (b - l_b) / 1024.0 / (m - l_m) : 0.0, (long long)(seg - l_seg), late_shadows.size());
                l_m = m, l_w = w, l_ns = ns, l_b = b, l_seg = seg;
                std::memcpy(l_why, why, sizeof why);
            }
        }
    }
    return true;
}
bool memory_take_capture(uint64_t value, uint64_t completed) {
    if (completed > mem_completed) mem_completed = completed;
    if (!capture_on()) return false;
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (captures.empty()) return false;
        c = std::move(captures.front());
        captures.pop_front();
    }
    return take_capture(std::move(c), value, completed);
}
/// (hookshot) A capture made and taken at once, for a submission going on from a wait the CPU's signal met
/// (vk_replay.cpp resume_queue): the CPU's writes up to that signal, which the console's GPU reads after its wait
bool memory_capture_now(uint64_t value, uint64_t completed) {
    if (completed > mem_completed) mem_completed = completed;
    std::unique_ptr<Capture> c = capture_make();
    return c && take_capture(std::move(c), value, completed);
}

namespace {

/// The imported pool holding [guest, guest + size) (imported now if it was not yet); null outside the pools.
/// pools_mu held.
Pool* pool_cache[4096];   // (by 64 KB page: the pools buffer bindings hit lately)
std::atomic<uint64_t> pools_gen{1};   // (bumped when a pool leaves the table: the recording threads' caches go stale)
struct TlPool {
    Pool* p;
    uint64_t gen;
};
thread_local TlPool tl_pools[1024];   // (a parallel replay thread's: by 64 KB page, read without the lock)
// (TICK-052) RX_VK_TL_POOLS_WIDE (on by default since TICK-054; =0: off): 16384 entries (1 GB before two pages share one) instead of 1024 (64 MB): the game's
// pools lie further apart than that, and pages 64 MB apart took turns in one entry (a third of a command buffer's buffer binds went
// to the lock, "bind page range")
constexpr uint64_t kTlWide = 16384;
thread_local TlPool* tl_pools_wide = nullptr;
bool tl_wide_on() {
    static const bool on = !getenv("RX_VK_TL_POOLS_WIDE") || atoi(getenv("RX_VK_TL_POOLS_WIDE")) != 0;   // (TICK-054: on by default)
    return on;
}
TlPool& tl_pool_entry(uint64_t guest) {
    if (!tl_wide_on()) return tl_pools[(guest >> 16) & 1023];
    if (!tl_pools_wide) tl_pools_wide = new TlPool[kTlWide]();   // (once a thread; kept for its life)
    return tl_pools_wide[gone_ring::page_slot(guest, kTlWide)];
}

Pool* pool_of(uint64_t guest, uint64_t size) {
    if (!size || size > UINT64_MAX - guest) return nullptr;
    if (last_pool && guest >= last_pool->lo && guest + size <= last_pool->hi) return last_pool;
    Pool*& slot = pool_cache[(guest >> 16) & 4095];
    if (slot && guest >= slot->lo && guest + size <= slot->hi) return last_pool = slot;
    Pool* found = pool_lookup(guest, size);
    if (found) slot = found;
    return found;
}

// ---- pools imported in the background: an import took 5-45 ms (the driver locks the pages; the game's threads keep
// the address space busy committing theirs), and the game makes ~13 pools a second. The host says when the game makes
// one (memory_pool_created), a thread of ours imports it, and a lookup that finds one under way waits for it rather
// than importing it again. Imports made are handed over in a list (the lookup holds pools_mu, the thread never). ----
std::mutex bg_mu;
std::condition_variable bg_cv, bg_done_cv;
std::deque<std::pair<uint64_t, uint64_t>> bg_queue;
std::vector<uint64_t> bg_inflight;           // (lo of the ranges queued or being imported)
std::vector<std::pair<uint64_t, Pool>> bg_ready;
int64_t st_bg_imports, st_bg_waits, st_prefetch_sent, st_prefetch_used, st_prefetch_dropped;
void bg_main();
void bg_start() {   // (bg_mu held)
    static bool started;
    if (!started) {
        started = true;
        std::thread(bg_main).detach();
    }
}

void bg_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk imports");
    for (;;) {
        std::pair<uint64_t, uint64_t> r;
        {
            std::unique_lock lk(bg_mu);
            bg_cv.wait(lk, [] { return !bg_queue.empty(); });
            r = bg_queue.front();
            bg_queue.pop_front();
        }
        Pool p;
        const bool ok = import_pool(r.first, r.second, p);
        std::lock_guard lk(bg_mu);
        p.prefetched = true;
        if (ok) bg_ready.push_back({r.first, p}), st_bg_imports++;
        std::erase(bg_inflight, r.first);
        bg_done_cv.notify_all();
    }
}

/// The imports the thread made, into the table (pools_mu held); one already there makes the new one redundant
void bg_drain() {
    std::vector<std::pair<uint64_t, Pool>> ready;
    {
        std::lock_guard lk(bg_mu);
        if (bg_ready.empty()) return;
        ready.swap(bg_ready);
    }
    for (auto& [lo, p] : ready) {
        auto imp = imported.find(lo);
        if (imp != imported.end() && imp->second.hi >= p.hi) {
            vkDestroyBuffer(dev.device, p.buffer, nullptr);
            vkFreeMemory(dev.device, p.memory, nullptr);
            for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);
            continue;
        }
        bool overlap = false;   // (amd-fix: a chunk imported ahead that an import made since covers in part: the pools stay disjoint)
        for (auto o = imported.lower_bound(p.hi); o != imported.begin();) {
            --o;
            if (o == imp) continue;
            if (o->second.hi <= p.lo) {
                if (o->second.hi + (1ull << 30) < p.lo) break;
                continue;
            }
            overlap = true;
            break;
        }
        if (overlap) {
            st_prefetch_dropped++;
            vkDestroyBuffer(dev.device, p.buffer, nullptr);
            vkFreeMemory(dev.device, p.memory, nullptr);
            for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);
            continue;
        }
        if (imp != imported.end()) retire_pool(imp);
        imported.emplace(lo, p);
        guest_pages_set(p.lo, p.hi, p.address);
    }
}

/// (amd-fix) An import on the replay thread stalled it 20-75 ms on an RX 9060 XT (the driver locks the chunk's pages, the
/// commit of fresh ones), a tiny uniform buffer's binding as long, ~40 times entering Kokiri: hitches. The chunks next to
/// one imported on demand, inside the game's pools, are imported ahead by the background thread; a lookup that needs one
/// still under way waits for it (less than importing it), and a chunk imported ahead and first bound sends for its own
/// neighbours. RX_VK_PREFETCH_CHUNKS: how many on each side (default 0: off, the background imports slowed the replay thread's own to 80-127 ms on AMD). (pools_mu held)
void prefetch_chunks(uint64_t lo, uint64_t hi, uint64_t chunk) {
    static const int n = getenv("RX_VK_PREFETCH_CHUNKS") ? std::max(0, atoi(getenv("RX_VK_PREFETCH_CHUNKS"))) : 0;
    if (n <= 0 || !chunk || !dev.external_memory_host || hi <= lo) return;
    auto in_game_pool = [](uint64_t a, uint64_t b) {
        auto g = std::lower_bound(guest_pools.begin(), guest_pools.end(), b, [](const GuestPool& p, uint64_t v) { return p.lo < v; });
        return g != guest_pools.begin() && std::prev(g)->hi > a;
    };
    auto imported_over = [](uint64_t a, uint64_t b) {
        for (auto o = imported.lower_bound(b); o != imported.begin();) {
            --o;
            if (o->second.hi > a) return true;
            if (o->second.hi + (1ull << 30) < a) break;
        }
        return false;
    };
    std::vector<std::pair<uint64_t, uint64_t>> want;
    for (int k = 0; k < n; k++) {
        const uint64_t up = hi + (uint64_t)k * chunk;
        if (up < UINT64_MAX - chunk) want.push_back({up, up + chunk});
        const uint64_t down = (uint64_t)(k + 1) * chunk;
        if (lo >= down) want.push_back({lo - down, lo - down + chunk});
    }
    std::lock_guard lk(bg_mu);
    for (const auto& [a, b] : want) {
        if (b > (1ull << 40) || !in_game_pool(a, b) || imported_over(a, b)) continue;
        if (std::find(bg_inflight.begin(), bg_inflight.end(), a) != bg_inflight.end()) continue;
        if (bg_inflight.size() >= 16) break;
        bg_start();
        bg_inflight.push_back(a);
        bg_queue.push_back({a, b});
        st_prefetch_sent++;
    }
    bg_cv.notify_one();
}

/// (imports by 16 MB chunk (RX_VK_IMPORT_CHUNK_MB), not by pool: each import cost 5-45 ms in the driver, more as their
/// number grew, and the game makes ~13 pools a second; a chunk covers the pools after it at no cost)
uint64_t import_chunk() {
    static const uint64_t chunk = (uint64_t)(getenv("RX_VK_IMPORT_CHUNK_MB") ? std::max(0, atoi(getenv("RX_VK_IMPORT_CHUNK_MB"))) : 16) << 20;
    return chunk;
}
/// (hookshot) A game pool across a chunk's end had each chunk imported on its own, at the first binding in it, and a later
/// binding across the end imported the two again as one, on the replay thread: OoT's 16 MB pool at 0x1021745000 bound at
/// 0x1021ffff00+0x160 across 0x1022000000, 130-147 ms, the one hitch of a panning run in Kokiri. Up to this size an import
/// takes the chunks of the game pools across its ends too (RX_VK_IMPORT_SPAN_MB, default 64; 0: not)
uint64_t import_span() {
    static const uint64_t span = (uint64_t)(getenv("RX_VK_IMPORT_SPAN_MB") ? std::max(0, atoi(getenv("RX_VK_IMPORT_SPAN_MB"))) : 64) << 20;
    return span;
}
/// [lo, hi), chunk-aligned, grown over the game pools that run across its ends, while it stays within import_span (pools_mu held)
void span_game_pools(uint64_t& lo, uint64_t& hi, uint64_t chunk) {
    const uint64_t span = import_span();
    if (!chunk || hi - lo >= span) return;
    for (bool grew = true; grew;) {
        grew = false;
        for (const uint64_t e : {lo, hi}) {
            // (a pool across e that fits starts above e - span: the pools from there to e, as the host's may overlap)
            auto g = std::upper_bound(guest_pools.begin(), guest_pools.end(), e, [](uint64_t v, const GuestPool& p) { return v < p.lo; });
            while (g != guest_pools.begin()) {
                --g;
                if (g->lo + span <= e) break;
                if (g->lo >= e || g->hi <= e) continue;
                const uint64_t nlo = std::min(lo, g->lo / chunk * chunk), nhi = std::max(hi, (g->hi + chunk - 1) / chunk * chunk);
                if (nhi - nlo > span || (nlo == lo && nhi == hi)) continue;
                lo = nlo, hi = nhi, grew = true;
            }
        }
    }
}

void noaccess_import_note(uint64_t bound_lo, uint64_t bound_hi, uint64_t lo, uint64_t hi);   // (below)
void clip_noaccess(uint64_t& lo, uint64_t& hi, uint64_t blo, uint64_t bhi);                  // (below)
Pool* pool_lookup(uint64_t guest, uint64_t size) {
    if (guest_pools.empty()) return nullptr;
    bg_drain();
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                               [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return nullptr;
    --it;
    if (guest < it->lo || guest >= it->hi || !size || size > it->hi - guest) return nullptr;
    const uint64_t chunk = import_chunk();
    const GuestPool range = chunk ? GuestPool{guest, guest + size} : *it;
    auto covering = [&]() {
        auto c = imported.upper_bound(range.lo);
        if (c == imported.begin()) return imported.end();
        --c;
        return c->second.lo <= range.lo && c->second.hi >= range.hi ? c : imported.end();
    };
    auto imp = chunk ? covering() : imported.find(range.lo);
    if (chunk && range.hi > UINT64_MAX - (chunk - 1)) return nullptr;
    const uint64_t want_lo = chunk ? range.lo / chunk * chunk : range.lo;
    const uint64_t want_hi = chunk ? (range.hi + chunk - 1) / chunk * chunk : range.hi;
    if (chunk && imp == imported.end()) imp = imported.find(want_lo);   // (a smaller one there: made again, larger)
    if (imp == imported.end() || imp->second.hi < range.hi) {   // (under way in the background: waited for)
        bool waited = false;
        {
            std::unique_lock lk(bg_mu);
            auto under_way = [&] {   // (the pool's own import, or a chunk of the range imported ahead)
                for (const uint64_t x : bg_inflight)
                    if (x == range.lo || (chunk && x >= want_lo && x < want_hi)) return true;
                return false;
            };
            if (under_way()) {
                bg_done_cv.wait(lk, [&] { return !under_way(); });
                waited = true;
                st_bg_waits++;
            }
        }
        if (waited) {
            bg_drain();
            imp = chunk ? covering() : imported.find(range.lo);
            if (chunk && imp == imported.end()) imp = imported.find(want_lo);
        }
    }
    if (imp == imported.end() || imp->second.hi < range.hi || imp->second.lo > range.lo) {
        // (the new pool takes in every imported pool its range overlaps, and they are retired: the imported pools stay
        // disjoint. A range crossing a chunk's end made the chunk's pool again, larger, over the next chunk, whose own
        // pool stayed: two mirrors of the same memory. MKW's GPU culling wrote its instance data (a 2.4 MB storage
        // buffer) through one, the draws read it through the other, synced from guest memory before the submission's
        // write-back: the instances of a frame before or older, objects popping in and out, one-frame flashes.)
        uint64_t lo = 0, hi = 0;
        std::vector<uint64_t> overlapped;
        auto plan = [&](bool spanned) {   // (the range, the imported pools it takes in; spanned: span_game_pools's chunks too)
            lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
            if (spanned && chunk) span_game_pools(lo, hi, chunk);
            if (chunk) clip_noaccess(lo, hi, range.lo, range.hi);   // (HOS_NOACCESS_RAM)
            for (bool grew = true; grew;) {
                grew = false;
                overlapped.clear();
                auto it = imported.lower_bound(hi);   // (the pools starting below hi, the last ones first)
                while (it != imported.begin()) {
                    --it;
                    if (it->second.hi <= lo) {
                        if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
                        continue;
                    }
                    overlapped.push_back(it->first);
                    if (it->second.lo < lo) lo = it->second.lo, grew = true;
                    if (it->second.hi > hi) hi = it->second.hi, grew = true;
                }
            }
            if (hi - lo > (1ull << 30)) {   // (no larger than 1 GB: then as before, the chunk's pool alone, overlaps logged)
                static int logged_big;
                if (logged_big++ < 8)
                    logf("vk: pool %#llx..%#llx would take in %zu pools, %llu MB: left overlapping", (unsigned long long)lo,
                         (unsigned long long)hi, overlapped.size(), (unsigned long long)((hi - lo) >> 20));
                lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
                overlapped.clear();
                if (imp != imported.end()) overlapped.push_back(imp->first);
            }
        };
        plan(true);
        const uint64_t spanned_lo = lo, spanned_hi = hi;
        Pool p;
        bool ok = false;
        for (int pass = 0; pass < 2 && !ok; pass++) {
            if (pass) {   // (a spanned import that failed: the range without the span, as imported before)
                plan(false);
                if (lo == spanned_lo && hi == spanned_hi) break;
            }
            const auto t0 = std::chrono::steady_clock::now();
            ok = import_pool(lo, hi, p);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            static double total_ms;
            static int n, logged;
            total_ms += ms, n++;
            if ((ms >= 2.0 && logged++ < 200) || n % 256 == 0)
                logf("vk: pool %#llx+%#llx imported in %.1f ms as %#llx..%#llx (%d imports, %.0f ms in all)",
                     (unsigned long long)range.lo, (unsigned long long)(range.hi - range.lo), ms, (unsigned long long)lo,
                     (unsigned long long)hi, n, total_ms);
        }
        if (!ok) {
            if (!import_cooling && failures++ < 16) {
                if (import_result != VK_SUCCESS)
                    logf("vk: pool %#llx+%#llx not imported (VkResult %d)", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo), (int)import_result);
                else
                    logf("vk: pool %#llx+%#llx not imported", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo));
            }
            return nullptr;
        }
        for (uint64_t k : overlapped) {   // (the pools it took in)
            auto o = imported.find(k);
            if (o != imported.end()) retire_pool(o);
        }
        imp = imported.emplace(p.lo, p).first;
        guest_pages_set(p.lo, p.hi, p.address);
        noaccess_import_note(range.lo, range.hi, p.lo, p.hi);
        if (chunk) prefetch_chunks(p.lo, p.hi, chunk);
        if (overlapped.size() > 1) {
            static int logged_merge;
            if (logged_merge++ < 16)
                logf("vk: pool %#llx..%#llx took in %zu pools", (unsigned long long)p.lo, (unsigned long long)p.hi,
                     overlapped.size());
        }
        static int logged;
        if (logged++ < 8 || imported.size() % 256 == 0) logf("vk: %zu pools imported", imported.size());
    }
    if (imp->second.prefetched) {   // (first bound: its neighbours ahead in turn)
        imp->second.prefetched = false;
        st_prefetch_used++;
        prefetch_chunks(imp->second.lo, imp->second.hi, chunk);
        static int64_t logged_at;
        if (st_prefetch_used / 16 != logged_at) {
            logged_at = st_prefetch_used / 16;
            logf("vk: chunks imported ahead: %lld sent, %lld bound, %lld dropped; %lld waits", (long long)st_prefetch_sent,
                 (long long)st_prefetch_used, (long long)st_prefetch_dropped, (long long)st_bg_waits);
        }
    }
    imp->second.range_lo = range.lo, imp->second.range_hi = range.hi;
    return last_pool = &imp->second;
}

// ---- the VRAM mirror (after totk-pc-recomp's vk_memory.c) ----
// An imported pool keeps vertices, indices, constants and storage buffers in system memory, so a discrete GPU reads
// them over PCIe on every draw (TOTK measured ~110 ms of GPU time a frame on a 3090 Ti). A pool bound as a buffer
// gets a device-local copy, made at the start of the next submission, and from then on its bindings use the copy at
// the same offsets:
// - CPU -> mirror: at a submission's first use of a mirror, the pages the CPU wrote since its last sync (the page
//   write generations) are copied in, in the recording's upload command buffer (submitted right before it).
// - GPU -> guest: what the submission's shaders and copies may write into a mirror is copied back into the imported
//   pool at the submission's end, so the CPU (and everything that reads guest memory) sees it.
// RX_VK_MIRROR=0: no mirrors; RX_VK_MIRROR_MB: their budget (default 3072).
bool mirror_on() {
    static const int on = [] {
        const char* e = getenv("RX_VK_MIRROR");
        const bool discrete = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        const int v = e ? atoi(e) != 0 : discrete;
        logf("vk: VRAM mirror of buffer pools %s", v ? "on" : "off");
        return v;
    }();
    return on != 0;
}

/// (hookshot, smoothness) A read binding in a chunk not imported yet: its import goes to the background thread and the binding is
/// served from a copy in the recording's stream, rather than the replay waiting for the import. Imports on the replay thread were
/// the main hitch left in play (54-61 a run of the verification walks, 5-380 ms each, 2026-11-09). True: an import of the range is
/// under way (sent now or before) and nothing covers it yet; false: the synchronous path as before (covered, outside the game's
/// pools, or next to an import that would have to grow). RX_VK_ASYNC_IMPORT=1 (opt-in: background imports slowed AMD before) (pools_mu held)
int64_t st_async_sent, st_async_copies;
bool import_in_background(uint64_t guest, uint64_t size) {
    if (guest_pools.empty() || !size || !dev.external_memory_host) return false;
    bg_drain();
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest, [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return false;
    --it;
    if (guest < it->lo || guest >= it->hi || size > it->hi - guest) return false;
    const uint64_t chunk = import_chunk();
    if (!chunk || guest + size > UINT64_MAX - chunk) return false;
    if (auto c = imported.upper_bound(guest); c != imported.begin() && std::prev(c)->second.lo <= guest &&
                                              std::prev(c)->second.hi >= guest + size)
        return false;   // (covered)
    const uint64_t lo = guest / chunk * chunk, hi = (guest + size + chunk - 1) / chunk * chunk;
    for (auto o = imported.lower_bound(hi); o != imported.begin();) {   // (an import there: the synchronous path grows it)
        --o;
        if (o->second.hi <= lo) {
            if (o->second.hi + (1ull << 30) < lo) break;
            continue;
        }
        return false;
    }
    std::lock_guard lk(bg_mu);
    for (const uint64_t x : bg_inflight)
        if (x >= lo && x < hi) return true;   // (under way)
    if (bg_inflight.size() >= 16) return false;
    bg_start();
    bg_inflight.push_back(lo);
    bg_queue.push_back({lo, hi});
    bg_cv.notify_one();
    if (st_async_sent++ % 32 == 0)
        logf("vk: imports in the background: %lld sent, %lld bindings served from a copy meanwhile", (long long)st_async_sent,
             (long long)st_async_copies);
    return true;
}
} // namespace
uint64_t device_vram_bytes() {
    uint64_t vram = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)dev.memory.memoryHeaps[h].size);
    return vram;
}
bool preset_caps_memory() {
    static const uint64_t nominal_gb[] = {0, 4, 6, 8, 12, 16};
    if (opts.preset <= 0 || opts.preset > 5) return true;   // (Custom: what the file says)
    const uint64_t vram = device_vram_bytes();
    return !vram || vram <= ((nominal_gb[opts.preset] << 30) + (768ull << 20));
}
namespace {
uint64_t mirror_budget() {
    // (a quarter of the card, at most the 3 GB it was for all: on a 6 GB laptop those and the textures were past what the card has)
    static const uint64_t b = [] {
        if (const char* e = getenv("RX_VK_MIRROR_MB")) return (uint64_t)atoi(e) << 20;
        uint64_t vram = 0;
        for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
            if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)dev.memory.memoryHeaps[h].size);
        // (cards of 6 GB or less only: on 8 GB ones, a 3070 or a 4060, the game's memory read over PCIe instead left the GPU at a few
        // percent and the frame stuttering)
        if (vram && vram <= (6ull << 30) + (512ull << 20)) return vram / 8;
        return vram ? std::min<uint64_t>(vram / 4, 3072ull << 20) : (uint64_t)(3072ull << 20);
    }();
    // (the Insert menu's quality preset sets one, 256 MB at Minimum (4GB): it was never read, and an 8 GB card kept ~2 GB of
    // mirrors with the 4GB preset on, 2026-11-08)
    // (only on a card the preset is for: preset_caps_memory)
    if (opts.mirror_mb > 0 && !getenv("RX_VK_MIRROR_MB") && preset_caps_memory()) return (uint64_t)opts.mirror_mb << 20;
    return b;
}
uint64_t mirror_bytes = 0, mirror_epoch = 0;
bool mirror_upload = false;   // (a submission is being recorded: its upload command buffer takes the syncs)
std::vector<Pool*> mirror_wanted, mirror_synced;
struct WriteBack {
    Pool* pool;
    uint64_t lo, hi;   // offsets in the pool
};
std::vector<WriteBack> writebacks;

// ---- (TICK-015) Write-backs left out: the game's pools the CPU does not read. Every range a submission's shaders write is
// copied from the mirror back to guest memory (69 MB a frame in Hyrule Field, 1.6 ms of GPU time), almost all of it into
// CPU_CACHED pools (NVN flags 0x24) the CPU never reads back: on the console a CPU read of GPU results in a cached pool needs
// nvnMemoryPoolInvalidateMappedRange first, and CPU_NO_ACCESS pools cannot be read at all. So a game pool's write-backs are
// left in the mirror once it has been written back for kLazyWarm submissions with neither an invalidate of it (the CPU reads
// it: memory_cpu_reads) nor a CPU write to it (a mirror sync's dirty page: a stale page from guest memory would go over the
// GPU's words). Either makes the pool eager for good. Not left out: CPU_UNCACHED pools (read without an invalidate), ranges
// outside any game pool, ranges where a live texture's storage lies (re-uploaded from guest memory). A mirror holding such
// writes is kept (not given back: the GPU would read stale guest memory). The counters (every 600 submissions) say how often
// a read or a write came after a range was left out (once each per pool at most: then eager). RX_VK_WB_EAGER=1: all, as before
struct NvnPool {
    uint64_t hi = 0;
    uint32_t flags = 0;
    uint64_t first_wb = 0;   // (mirror_epoch + 1 of its first write-back)
    bool eager = false, skipped = false;
    uint8_t why = 0;   // (eager: 1 read by the CPU, 2 written by it)
};
std::mutex nvn_pools_mu;   // (after pools_mu when both)
std::map<uint64_t, NvnPool> nvn_pools;   // by guest lo
std::unordered_set<uint32_t> cpu_pages, skipped_pages;   // (guest pages the CPU writes, and the ones whose GPU writes were left out; nvn_pools_mu held)
constexpr uint64_t kLazyWarm = 600;
std::atomic<uint64_t> nvn_pools_gen{1};   // (bumped when a pool is made: lazy_cpu_page's cache)
int64_t st_lazy_bytes = 0, st_lazy_late = 0, st_lazy_cpu = 0, st_lazy_pools = 0;
std::atomic<int64_t> st_lazy_direct{0};
std::atomic<int64_t> st_direct_fast{0}, st_direct_fast_bytes{0};   // (TICK-019: guest-memory binds on the lock-free path, not in direct_by_pool)
int64_t st_kept_by[6];   // (bytes written back, by why: 0 no game pool, 1 read, 2 written by the CPU, 3 uncached, 4 warming, 5 texture)   // (bindings without the lock too)
bool wb_lazy_on() {
    static const bool v = !getenv("RX_VK_WB_EAGER") || getenv("RX_VK_WB_EAGER")[0] == '0';
    return v;
}
NvnPool* nvn_pool_at(uint64_t g, uint64_t* lo) {   // (nvn_pools_mu held)
    auto it = nvn_pools.upper_bound(g);
    if (it == nvn_pools.begin()) return nullptr;
    --it;
    if (g >= it->second.hi) return nullptr;
    if (lo) *lo = it->first;
    return &it->second;
}
void nvn_pools_eager(uint64_t lo, uint64_t hi, bool cpu_write) {   // (nvn_pools_mu held)
    auto it = nvn_pools.upper_bound(lo);
    if (it != nvn_pools.begin()) --it;
    for (; it != nvn_pools.end() && it->first < hi; ++it) {
        NvnPool& np = it->second;
        if (np.hi <= lo || np.eager) continue;
        np.eager = true;
        np.why = cpu_write ? 2 : 1;
        if (np.skipped) (cpu_write ? st_lazy_cpu : st_lazy_late)++;
        for (uint64_t q = std::max(lo, it->first) >> kPageShift; q < (std::min(hi, np.hi) + 4095) >> kPageShift; q++)
            skipped_pages.erase((uint32_t)q);
        static int logged;
        if (logged++ < 40 || np.skipped)
            logf("vk: write-backs: pool %#llx (+%#llx, flags %#x) %s%s: written back from now on",
                 (unsigned long long)it->first, (unsigned long long)(np.hi - it->first), np.flags,
                 cpu_write ? "written by the CPU" : "read by the CPU (invalidate)",
                 np.skipped ? " AFTER its write-backs were left out" : "");
    }
}
// (HOS_NOACCESS_RAM) An import [lo, hi) made for a binding [blo, bhi) leaves out the game's CPU_NO_ACCESS pools the binding is not
// in: the 16 MB chunks (and the spans over pools across their ends) took in 173 MB of them around bindings of neighbouring pools on
// a castle walk, and the driver locks an import's pages into RAM (zero pages, as the game never writes there). Cut at the import
// alignment, never into the binding. A binding inside such a pool imports it as before. (pools_mu held)
void clip_noaccess(uint64_t& lo, uint64_t& hi, uint64_t blo, uint64_t bhi) {
    if (!noaccess_ram_on() || hi <= lo) return;
    const uint64_t a = align ? align : 4096;
    auto down = [a](uint64_t v) { return v / a * a; };
    auto up = [a](uint64_t v) { return (v + a - 1) / a * a; };
    std::lock_guard lk(nvn_pools_mu);
    auto it = nvn_pools.upper_bound(lo);
    if (it != nvn_pools.begin()) --it;
    for (; it != nvn_pools.end() && it->first < hi; ++it) {
        const uint64_t plo = it->first, phi = it->second.hi;
        if (!(it->second.flags & 0x1) || phi <= lo) continue;
        if (phi <= blo) lo = std::max(lo, std::min(up(phi), down(blo)));        // (below the binding)
        else if (plo >= bhi) hi = std::min(hi, std::max(down(plo), up(bhi)));   // (above it)
    }
}
// (RX_VK_LOG_NOACCESS_IMPORT=1, diagnostic: an import that takes in pages of CPU_NO_ACCESS pools, the driver locking them into
// RAM, and the binding that asked for it, with its pool's flags. pools_mu held)
void noaccess_import_note(uint64_t bound_lo, uint64_t bound_hi, uint64_t lo, uint64_t hi) {
    static const bool on = getenv("RX_VK_LOG_NOACCESS_IMPORT") != nullptr;
    if (!on) return;
    std::lock_guard lk(nvn_pools_mu);
    uint64_t na = 0;
    auto it = nvn_pools.upper_bound(lo);
    if (it != nvn_pools.begin()) --it;
    for (; it != nvn_pools.end() && it->first < hi; ++it)
        if ((it->second.flags & 1) && it->second.hi > lo) na += std::min(hi, it->second.hi) - std::max(lo, it->first);
    if (!na) return;
    const NvnPool* bp = nvn_pool_at(bound_lo, nullptr);
    static int64_t total;
    total += (int64_t)na;
    logf("vk: noaccess import: %#llx..%#llx (%llu MB) for a binding %#llx+%#llx in a pool of flags %#x: %llu MB of CPU_NO_ACCESS "
         "pools in it (%lld MB so far)", (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)((hi - lo) >> 20),
         (unsigned long long)bound_lo, (unsigned long long)(bound_hi - bound_lo), bp ? bp->flags : 0u, (unsigned long long)(na >> 20),
         (long long)(total >> 20));
}
// (a mirror sync copies a page the CPU wrote: that page is written back from now on; if its GPU writes were left out, the
// copy from guest memory goes over them (counted). pools_mu held)

void lazy_cpu_page(uint64_t page) {
    std::lock_guard lk(nvn_pools_mu);
    NvnPool* np = nvn_pool_at(page << kPageShift, nullptr);
    if (!np || np->eager) return;
    cpu_pages.insert((uint32_t)page);
    if (skipped_pages.erase((uint32_t)page)) {
        st_lazy_cpu++;
        static int logged;
        if (logged++ < 16)
            logf("vk: write-backs: page %#llx (pool flags %#x) written by the CPU after its GPU writes were left out: written back "
                 "from now on", (unsigned long long)(page << kPageShift), np->flags);
    }
}
// (TICK-056, RX_VK_MSYNC_FAST) lazy_cpu_page for a mirror sync's dirty pages at once, in their order: the lock taken once, and the
// game pool looked up only when a page leaves the last one found (a sync's pages are in address order). The same sets result.
// A page already put in cpu_pages here is marked in a bitmap (by chunks of 64K pages) and skipped: cpu_pages never loses a page,
// and skipped_pages takes only pages cpu_pages does not hold (lazy_filter), so for such a page both calls below change nothing
// (the castle-move pre-sync's ~8,600 dirty pages a frame were each a hash insert and erase)
std::vector<std::unique_ptr<uint64_t[]>> cpu_page_bits;   // (nvn_pools_mu held)
void lazy_cpu_pages(const std::vector<uint32_t>& pages) {
    if (pages.empty()) return;
    std::lock_guard lk(nvn_pools_mu);
    if (cpu_page_bits.empty()) cpu_page_bits.resize((size_t)(kPages / 65536));
    uint64_t lo = 1, hi = 0;   // (the last pool found, or the gap between two: [lo, hi))
    NvnPool* np = nullptr;
    for (const uint32_t page : pages) {
        const uint64_t g = (uint64_t)page << kPageShift;
        if (!(g >= lo && g < hi)) {
            np = nvn_pool_at(g, &lo);
            if (np) {
                hi = np->hi;
            } else {   // (no pool here: none up to the next one's start)
                const auto nx = nvn_pools.upper_bound(g);
                lo = g, hi = nx == nvn_pools.end() ? UINT64_MAX : nx->first;
            }
        }
        if (!np || np->eager) continue;
        std::unique_ptr<uint64_t[]>& bits = cpu_page_bits[page >> 16];
        if (!bits) bits.reset(new uint64_t[65536 / 64]());   // (zeroed)
        uint64_t& word = bits[(page & 65535) >> 6];
        const uint64_t bit = 1ull << (page & 63);
        if (word & bit) continue;   // (in cpu_pages already, and so not in skipped_pages)
        word |= bit;
        cpu_pages.insert(page);
        if (skipped_pages.erase(page)) {
            st_lazy_cpu++;
            static int logged;
            if (logged++ < 16)
                logf("vk: write-backs: page %#llx (pool flags %#x) written by the CPU after its GPU writes were left out: written back "
                     "from now on", (unsigned long long)g, np->flags);
        }
    }
}
// (the runs of p to copy back: the parts left out removed)
void lazy_filter(Pool& p, std::vector<VkBufferCopy>& runs) {
    std::lock_guard lk(nvn_pools_mu);
    if (nvn_pools.empty()) return;
    std::vector<VkBufferCopy> out;
    for (const VkBufferCopy& r : runs) {
        uint64_t g = p.lo + r.srcOffset;
        const uint64_t end = g + r.size;
        while (g < end) {
            uint64_t lo = 0;
            NvnPool* np = nvn_pool_at(g, &lo);
            uint64_t seg_end;
            bool keep = true;
            int why = 0;
            if (!np) {
                auto nx = nvn_pools.upper_bound(g);
                seg_end = nx == nvn_pools.end() ? end : std::min(end, nx->first);
            } else {
                why = np->eager ? np->why : 3;
                seg_end = std::min(end, np->hi);
                // (CPU_NO_ACCESS 0x1 or CPU_CACHED 0x4, not CPU_UNCACHED 0x2)
                const bool may_defer = (np->flags & (0x1u | 0x4u)) &&
                                      !(np->flags & 0x2u);
                if (!np->eager && may_defer) {
                    if (!np->first_wb) np->first_wb = mirror_epoch + 1;
                    why = mirror_epoch + 1 - np->first_wb < kLazyWarm ? 4 : 5;
                    if (why == 5 && !replay_tex_storage_in(g, seg_end - g)) {
                        // (page by page: the pages the CPU writes are written back)
                        const uint64_t pg_end = std::min(seg_end, ((g >> kPageShift) + 1) << kPageShift);
                        if (cpu_pages.count((uint32_t)(g >> kPageShift))) {
                            why = 2, seg_end = pg_end;
                        } else {
                            uint64_t e = pg_end;
                            while (e < seg_end && !cpu_pages.count((uint32_t)(e >> kPageShift))) e = std::min(seg_end, e + 4096);
                            seg_end = e;
                            keep = false;
                            if (!np->skipped) st_lazy_pools++;
                            np->skipped = true;
                            for (uint64_t q = g >> kPageShift; q <= (seg_end - 1) >> kPageShift; q++) skipped_pages.insert((uint32_t)q);
                        }
                    }
                }
            }
            if (keep) {
                st_kept_by[why] += (int64_t)(seg_end - g);
                const uint64_t a = g - p.lo;
                if (!out.empty() && out.back().srcOffset + out.back().size == a) out.back().size += seg_end - g;
                else out.push_back({a, a, seg_end - g});
            } else {
                st_lazy_bytes += (int64_t)(seg_end - g);
                p.lazy_held = true;
            }
            g = seg_end;
        }
    }
    runs.swap(out);
}
// (hookshot, 11-07) The counter reports the replay writes into guest memory on the CPU (vk_replay.cpp NOP_REPORT_COUNTER), by
// guest address: a mirrored pool's write-back copied the GPU's mirror over them at the end of the submission, the mirror's
// stale words over the replay's newer ones, and the next render condition read those: a lighting pass left out, the whole lit
// scene black for a frame (the one-frame dark flashes, every 15-30 frames; 17 in 40 s of Kokiri, 0 with mirrors off). They are
// left out of the write-back now. RX_VK_WB_REPORTS=1: written back with the rest, as before
std::mutex cpu_words_mu;
// (two sorted sets with no word in both, and the words noted since the last merge: those go into the recent set, which
// goes into the big one only once it is an eighth of its size. A std::set took a node a word and freed 65536 of them at
// once when it filled, ~4% of the replay thread in a Kokiri profile; one sorted vector with every write-back's new words
// merged in moved the whole of it each time, ~2.4% on a 3900X there (11-08): report words come at new addresses all the time)
std::vector<uint64_t> cpu_words, cpu_words_recent, cpu_words_new;
void sorted_merge(std::vector<uint64_t>& into, const uint64_t* b, const uint64_t* e) {
    const size_t mid = into.size();
    into.insert(into.end(), b, e);
    std::inplace_merge(into.begin(), into.begin() + (ptrdiff_t)mid, into.end());
}
void cpu_words_merge() {   // (holding cpu_words_mu)
    if (cpu_words_new.empty()) return;
    std::sort(cpu_words_new.begin(), cpu_words_new.end());
    cpu_words_new.erase(std::unique(cpu_words_new.begin(), cpu_words_new.end()), cpu_words_new.end());
    // (only the words not kept already: the same report words come every frame)
    size_t fresh = 0;
    for (const uint64_t w : cpu_words_new)
        if (!std::binary_search(cpu_words.begin(), cpu_words.end(), w) &&
            !std::binary_search(cpu_words_recent.begin(), cpu_words_recent.end(), w))
            cpu_words_new[fresh++] = w;
    if (fresh) sorted_merge(cpu_words_recent, cpu_words_new.data(), cpu_words_new.data() + fresh);
    if (cpu_words_recent.size() > std::max<size_t>(4096, cpu_words.size() / 8)) {
        sorted_merge(cpu_words, cpu_words_recent.data(), cpu_words_recent.data() + cpu_words_recent.size());
        cpu_words_recent.clear();
    }
    cpu_words_new.clear();
}
} // namespace
// (HeyListen, 2026-11-13) HOS_NOACCESS_RAM (on; =0 off; host/hos_memsplit.c has the whole of it): the pools the game made with
// NVN's CPU_NO_ACCESS flag keep no zero pages in RAM. The back end's part: no import of such a pool's pages for another pool's
// binding (clip_noaccess below), and no upload of a texture whose storage lies in such a pool and is all reserved
// (texture_touch: the image is made cleared to zero, which is what the storage reads as)
bool memory_nvn_pool_of(uint64_t guest, uint64_t* lo, uint64_t* hi, uint32_t* flags) {
    std::lock_guard lk(nvn_pools_mu);
    const NvnPool* np = nvn_pool_at(guest, lo);
    if (!np) return false;
    *hi = np->hi, *flags = np->flags;
    return true;
}
bool noaccess_ram_on() {
    static const bool on = [] {
        const char* e = getenv("HOS_NOACCESS_RAM");
        const char* lazy = getenv("HOS_LAZY_COMMIT");
        return !(e && e[0] == '0') && !(lazy && lazy[0] == '0') && !getenv("HOS_LARGE_PAGES");
    }();
    return on;
}
bool memory_in_noaccess_pool(uint64_t guest, uint64_t size) {
    if (!size || guest >= (1ull << 40)) return false;
    std::lock_guard lk(nvn_pools_mu);
    uint64_t lo = 0;
    const NvnPool* np = nvn_pool_at(guest, &lo);
    return np && (np->flags & 0x1) && guest + size <= np->hi;
}
bool memory_all_reserved(uint64_t guest, uint64_t size) {
#ifdef _WIN32
    if (!size || guest >= (1ull << 40) || guest + size > host->guest_size()) return false;
    const uint8_t* p = host->guest_base() + guest;
    const uint8_t* const end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi{};
        if (VirtualQuery(p, &mi, sizeof mi) != sizeof mi || mi.State != MEM_RESERVE) return false;
        p = (const uint8_t*)mi.BaseAddress + mi.RegionSize;
    }
    return true;
#else
    (void)guest, (void)size;
    return false;   // (linux: the window is one mapping, decommitted pages read as zero but are not told apart)
#endif
}
void memory_cpu_reads(uint64_t guest, uint64_t size) {
    if (!guest || !size || !wb_lazy_on()) return;
    std::lock_guard lk(nvn_pools_mu);
    nvn_pools_eager(guest, guest + size, false);
}
void memory_cpu_owned(uint64_t guest, uint64_t size) {
    static const bool off = getenv("RX_VK_WB_REPORTS") != nullptr;
    if (off || !guest) return;
    std::lock_guard lk(cpu_words_mu);
    if (cpu_words.size() + cpu_words_recent.size() + cpu_words_new.size() > 65536) {   // (as before: forgotten when it gets this big)
        cpu_words_merge();   // (which leaves cpu_words_new empty)
        if (cpu_words.size() + cpu_words_recent.size() > 65536) cpu_words.clear(), cpu_words_recent.clear();
    }
    for (uint64_t a = guest & ~7ull; a < guest + size; a += 8) cpu_words_new.push_back(a);
    // (written on the CPU: their pages unknown to the write-back diff's shadows)
    for (uint64_t pg = guest >> kPageShift; pg <= (guest + std::max<uint64_t>(size, 1) - 1) >> kPageShift; pg++) {
        const uint32_t page = (uint32_t)pg;
        shadow_written(&page, 1);
    }
}
namespace {
// (a write-back's runs with the replay's own words cut out of them: runs of p, offsets in the pool)
void cut_cpu_words(const Pool& p, std::vector<VkBufferCopy>& runs) {
    std::lock_guard lk(cpu_words_mu);
    cpu_words_merge();
    if (cpu_words.empty() && cpu_words_recent.empty()) return;
    std::vector<VkBufferCopy> out;
    for (const VkBufferCopy& r : runs) {
        uint64_t at = r.srcOffset;
        const uint64_t end = r.srcOffset + r.size;
        // A run can start inside an owned word: include that word when cutting the overlap.
        const uint64_t first = (p.lo + at) & ~7ull, last = p.lo + end;
        auto a = std::lower_bound(cpu_words.begin(), cpu_words.end(), first);
        auto b = std::lower_bound(cpu_words_recent.begin(), cpu_words_recent.end(), first);
        for (;;) {   // (both sets' words in the run, in order)
            const bool in_a = a != cpu_words.end() && *a < last, in_b = b != cpu_words_recent.end() && *b < last;
            if (!in_a && !in_b) break;
            const uint64_t w = (in_a && (!in_b || *a < *b) ? *a++ : *b++) - p.lo;
            if (w > at) out.push_back({at, at, w - at});
            at = std::max(at, w + 8);
        }
        if (end > at) out.push_back({at, at, end - at});
    }
    runs.swap(out);
}

// ---- a pool remade larger (a 16 MB chunk the game's next pool runs past): the old one leaves the table but stays
// alive, as this submission's bindings, its mirror syncs and its write-backs still use it. Erasing it left those
// lists pointing into a freed map node: the write-back copied with garbage handles and the device was lost (the
// daytime title scene, whose pools cross chunk ends). Retired pools are kept (a few, never reused). ----
// (Extracted from the table, not erased: the node, and the Pool in it, stay where they were, so the mirror's lists and
// a parallel replay thread's pool cache still point at a live one.)
std::deque<std::map<uint64_t, Pool>::node_type> retired_pools;

void retire_pool(std::map<uint64_t, Pool>::iterator it) {
    Pool* old = &it->second;
    if (inherit_watch_page() != ~0ull && (inherit_watch_page() << kPageShift) >= old->lo && (inherit_watch_page() << kPageShift) < old->hi)
        logf("vk: inherit watch: pool %#llx..%#llx retired (mirror state %d, synced gen %u, gen now %u)", (unsigned long long)old->lo,
             (unsigned long long)old->hi, (int)old->mstate, (unsigned)old->synced_gen, gen_now);
    shadow_free(*old);   // (its write-backs in this submission, if any, copied whole)
    guest_pages_set(old->lo, old->hi, 0);
    old->retired_value = std::max(old->last_value, mem_rec_value);
    std::erase(mirror_wanted, old);   // (the new pool gets the mirror)
    if (old->mstate == Pool::MIRROR_WANTED) old->mstate = Pool::MIRROR_NONE;
    // (EvilOOT, 11-07) what the GPU wrote into its mirror and nowhere else (the culling's bare table entries past their first
    // 64 bytes, stores through the page table) was lost here: the new pool's mirror is filled from guest memory. Kokiri's
    // foliage records, written by the GPU while the scene loaded and its pools were remade larger, came out zero: every
    // count 0, the trees' leaf clusters never drawn. RX_VK_NO_INHERIT=1: as before
    static const bool no_inherit = getenv("RX_VK_NO_INHERIT") != nullptr;
    if (old->mstate == Pool::MIRROR_ON && !no_inherit) old->inherit_pending = true, old->retired_epoch = mirror_epoch;
    pools_gen++;
    retired_pools.push_back(imported.extract(it));
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8 || retired_pools.size() % 64 == 0)
        logf("vk: pool %#llx remade larger (%zu retired)", (unsigned long long)old->lo, retired_pools.size());
}
int64_t st_sync_bytes, st_sync_copies, st_wb_bytes, st_mirrors;
int64_t st_inherit_kept, st_inherit_newer, st_inherit_unfilled;   // (TICK-036: bytes a new mirror took from retired mirrors / from guest memory instead)
int64_t st_sync_live_pages, st_sync_cap_pages, st_sync_old_pages;   // (mirror syncs' dirty pages: live / this submit's capture / an earlier one's)
std::unordered_map<uint64_t, int64_t> live_by_pool;
std::unordered_map<uint64_t, std::pair<int64_t, int>> direct_by_pool;   // (bindings the GPU reads in guest memory: count, state)
std::unordered_map<uint64_t, uint64_t> direct_bytes_by_pool;           // (and the bytes of those bindings: what the GPU may read over PCIe, a binding's whole range)
int64_t live_seen = 0;   // (pool -> pages its syncs read live, since the last report)

void transfer_barrier(VkCommandBuffer cmd) {
    full_barrier(cmd, "barrier (write-back or sync)");   // (the same barrier; one right after another is left out)
}

// ---- (TICK-021) the mirror syncs' scatter copy ----
// The syncs copied ~1570 regions a frame of 4-8 KB each (dirty pages, scattered) with vkCmdCopyBuffer: 1.3 ms of GPU time for
// ~11 MB, ~0.8 us a region whatever its size. Here the regions are cut into entries of at most 4 KB, listed in a host-visible
// ring, and one compute dispatch (shaders/msync_scatter.comp, buffer device addresses) copies them all: a workgroup an entry.
// The presync (memory_sync_mirrors_before_segments) batches every pool's into one dispatch; a sync at a binding is one of its
// own. A full barrier before and after (the upload command buffer's copies, the mirrors' readers). Regions not 16-byte aligned
// go to vkCmdCopyBuffer as before. RX_VK_MSYNC_SCATTER=0: all of them as before
struct ScatterEntry { uint64_t src, dst; uint32_t size, pad[3]; };
static_assert(sizeof(ScatterEntry) == 32);
struct Scatter {
    bool tried = false, ok = false;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkBuffer ring = VK_NULL_HANDLE;
    VmaAllocation ring_alloc = nullptr;
    ScatterEntry* map = nullptr;
    VkDeviceAddress ring_addr = 0;
    size_t pos = 0;
    uint64_t lap_value = 0, prev_lap_value = 0;   // (the newest recording that listed entries in this lap, and in the last one)
} scatter;
constexpr size_t kScatterEntries = 1u << 17;   // (4 MB of entries: ~45 frames of Hyrule Field's)
std::mutex scatter_mu;
thread_local std::vector<ScatterEntry> scatter_pending;
thread_local bool scatter_batch = false;
std::atomic<int64_t> st_scatter_entries{0}, st_scatter_dispatches{0}, st_scatter_bytes{0};
bool scatter_on() {
    static const bool v = !getenv("RX_VK_MSYNC_SCATTER") || getenv("RX_VK_MSYNC_SCATTER")[0] != '0';
    return v;
}
bool scatter_ready() {   // (scatter_mu held)
    if (scatter.tried) return scatter.ok;
    scatter.tried = true;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(MSYNC_SCATTER_COMP_SPV), mi.pCode = MSYNC_SCATTER_COMP_SPV;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1, li.pPushConstantRanges = &pr;
    bool ok = vkCreatePipelineLayout(dev.device, &li, nullptr, &scatter.layout) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = module, ci.stage.pName = "main";
        ci.layout = scatter.layout;
        ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &scatter.pipeline) == VK_SUCCESS;
    }
    vkDestroyShaderModule(dev.device, module, nullptr);
    void* m = nullptr;
    MemKindScope mem_kind_(MemKind::Staging);   // (TICK-038: RX_VK_RES_LOG)
    ok = ok && create_buffer(kScatterEntries * sizeof(ScatterEntry), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             true, scatter.ring, scatter.ring_alloc, &m, false) && m;
    if (ok) {
        scatter.map = (ScatterEntry*)m;
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = scatter.ring;
        scatter.ring_addr = vkGetBufferDeviceAddress(dev.device, &da);
        ok = scatter.ring_addr != 0;
    }
    scatter.ok = ok;
    logf("vk: mirror syncs: scatter copy %s", ok ? "on" : "NOT available (copies as before)");
    return ok;
}
VkDeviceAddress buffer_address(VkBuffer b) {
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    return vkGetBufferDeviceAddress(dev.device, &da);
}
/// (regions of src -> dst queued for the scatter, cut into 4 KB entries; false: not possible, copy them as before)
bool scatter_add(VkDeviceAddress src, VkDeviceAddress dst, const std::vector<VkBufferCopy>& rs) {
    if (!scatter_on() || !src || !dst) return false;
    {
        std::lock_guard lk(scatter_mu);
        if (!scatter_ready()) return false;
    }
    uint64_t entries = 0;
    for (const VkBufferCopy& r : rs) entries += (r.size + 4095) / 4096;
    if (scatter_pending.size() + entries > kScatterEntries / 2) return false;   // (a load's hundreds of MB: plain copies)
    for (const VkBufferCopy& r : rs)
        if (((src + r.srcOffset) | (dst + r.dstOffset) | r.size) & 15) return false;
    for (const VkBufferCopy& r : rs)
        for (uint64_t o = 0; o < r.size; o += 4096)
            scatter_pending.push_back({src + r.srcOffset + o, dst + r.dstOffset + o, (uint32_t)std::min<uint64_t>(4096, r.size - o), {}});
    return true;
}
/// The queued entries, one dispatch into cmd, between two full barriers
bool scatter_flush(VkCommandBuffer cmd, const char* label) {
    if (scatter_pending.empty()) return false;
    const size_t n = scatter_pending.size();
    size_t at;
    {
        std::lock_guard lk(scatter_mu);
        if (scatter.pos + n > kScatterEntries) {   // (a new lap: what the lap before the last one listed must be done; it is, ~90 frames ago)
            if (scatter.prev_lap_value > vk_timeline_completed()) vk_timeline_wait(scatter.prev_lap_value);
            scatter.prev_lap_value = scatter.lap_value;
            scatter.pos = 0;
        }
        at = scatter.pos;
        scatter.pos += n;
        scatter.lap_value = std::max(scatter.lap_value, mem_rec_value);
    }
    std::memcpy(scatter.map + at, scatter_pending.data(), n * sizeof(ScatterEntry));
    flush_mapped(scatter.ring_alloc, (at + n) * sizeof(ScatterEntry));
    full_barrier(cmd, "barrier (write-back or sync)");
    {
        GprofSpan gp(cmd, label);
        gp.items = (int)n;
        for (size_t i = 0; i < n; i++) gp.bytes += scatter_pending[i].size;
        st_scatter_bytes += (int64_t)gp.bytes;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatter.pipeline);
        for (size_t done = 0; done < n;) {   // (65535 workgroups a dispatch at most)
            const uint32_t k = (uint32_t)std::min<size_t>(n - done, 65535);
            struct { uint64_t list; uint32_t count, pad; } pc{scatter.ring_addr + (at + done) * sizeof(ScatterEntry), k, 0};
            vkCmdPushConstants(cmd, scatter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
            vkCmdDispatch(cmd, k, 1, 1);
            done += k;
        }
    }
    full_barrier(cmd, "barrier (write-back or sync)");
    st_scatter_entries += (int64_t)n, st_scatter_dispatches++;
    scatter_pending.clear();
    return true;
}
// Captured pages in a run are copied from one buffer to one mirror. Combining only contiguous source and
// destination bytes leaves gaps, overlaps and the order of the remaining regions unchanged.
void append_capture_copy(std::vector<VkBufferCopy>& runs, const VkBufferCopy& next) {
    if (!runs.empty()) {
        VkBufferCopy& last = runs.back();
        if (last.size && next.size && last.size <= UINT64_MAX - last.srcOffset &&
            last.size <= UINT64_MAX - last.dstOffset && next.size <= UINT64_MAX - last.size &&
            next.size <= UINT64_MAX - next.srcOffset && next.size <= UINT64_MAX - next.dstOffset &&
            last.srcOffset + last.size == next.srcOffset && last.dstOffset + last.size == next.dstOffset) {
            last.size += next.size;
            return;
        }
    }
    runs.push_back(next);
}

// ---- (TICK-048) the mirror syncs' diff: chunks of a dirty page that its mirror already holds, left out of the upload ----
// The syncs copy whole 4 KB pages: every page the CPU wrote since the pool's last sync. On a PCIe 3.0 x4 card (RTX 3050 on an
// i5-10400F board) that was 22-34 MB a frame and 8-12 ms of GPU time (the scatter reads host memory at ~2.8 GB/s). The game
// rewrites many pages with the bytes they held: its per-frame data comes round in rings, and a static object's constants land
// where they were. So each page a sync copies from a capture is hashed by 256-byte chunk (msync_diff.h) and the hashes kept by
// guest page; at the page's next sync only the chunks whose hash changed are copied.
// When the mirror holds the hashed bytes: the hashes are recorded only by a sync outside a parallel replay's segments (those
// syncs' uploads run in the queue's segment order, not in the order the workers record them, so a later record could describe
// an earlier copy); they name the mirror (mirror_id, new at each mirror_make: a new mirror is filled from guest memory and
// retired mirrors) and the submission (rec, the mirror epoch) whose uploads copied them. Anything else that may write the mirror
// there notes the page's gw (the epoch it was recorded in): a written binding (the write-backs' list), a stamp of GPU writes
// (storage writes, copies, transfers), a bare entry's or a wide binding's guessed range, a readback refresh. A submission's
// uploads run before its work, and its GPU writes after them, so the hashes hold while rec > gw. Pages a sync copies another
// way (from guest memory as it is at the replay, a segment's sync, kept for the GPU by RX_VK_PROTECT_BARE) lose theirs.
// Not seen: shader stores through the guest page table past what a binding or a bare entry's guess covers (the syncs already
// copy the CPU's whole page over those today). RX_VK_MSYNC_DIFF=0: off (whole pages, as before), 1: hashed and counted but
// whole pages copied (the measure), 2: changed chunks only (the default on a PCIe link of <= 8 GB/s, see pcie_link_gbps) The cost: hashing the synced pages on the replay
// thread, ~0.13 ms a submission part on an i5-10400F (~30 parts a frame at the Market). RX_VK_MSYNC_DIFF_GROUP=<chunks> (1, 2, 4, 8): copy granularity
// (default 1, 256 bytes). RX_VK_MSYNC_STATS=1: the syncs' bytes by pool and kind every 600 submits, without the diff
struct DiffPage { uint32_t rec, gw, mid, pad; uint64_t h[msync_diff::kChunks]; };
constexpr uint64_t kDiffChunkPages = 1u << 13;   // (32 MB of guest memory a table chunk, 1.1 MB of entries)
std::mutex diff_mu;   // (after pools_mu when both)
std::vector<std::unique_ptr<DiffPage[]>> diff_table;
uint32_t diff_gw_all = 0;   // (a GPU write too large to note by page: every hash older than this epoch is void)
uint32_t diff_mirror_ids = 0;
uint64_t diff_table_bytes = 0;
// (HeyListen TICK-048 verify) The default depends on the card's PCIe link: the diff gained 10-16% on a PCIe 3.0 x4 link (PC 3050,
// ~3.9 GB/s) and lost ~10% on a full Gen4/5 x16 link (RTX 5070: the bytes cost 1 ms of GPU, the hashing ~3 ms of replay thread).
// NVML (nvml.dll of the NVIDIA driver) gives the link's maximum generation and width; the link is throttled at idle, so the
// maximum is read, not the current state. On when the maximum is <= 8 GB/s (Gen3 x8, Gen4 x4 and narrower); off when it is
// wider, or when NVML is missing (other vendors, integrated GPUs: no known slow link).
double pcie_link_gbps() {
    HMODULE h = LoadLibraryA("nvml.dll");
    if (!h) return -1;
    typedef int (*fn_init)();
    typedef int (*fn_idx)(unsigned, void**);
    typedef int (*fn_u)(void*, unsigned*);
    auto init = (fn_init)GetProcAddress(h, "nvmlInit_v2");
    auto byidx = (fn_idx)GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
    auto mgen = (fn_u)GetProcAddress(h, "nvmlDeviceGetMaxPcieLinkGeneration");
    // (the width is the slot's, the current one: the maximum width is the card's, x16 for an RTX 3050 in an x4 slot. The
    // width does not drop at idle as the generation does, so the generation stays the maximum)
    auto mw = (fn_u)GetProcAddress(h, "nvmlDeviceGetCurrPcieLinkWidth");
    auto mwmax = (fn_u)GetProcAddress(h, "nvmlDeviceGetMaxPcieLinkWidth");
    double gbps = -1;
    if (init && byidx && mgen && mw && init() == 0) {
        void* dev = nullptr;
        unsigned gen = 0, w = 0, wmax = 0;
        if (byidx(0, &dev) == 0 && mgen(dev, &gen) == 0 && mw(dev, &w) == 0 && gen >= 1 && gen <= 6 && w) {
            if (mwmax && mwmax(dev, &wmax) == 0 && wmax && wmax < w) w = wmax;
            static const double per_lane[] = {0, 0.25, 0.5, 0.985, 1.969, 3.938, 7.563};   // GB/s a lane a direction
            gbps = per_lane[gen] * w;
            logf("vk: mirror syncs: PCIe link Gen%u (maximum) x%u (current; card maximum x%u) (%.1f GB/s)", gen, w, wmax, gbps);
        }
        if (auto sd = (int (*)())GetProcAddress(h, "nvmlShutdown")) sd();
    }
    FreeLibrary(h);
    return gbps;
}
int diff_mode() {
    static const int v = [] {
        const char* e = getenv("RX_VK_MSYNC_DIFF");
        int m;
        if (e) m = std::clamp(atoi(e), 0, 2);
        else {
            const double g = pcie_link_gbps();
            m = (g > 0 && g <= 8.0) ? 2 : 0;   // (on by default only on a slow link: PC 3050, Gen3 x4, +10..16%; off on x16 Gen4/5: -10%)
            logf("vk: mirror syncs: chunk diff %s by default (RX_VK_MSYNC_DIFF unset)", m ? "on" : "off");
        }
        if (m) logf("vk: mirror syncs: chunk diff %s (RX_VK_MSYNC_DIFF=%d)", m == 2 ? "on: changed 256-byte chunks only" : "measured only", m);
        return m;
    }();
    return v;
}
GranTakeFn gran_take_fn() {
    static const GranTakeFn f = (GranTakeFn)(void*)GetProcAddress(GetModuleHandleA(nullptr), "hos_gran_take");
    return f;
}
bool capture_gran_on() {
    static const bool v = [] {
        const char* e = getenv("RX_VK_CAPTURE_GRAN");
        const bool want = e && atoi(e) != 0;
        const bool on = want && gran_take_fn() && !diff_mode() && !precopy_on() && !(getenv("RX_VK_ASYNC") && atoi(getenv("RX_VK_ASYNC")) != 0);
        if (want) logf("vk: the capture by written granules: %s (RX_VK_CAPTURE_GRAN)", on ? "on" : !gran_take_fn() ? "off: the game was not built with A64_GRAN" : "off: the chunk diff, the pre-copy or RX_VK_ASYNC is on");
        return on;
    }();
    return v;
}

unsigned diff_group() {
    static const unsigned v = getenv("RX_VK_MSYNC_DIFF_GROUP") ? std::clamp(atoi(getenv("RX_VK_MSYNC_DIFF_GROUP")), 1, 16) : 1;
    return v;
}
bool msync_stats_on() {
    static const bool v = (getenv("RX_VK_MSYNC_STATS") && atoi(getenv("RX_VK_MSYNC_STATS")) != 0) || diff_mode() != 0;
    return v;
}
DiffPage* diff_page(uint64_t page, bool make) {   // (diff_mu held)
    const uint64_t c = page / kDiffChunkPages;
    if (c >= diff_table.size()) {
        if (!make) return nullptr;
        diff_table.resize(c + 1);
    }
    if (!diff_table[c]) {
        if (!make) return nullptr;
        diff_table[c].reset(new DiffPage[kDiffChunkPages]);
        std::memset(diff_table[c].get(), 0, kDiffChunkPages * sizeof(DiffPage));
        diff_table_bytes += kDiffChunkPages * sizeof(DiffPage);
    }
    return &diff_table[c][page % kDiffChunkPages];
}
void diff_gpu_wrote(uint64_t guest, uint64_t size) {
    if (!size || !diff_mode() || guest >= (1ull << 40)) return;
    const uint32_t e = (uint32_t)mirror_epoch;
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    std::lock_guard lk(diff_mu);
    if (p1 - p0 > 65536) {   // (256 MB and more: all of them)
        diff_gw_all = e;
        return;
    }
    for (uint64_t p = p0; p <= p1; p++)   // (only where hashes are: a page with none has nothing to void)
        if (DiffPage* d = diff_page(p, false)) d->gw = e;
}
// (the counters: bytes a sync copied, by where from and when; and the diff's)
struct SyncStats {
    int64_t pre = 0, bind = 0, seg = 0;          // (bytes copied: the presync, a binding outside segments, a segment's)
    int64_t cap = 0, old = 0, live = 0;          // (from this submit's capture, an earlier one's, guest memory)
    int64_t pre_unbound = 0;                     // (the presync's bytes of pools no binding of their submission used)
    int64_t d_pages = 0, d_valid = 0;            // (pages hashed; of them with hashes that held)
    int64_t d_same[5] = {};                      // (bytes unchanged at 256, 512, 1K, 2K, 4K granularity)
    int64_t d_skipped = 0, d_inval = 0;          // (bytes left out; pages whose hashes were voided by another copy)
    int64_t d_ns = 0;                            // (time hashing and comparing)
    int64_t d_gw = 0, d_mid = 0, d_new = 0;      // (pages with hashes not holding: written by the GPU, another mirror, none yet)
} sst;
struct PoolSync { int64_t bytes = 0, same = 0, skipped = 0, pages = 0; uint64_t size = 0; };
std::unordered_map<uint64_t, PoolSync> sync_by_pool;
struct PresyncNote { Pool* p; uint64_t key, bound_before; int64_t bytes; };
std::vector<PresyncNote> presync_notes;   // (this submission's presync, checked against its bindings at its end)
thread_local bool in_presync = false;
// (TICK-056, RX_VK_PRESYNC_STATS=1, diagnosis) the pre-syncs' work: pages looked at and dirty ones, pools synced, time, logged
// every 600 pre-syncs that copied anything
struct PresyncScan { int64_t pages = 0, dirty = 0, loop_ns = 0, lazy_ns = 0, cap_pages = 0, live_pages = 0; };
thread_local PresyncScan presync_scan;
bool presync_stats_on() {
    static const bool v = getenv("RX_VK_PRESYNC_STATS") && atoi(getenv("RX_VK_PRESYNC_STATS")) != 0;
    return v;
}

/// (TICK-056) RX_VK_MSYNC_FAST=1: a mirror sync's per-page work cut, the same copies made. Per dirty page the sync took the game
/// pools' lock and looked its pool up (lazy_cpu_page), found its capture's buffer in cap_bufs and its runs in cap_runs (two map
/// lookups): ~0.13 us a page, ~1 ms for the frame's big pre-sync (castle-move). Now the dirty pages go to lazy_cpu_pages once, in
/// order, and the last capture buffer and run list found are kept while the next pages have the same ones
bool msync_fast_on() {
    // (TICK-060) on by default where the chunk diff is on (links of 8 GB/s or less: PC 3050's confirm, with RX_VK_LATE_DEFER and
    // RX_VK_CAPTURE_HASH); elsewhere off as TICK-056 left it. RX_VK_MSYNC_FAST=0 / 1: off / on anywhere
    static const bool v = getenv("RX_VK_MSYNC_FAST") ? atoi(getenv("RX_VK_MSYNC_FAST")) != 0 : diff_mode() != 0;
    return v;
}

/// (TICK-056, RX_VK_MSYNC_FAST) the first page in [page, end) written after generation `synced` (page_gen > synced), or end: a region
/// whose newest generation is not past it is skipped whole wherever the range starts in it, and a region's pages are read from
/// its chunk directly (the castle-move pre-sync looked at ~105,000 pages a frame for ~9,000 dirty ones)
uint64_t next_dirty_page(uint64_t page, uint64_t end, uint32_t synced) {
    while (page < end) {
        const uint64_t r = page / kRegionPages;
        const uint64_t rend = std::min(end, (r + 1) * kRegionPages);
        if (region_gen[r] <= synced) {   // (page_gen <= region_gen: every stamp of the region's pages set it)
            page = rend;
            continue;
        }
        if (load_u32(region_full[r]) > synced) return page;   // (every page of the region is at least that new)
        const auto& c = gen_chunks[page / kChunkPages];   // (a region lies in one chunk)
        if (c)
            for (uint64_t i = page; i < rend; i++)
                if (load_u32(c[i % kChunkPages]) > synced) return i;
        page = rend;
    }
    return end;
}

/// The pages of the pool the CPU wrote since its mirror's sync, copied in (the upload command buffer)
int64_t mirror_sync(Pool& p) {
    const bool fast = msync_fast_on();
    static thread_local std::vector<uint32_t> lazy_pages;   // (fast: the dirty pages for lazy_cpu_pages)
    lazy_pages.clear();
    uint32_t last_gen = 0;   // (fast: the last capture generation looked up, its buffer and its runs)
    auto last_buf = cap_bufs.end();
    std::vector<VkBufferCopy>* last_runs = nullptr;
    VkBuffer last_runs_buf = VK_NULL_HANDLE;
    p.synced_epoch = mirror_epoch;
    p.sync_seg = texture_segment();
    mirror_synced.push_back(&p);
    std::vector<VkBufferCopy> runs;   // (from guest memory)
    std::map<VkBuffer, std::vector<VkBufferCopy>> cap_runs;   // (from the captures' copies)
    std::map<VkBuffer, std::vector<VkBufferCopy>> late_runs;  // (TICK-053: the late pages' changed words, 4-byte granular copies)
    uint64_t run_lo = 0, run_hi = 0;
    // (TICK-048: the diff, and the counters)
    const int dmode = diff_mode();
    const bool stats = msync_stats_on();
    const bool d_ok = dmode && p.mirror_id && !par_active.load(std::memory_order_relaxed);   // (hashes recorded outside segments only)
    const uint32_t d_epoch = (uint32_t)mirror_epoch;
    std::unique_lock<std::mutex> dlk(diff_mu, std::defer_lock);
    if (dmode) dlk.lock();
    const auto d_t0 = dmode ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    int64_t b_cap = 0, b_old = 0, b_live = 0, b_same = 0, b_skip = 0;
    auto d_void = [&](uint64_t page) {   // (copied another way: its hashes no longer say what the mirror holds)
        if (!dmode) return;
        if (DiffPage* d = diff_page(page, false); d && d->rec) d->rec = 0, sst.d_inval++;
    };
    const bool pstats = in_presync && presync_stats_on();
    const auto lt0 = pstats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint64_t page_end = (p.hi + 4095) >> kPageShift;
    for (uint64_t page = p.lo >> kPageShift; page < page_end;) {
        if (fast) {   // (TICK-056: straight to the next dirty page; the pages skipped are those the loop leaves alone)
            page = next_dirty_page(page, page_end, p.synced_gen);
            if (page >= page_end) break;
        } else if (page % kRegionPages == 0 && region_gen[page / kRegionPages] <= p.synced_gen) {
            page += kRegionPages;   // (a clean 2 MB region)
            continue;
        }
        const uint32_t pg = page_gen(page);
        const bool dirty = pg > p.synced_gen;
        presync_scan.pages++, presync_scan.dirty += dirty;
        if (dirty && wb_lazy_on()) {   // (TICK-015: the CPU writes this pool: written back)
            if (fast) lazy_pages.push_back((uint32_t)page);
            else lazy_cpu_page(page);
        }
        if (dirty && gpu_page(page)) {   // (diagnostic: see memory_stamp_gpu)
            st_sync_gpu_pages++;
            const CapEntry e0 = cap_entry(page);
            const bool live = !(e0.gen && e0.gen == pg && cap_bufs.count(e0.gen));
            st_sync_gpu_live += live;
            static int logged;
            if (logged++ < 40)
                logf("vk: a mirror sync copies the CPU's version of page %#llx over the GPU's (pool %#llx, %s, page gen %u, "
                     "pool synced at %u, gen now %u)", (unsigned long long)(page << kPageShift), (unsigned long long)p.lo,
                     live ? "live" : "from a capture", pg, (unsigned)p.synced_gen, gen_now);
        }
        if (dirty && protect_bare_on() && prot_page_since(page, p.synced_gen)) {   // (the GPU's records kept: see prot_pages)
            st_prot_kept++;
            d_void(page);
            late_shadow_void(page);
            page++;
            continue;
        }
        if (dirty && !sync_live()) {   // (as a capture took it, if the page was not written since)
            const CapEntry e = cap_entry(page);
            if (e.gen && e.gen == pg) {
                auto b = fast && e.gen == last_gen ? last_buf : cap_bufs.find(e.gen);
                if (fast) last_gen = e.gen, last_buf = b;
                // (sprint 5) the slot's granules; a page the pool cuts with a partial slot is copied from guest memory below
                const uint8_t gm8 = b != cap_bufs.end() && b->second.gmask && e.slot < b->second.gmask->size() ? (*b->second.gmask)[e.slot] : 0xff;
                const bool gcut = gm8 != 0xff && (std::min((page + 1) << kPageShift, p.hi) - std::max(page << kPageShift, p.lo)) != 4096;
                if (b != cap_bufs.end() && !gcut) {
                    if (PageX* x = pagex(page, false)) x->syn = pg;
                    const uint64_t ga = std::max(page << kPageShift, p.lo), gb = std::min((page + 1) << kPageShift, p.hi);
                    const uint64_t src0 = (uint64_t)e.slot * 4096 + (ga - (page << kPageShift));
                    // (TICK-053) a late page, merged: the words the CPU changed since the mirror's last sync of the page, from
                    // the capture; the GPU's words stay. The shadow then holds the capture's page. In a parallel segment
                    // (the uploads' order is not the records'), or with the page cut by the pool's end, the whole page as
                    // before and the shadow void (the next late capture of it waits once)
                    auto lsh = late_merge_on() && !late_shadows.empty() ? late_shadows.find((uint32_t)page) : late_shadows.end();
                    const bool seg = par_active.load(std::memory_order_relaxed);
                    if (lsh != late_shadows.end() && cap_entry_late(page) && b->second.map) {
                        LateShadow& s = lsh->second;
                        if (!seg && gb - ga == 4096 && s.mid == p.mirror_id && s.bytes) {
                            const uint8_t* now = (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096;
                            uint32_t off[512], len[512];   // (512: a page's words alternating, the most runs there can be)
                            const int nr = late_diff_runs(now, s.bytes.get(), off, len, 512);
                            int64_t copied = 0;
                            for (int k = 0; k < nr; k++) late_runs[b->second.buffer].push_back({src0 + off[k], ga - p.lo + off[k], len[k]});
                            for (int k = 0; k < nr; k++) copied += len[k];
                            late_shadow_set(page, p.mirror_id, now);
                            d_void(page);   // (the GPU's words in the mirror are not the capture's: the hashes do not describe it)
                            st_late_merged_bytes.fetch_add(copied, std::memory_order_relaxed);
                            (e.gen == cur_cap_gen ? b_cap : b_old) += copied;
                            b->second.last_use = std::max(b->second.last_use, cur_cap_value);
                            (e.gen == cur_cap_gen ? st_sync_cap_pages : st_sync_old_pages)++;
                            page++;
                            continue;
                        }
                        if (seg) st_late_seg.fetch_add(1, std::memory_order_relaxed);
                        s.mid = 0;   // (copied whole below, in a segment or cut: the shadow does not say what the mirror holds)
                    }
                    uint32_t mask = gm8 != 0xff ? gran_chunk_mask(gm8) : 0xffff;   // (TICK-048: the chunks to copy; sprint 5: the granules)
                    if (dmode) {
                        // (TICK-060) a late slot whose fix is pending: its bytes are not yet the ones the GPU will copy: whole, no hash
                        const bool late_pending = late_fix_slot_pending(e.gen, e.slot, nullptr);
                        if (late_pending) st_late_syncs.fetch_add(1, std::memory_order_relaxed);
                        if (!late_pending && d_ok && gb - ga == 4096 && b->second.map) {
                            DiffPage* d = diff_page(page, true);
                            uint64_t h[msync_diff::kChunks];
                            const CapHash* ch = b->second.hash.get();   // (TICK-060: the capture's copy made them)
                            if (ch && e.slot < ch->ok.size() && ch->ok[e.slot]) {
                                std::memcpy(h, ch->h.data() + (uint64_t)e.slot * msync_diff::kChunks, sizeof h);
                                st_hash_used.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                msync_diff::page_hash((const uint8_t*)b->second.map + (uint64_t)e.slot * 4096, h);
                                if (capture_hash_on()) st_hash_again.fetch_add(1, std::memory_order_relaxed);
                            }
                            sst.d_pages++;
                            if (d->rec && d->mid == p.mirror_id && d->rec > d->gw && d->rec > diff_gw_all) {
                                sst.d_valid++;
                                uint32_t m = 0;
                                for (unsigned c = 0; c < msync_diff::kChunks; c++)
                                    if (h[c] != d->h[c]) m |= 1u << c;
                                static const unsigned groups[5] = {1, 2, 4, 8, 16};
                                for (int k = 0; k < 5; k++)
                                    sst.d_same[k] += 256 * (16 - __builtin_popcount(msync_diff::group_mask(m, groups[k])));
                                b_same += 256 * (16 - __builtin_popcount(msync_diff::group_mask(m, diff_group())));
                                if (dmode >= 2) mask = msync_diff::group_mask(m, diff_group());
                            } else {
                                (!d->rec ? sst.d_new : d->mid != p.mirror_id ? sst.d_mid : sst.d_gw)++;
                            }
                            std::memcpy(d->h, h, sizeof h);
                            d->rec = d_epoch, d->mid = p.mirror_id;
                        } else {
                            d_void(page);
                        }
                    }
                    int64_t copied = 0;
                    std::vector<VkBufferCopy>& cr = fast && last_runs && last_runs_buf == b->second.buffer ? *last_runs : cap_runs[b->second.buffer];
                    if (fast) last_runs = &cr, last_runs_buf = b->second.buffer;
                    if (mask == 0xffff) {
                        append_capture_copy(cr, {src0, ga - p.lo, gb - ga});
                        copied = (int64_t)(gb - ga);
                    } else if (mask) {
                        uint32_t off[8], len[8];
                        const int nr = msync_diff::mask_runs(mask, off, len);
                        for (int k = 0; k < nr; k++) {
                            append_capture_copy(cr, {src0 + off[k], ga - p.lo + off[k], len[k]});
                            copied += len[k];
                        }
                    }
                    b_skip += (int64_t)(gb - ga) - copied;
                    // (TICK-053) a page with a shadow, synced whole or by chunks: the mirror holds the capture's page now
                    if (lsh != late_shadows.end()) {
                        if (!seg && gb - ga == 4096 && b->second.map)
                            late_shadow_set(page, p.mirror_id, (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096);
                        else
                            lsh->second.mid = 0;
                    }
                    (e.gen == cur_cap_gen ? b_cap : b_old) += copied;
                    b->second.last_use = std::max(b->second.last_use, cur_cap_value);
                    (e.gen == cur_cap_gen ? st_sync_cap_pages : st_sync_old_pages)++;
                    page++;
                    continue;
                }
            }
        }
        if (dirty) {
            if (PageX* x = pagex(page, false)) x->syn = pg;   // (sprint 5: the whole page as it is now: its entry's granules are in)
            st_sync_live_pages++;   // (dirty, not in this submit's capture: guest memory as it is now)
            live_by_pool[p.lo]++;
            d_void(page);
            late_shadow_void(page);   // (TICK-053: the mirror takes guest memory as it is at the replay, not a capture's page)
            {   // (RX_VK_LOG_LIVE=1: why, 300 lines: the page's generation, its entry's, the newest capture's)
                static const bool log_live = getenv("RX_VK_LOG_LIVE") != nullptr;
                static int logged;
                if (log_live && logged < 300 && (++live_seen % 97) == 0) {
                    logged++;
                    const CapEntry e = cap_entry(page);
                    logf("vk: live page %#llx: gen %u, entry gen %u (buffer %s), current capture %u, pool synced at %u, gen now %u",
                         (unsigned long long)(page << kPageShift), pg, e.gen,
                         e.gen ? (cap_bufs.count(e.gen) ? "kept" : "gone") : "-", cur_cap_gen, (unsigned)p.synced_gen, gen_now);
                }
            }
            const uint64_t a = std::max(page << kPageShift, p.lo) - p.lo, b = std::min((page + 1) << kPageShift, p.hi) - p.lo;
            b_live += (int64_t)(b - a);
            if (run_hi == a && run_hi) run_hi = b;
            else {
                if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
                run_lo = a, run_hi = b;
            }
        }
        page++;
    }
    if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
    const auto lt1 = pstats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (fast) lazy_cpu_pages(lazy_pages);
    if (pstats) {
        presync_scan.loop_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(lt1 - lt0).count();
        presync_scan.lazy_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - lt1).count();
        presync_scan.cap_pages += b_cap + b_old, presync_scan.live_pages += b_live;
    }
    if (dmode) {
        sst.d_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - d_t0).count();
        dlk.unlock();
    }
    const int64_t b_all = b_cap + b_old + b_live;
    if (stats) {
        sst.cap += b_cap, sst.old += b_old, sst.live += b_live, sst.d_skipped += b_skip;
        (in_presync ? sst.pre : par_active.load(std::memory_order_relaxed) ? sst.seg : sst.bind) += b_all;
        PoolSync& ps = sync_by_pool[p.lo];
        ps.bytes += b_all, ps.same += b_same, ps.skipped += b_skip, ps.size = p.hi - p.lo;
    }
    const VkDeviceAddress maddr = scatter_on() && (!cap_runs.empty() || !runs.empty()) ? buffer_address(p.mirror) : 0;
    for (const auto& [buf, rs] : cap_runs) {
        if (maddr && scatter_add(buffer_address(buf), maddr, rs)) {
            for (const VkBufferCopy& r : rs) st_sync_bytes += (int64_t)r.size;
            st_sync_copies += (int64_t)rs.size();
            continue;
        }
        GprofSpan gp(upload_command_buffer(), "msync copies (CPU pages from a capture -> mirror)");
        gp.items = (int)rs.size();
        vkCmdCopyBuffer(upload_command_buffer(), buf, p.mirror, (uint32_t)rs.size(), rs.data());
        for (const VkBufferCopy& r : rs) st_sync_bytes += (int64_t)r.size, gp.bytes += r.size;
        st_sync_copies += (int64_t)rs.size();
    }
    if (!runs.empty() && maddr && scatter_add(p.address, maddr, runs)) {
        for (const VkBufferCopy& r : runs) st_sync_bytes += (int64_t)r.size;
        st_sync_copies += (int64_t)runs.size();
        runs.clear();
    }
    if (!scatter_batch) scatter_flush(upload_command_buffer(), "msync scatter (CPU pages -> mirrors, one dispatch)");
    if (!late_runs.empty()) {   // (TICK-053: the late pages' changed words, between two full barriers as the scatter's are)
        full_barrier(upload_command_buffer(), "barrier (write-back or sync)");
        for (const auto& [buf, rs] : late_runs) {
            GprofSpan gp(upload_command_buffer(), "late merge copies (a late page's changed words -> mirror)");
            gp.items = (int)rs.size();
            vkCmdCopyBuffer(upload_command_buffer(), buf, p.mirror, (uint32_t)rs.size(), rs.data());
            for (const VkBufferCopy& r : rs) st_sync_bytes += (int64_t)r.size, gp.bytes += r.size;
            st_sync_copies += (int64_t)rs.size();
        }
        full_barrier(upload_command_buffer(), "barrier (write-back or sync)");
    }
    if (runs.empty()) return b_all;
    checkpoint_mark(upload_command_buffer(), p.lo, 'm');
    GprofSpan gp(upload_command_buffer(), "msync copies (CPU pages live from guest memory -> mirror)");
    gp.items = (int)runs.size();
    vkCmdCopyBuffer(upload_command_buffer(), p.buffer, p.mirror, (uint32_t)runs.size(), runs.data());
    for (const VkBufferCopy& r : runs) st_sync_bytes += (int64_t)r.size, gp.bytes += r.size;
    st_sync_copies += (int64_t)runs.size();
    return b_all;
}

} // namespace
std::atomic<int64_t> st_import_texel_views{0}, st_import_texel_bytes{0}, st_import_unswizzle_bytes{0};

// ---- (from PR 430, YunaraDebugs) Readback pages: GPU results the CPU reads (invalidate) and then writes, as OoT's exposure meter (a
// sum the GPU adds into each frame; the CPU reads it and stores 0 back). On the console the GPU adds into that memory as it is when
// its work runs, the CPU's 0 included. Here it adds into the pool's mirror, which takes the CPU's 0 only at a mirror sync of a submit
// made after it: one made just before (the game's threads overlap) added onto the old sum, the CPU read 2x the next frame and 0 the
// one after, and the exposure stepped (the light dimming or flaring for a frame in Kokiri). So each submission first copies those
// pages from guest memory into the mirror, on the GPU as its work starts, last of the uploads: what the CPU wrote by then is what the
// GPU adds to. RX_VK_NO_RB_REFRESH=1: off ----
// (hookshot, 11-09) A parallel replay's recordings share one submission (mirror_epoch) and its one write-back at its end: the part
// before the segments, each segment and the recording after them (vk_replay.cpp replay_handle). A refresh in a later one copied the
// guest's page, which has none of the submission's GPU writes, over what the recordings ahead of it had written in the mirror: OoT's
// exposure sum came back ~0 and every frame was too bright (RX_VK_PARALLEL=4). So a recording that joined an open submission
// refreshes a page only when every recording ahead of it in the queue is made and none of them may have written the page: the copy
// then brings the CPU's writes and wipes nothing (and a page first read in the middle of a submission is refreshed ahead of its first
// use, unless the recordings up to that use were all made before it was read). What they may have written are the GPU write
// hazards: every range bound for writing in the mirror, and the ranges a program may write that are not copied back (a bare table
// entry's guessed range past its first 64 bytes, the pointer table's, a wide binding: vk_draw.cpp, memory_note_gpu_write). A
// recording that opens its submission, or comes after its write-back (frame generation's), refreshes every page as before: serial
// replay records the same copies. With RX_VK_FOLLOW_STORES (stores through the page table, bound nowhere) a joining one refreshes
// none. RX_VK_RB_REFRESH_EACH=1: in every recording, as before
namespace {
std::unordered_map<uint32_t, uint64_t> rb_pages;   // (guest page -> mirror_epoch it was last read at)
std::atomic<int64_t> st_rb_refresh{0}, st_rb_held{0};
// (TICK-048) RX_VK_RB_RANGES=1: a readback page's refresh copies only the bytes the CPU's invalidates of it covered (their union,
// 16-byte aligned outwards), not the whole 4 KB page. The results are small (meters, counters: 0x4000 at most), and the refresh is
// there for the CPU's writes into them (OoT's exposure meter: read, then 0 stored). On a PCIe 3.0 x4 card the ~540 whole pages a
// frame were ~2 MB and ~2 ms of GPU time. Off by default
std::unordered_map<uint32_t, uint32_t> rb_spans;   // (guest page -> lo | hi << 16, bytes in the page)
std::atomic<int64_t> st_rb_bytes{0};
bool rb_ranges_on() {
    static const bool v = getenv("RX_VK_RB_RANGES") && atoi(getenv("RX_VK_RB_RANGES")) != 0;
    return v;
}
// (the open submission the recording this thread is making joined; 0: none: it opens one, or comes after one's end)
thread_local uint64_t rb_joined = 0;
// (the GPU write hazards of submission rb_hazards_epoch, by guest address, with their recording's place in the queue
// (texture_segment); kept past its write-back, for the recording that ends it)
struct RbHazard {
    uint64_t lo, hi, seg;
};
std::vector<RbHazard> rb_hazards;
uint64_t rb_hazards_epoch = 0;
// (a parallel replay's segments: the submission, the first one's order (texture_segment) and how many; the ones made, by order)
struct RbSegments {
    uint64_t epoch = 0, base = 0, n = 0, made = 0;
} rb_segs;
void rb_hazard_locked(uint64_t lo, uint64_t hi) {   // (pools_mu held)
    if (rb_hazards_epoch != mirror_epoch) rb_hazards.clear(), rb_hazards_epoch = mirror_epoch;
    rb_hazards.push_back({lo, hi, texture_segment()});
}
}
void memory_note_readback(uint64_t guest, uint64_t size) {
    static const bool off = getenv("RX_VK_NO_RB_REFRESH") != nullptr;
    if (off || !size || size > 0x4000) return;   // (small results only: meters and counters)
    std::lock_guard lk(pools_mu);
    for (uint64_t pg = guest >> kPageShift; pg <= (guest + size - 1) >> kPageShift; pg++) {
        rb_pages[(uint32_t)pg] = mirror_epoch;
        if (rb_ranges_on()) {   // (TICK-048: the bytes of it read)
            const uint64_t base = pg << kPageShift;
            const uint32_t a = (uint32_t)((std::max(guest, base) - base) & ~15ull);
            const uint32_t b = (uint32_t)std::min<uint64_t>(4096, (std::min(guest + size, base + 4096) - base + 15) & ~15ull);
            auto [it, fresh] = rb_spans.try_emplace((uint32_t)pg, a | b << 16);
            if (!fresh) it->second = std::min(it->second & 0xffff, a) | std::max(it->second >> 16, b) << 16;
        }
    }
}
bool memory_is_rb_page_locked(uint32_t page) {   // (TICK-017's log; pools_mu held)
    return rb_pages.count(page) != 0;
}
void memory_note_gpu_write(uint64_t gpu, uint64_t size) {
    if (!size) return;
    uint64_t guest = gpu;
    if (gpu >= (1ull << 40)) {   // (as gpu_buffer_bind)
        uint64_t avail = 0;
        if (!host->gpu_ptr(gpu, &avail) || avail < size) return;
        guest = host->gpu_guest(gpu);
    }
    diff_gpu_wrote(guest, size);   // (TICK-048)
    std::lock_guard lk(pools_mu);
    rb_hazard_locked(guest, guest + size);
}
void memory_rb_recording_begun() {
    std::lock_guard lk(pools_mu);
    rb_joined = mirror_upload ? mirror_epoch : 0;
}
void memory_rb_segments_begin(uint64_t base, uint64_t n) {
    std::lock_guard lk(pools_mu);
    rb_segs = {mirror_epoch, base, n, 0};
}
/// Records the refresh copies into the submission's upload command buffer; true when it recorded any
bool memory_rb_refresh(VkCommandBuffer up) {
    std::lock_guard lk(pools_mu);
    static const bool each = getenv("RX_VK_RB_REFRESH_EACH") != nullptr;
    static const bool follow_stores = getenv("RX_VK_FOLLOW_STORES") != nullptr;
    const bool joined = !each && rb_joined && rb_joined == mirror_epoch;
    const uint64_t seg = texture_segment();
    bool ahead_made = !follow_stores;   // (every recording ahead of this one in the queue made, and what they may write known)
    if (joined && rb_segs.epoch == mirror_epoch && seg >= rb_segs.base && seg - rb_segs.base < rb_segs.n) {
        const uint64_t k = seg - rb_segs.base;   // (a segment's: the ones before it made, and this one now)
        const uint64_t below = k < 64 ? (1ull << k) - 1 : ~0ull;
        ahead_made = ahead_made && k < 64 && (rb_segs.made & below) == below;
        if (k < 64) rb_segs.made |= 1ull << k;
    }
    if (rb_pages.empty() || !up) return false;
    // (hookshot, TICK-006) once a submission part (mirror_epoch), in its first recording. A parallel replay records a part's
    // segments, and what follows them, into recordings of their own in the same epoch, with no write-back between them: each
    // one's refresh copied the CPU's 0 over the sum the segments before it had added on the GPU. The meter read only the last
    // recording's share, the exposure rose to its maximum: fog washed out, the ground's shadows gone, in every frame
    static uint64_t done_epoch = ~0ull;   // (not 0: mirror_epoch starts at 0)
    if (done_epoch == mirror_epoch) return false;
    done_epoch = mirror_epoch;
    auto written_ahead = [&](uint64_t lo, uint64_t hi) {   // (may be, by a recording ahead of this one in its submission)
        if (rb_hazards_epoch != mirror_epoch) return false;
        for (const RbHazard& h : rb_hazards)
            if (h.seg < seg && h.lo < hi && lo < h.hi) return true;
        return false;
    };
    std::map<Pool*, std::vector<VkBufferCopy>> by_pool;
    for (auto it = rb_pages.begin(); it != rb_pages.end();) {
        if (it->second + 600 < mirror_epoch) { rb_spans.erase(it->first); it = rb_pages.erase(it); continue; }   // (not read for a while)
        const uint64_t g = (uint64_t)it->first << kPageShift;
        uint32_t span_lo = 0, span_hi = 4096;   // (TICK-048, RX_VK_RB_RANGES: the bytes read of it)
        if (rb_ranges_on())
            if (const auto sp = rb_spans.find(it->first); sp != rb_spans.end()) span_lo = sp->second & 0xffff, span_hi = sp->second >> 16;
        ++it;
        auto pit = imported.upper_bound(g);
        if (pit == imported.begin()) continue;
        --pit;
        Pool& p = pit->second;
        if (g < p.lo || g >= p.hi || p.mstate != Pool::MIRROR_ON || !p.mirror) continue;
        const uint64_t a = std::max(g + span_lo, p.lo) - p.lo, b = std::min(g + span_hi, p.hi) - p.lo;
        if (b <= a) continue;
        if (joined && (!ahead_made || written_ahead(p.lo + a, p.lo + b))) {   // (not here: see above)
            st_rb_held++;
            continue;
        }
        by_pool[&p].push_back({a, a, b - a});
    }
    if (by_pool.empty()) return false;
    for (auto& [pp, rs] : by_pool)   // (TICK-048: the mirror written there from guest memory)
        for (const VkBufferCopy& r : rs) {
            diff_gpu_wrote(pp->lo + r.dstOffset, r.size), st_rb_bytes += (int64_t)r.size;
            late_shadow_void_range(pp->lo + r.dstOffset, pp->lo + r.dstOffset + r.size);   // (TICK-053: guest memory, not a capture)
        }
    full_barrier(up);   // (after the older submissions' write-backs into guest memory and this one's other uploads)
    {   // (TICK-021: one dispatch rather than a copy region each; RX_VK_MSYNC_SCATTER=0: copies)
        GprofSpan gp(up, "rbrefresh copies (readback pages guest -> mirror)");
        gp.items = 0;
        for (auto& [pp, rs] : by_pool) {
            st_rb_refresh += (int64_t)rs.size();
            // (hookshot) pages next to each other as one copy (~530 a frame in Hyrule Field, a copy a page)
            std::sort(rs.begin(), rs.end(), [](const VkBufferCopy& x, const VkBufferCopy& y) { return x.srcOffset < y.srcOffset; });
            size_t k = 0;
            for (size_t i = 1; i < rs.size(); i++)
                if (rs[k].srcOffset + rs[k].size == rs[i].srcOffset) rs[k].size += rs[i].size;
                else rs[++k] = rs[i];
            rs.resize(k + 1);
            if (scatter_add(pp->address, buffer_address(pp->mirror), rs)) continue;
            vkCmdCopyBuffer(up, pp->buffer, pp->mirror, (uint32_t)rs.size(), rs.data());
            gp.items += (int)rs.size();
            for (const VkBufferCopy& r : rs) gp.bytes += r.size;
        }
    }
    scatter_flush(up, "rbrefresh scatter (readback pages guest -> mirror, one dispatch)");
    static std::atomic<int64_t> n{0};
    if (++n % 3000 == 1)
        logf("vk: readback pages refreshed from guest memory: %lld pages so far (%lld held: may be written ahead in their submission)",
             (long long)st_rb_refresh.load(), (long long)st_rb_held.load());
    return true;
}

BufferRef gpu_buffer(uint64_t gpu, uint64_t size) {
    uint64_t avail = 0;
    if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
    const uint64_t guest = host->gpu_guest(gpu);
    std::lock_guard lk(pools_mu);
    Pool* p = pool_of(guest, size);
    if (!p) return {};
    if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;   // (in use: not given back under it, imports_give_back)
    return {p->buffer, guest - p->lo, size, p->address + (guest - p->lo)};
}

void memory_pool_flags(uint64_t gpu, uint64_t size, uint32_t flags) {   // (TICK-015: the game's pools and their flags, for the write-backs left out)
    if (!size || gpu >= (1ull << 40)) return;
    std::lock_guard lk(nvn_pools_mu);
    auto it = nvn_pools.upper_bound(gpu);
    if (it != nvn_pools.begin() && std::prev(it)->second.hi > gpu) --it;
    while (it != nvn_pools.end() && it->first < gpu + size) it = nvn_pools.erase(it);   // (memory reused)
    NvnPool np;
    np.hi = gpu + size, np.flags = flags;
    nvn_pools[gpu] = np;
    nvn_pools_gen.fetch_add(1, std::memory_order_relaxed);
}
void memory_pool_created(uint64_t gpu, uint64_t size) {
    // (RX_VK_BG_IMPORT=1 only: imported eagerly, every pool the game made, most never used by the GPU, the thread's
    // page locking stalled the game's threads (3-7 fps); chunked imports at first use are the default)
    static const bool on = getenv("RX_VK_BG_IMPORT") != nullptr;
    if (!on || !size || gpu >= (1ull << 40) || !dev.external_memory_host) return;   // (virtual pools are not imported)
    const uint64_t lo = gpu & ~(align - 1), hi = (gpu + size + align - 1) & ~(align - 1);
    std::lock_guard lk(bg_mu);
    bg_start();
    if (std::find(bg_inflight.begin(), bg_inflight.end(), lo) != bg_inflight.end()) return;
    bg_inflight.push_back(lo);
    bg_queue.push_back({lo, hi});
    bg_cv.notify_one();
}

int memory_guest_imported(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(pools_mu);
    auto it = imported.upper_bound(hi);   // (pools by lo: those starting below hi, the last ones checked first)
    while (it != imported.begin()) {
        --it;
        if (it->second.hi > lo) return 1;
        if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
    }
    for (const auto& nh : retired_pools)
        if (nh.mapped().memory && nh.mapped().lo < hi && lo < nh.mapped().hi) return 1;
    return 0;
}

bool mirror_make(Pool& pool);   // (below)
/// The mirrored pools, for the submit-time capture (memory_capture_submit) (pools_mu held)
void publish_cap_ranges() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, pool] : imported)
        if (pool.mstate == Pool::MIRROR_ON || pool.was_mirrored) v.push_back({pool.lo, pool.hi});
    std::lock_guard lk(cap_ranges_mu);
    cap_ranges.swap(v);
    cap_ranges_ver++;
}
// (EvilOOT, 11-07) the game's own memory pool holding a GPU address (below 1 TB, where the address is the guest's), not the
// import that happens to cover it: imports are 16 MB chunks around what was bound, and since upstream 1c7c9c7 only around
// the bound range, so the pool's end was a chunk's end: the pointer tables of the task and mesh stages (whole pools) and the
// bare entries stopped there, the grass's programs read their records 12-18 MB further on as 0 and culled every blade
static bool game_pool_of(uint64_t gpu, uint64_t& lo, uint64_t& hi) {   // (pools_mu held)
    if (gpu >= (1ull << 40) || guest_pools.empty()) return false;
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), gpu, [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return false;
    --it;
    if (gpu < it->lo || gpu >= it->hi) return false;
    lo = it->lo, hi = it->hi;
    return true;
}
/// True when gpu sits in [lo, hi) and that span was cached from the guest-pool table this generation still is.
bool guest_span_covers(uint64_t gpu, uint64_t lo, uint64_t hi, uint64_t cached_gen, uint64_t gen) {
    return cached_gen == gen && lo < hi && gpu >= lo && gpu < hi;
}
struct GuestSpan { uint64_t lo = 1, hi = 0, gen = 0; };
thread_local GuestSpan guest_span;
bool guest_span_cached(uint64_t gpu, uint64_t* lo, uint64_t* hi) {
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;   // (A/B: the import's end, as before)
    if (imported_only || gpu >= (1ull << 40)) return false;
    const uint64_t gen = guest_pools_gen.load(std::memory_order_relaxed);
    if (!guest_span_covers(gpu, guest_span.lo, guest_span.hi, guest_span.gen, gen)) return false;
    *lo = guest_span.lo;
    *hi = guest_span.hi;
    return true;
}
void guest_span_store(uint64_t lo, uint64_t hi) {   // (pools_mu held: the generation matches the table just read)
    guest_span = {lo, hi, guest_pools_gen.load(std::memory_order_relaxed)};
}
uint64_t gpu_pool_end(uint64_t gpu) {
    uint64_t lo, hi;
    // (storage-buffer tables ask this per entry, and the entries of one table sit in one pool: the locked lookup of a
    // pool was ~8% of replay time before buffer binds cached it)
    if (guest_span_cached(gpu, &lo, &hi)) return hi;
    ParLock plk;   // (before pools_mu, always)
    std::lock_guard lk(pools_mu);
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;   // (A/B: the import's end, as before)
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return hi;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->hi : 0;
}
uint64_t gpu_pool_start(uint64_t gpu) {   // (EvilOOT) where it begins
    uint64_t lo, hi;
    if (guest_span_cached(gpu, &lo, &hi)) return lo;
    ParLock plk;
    std::lock_guard lk(pools_mu);
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return lo;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->lo : 0;
}

// (pools leaving, a mirror made, or the next submission: a remembered read binding is looked up again)
uint64_t gpu_bind_stamp() {
    return pools_gen.load(std::memory_order_relaxed) ^ (mirror_epoch * 0x9E3779B97F4A7C15ull);
}

uint32_t bind_slot(uint64_t guest, uint64_t size) {
    // (a page-aligned range's low bits are zero: fold the rest in, or every one of them lands in one slot)
    uint64_t h = guest ^ (size * 0x9E3779B97F4A7C15ull);
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    h ^= h >> 16;
    return (uint32_t)(h & 255u);
}

bool bind_memo_hit(uint64_t guest, uint64_t size, uint64_t stamp, uint64_t cached_guest, uint64_t cached_size,
                   uint64_t cached_stamp, int has_buffer) {
    return has_buffer && size && guest == cached_guest && size == cached_size && stamp == cached_stamp;
}

struct BindMemo {
    uint64_t guest = 0, size = 0, stamp = 0;
    BufferRef ref{};
};
thread_local BindMemo bind_memo[256];

BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {
    if (written) texture_gpu_wrote(gpu, size);
    // (below 1 TB a GPU address is the guest address itself, and a pool holds the whole range or nothing: the host's
    // two translations are for the virtual pools above)
    uint64_t guest = gpu;
    if (gpu >= (1ull << 40)) {
        uint64_t avail = 0;
        if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
        guest = host->gpu_guest(gpu);
    }
    // (the same read, while that stamp still stands: the buffer and the offset are the ones found before. a miss
    // runs the lookup below, including a mirror sync, and remembers what it returned. a write is not remembered)
    auto remember = [&](BufferRef ref) {
        if (!written && ref.buffer != VK_NULL_HANDLE && size)
            bind_memo[bind_slot(guest, size)] = BindMemo{guest, size, gpu_bind_stamp(), ref};
        return ref;
    };
    if (!written) {
        const BindMemo& m = bind_memo[bind_slot(guest, size)];
        if (bind_memo_hit(guest, size, gpu_bind_stamp(), m.guest, m.size, m.stamp, m.ref.buffer != VK_NULL_HANDLE))
            return m.ref;
    }
    // (a read binding of a pool this thread found before and whose mirror is in use this submission or that has none,
    // without the lock: every thread's, the replay thread's too (each draw binds several uniform buffers: the pool's
    // lookup under the lock was ~8% of the replay's time in a race); the rest under it, as below)
    const bool par = par_active.load(std::memory_order_relaxed);
    TlPool& tl = tl_pool_entry(guest);
    if (par_stats_on && par)
        par_why(written ? PW_BIND_WRITTEN : !tl.p ? PW_BIND_NO_TL : tl.gen != pools_gen.load(std::memory_order_relaxed) ? PW_BIND_TL_GEN
                : !(guest >= tl.p->lo && guest <= tl.p->hi) ? PW_BIND_TL_RANGE
                : !(size <= tl.p->hi - guest) ? PW_BIND_SIZE
                : (!mirror_on() || !mirror_upload || tl.p->mstate == Pool::MIRROR_REFUSED || tl.p->mstate == Pool::MIRROR_WANTED) ? PW_COUNT
                : tl.p->mstate != Pool::MIRROR_ON ? PW_BIND_MIRROR_STATE : tl.p->synced_epoch != mirror_epoch ? PW_BIND_SYNC_EPOCH
                : tl.p->sync_seg > texture_segment() ? PW_BIND_SYNC_SEG : PW_COUNT);
    if (!written && tl.p && tl.gen == pools_gen.load(std::memory_order_relaxed) && guest >= tl.p->lo &&
        guest <= tl.p->hi && size <= tl.p->hi - guest) {   // (hookshot: no wrap-around for a huge size, as pool_of)
        Pool* p = tl.p;
        const uint64_t off = guest - p->lo;
        if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
        if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED || p->mstate == Pool::MIRROR_WANTED) {
            if (p->lazy_held) st_lazy_direct.fetch_add(1, std::memory_order_relaxed);   // (a diagnostic: guest memory without the GPU writes left in the mirror)
            if (mirror_on()) st_direct_fast.fetch_add(1, std::memory_order_relaxed), st_direct_fast_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
            return remember({p->buffer, off, size, 0});
        }
        if (p->mstate == Pool::MIRROR_ON && p->synced_epoch == mirror_epoch && p->sync_seg <= texture_segment())
            return remember({p->mirror, off, size, 0});
    }
    ParLock plk;   // (before pools_mu, always: a mirror sync records into the shared upload command buffer)
    std::lock_guard lk(pools_mu);
    {   // (hookshot, smoothness: import_in_background above. Reads up to 4 MB; not in a parallel replay's segment)
        static const bool async_import = [] { const char* e = getenv("RX_VK_ASYNC_IMPORT"); return e && atoi(e) != 0; }();
        if (async_import && !written && !par && size <= (4u << 20) && guest < (1ull << 40) &&
            !(last_pool && guest >= last_pool->lo && guest + size <= last_pool->hi) && import_in_background(guest, size)) {
            uint64_t avail = 0;
            const uint8_t* src = host->gpu_ptr(gpu, &avail);
            if (src && avail >= size)
                if (const BufferRef r = stream_copy(src, size)) {
                    st_async_copies++;
                    return r;
                }
        }
    }
    Pool* p = pool_of(guest, size);
    if (!p) return {};
    tl = {p, pools_gen.load(std::memory_order_relaxed)};
    if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
    const uint64_t off = guest - p->lo;
    if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED) {
        if (p->lazy_held) st_lazy_direct.fetch_add(1, std::memory_order_relaxed);
        if (mirror_on()) { auto& e = direct_by_pool[p->lo]; e.first++, e.second = mirror_upload ? (int)p->mstate : 9; direct_bytes_by_pool[p->lo] += size; }
        return remember({p->buffer, off, size, 0});
    }
    // (a pool's mirror is made at its first binding, in this part's uploads: bound in guest memory until the next
    // submission, the GPU read it there while the game's threads wrote it for their next frames, and a pass that drew
    // over the moment a write landed came out in two (shadows dark below a line for a frame, the line moving; a new
    // pool every so many frames as the per-frame data moves on). Not when this part already wrote it in guest memory:
    // the copy at the part's start would miss that. Not in a parallel replay's segment either: its copy goes into the
    // uploads of the segment whose worker bound the pool first, which need not be the first in the queue. A segment
    // before it found the mirror in the page table and read it unfilled (houses, rocks and bushes missing, geometry
    // torn, the more the segments the worse; #994 with RX_VK_PARALLEL). RX_VK_MIRROR_DEFER=1: at the next submission, as before)
    static const bool defer = getenv("RX_VK_MIRROR_DEFER") != nullptr;
    if (p->mstate == Pool::MIRROR_NONE && !defer && !par && p->direct_write_epoch != mirror_epoch && mirror_make(*p)) {
        publish_cap_ranges();
        static int64_t made_now;
        if (++made_now % 200 == 1)
            logf("vk: pool %#llx (%llu MB): mirror made at its first binding (%lld so)", (unsigned long long)p->lo,
                 (unsigned long long)((p->hi - p->lo) >> 20), (long long)made_now);
    }
    if (p->mstate == Pool::MIRROR_NONE) {   // (made at the next submission's start)
        p->mstate = Pool::MIRROR_WANTED;
        mirror_wanted.push_back(p);
    }
    if (p->mstate != Pool::MIRROR_ON) {
        auto& e = direct_by_pool[p->lo];
        e.first++, e.second = (int)p->mstate;
        direct_bytes_by_pool[p->lo] += size;
        if (written) p->direct_write_epoch = mirror_epoch;
        if (written && size <= (1u << 20))   // (EvilBoyce: GPU results written in place; small ones: the readbacks are)
            wb_pages_note(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo), mem_rec_value);
        return remember({p->buffer, off, size, 0});
    }
    if (p->synced_epoch != mirror_epoch || p->sync_seg > texture_segment()) mirror_sync(*p);
    if (written) {
        writebacks.push_back({p, off, std::min(off + size, p->hi - p->lo)});
        diff_gpu_wrote(p->lo + off, std::min(off + size, p->hi - p->lo) - off);   // (TICK-048)
        rb_hazard_locked(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo));   // (memory_rb_refresh)
    }
    return remember({p->mirror, off, size, 0});
}

// ---- mirrors given back ----
// A mirror was never freed: the retired pools' stayed (every pool remade larger, or taken in by a larger one, kept
// its VRAM), and pools the game no longer used kept theirs until the mirrors' budget was full; the pools after that
// stayed in system memory for good (the GPU read them over PCIe). Now a retired pool's mirror goes once the GPU is done
// with the submissions that bound it; a mirror not bound for kIdleEpochs submissions goes when a wanted one does not
// fit, or after kLongIdle; and a pool refused for the budget is tried again when it is next bound.
constexpr uint64_t kIdleEpochs = 600, kLongIdle = 6000;
// (TICK-057) the idle mirrors given back by why, the ones made again soon after, and RX_VK_MIRROR_STICKY (mirrors_give_back)
int64_t st_freed_tight = 0, st_freed_budget = 0, st_freed_long = 0, st_remade = 0, st_sticky_kept = 0;
uint64_t st_remade_bytes = 0;
bool mirror_sticky_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_MIRROR_STICKY");
        const bool v = e && atoi(e) != 0;
        logf("vk: mirrors made again soon after going: %s (RX_VK_MIRROR_STICKY)", v ? "kept against the VRAM rule" : "given back as before");
        return v;
    }();
    return on;
}
int64_t st_mirrors_freed = 0;
uint64_t st_mirror_bytes_freed = 0;
// (hookshot) A freed mirror's buffer is destroyed only once the GPU is past everything recorded until then. Its pool's
// last_value counts the draws and dispatches that bound it, not the shaders that reach it through the guest page table:
// a culling or fog compute pass already running read a mirror destroyed under it (device lost in 53caac5f and ee7764a6,
// issues 132 and 195, AMD after a scene change). The table goes back to guest memory at once, as before
struct DeadMirror { VkBuffer buffer; VmaAllocation alloc; uint64_t value; };
std::deque<DeadMirror> dead_mirrors;
// A retired pool's import, given back the same way. Each import is a userptr buffer object to amdgpu, and RADV lists every
// buffer object in every submission (buffer device addresses): the kernel checks every page of every userptr object at each
// DRM_IOCTL_AMDGPU_CS. Retired pools were never given back, so the pages it checked only grew with each pool merge: in Kokiri
// Forest ~1.3 GB of imports, 15-45 ms a submission (3-4 fps on an RX 7800 XT with 11 ms of GPU time; issues 312, 413). Its
// table entries went at its retirement, and its mirror goes before (mstate), so once the GPU is past retired_value nothing
// reaches it. The node stays (pointers to it are kept elsewhere); only its buffer and memory go. RX_VK_KEEP_RETIRED=1: kept
struct DeadImport { VkBuffer buffer; VkDeviceMemory memory; uint64_t value; uint64_t bytes; std::vector<VkDeviceMemory> pieces; };
std::deque<DeadImport> dead_imports;
uint64_t st_retired_imports_freed = 0, st_retired_import_bytes_freed = 0;
void reap_mirrors() {   // (pools_mu held)
    while (!dead_mirrors.empty() && dead_mirrors.front().value <= mem_completed) {
        if (getenv("RX_VK_LOG_ADDR")) {
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = dead_mirrors.front().buffer;
            logf("vk: addr mirror freed at gpu %#llx (value %llu, completed %llu)", (unsigned long long)vkGetBufferDeviceAddress(dev.device, &da),
                 (unsigned long long)dead_mirrors.front().value, (unsigned long long)mem_completed);
        }
        destroy_buffer(dead_mirrors.front().buffer, dead_mirrors.front().alloc);
        dead_mirrors.pop_front();
    }
    while (!dead_imports.empty() && dead_imports.front().value <= mem_completed) {
        vkDestroyBuffer(dev.device, dead_imports.front().buffer, nullptr);
        vkFreeMemory(dev.device, dead_imports.front().memory, nullptr);
        for (VkDeviceMemory m : dead_imports.front().pieces) vkFreeMemory(dev.device, m, nullptr);
        st_retired_imports_freed++, st_retired_import_bytes_freed += dead_imports.front().bytes;
        dead_imports.pop_front();
    }
}
void retired_imports_give_back() {   // (pools_mu held)
    // (hookshot) Kept by default on Windows: giving them back is for the Linux amdgpu submit above, and on Windows an Intel Arc
    // B390 lost the device entering Kokiri every time with it and 1 run in 5 without (issue 17, fuxau); the AMD device losses
    // on the Ally X (309) and RX 6600 (571) came right after retired pools went back. RX_VK_KEEP_RETIRED=0 or 1 chooses
#ifdef _WIN32
    // (not on NVIDIA: kept there, ~20 areas of play left 6.8 GB of retired pools imported on an RTX 3070 (8 GB), and the next area
    // lost the device, twice; given back, the same tour finished every area (issue 1057). The losses above were Intel's and AMD's)
    static const bool keep = getenv("RX_VK_KEEP_RETIRED") ? getenv("RX_VK_KEEP_RETIRED")[0] != '0' : dev.props.vendorID != 0x10DE;
#else
    static const bool keep = getenv("RX_VK_KEEP_RETIRED") && getenv("RX_VK_KEEP_RETIRED")[0] != '0';
#endif
    uint64_t live_bytes = 0, retired_bytes = 0;
    size_t retired_alive = 0;
    for (auto& [lo, p] : imported) live_bytes += p.hi - p.lo;
    for (auto& nh : retired_pools) {
        Pool& p = nh.mapped();
        if (!p.memory) continue;
        if (!keep && p.mstate == Pool::MIRROR_NONE && !p.inherit_pending && p.retired_value <= mem_completed &&
            p.last_value <= mem_completed) {
            dead_imports.push_back({p.buffer, p.memory, std::max(mem_rec_value, p.last_value), p.hi - p.lo, std::move(p.pieces)});
            p.pieces.clear();
            p.buffer = VK_NULL_HANDLE, p.memory = VK_NULL_HANDLE, p.address = 0;
            pools_gen++;
            continue;
        }
        retired_alive++, retired_bytes += p.hi - p.lo;
    }
    static uint64_t logged_epoch;
    if (mirror_epoch - logged_epoch >= 1200) {
        logged_epoch = mirror_epoch;
        logf("vk: imports: %zu live (%llu MB), %zu retired still imported (%llu MB), %llu retired given back (%llu MB)",
             imported.size(), (unsigned long long)(live_bytes >> 20), retired_alive, (unsigned long long)(retired_bytes >> 20),
             (unsigned long long)st_retired_imports_freed, (unsigned long long)(st_retired_import_bytes_freed >> 20));
    }
}
void mirror_free(Pool& p) {
    p.was_mirrored = true;
    {   // (the table back to guest memory, for a pool still in use only: a retired one's range is another pool's now, and its
        // address there sent the foliage culling into freed memory, a device lost)
        const auto live = imported.find(p.lo);
        if (live != imported.end() && &live->second == &p) guest_pages_set(p.lo, p.hi, p.address);
    }
    dead_mirrors.push_back({p.mirror, p.mirror_alloc, std::max(mem_rec_value, p.last_value)});
    shadow_free(p);
    p.mirror = VK_NULL_HANDLE, p.mirror_alloc = nullptr;
    mirror_bytes -= p.hi - p.lo;
    st_mirror_bytes_freed += p.hi - p.lo;
    st_mirrors_freed++;
    p.mstate = Pool::MIRROR_NONE;
    pools_gen++;   // (a parallel replay thread's pool cache: looked up again)
}
// (TICK-023) A live pool's mirror holding GPU writes left out of the write-back (TICK-015) was never given back. One idle as
// long as the others that go (kLongIdle, or kIdleEpochs when the mirrors' budget is short) is listed here; the next submission's
// end copies its left-out pages (the CPU's pages and words, and texture storage, cut out, as the write-backs do) from the mirror
// into guest memory and then frees the mirror: the buffer is destroyed once the GPU is past that copy (mirror_free's value), so
// a pool bound later is synced from guest memory holding the GPU's words. RX_VK_KEEP_LAZY_MIRRORS=1: kept, as before
std::vector<Pool*> lazy_release;
int64_t st_lazy_released = 0;
uint64_t st_lazy_released_bytes = 0, st_lazy_written_back = 0;
void mirrors_give_back(uint64_t want_bytes) {   // (pools_mu held)
    static const bool keep = getenv("RX_VK_MIRROR_KEEP") != nullptr;   // (A/B: mirrors kept, as before)
    if (keep) return;
    for (auto& nh : retired_pools) {
        Pool& p = nh.mapped();
        // (EvilOOT: kept while the pool that took it in has not made its mirror, for 600 submissions at most)
        if (p.inherit_pending && !p.lazy_held && mirror_epoch - p.retired_epoch > kIdleEpochs) p.inherit_pending = false;
        if (p.mstate == Pool::MIRROR_ON && p.retired_value <= mem_completed && !p.inherit_pending) mirror_free(p);
    }
    // (TICK-023) the process past 80% of the VRAM the driver allows (VK_EXT_memory_budget): the mirrors idle for kIdleEpochs go as
    // when their own budget is short (1.3-1.4 GB of mirrors held through a walk with the heaps at 83-89%)
    bool vram_tight = false;
    {
        // (TICK-057, RX_VK_MIRROR_VRAM_FREE=1) what the process holds less the free space in VMA's blocks, as the textures'
        // budget counts it (texture_evict): measured by the blocks alone, castle-move on PC 3050 read 84-87% with ~0.7 GB of
        // the blocks free, and the mirrors went and came back ~3 times a second
        static const bool count_free = [] {
            const char* e = getenv("RX_VK_MIRROR_VRAM_FREE");
            const bool v = e && atoi(e) != 0;
            logf("vk: the mirrors' VRAM rule: %s (RX_VK_MIRROR_VRAM_FREE)", v ? "the free space in VMA's blocks counted as free" : "by the blocks, as before");
            return v;
        }();
        uint64_t used = 0, allowed = 0, block_free = 0;
        const bool known = vram_usage(&used, &allowed, &block_free);
        if (count_free) used -= std::min(used, block_free);
        vram_tight = known && allowed && used > allowed / 100 * 80;
    }
    const uint64_t want_own = want_bytes;   // (TICK-057: what the mirrors' own budget needs, without the VRAM rule)
    if (vram_tight) want_bytes = std::max(want_bytes, mirror_budget());
    std::vector<Pool*> idle;
    for (auto& [lo, p] : imported)
        if (p.mstate == Pool::MIRROR_ON && !p.lazy_held && mirror_epoch - p.synced_epoch > kIdleEpochs && p.last_value <= mem_completed)
            idle.push_back(&p);
    std::sort(idle.begin(), idle.end(), [](const Pool* a, const Pool* b) { return a->synced_epoch < b->synced_epoch; });
    for (Pool* p : idle) {
        const bool long_idle = mirror_epoch - p->synced_epoch > kLongIdle;
        if (!long_idle && mirror_bytes + want_bytes <= mirror_budget()) continue;
        // (TICK-057, RX_VK_MIRROR_STICKY=1) a pool whose mirror came back within kLongIdle of being given back is not given back
        // for the VRAM rule alone: castle-move on an 8 GB card sat at 84-87% of the allowance, every mirror idle 600 submissions
        // went, and ~3 a second were made again (a 16 MB copy from guest memory over PCIe each, ~5.6 ms of GPU on PC 3050)
        if (!long_idle && p->remade && mirror_sticky_on() && mirror_bytes + want_own <= mirror_budget()) {
            st_sticky_kept++;
            continue;
        }
        (long_idle ? st_freed_long : vram_tight && mirror_bytes + want_own <= mirror_budget() ? st_freed_tight : st_freed_budget)++;
        p->idle_freed_epoch = mirror_epoch;
        mirror_free(*p);
    }
    static const bool keep_lazy = getenv("RX_VK_KEEP_LAZY_MIRRORS") != nullptr;
    if (!keep_lazy)
        for (auto& [lo, p] : imported)
            if (p.mstate == Pool::MIRROR_ON && p.lazy_held && mirror_epoch - p.synced_epoch > kIdleEpochs && p.last_value <= mem_completed &&
                (mirror_epoch - p.synced_epoch > kLongIdle || mirror_bytes + want_bytes > mirror_budget()) &&
                std::find(lazy_release.begin(), lazy_release.end(), &p) == lazy_release.end())
                lazy_release.push_back(&p);
    static int64_t logged_at;
    if (st_mirrors_freed / 16 != logged_at) {
        logged_at = st_mirrors_freed / 16;
        logf("vk: %lld mirrors given back (%llu MB in all); %llu MB of mirrors now; idle ones by why: the VRAM rule %lld, the "
             "mirrors' budget %lld, long idle %lld; made again within %llu submissions of going %lld (%llu MB); kept as made again "
             "(RX_VK_MIRROR_STICKY) %lld", (long long)st_mirrors_freed, (unsigned long long)(st_mirror_bytes_freed >> 20),
             (unsigned long long)(mirror_bytes >> 20), (long long)st_freed_tight, (long long)st_freed_budget, (long long)st_freed_long,
             (unsigned long long)kLongIdle, (long long)st_remade, (unsigned long long)(st_remade_bytes >> 20), (long long)st_sticky_kept);
    }
}

// ---- imports given back under pressure (16 GB PCs). The driver pins every imported pool and counts it in the system heap
// the GPU reaches, whose budget is about half the PC's RAM (7.5 GB of 16): imports were kept for the session, the heap
// went past its budget (7758 of 7503 MB on an RTX 3060 with 16 GB) and new imports failed with VK_ERROR_OUT_OF_DEVICE_MEMORY:
// terrain left out, load zones stuck. Past RX_VK_IMPORT_HIGH percent of the budget (85) the pools bound least lately give
// their imports back down to RX_VK_IMPORT_LOW percent (75): only pools without a mirror, not bound for kImportIdle
// submissions and done on the GPU. They leave the table as a retired pool does (the page table's entries 0, the caches
// emptied) and are imported again at their next binding; their memory is freed once everything recorded until now is
// done. On Linux, on by default only where that heap's budget is under 12 GB (where it ran out); off by default on Windows.
// RX_VK_IMPORT_HIGH=0: off, =85: on anywhere. (Caveat: last_value counts bindings, not shaders reaching a pool through the
// page table; a pool only reached that way would read zeros until it is bound again. Not seen in play: the pools given back
// were long unbound.) ----
bool system_heap_usage(uint64_t* used, uint64_t* budget);   // (vk_vma.cpp)
std::deque<std::map<uint64_t, Pool>::node_type> evicted_pools;
int64_t st_imports_evicted = 0;
uint64_t st_import_bytes_evicted = 0;
constexpr uint64_t kImportIdle = 600;

void imports_give_back() {   // (pools_mu held)
    while (!evicted_pools.empty() && evicted_pools.front().mapped().retired_value <= mem_completed) {
        Pool& p = evicted_pools.front().mapped();
        vkDestroyBuffer(dev.device, p.buffer, nullptr);
        vkFreeMemory(dev.device, p.memory, nullptr);
        for (VkDeviceMemory m : p.pieces) vkFreeMemory(dev.device, m, nullptr);   // (a pool imported as several dma-bufs)
        evicted_pools.pop_front();
    }
    // (hookshot) Off by default on Windows, as giving retired imports back is (RX_VK_KEEP_RETIRED, retired_imports_give_back:
    // device losses on Intel Arc and AMD right after imports went back): RX_VK_IMPORT_HIGH=85 turns it on there
    static const char* env_high = getenv("RX_VK_IMPORT_HIGH");
#ifdef _WIN32
    static const int high = env_high ? atoi(env_high) : 0;
#else
    static const int high = env_high ? atoi(env_high) : 85;
#endif
    static const int low = getenv("RX_VK_IMPORT_LOW") ? std::clamp(atoi(getenv("RX_VK_IMPORT_LOW")), 10, 95) : 75;
    static uint64_t calls = 0;
    if (high <= 0 || ++calls % 16) return;   // (the driver asked every 16 submissions)
    uint64_t used = 0, budget = 0;
    if (!system_heap_usage(&used, &budget)) return;
    static const bool small = [&] {   // (on by default where the budget is small: 16 GB of RAM and less)
        const bool on = env_high || budget < (12ull << 30);
        logf("vk: imports given back past %d%% of a %llu MB system heap budget: %s", high, (unsigned long long)(budget >> 20),
             on ? "on" : "off (RX_VK_IMPORT_HIGH=85 turns it on)");
        return on;
    }();
    if (!small || used <= budget / 100 * (uint64_t)high) return;
    const uint64_t target = budget / 100 * (uint64_t)low;
    const uint64_t want = used > target ? used - target : 0;
    std::vector<std::map<uint64_t, Pool>::iterator> idle;
    for (auto it = imported.begin(); it != imported.end(); ++it) {
        const Pool& p = it->second;
        if ((p.mstate == Pool::MIRROR_NONE || p.mstate == Pool::MIRROR_REFUSED) && p.last_value <= mem_completed &&
            mem_rec_value > p.last_value + kImportIdle)
            idle.push_back(it);
    }
    std::sort(idle.begin(), idle.end(), [](auto a, auto b) { return a->second.last_value < b->second.last_value; });
    uint64_t freed = 0;
    int n = 0;
    for (auto it : idle) {
        if (freed >= want) break;
        Pool& p = it->second;
        guest_pages_set(p.lo, p.hi, 0);
        p.retired_value = mem_rec_value;   // (this submission's recording is being made: freed once it is done)
        freed += p.hi - p.lo;
        n++;
        evicted_pools.push_back(imported.extract(it));
    }
    if (!n) {
        static int logged_none;
        if (logged_none++ < 8)
            logf("vk: system heap %llu of %llu MB: no idle import to give back (%zu imported)", (unsigned long long)(used >> 20),
                 (unsigned long long)(budget >> 20), imported.size());
        return;
    }
    pools_gen++;
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    if (mirror_on()) publish_cap_ranges();
    st_imports_evicted += n, st_import_bytes_evicted += freed;
    static int logged;
    if (logged++ < 40 || logged % 64 == 0)
        logf("vk: system heap %llu of %llu MB: %d idle imports given back (%llu MB); %lld (%llu MB) in all",
             (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20), n, (unsigned long long)(freed >> 20),
             (long long)st_imports_evicted, (unsigned long long)(st_import_bytes_evicted >> 20));
}

void memory_begin_submission() {
    std::lock_guard lk(pools_mu);
    reap_mirrors();
    retired_imports_give_back();
    mirror_epoch++;
    mirror_upload = true;
    mirror_synced.clear();
    writebacks.clear();
    imports_give_back();
    if (tex_capture_on()) publish_tex_spans(texture_frame_no());
    if (!mirror_on()) return;
    {
        uint64_t want = 0;
        for (Pool* p : mirror_wanted) want += p->hi - p->lo;
        if (mirror_epoch % 64 == 0 || mirror_bytes + want > mirror_budget()) mirrors_give_back(want);
    }
    if (mirror_wanted.empty()) return;
    for (Pool* p : mirror_wanted) mirror_make(*p);
    mirror_wanted.clear();
    publish_cap_ranges();
    static uint64_t logged_at;
    if (mirror_bytes >> 28 != logged_at) {
        logged_at = mirror_bytes >> 28;
        logf("vk: %lld pools mirrored in VRAM (%llu MB)", (long long)st_mirrors, (unsigned long long)(mirror_bytes >> 20));
    }
}

/// (hookshot) Before a command buffer's parallel segments: every mirrored pool synced once, into the replay thread's
/// recording, ahead of all of them. Workers record out of order, so a pool a later segment bound first was synced in
/// that segment's upload, which runs after the earlier segments' work, and put the CPU's pages back over what the GPU
/// had just written in them (one frame of wrong lighting or exposure: 5x the flashes of serial replay). A segment
/// finds its pools synced now (synced_epoch, sync_seg 0). RX_VK_NO_PRESYNC=1: as before
void memory_sync_mirrors_before_segments() {
    static const bool off = getenv("RX_VK_NO_PRESYNC") != nullptr;
    if (off || !mirror_on() || !mirror_upload) return;
    const bool stats = presync_stats_on();
    const auto t0 = stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::lock_guard lk(pools_mu);
    const auto t1 = stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    presync_scan = {};
    int64_t pools = 0, synced = 0, bytes_all = 0;
    scatter_batch = true;   // (TICK-021: every pool's in one dispatch)
    in_presync = true;
    for (auto& kv : imported) {
        Pool& p = kv.second;
        pools++;
        if (p.mstate != Pool::MIRROR_ON || p.synced_epoch == mirror_epoch) continue;
        const uint64_t bound_before = p.last_value;
        const int64_t bytes = mirror_sync(p);
        synced++, bytes_all += bytes;
        if (bytes && msync_stats_on()) presync_notes.push_back({&p, kv.first, bound_before, bytes});
        p.sync_seg = 0;
    }
    in_presync = false;
    scatter_batch = false;
    const size_t entries = scatter_pending.size();
    const auto t2 = stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    scatter_flush(upload_command_buffer(), "msync scatter (CPU pages -> mirrors, one dispatch)");
    if (stats && bytes_all) {
        const auto t3 = std::chrono::steady_clock::now();
        static int64_t n, s_pools, s_synced, s_pages, s_dirty, s_bytes, s_entries, s_loop, s_lazy, s_capb, s_liveb;
        static double s_lock, s_scan, s_flush, s_max;
        s_loop += presync_scan.loop_ns, s_lazy += presync_scan.lazy_ns, s_capb += presync_scan.cap_pages, s_liveb += presync_scan.live_pages;
        using ms = std::chrono::duration<double, std::milli>;
        n++, s_pools += pools, s_synced += synced, s_pages += presync_scan.pages, s_dirty += presync_scan.dirty, s_bytes += bytes_all;
        s_entries += (int64_t)entries, s_lock += ms(t1 - t0).count(), s_scan += ms(t2 - t1).count(), s_flush += ms(t3 - t2).count();
        s_max = std::max(s_max, ms(t3 - t0).count());
        if (n == 600) {
            logf("vk: pre-sync (RX_VK_PRESYNC_STATS), a pre-sync with copies: pools %.1f (synced %.1f), pages looked at %.0f, dirty %.0f, "
                 "%.2f MB (%.2f from captures, %.2f live), %.0f scatter entries; ms: lock %.3f, pools %.3f (their page loops %.3f, the "
                 "write-back notes %.3f), scatter %.3f, worst %.2f", s_pools / 600.0, s_synced / 600.0,
                 s_pages / 600.0, s_dirty / 600.0, s_bytes / 600.0 / 1048576.0, s_capb / 600.0 / 1048576.0, s_liveb / 600.0 / 1048576.0,
                 s_entries / 600.0, s_lock / 600, s_scan / 600, s_loop / 600e6, s_lazy / 600e6, s_flush / 600, s_max);
            n = s_pools = s_synced = s_pages = s_dirty = s_bytes = s_entries = s_loop = s_lazy = s_capb = s_liveb = 0;
            s_lock = s_scan = s_flush = s_max = 0;
        }
    }
}

/// A pool's mirror made, filled in the recording's upload command buffer: all of it, then the pages a capture holds
/// as they were at their submit (pools_mu held). False over the budget (tried again when next bound, as mirrors are
/// given back) or over 1 GB (never)
bool mirror_make(Pool& pool) {
    Pool* p = &pool;
    if (inherit_watch_page() != ~0ull && (inherit_watch_page() << kPageShift) >= p->lo && (inherit_watch_page() << kPageShift) < p->hi)
        logf("vk: inherit watch: mirror of %#llx..%#llx made, gen now %u, watched page gen %u", (unsigned long long)p->lo,
             (unsigned long long)p->hi, gen_now, page_gen(inherit_watch_page()));
    {
        const uint64_t n = p->hi - p->lo;
        // (hookshot: RX_VK_MIRROR_TIGHT=1, none while VRAM is 90% full. Off: 8 GB cards sit there all the time, and without their mirrors
        // the GPU read game memory over PCIe)
        static const bool tight_rule = getenv("RX_VK_MIRROR_TIGHT") != nullptr;
        uint64_t used = 0, allowed = 0;
        const bool tight = tight_rule && vram_usage(&used, &allowed) && allowed && used > allowed / 100 * 90;
        if (mirror_bytes + n > mirror_budget() || n > (1ull << 30) || tight) {
            p->mstate = n > (1ull << 30) ? Pool::MIRROR_REFUSED : Pool::MIRROR_NONE;
            static int logged;
            if (logged++ < 8)
                logf("vk: pool %#llx (%llu MB) stays in system memory: the mirrors have %llu MB of %llu", (unsigned long long)p->lo,
                     (unsigned long long)(n >> 20), (unsigned long long)(mirror_bytes >> 20),
                     (unsigned long long)(mirror_budget() >> 20));
            return false;
        }
        MemKindScope mem_kind_(MemKind::Mirror);   // (TICK-038: RX_VK_RES_LOG)
        if (!create_device_buffer(n, kUsage, p->mirror, p->mirror_alloc)) {
            p->mstate = Pool::MIRROR_REFUSED;
            return false;
        }
        if (p->idle_freed_epoch && mirror_epoch - p->idle_freed_epoch <= kLongIdle) {   // (TICK-057: given back as idle, wanted again)
            st_remade++, st_remade_bytes += n;
            p->remade++;
            p->idle_freed_epoch = 0;
        }
        {   // (hookshot) the page table points at the mirror: what the GPU wrote is there, and guest memory has it only after the write-back
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = p->mirror;
            const uint64_t ma = vkGetBufferDeviceAddress(dev.device, &da);
            guest_pages_set(p->lo, p->hi, ma);
            if (getenv("RX_VK_LOG_ADDR"))
                logf("vk: addr mirror %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)p->lo, (unsigned long long)p->hi,
                     (unsigned long long)ma, (unsigned long long)(ma + n));
        }
        const VkBufferCopy all{0, 0, n};
        {
            GprofSpan gp(upload_command_buffer(), "mmake copy (a whole pool -> its new mirror)");
            gp.bytes = n;
            vkCmdCopyBuffer(upload_command_buffer(), p->buffer, p->mirror, 1, &all);   // (all of it, at this generation)
        }
        {   // (EvilOOT) then the mirrors of the pools it took in, over their part: what the GPU wrote there and only there
            // (retire_pool). The captures' pages go over it after, as before: the CPU's newer writes win
            bool barrier_done = false;
            int inherited = 0;
            for (auto& nh : retired_pools) {
                Pool& r = nh.mapped();
                if (!r.inherit_pending || r.mstate != Pool::MIRROR_ON || !r.mirror || r.hi <= p->lo || r.lo >= p->hi) continue;
                // (TICK-036) a mirror made in this submission by a later segment of the parallel replay is filled by that
                // segment's uploads, which the queue runs after this one's: copied now it is still empty, and its zeros went
                // over the new mirror (Castle Town Market's ground and fountain: the vertices and indices of two pools taken
                // in at the scene load, 0 in VRAM, every triangle clipped). It holds guest memory as of this submission and
                // nothing the GPU wrote before this segment: the new mirror's own copy has all of it
                if (r.made_epoch == mirror_epoch && r.made_seg > texture_segment()) {
                    r.inherit_pending = false;
                    r.retired_value = std::max(r.retired_value, mem_rec_value);
                    st_inherit_unfilled++;
                    if (inherit_watch_page() != ~0ull)
                        logf("vk: inherit watch: retired %#llx..%#llx left out: its mirror is filled by a later segment (%llu > %llu)",
                             (unsigned long long)r.lo, (unsigned long long)r.hi, (unsigned long long)r.made_seg,
                             (unsigned long long)texture_segment());
                    continue;
                }
                if (!barrier_done) {   // (after the full copy, and after the shaders of earlier submissions wrote the old mirror)
                    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
                    vkCmdPipelineBarrier2(upload_command_buffer(), &di);
                    barrier_done = true;
                }
                const uint64_t a = std::max(r.lo, p->lo), b = std::min(r.hi, p->hi);
                // (TICK-036 / upstream #1127) only the pages not stamped since its last sync: one the CPU wrote after it has the
                // game's newer data in the copy of guest memory above (and the captures below), and the old mirror's over it
                // counted as synced, stale for good (mirror_inherit.h): Castle Town Market's ground and fountain (a scene's
                // vertices and indices in a pool remade larger while it loaded) came back as zeros; OoT's light probes stale
                // in the fog. As mirror_sync, a page a shader wrote through a bare entry keeps the mirror's copy
                // (RX_VK_PROTECT_BARE=1). Synced in this submission: the whole part. Its synced_gen is raised only at the
                // submission's end (memory_end_submission), so what the GPU wrote in this submission's earlier segments
                // (RX_VK_PARALLEL) is stamped above it, and guest memory has that only after the write-back; the CPU's writes
                // were collected before the sync. RX_VK_INHERIT_ALL=1: the whole part always, as before
                static const bool inherit_all = getenv("RX_VK_INHERIT_ALL") != nullptr;
                static thread_local std::vector<VkBufferCopy> parts;
                parts.clear();
                uint64_t from_guest = 0;
                const uint32_t synced = r.synced_gen;
                if (inherit_all || r.synced_epoch == mirror_epoch)
                    parts.push_back({a - r.lo, a - p->lo, b - a});
                else
                    from_guest = mirror_inherit::runs(a, b, r.lo, p->lo, kPageShift, kRegionPages,
                                                      [&](uint64_t region) { return region_gen[region] <= synced; },
                                                      [&](uint64_t page) {
                                                          return page_gen(page) <= synced ||
                                                                 (protect_bare_on() && prot_page_since(page, synced));
                                                      },
                                                      [&](uint64_t src, uint64_t dst, uint64_t size) { parts.push_back({src, dst, size}); });
                const uint64_t kept = (b - a) - from_guest;
                st_inherit_kept += (int64_t)kept, st_inherit_newer += (int64_t)from_guest;
                if (inherit_watch_page() != ~0ull)
                    logf("vk: inherit watch: pool %#llx..%#llx takes retired %#llx..%#llx (synced gen %u epoch %llu; now gen %u epoch %llu): "
                         "%#llx kept, %#llx newer; watched page gen %u%s", (unsigned long long)p->lo, (unsigned long long)p->hi,
                         (unsigned long long)r.lo, (unsigned long long)r.hi, (unsigned)r.synced_gen, (unsigned long long)r.synced_epoch,
                         gen_now, (unsigned long long)mirror_epoch, (unsigned long long)kept, (unsigned long long)from_guest,
                         page_gen(inherit_watch_page()), (inherit_watch_page() << kPageShift) >= a && (inherit_watch_page() << kPageShift) < b ? " (in it)" : "");
                if (!parts.empty())
                    vkCmdCopyBuffer(upload_command_buffer(), r.mirror, p->mirror, (uint32_t)parts.size(), parts.data());
                if (const uint64_t wa = inherit_watch_page() << kPageShift; inherit_watch_page() != ~0ull && wa >= a && wa < b) {
                    static const uint64_t at = getenv("RX_VK_LOG_INHERIT") ? strtoull(getenv("RX_VK_LOG_INHERIT"), nullptr, 16) & ~15ull : 0;
                    if (!iw_buf) create_buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, iw_buf, iw_alloc, &iw_map);
                    if (iw_buf && at >= a && at + 64 <= b) {
                        const VkBufferCopy c0{at - r.lo, 0, 64}, c1{at - p->lo, 64, 64}, c2{at - p->lo, 128, 64};
                        vkCmdCopyBuffer(upload_command_buffer(), r.mirror, iw_buf, 1, &c0);
                        vkCmdCopyBuffer(upload_command_buffer(), p->buffer, iw_buf, 1, &c1);
                        full_barrier(upload_command_buffer());
                        vkCmdCopyBuffer(upload_command_buffer(), p->mirror, iw_buf, 1, &c2);
                        iw_value = mem_rec_value;
                    }
                }
                r.inherit_pending = false;
                if (r.lazy_held) p->lazy_held = true, r.lazy_held = false;   // (TICK-015: its GPU writes are in the new mirror now)
                r.retired_value = std::max(r.retired_value, mem_rec_value);   // (in use by this upload until it is done)
                inherited++;
            }
            if (inherited) {
                static int logged;
                if (logged++ < 16)
                    logf("vk: pool %#llx..%#llx: mirror made with the GPU's contents of %d retired pool(s) (so far %.1f MB of "
                         "theirs kept, %.1f MB the CPU wrote since taken from guest memory; %lld retired mirrors left out, filled by a later "
                         "segment)", (unsigned long long)p->lo, (unsigned long long)p->hi, inherited, st_inherit_kept / 1048576.0,
                         st_inherit_newer / 1048576.0, (long long)st_inherit_unfilled);
            }
        }
        {   // (then the pages a capture holds as they were at their submit: guest memory is the game's frame ahead)
            std::map<VkBuffer, std::vector<VkBufferCopy>> over;
            for (uint64_t page = p->lo >> kPageShift; page < (p->hi + 4095) >> kPageShift; page++) {
                const CapEntry e = cap_entry(page);
                if (!e.gen || e.gen != page_gen(page)) continue;
                auto b = cap_bufs.find(e.gen);
                if (b == cap_bufs.end()) continue;
                const uint64_t ga = std::max(page << kPageShift, p->lo), gb = std::min((page + 1) << kPageShift, p->hi);
                if (const uint8_t gm = b->second.gmask && e.slot < b->second.gmask->size() ? (*b->second.gmask)[e.slot] : 0xff; gm != 0xff) {
                    for (unsigned k = 0; k < 8; k++) {   // (sprint 5: the slot's granules only)
                        if (!(gm >> k & 1)) continue;
                        const uint64_t g0 = (page << kPageShift) + (uint64_t)k * 512, a2 = std::max(ga, g0), z2 = std::min(gb, g0 + 512);
                        if (a2 < z2) over[b->second.buffer].push_back({(uint64_t)e.slot * 4096 + (a2 - (page << kPageShift)), a2 - p->lo, z2 - a2});
                    }
                    b->second.last_use = std::max(b->second.last_use, mem_rec_value);
                    continue;
                }
                over[b->second.buffer].push_back({(uint64_t)e.slot * 4096 + (ga - (page << kPageShift)), ga - p->lo, gb - ga});
                b->second.last_use = std::max(b->second.last_use, mem_rec_value);
            }
            if (!over.empty()) {
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (after the full copy)
                mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(upload_command_buffer(), &di);
                for (const auto& [buf, rs] : over)
                    vkCmdCopyBuffer(upload_command_buffer(), buf, p->mirror, (uint32_t)rs.size(), rs.data());
            }
        }
        p->mstate = Pool::MIRROR_ON;
        p->mirror_id = ++diff_mirror_ids;   // (TICK-048: the diff's hashes of another mirror do not hold for this one)
        if (!p->mirror_id) p->mirror_id = ++diff_mirror_ids;
        pools_gen.fetch_add(1, std::memory_order_relaxed);   // (the binding's buffer changes: guest memory, then this mirror)
        p->synced_gen = gen_now;
        p->synced_epoch = mirror_epoch;
        p->sync_seg = texture_segment();
        p->made_epoch = mirror_epoch, p->made_seg = texture_segment();
        mirror_synced.push_back(p);
        mirror_bytes += n;
        st_mirrors++;
        st_sync_bytes += (int64_t)n;
    }
    return true;
}

// ---- the write-back's diff (hookshot, 2026-11-09) ----
// A mirrored pool's write-back copied every written run whole from the mirror to guest memory: ~69 MB a frame in Hyrule Field
// (an i7-14700K and an RTX 5070 Ti), 4.4 ms of the GPU's ~13, at the end of the frame's last submission, which the pages of the
// next capture wait for (the capture's write-back wait above). The runs are the written bindings' whole ranges: ~82% of what
// they copied was zeros, and of the rest little changed from one frame to the next. Now a pool written back keeps a shadow in
// VRAM, what guest memory holds there as far as it is known, page by page. A compute pass (shaders/wb_diff.comp) copies the
// words of a run that differ from the shadow, into guest memory and the shadow both; a page the CPU wrote since (the captures'
// pages and the replay's own words, shadow_written) is read from guest memory into the shadow first. Guest memory comes out as
// with the whole copy, or, for words the GPU left as they were and the CPU wrote since, with the CPU's words, as on the console:
// the pass only ever writes fewer words than the copy did. The pass takes ~0.7 ms there, the GPU's frame went from ~13.1 to
// ~9.2 ms, and running there the game went from ~53.9 to ~57.9 fps with 4x frame interpolation (from ~55.2 to ~58.7 without).
// (Word by word it was far slower than the copies: the GPU's writes to system memory went at ~1 GB/s; a subgroup's whole span
// written at once goes at the copies' speed or better.)
// RX_VK_WB_DIFF=0: the whole copies, as before. RX_VK_WB_SHADOW_MB: the shadows' VRAM at most (default 1/8 of it)
std::mutex shadow_written_mu;
std::vector<uint32_t> shadow_written_pages;   // (guest pages the CPU wrote: unknown to the shadows at the next write-back)
bool shadow_written_all = false;              // (more than that list holds: every page unknown at the next write-back)
struct DeadShadow {
    VkBuffer buffer;
    VmaAllocation alloc;
    uint64_t value;
};
std::deque<DeadShadow> dead_shadows;
uint64_t shadow_bytes = 0;
int64_t st_wbd_runs = 0, st_wbd_bytes = 0, st_wbd_fill_pages = 0;
bool wb_diff_on() {
    // (hookshot) On by default again (v2.0.11). v2.0.9 turned it off with the presents' own queue after the device lost on an RTX 4090
    // with NVIDIA's 621 driver (issue 1161); the reporter's A/B then put that on the present queue (black screen with it alone), and the
    // write-back diff alone ran there and on drivers 615 and 621 for others: ~20% fps (an RTX 3080 laptop went 40 -> 25 fps without
    // it). RX_VK_WB_DIFF=0: off
    // (integrated GPUs too: an Ally X's hard reboots stopped with v2.0.8 and came back with v2.0.9's off, issue 997: the whole copies
    // are long submissions there. Its shadows are the system's RAM on them, so at most 512 MB: shadow_room)
    static const bool on = !getenv("RX_VK_WB_DIFF") || atoi(getenv("RX_VK_WB_DIFF")) != 0;
    return on;
}
void shadow_free(Pool& p) {   // (pools_mu held: destroyed once the submissions so far are done)
    if (!p.shadow) return;
    dead_shadows.push_back({p.shadow, p.shadow_alloc, std::max(mem_rec_value, p.last_value)});
    shadow_bytes -= p.hi - p.lo;
    p.shadow = VK_NULL_HANDLE, p.shadow_alloc = nullptr, p.shadow_address = 0;
    p.shadow_known.clear();
}
void shadows_reap() {
    while (!dead_shadows.empty() && dead_shadows.front().value <= mem_completed) {
        destroy_buffer(dead_shadows.front().buffer, dead_shadows.front().alloc);
        dead_shadows.pop_front();
    }
}
bool shadow_room(uint64_t n) {   // (the shadows' budget, and VRAM 85% full at most: the textures and the mirrors come first)
    static const uint64_t cap = [] {
        if (const char* e = getenv("RX_VK_WB_SHADOW_MB")) return (uint64_t)atoll(e) << 20;
        uint64_t used = 0, allowed = 0;
        const uint64_t eighth = vram_usage(&used, &allowed) && allowed ? allowed / 8 : (uint64_t)512 << 20;   // (unsigned long on Linux)
        // (an integrated GPU's VRAM is the system's RAM: 512 MB at most there)
        return dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? std::min<uint64_t>(eighth, (uint64_t)512 << 20) : eighth;
    }();
    if (shadow_bytes + n > cap) return false;
    uint64_t used = 0, allowed = 0;
    return !(vram_usage(&used, &allowed) && allowed && used + n > allowed / 100 * 85);
}
bool shadow_make(Pool& p) {
    const uint64_t n = p.hi - p.lo;
    if (!p.mirror || !shadow_room(n)) return false;
    MemKindScope mem_kind_(MemKind::WbShadow);   // (TICK-038: RX_VK_RES_LOG)
    if (!create_device_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, p.shadow, p.shadow_alloc))
        return false;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.shadow;
    p.shadow_address = vkGetBufferDeviceAddress(dev.device, &da);
    p.shadow_known.assign((size_t)((n >> kPageShift) + 63) / 64, 0);
    shadow_bytes += n;
    static int logged;
    if (logged++ < 16)
        logf("vk: write-back diff: a shadow for pool %#llx (%llu MB; %llu MB of shadows)", (unsigned long long)p.lo,
             (unsigned long long)(n >> 20), (unsigned long long)(shadow_bytes >> 20));
    return true;
}
// (pools_mu held) every shadow given back while VRAM is 95% full (every 120 write-back submissions): made again when there is room
void shadows_check_room() {
    static int64_t n;
    if (!shadow_bytes || ++n % 120) return;
    uint64_t used = 0, allowed = 0;
    if (!vram_usage(&used, &allowed) || !allowed || used <= allowed / 100 * 95) return;
    const uint64_t had = shadow_bytes;
    for (auto& [lo, p] : imported) shadow_free(p);
    for (auto& nh : retired_pools) shadow_free(nh.mapped());
    static int logged;
    if (logged++ < 8)
        logf("vk: write-back diff: VRAM %llu of %llu MB: the shadows given back (%llu MB)", (unsigned long long)(used >> 20),
             (unsigned long long)(allowed >> 20), (unsigned long long)(had >> 20));
}
// (pools_mu held) a range of the pool (offsets) unknown to its shadow again
void shadow_forget(Pool& p, uint64_t at, uint64_t len) {
    if (!p.shadow || !len) return;
    for (uint64_t pg = at >> kPageShift; pg <= (at + len - 1) >> kPageShift; pg++) p.shadow_known[pg >> 6] &= ~(1ull << (pg & 63));
}
// (pools_mu held) the pages written since: unknown to their pools' shadows
void shadows_forget_written() {
    std::vector<uint32_t> pages;
    bool all;
    {
        std::lock_guard lk(shadow_written_mu);
        pages.swap(shadow_written_pages);
        all = shadow_written_all, shadow_written_all = false;
    }
    if (all)   // (written for a long while with no write-back to read the list: nothing of guest memory is known)
        for (auto& [lo, pool] : imported)
            if (pool.shadow) std::fill(pool.shadow_known.begin(), pool.shadow_known.end(), 0ull);
    if (pages.empty() || !shadow_bytes) return;
    std::sort(pages.begin(), pages.end());
    Pool* p = nullptr;
    for (uint32_t pg : pages) {
        const uint64_t a = (uint64_t)pg << kPageShift;
        if (!p || a < p->lo || a >= p->hi) {
            p = nullptr;
            auto it = imported.upper_bound(a);
            if (it == imported.begin()) continue;
            --it;
            if (a < it->second.hi) p = &it->second;
            if (!p) continue;
        }
        if (!p->shadow) continue;
        const uint64_t i = (a - p->lo) >> kPageShift;
        p->shadow_known[i >> 6] &= ~(1ull << (i & 63));
    }
}
// (the pass's pipeline: 16 bytes of push constants and no descriptors, its buffers by address; made once)
struct WbdPipe {
    bool tried = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
} wbd_pipe;
bool wbd_pipe_ready() {
    if (wbd_pipe.tried) return wbd_pipe.pipe != VK_NULL_HANDLE;
    wbd_pipe.tried = true;
    // (the pass votes across a subgroup (shaders/wb_diff.comp): a GPU without votes in compute keeps the whole copies)
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    if (!(sg.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) || !(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
        logf("vk: write-back diff off: the GPU has no subgroup votes in compute shaders");
        return false;
    }
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev.device, &li, nullptr, &wbd_pipe.layout) == VK_SUCCESS) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof WB_DIFF_COMP_SPV;
        mi.pCode = WB_DIFF_COMP_SPV;
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) == VK_SUCCESS) {
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = m;
            ci.stage.pName = "main";
            ci.layout = wbd_pipe.layout;
            if (vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &wbd_pipe.pipe) != VK_SUCCESS)
                wbd_pipe.pipe = VK_NULL_HANDLE;
            vkDestroyShaderModule(dev.device, m, nullptr);
        }
    }
    const bool ok = wbd_pipe.pipe != VK_NULL_HANDLE;
    logf("vk: write-back diff %s", ok ? "on (RX_VK_WB_DIFF=0: off)" : "off: its pipeline was not made");
    return ok;
}
void shadow_written(const uint32_t* pages, int64_t n) {
    if (n <= 0 || !wb_diff_on()) return;
    std::lock_guard lk(shadow_written_mu);
    if (shadow_written_pages.size() > (8u << 20)) {   // (no write-back has read them for a long while: all of them, then)
        shadow_written_all = true;
        shadow_written_pages.clear();
        return;
    }
    shadow_written_pages.insert(shadow_written_pages.end(), pages, pages + n);
}
// A run of a pool (offsets, in whole words) as the pass's chunks, in order: the words before its first 16-byte vector, the
// vectors 64 KB at most at a time, the words after the last; emit(offset, bytes, vectors). (tests/test_wb_diff_split.py)
template <class F> void wbd_split(uint64_t at, uint64_t size, F&& emit) {
    const uint64_t end = at + size;
    const uint64_t body = std::min<uint64_t>(end, (at + 15) & ~(uint64_t)15), body_end = std::max<uint64_t>(body, end & ~(uint64_t)15);
    if (body > at) emit(at, body - at, false);
    for (uint64_t a = body; a < body_end;) {
        const uint64_t len = std::min<uint64_t>(body_end - a, 64u << 10);
        emit(a, len, true);
        a += len;
    }
    if (end > body_end) emit(body_end, end - body_end, false);
}
struct WbdChunk {   // (shaders/wb_diff.comp's Chunk)
    uint64_t mirror, shadow, guest;
    uint32_t count, vecs;   // (16-byte vectors when vecs is 1, else words)
};
// (the pass's tables, mapped, a ring: one is taken again once the submission that read it is done)
struct WbdTable {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    uint64_t bytes = 0, value = 0;
};
WbdTable wbd_tables[64];
int wbd_table_next = 0;
WbdTable* wbd_table_get(uint64_t bytes) {
    WbdTable& t = wbd_tables[wbd_table_next];
    if (t.value > mem_completed) return nullptr;   // (still read by a submission the GPU has not done)
    if (t.bytes < bytes) {
        if (t.buf) destroy_buffer(t.buf, t.alloc);
        t = {};
        const uint64_t n = std::max<uint64_t>(128u << 10, (bytes + 65535) & ~65535ull);
        void* map = nullptr;
        MemKindScope mem_kind_(MemKind::Staging);   // (TICK-038: RX_VK_RES_LOG)
        if (!create_buffer(n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, t.buf, t.alloc, &map) ||
            !map) {
            if (t.buf) destroy_buffer(t.buf, t.alloc);
            t = {};
            return nullptr;
        }
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = t.buf;
        t.address = vkGetBufferDeviceAddress(dev.device, &da);
        t.map = (uint8_t*)map;
        t.bytes = n;
    }
    wbd_table_next = (wbd_table_next + 1) % 64;
    t.value = mem_rec_value;
    return &t;
}

void memory_end_submission(VkCommandBuffer cmd) {
    std::lock_guard lk(pools_mu);
    for (const PresyncNote& n : presync_notes) {   // (TICK-048: a pool the presync synced that no binding used after it)
        const auto it = imported.find(n.key);
        if (it != imported.end() && &it->second == n.p && n.p->last_value == n.bound_before) sst.pre_unbound += n.bytes;
    }
    presync_notes.clear();
    {   // (every 600 submissions: how the mirror syncs' dirty pages were read)
        static int64_t n, l_live, l_cap;
        if (++n % 600 == 0) {
            static int64_t l_old;
            logf("vk: mirror syncs (600 submits): %lld dirty pages from their submits' captures, %lld from earlier ones', "
                 "%lld from guest memory as it was at the replay; %zu captures kept; %lld pages the GPU wrote copied over "
                 "(%lld live); %lld kept for the GPU (RX_VK_PROTECT_BARE)", (long long)(st_sync_cap_pages - l_cap),
                 (long long)(st_sync_old_pages - l_old), (long long)(st_sync_live_pages - l_live), cap_bufs.size(),
                 (long long)st_sync_gpu_pages, (long long)st_sync_gpu_live, (long long)st_prot_kept.load());
            l_cap = st_sync_cap_pages, l_live = st_sync_live_pages, l_old = st_sync_old_pages;
            if (msync_stats_on()) {   // (TICK-048: the syncs' bytes a submit, by kind and pool, and the diff's)
                const double k = 1.0 / 600 / 1048576;
                const int64_t all = sst.pre + sst.bind + sst.seg;
                logf("vk: msync bytes (600 submits, MB a submit): %.3f copied: presync %.3f (of pools no binding of the submission used "
                     "after it: %.3f), at a binding %.3f, in a segment %.3f; from this submit's capture %.3f, an earlier one's %.3f, "
                     "guest memory %.3f; readback refresh %.3f", all * k, sst.pre * k, sst.pre_unbound * k, sst.bind * k, sst.seg * k, sst.cap * k,
                     sst.old * k, sst.live * k, st_rb_bytes.exchange(0) * k);
                if (diff_mode()) {
                    logf("vk: msync diff (600 submits, a submit): %.0f pages hashed, %.0f with hashes that held (others: %.0f new, "
                         "%.0f another mirror, %.0f written by the GPU since; %.0f voided by other copies); unchanged MB a submit at "
                         "256 B %.3f, 512 B %.3f, 1 KB %.3f, 2 KB %.3f, 4 KB %.3f; left out %.3f MB; hashing %.3f ms a submit; "
                         "table %llu MB", sst.d_pages / 600.0, sst.d_valid / 600.0, sst.d_new / 600.0, sst.d_mid / 600.0,
                         sst.d_gw / 600.0, sst.d_inval / 600.0, sst.d_same[0] * k, sst.d_same[1] * k, sst.d_same[2] * k,
                         sst.d_same[3] * k, sst.d_same[4] * k, sst.d_skipped * k, sst.d_ns / 600.0 / 1e6,
                         (unsigned long long)(diff_table_bytes >> 20));
                }
                std::vector<std::pair<int64_t, uint64_t>> top;
                for (const auto& [lo, ps] : sync_by_pool) top.push_back({ps.bytes + ps.skipped, lo});
                std::sort(top.rbegin(), top.rend());
                std::string s;
                for (size_t i = 0; i < top.size() && i < 10; i++) {
                    const PoolSync& ps = sync_by_pool[top[i].second];
                    uint32_t flags = 0;
                    {
                        std::lock_guard nk(nvn_pools_mu);
                        if (const NvnPool* np = nvn_pool_at(top[i].second, nullptr)) flags = np->flags;
                    }
                    char b[200];
                    snprintf(b, sizeof b, " %#llx (%llu MB, flags %#x): %.3f copied, %.3f unchanged, %.3f left out;",
                             (unsigned long long)top[i].second, (unsigned long long)(ps.size >> 20), flags, ps.bytes * k,
                             ps.same * k, ps.skipped * k);
                    s += b;
                }
                logf("vk:   msync by pool (MB a submit):%s", s.c_str());
                sync_by_pool.clear();
                sst = SyncStats{};
            }
            {   // (TICK-021: the scatter copy's share)
                static int64_t l_se, l_sd, l_sb, l_sc, l_sy;
                const int64_t se = st_scatter_entries.load(), sd = st_scatter_dispatches.load(), sb = st_scatter_bytes.load();
                logf("vk: mirror syncs (600 submits, a submit): %.1f regions, %.2f MB; scatter %.1f dispatches, %.1f entries, %.2f MB "
                     "(rbrefresh included)", (st_sync_copies - l_sc) / 600.0, (st_sync_bytes - l_sy) / 600.0 / 1048576, (sd - l_sd) / 600.0,
                     (se - l_se) / 600.0, (sb - l_sb) / 600.0 / 1048576);
                l_se = se, l_sd = sd, l_sb = sb, l_sc = st_sync_copies, l_sy = st_sync_bytes;
            }
            if (shadow_bytes || st_wbd_runs)
                logf("vk: write-back diff (600 submits): %lld runs, %.1f MB compared; %lld pages read into the shadows; %llu MB of shadows",
                     (long long)st_wbd_runs, st_wbd_bytes / 1048576.0, (long long)st_wbd_fill_pages, (unsigned long long)(shadow_bytes >> 20));
            st_wbd_runs = st_wbd_bytes = st_wbd_fill_pages = 0;
            std::vector<std::pair<int64_t, uint64_t>> top;
            for (const auto& [lo, k] : live_by_pool) top.push_back({k, lo});
            std::sort(top.rbegin(), top.rend());
            std::string s;
            for (size_t i = 0; i < top.size() && i < 8; i++) {
                const auto it = imported.find(top[i].second);
                char b[160];
                bool captured = false;
                {
                    std::lock_guard ck(cap_ranges_mu);
                    for (const auto& r : cap_ranges) captured |= top[i].second >= r.first && top[i].second < r.second;
                }
                snprintf(b, sizeof b, " %#llx (%llu MB%s, state %d): %lld;", (unsigned long long)top[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         captured ? ", captured" : ", NOT captured", it != imported.end() ? (int)it->second.mstate : -1,
                         (long long)top[i].first);
                s += b;
            }
            logf("vk:   read live, by pool:%s", s.c_str());
            live_by_pool.clear();
            // (and the bindings the GPU read in guest memory itself: pools with no mirror yet, or none (state 9: bound
            // outside a recording))
            std::vector<std::pair<int64_t, uint64_t>> dtop;
            for (const auto& [lo, e] : direct_by_pool) dtop.push_back({e.first, lo});
            std::sort(dtop.rbegin(), dtop.rend());
            std::string ds;
            for (size_t i = 0; i < dtop.size() && i < 8; i++) {
                const auto it = imported.find(dtop[i].second);
                char b[160];
                snprintf(b, sizeof b, " %#llx (%llu MB, state %d): %lld binds, %.0f MB;", (unsigned long long)dtop[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         direct_by_pool[dtop[i].second].second, (long long)dtop[i].first,
                         direct_bytes_by_pool[dtop[i].second] / 1048576.0);
                ds += b;
            }
            if (!dtop.empty()) {
                double all = 0;
                for (const auto& [lo, bytes] : direct_bytes_by_pool) all += (double)bytes;
                logf("vk:   bound in guest memory, by pool (the GPU reads these over PCIe; binds and the bytes of their ranges over these 600 submits, "
                     "%.1f MB in all, a range counted once a submit): %s", all / 1048576.0, ds.c_str());
            }
            direct_by_pool.clear(), direct_bytes_by_pool.clear();
            {   // (TICK-019) what else the GPU reads in imported guest memory, not through a mirror
                static int64_t l_fast, l_fastb, l_tv, l_tvb, l_un;
                const int64_t f = st_direct_fast.load(), fb = st_direct_fast_bytes.load(), tv = st_import_texel_views.load(),
                              tvb = st_import_texel_bytes.load(), un = st_import_unswizzle_bytes.load();
                logf("vk: GPU reads of imported guest memory (600 submits, a submit): binds on the lock-free path %.1f (%.2f MB), texel buffer "
                     "views %.1f (%.2f MB), GPU unswizzles %.2f MB; mirrors %llu MB", (f - l_fast) / 600.0, (fb - l_fastb) / 600.0 / 1048576,
                     (tv - l_tv) / 600.0, (tvb - l_tvb) / 600.0 / 1048576, (un - l_un) / 600.0 / 1048576, (unsigned long long)(mirror_bytes >> 20));
                l_fast = f, l_fastb = fb, l_tv = tv, l_tvb = tvb, l_un = un;
            }
            if (wb_lazy_on()) {
                static int64_t l_lazy, l_wb;
                logf("vk: write-backs (600 submits): %.1f MB a submit copied, %.1f MB left in the mirrors (pools the CPU does not "
                     "read); %lld pools left out so far, of them read later %lld, written later %lld; bound in guest memory while "
                     "holding such writes %lld", (st_wb_bytes - l_wb) / 600.0 / 1048576, (st_lazy_bytes - l_lazy) / 600.0 / 1048576,
                     (long long)st_lazy_pools, (long long)st_lazy_late, (long long)st_lazy_cpu, (long long)st_lazy_direct.load());
                l_lazy = st_lazy_bytes, l_wb = st_wb_bytes;
                logf("vk:   written back, MB a submit: outside the game's pools %.2f, pools the CPU reads %.2f, writes %.2f, "
                     "uncached %.2f, not left out yet %.2f, texture storage %.2f", st_kept_by[0] / 600.0 / 1048576,
                     st_kept_by[1] / 600.0 / 1048576, st_kept_by[2] / 600.0 / 1048576, st_kept_by[3] / 600.0 / 1048576,
                     st_kept_by[4] / 600.0 / 1048576, st_kept_by[5] / 600.0 / 1048576);
                for (int64_t& k : st_kept_by) k = 0;
            }
        }
    }
    static const bool no_writeback = getenv("RX_VK_NO_WRITEBACK") != nullptr;   // (an experiment: none)
    if (no_writeback) writebacks.clear();
    if (!writebacks.empty()) {
        // per pool, the ranges merged, one copy each (mirror -> the guest's pool)
        std::sort(writebacks.begin(), writebacks.end(), [](const WriteBack& a, const WriteBack& b) {
            return a.pool != b.pool ? a.pool < b.pool : a.lo < b.lo;
        });
        GprofSpan gp(cmd, "wb copies (mirror -> guest memory, with its two barriers)");   // (RX_VK_PASS_TIMING)
        gp.items = 0;
        transfer_barrier(cmd);
        std::vector<VkBufferCopy> runs;
        // (the write-back's diff: the runs of pools with a shadow, as chunks of 64 KB at most, for one pass after the loop)
        const bool diff = wb_diff_on() && wbd_pipe_ready();
        if (diff) shadows_forget_written(), shadows_reap(), shadows_check_room();
        static thread_local std::vector<WbdChunk> chunks;
        struct ChunkSrc { Pool* p; uint64_t at, len; };
        static thread_local std::vector<ChunkSrc> chunk_src;   // (for the whole copy, when no table is free)
        static thread_local std::vector<VkBufferCopy> fills, odd;
        chunks.clear(), chunk_src.clear();
        bool filled = false;
        for (size_t i = 0; i < writebacks.size();) {
            Pool* p = writebacks[i].pool;
            runs.clear();
            for (; i < writebacks.size() && writebacks[i].pool == p; i++) {
                const WriteBack& w = writebacks[i];
                if (!runs.empty() && w.lo <= runs.back().srcOffset + runs.back().size)
                    runs.back().size = std::max(runs.back().size, w.hi - runs.back().srcOffset);
                else
                    runs.push_back({w.lo, w.lo, w.hi - w.lo});
            }
            cut_cpu_words(*p, runs);
            if (wb_lazy_on()) lazy_filter(*p, runs);   // (TICK-015)
            if (runs.empty()) continue;
            checkpoint_mark(cmd, p->lo, 'b');
            uint64_t run_bytes = 0;
            for (const VkBufferCopy& r : runs) run_bytes += r.size;
            if (diff && !p->shadow && run_bytes >= (256u << 10)) shadow_make(*p);   // (a pool whose write-back is worth one, as VRAM allows)
            if (diff && p->shadow && p->address && p->mirror) {
                VkBufferDeviceAddressInfo mda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                mda.buffer = p->mirror;
                const VkDeviceAddress mirror_address = vkGetBufferDeviceAddress(dev.device, &mda);
                // (a run not in whole words, as a copy of the game's may be: copied whole, its pages then unknown to the shadow)
                odd.clear();
                for (const VkBufferCopy& r : runs)
                    if ((r.dstOffset | r.size) & 3) odd.push_back(r);
                // the pages of the runs the shadow does not know: from guest memory, whole, first
                fills.clear();
                const uint64_t pool_n = p->hi - p->lo;
                for (const VkBufferCopy& r : runs)
                    for (uint64_t pg = r.dstOffset >> kPageShift; !((r.dstOffset | r.size) & 3) && pg <= (r.dstOffset + r.size - 1) >> kPageShift; pg++) {
                        uint64_t& bits = p->shadow_known[pg >> 6];
                        if (bits & (1ull << (pg & 63))) continue;
                        bits |= 1ull << (pg & 63);
                        const uint64_t at = pg << kPageShift, len = std::min<uint64_t>(4096, pool_n - at);
                        if (!fills.empty() && fills.back().srcOffset + fills.back().size == at)
                            fills.back().size += len;
                        else
                            fills.push_back({at, at, len});
                        st_wbd_fill_pages++;
                    }
                if (!fills.empty()) {
                    vkCmdCopyBuffer(cmd, p->buffer, p->shadow, (uint32_t)fills.size(), fills.data());
                    filled = true;
                }
                if (!odd.empty()) {
                    vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)odd.size(), odd.data());
                    for (const VkBufferCopy& r : odd) shadow_forget(*p, r.dstOffset, r.size);
                }
                for (const VkBufferCopy& r : runs) {
                    if ((r.dstOffset | r.size) & 3) continue;
                    wbd_split(r.dstOffset, r.size, [&](uint64_t at, uint64_t len, bool vecs) {
                        chunks.push_back({mirror_address + at, p->shadow_address + at, p->address + at,
                                          (uint32_t)(len / (vecs ? 16 : 4)), vecs ? 1u : 0u});
                        chunk_src.push_back({p, at, len});
                    });
                }
                st_wbd_runs += (int64_t)runs.size(), st_wbd_bytes += (int64_t)run_bytes;
            } else {
                vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)runs.size(), runs.data());
            }
            for (const VkBufferCopy& r : runs) st_wb_bytes += (int64_t)r.size, gp.bytes += r.size;
            gp.items += (int)runs.size();
            for (const VkBufferCopy& r : runs) wb_pages_note(p->lo + r.dstOffset, p->lo + r.dstOffset + r.size, mem_rec_value);   // (EvilBoyce)
            {   // (hookshot) RX_VK_LOG_WRITEBACK=1: the pools written back the most, every 600 write-back submissions
                static const bool log_wb = getenv("RX_VK_LOG_WRITEBACK") != nullptr;
                if (log_wb) {
                    static std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> by_pool;   // lo -> bytes, runs
                    static int64_t subs;
                    auto& e = by_pool[p->lo];
                    for (const VkBufferCopy& r : runs) e.first += r.size;
                    e.second += runs.size();
                    if (i == writebacks.size() && ++subs % 600 == 0) {
                        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> v(by_pool.begin(), by_pool.end());
                        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                        std::string s;
                        for (size_t k = 0; k < v.size() && k < 8; k++) {
                            char b[120];
                            const auto it = imported.find(v[k].first);
                            snprintf(b, sizeof b, " %#llx (%llu MB pool): %.1f MB in %.0f runs;", (unsigned long long)v[k].first,
                                     it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                                     v[k].second.first / 600.0 / 1048576.0, v[k].second.second / 600.0);
                            s += b;
                        }
                        logf("vk: write-back a submission, by pool:%s", s.c_str());
                        by_pool.clear();
                    }
                }
            }
        }
        if (!chunks.empty()) {
            WbdTable* t = wbd_table_get(chunks.size() * sizeof(WbdChunk));
            if (!t) {   // (no table free: the whole copies, the shadows forgotten for those pages)
                for (const ChunkSrc& c : chunk_src) {
                    const VkBufferCopy r{c.at, c.at, c.len};
                    vkCmdCopyBuffer(cmd, c.p->mirror, c.p->buffer, 1, &r);
                    shadow_forget(*c.p, c.at, c.len);
                }
                static int logged;
                if (logged++ < 8) logf("vk: write-back diff: no chunk table free (%zu chunks): this one copied whole", chunks.size());
            }
            if (filled) transfer_barrier(cmd);   // (the shadows' pages from guest memory before the pass reads them)
            if (t) {
                memcpy(t->map, chunks.data(), chunks.size() * sizeof(WbdChunk));
                flush_mapped(t->alloc, chunks.size() * sizeof(WbdChunk));
                GprofSpan gd(cmd, "wb diff (mirror -> guest memory where it differs from the shadow)");
                gd.items = (int)chunks.size();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wbd_pipe.pipe);
                const struct { uint64_t table; uint32_t count, pad; } pc{t->address, (uint32_t)chunks.size(), 0};
                vkCmdPushConstants(cmd, wbd_pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                const uint32_t n = (uint32_t)chunks.size();
                vkCmdDispatch(cmd, std::min<uint32_t>(n, 65535u), (n + 65534u) / 65535u, 1);
            }
        }
        transfer_barrier(cmd);
        writebacks.clear();
    }
    if (!lazy_release.empty()) {   // (TICK-023, above: the left-out pages written back, then the mirror freed)
        std::vector<VkBufferCopy> runs;
        bool barrier = false;
        for (Pool* p : lazy_release) {
            const auto live = imported.find(p->lo);
            // (still a live pool, mirrored, idle: one bound again since the listing keeps its mirror)
            if (live == imported.end() || &live->second != p || p->mstate != Pool::MIRROR_ON || !p->lazy_held ||
                p->synced_epoch + kIdleEpochs >= mirror_epoch)
                continue;
            runs.clear();
            {
                std::lock_guard nk(nvn_pools_mu);
                for (uint64_t q = p->lo >> kPageShift; q <= (p->hi - 1) >> kPageShift; q++) {
                    if (!skipped_pages.count((uint32_t)q) || cpu_pages.count((uint32_t)q)) continue;
                    const uint64_t a = std::max(p->lo, q << kPageShift), b = std::min(p->hi, (q + 1) << kPageShift);
                    if (a >= b || replay_tex_storage_in(a, b - a)) continue;
                    if (!runs.empty() && runs.back().srcOffset + runs.back().size == a - p->lo) runs.back().size += b - a;
                    else runs.push_back({a - p->lo, a - p->lo, b - a});
                }
            }
            cut_cpu_words(*p, runs);
            if (!runs.empty()) {
                if (!barrier) transfer_barrier(cmd), barrier = true;
                vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)runs.size(), runs.data());
                for (const VkBufferCopy& r : runs) {
                    st_lazy_written_back += r.size;
                    wb_pages_note(p->lo + r.dstOffset, p->lo + r.dstOffset + r.size, mem_rec_value);
                }
            }
            p->lazy_held = false;
            if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;   // (the copy reads the mirror: freed after it)
            st_lazy_released++, st_lazy_released_bytes += p->hi - p->lo;
            mirror_free(*p);
        }
        if (barrier) transfer_barrier(cmd);
        lazy_release.clear();
        static int64_t logged_at = -1;
        if (st_lazy_released / 8 != logged_at) {
            logged_at = st_lazy_released / 8;
            logf("vk: mirrors holding left-out write-backs given back: %lld (%llu MB), %.2f MB written back into guest memory first; "
                 "%llu MB of mirrors now", (long long)st_lazy_released, (unsigned long long)(st_lazy_released_bytes >> 20),
                 st_lazy_written_back / 1048576.0, (unsigned long long)(mirror_bytes >> 20));
        }
    }
    // (what this submission synced is current as of now: the GPU's own writes, stamped while it was recorded, are
    // in the mirror and written back; CPU writes collected at the next submission get a newer generation)
    for (Pool* p : mirror_synced) p->synced_gen = gen_now;
    mirror_synced.clear();
    mirror_upload = false;
}

/// (TICK-038, RX_VK_RES_LOG=1) one line of the process's memory: commit, working set, the C heaps, and what this file keeps in
/// system memory (the captures' buffers, the texture-storage shadow, the capture table)
void memory_res_log() {
    if (!res_log_on()) return;
    struct Counters {
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
            QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    };
    using MemInfo = BOOL(WINAPI*)(HANDLE, Counters*, DWORD);
    struct Summary { DWORD cb; SIZE_T allocated, committed, reserved, max_reserve; };
    using HeapSum = BOOL(WINAPI*)(HANDLE, DWORD, Summary*);
    static const MemInfo mem_info = (MemInfo)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    static const HeapSum heap_sum = (HeapSum)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapSummary");
    Counters m{};
    m.cb = sizeof m;
    if (mem_info) mem_info(GetCurrentProcess(), &m, sizeof m);
    uint64_t h_alloc = 0, h_commit = 0;
    HANDLE heaps[64];
    const DWORD nh = GetProcessHeaps(64, heaps);
    for (DWORD i = 0; i < nh && i < 64 && heap_sum; i++) {
        Summary su{};
        su.cb = sizeof su;
        if (heap_sum(heaps[i], 0, &su)) h_alloc += su.allocated, h_commit += su.committed;
    }
    size_t kept = 0, spares = 0;
    {
        std::lock_guard lk(cap_mu);
        kept = captures.size(), spares = spare.size();
    }
    size_t chunks = 0;   // (the capture table's chunks made: the replay's, read racily, for the count only)
    for (const auto& c : cap_table) chunks += c != nullptr;
    uint64_t free_n = 0, free_b = 0, made = 0, aged = 0, gets = 0, need_max = 0, taken_over = 0, free_low = 0, used_b = 0;
    char spare_sizes[160] = "";
    {
        std::lock_guard lk(cap_free_mu);
        free_n = cap_free.size(), free_b = cap_free_bytes, made = cap_bufs_made, aged = cap_spares_aged, gets = cap_gets;
        need_max = cap_need_max, taken_over = cap_taken_over, free_low = cap_free_low == ~0ull ? cap_free.size() : cap_free_low;
        cap_need_max = 0, cap_taken_over = 0, cap_free_low = ~0ull;
        int n = 0;
        for (const FreeBuf& f : cap_free)
            if (n < 150) n += snprintf(spare_sizes + n, sizeof spare_sizes - n, " %llu", (unsigned long long)(f.size >> 20));
    }
    used_b = cap_used;   // (the replay's, read racily, for the count only)
    logf("vk: capture buffers since the last line: largest need %llu KB, %llu spares over twice the need's size class (refused with "
         "RX_VK_CAP_FIT, else taken), fewest "
         "spares left after a take %llu; kept buffers filled %llu MB of %llu; spare sizes (MB):%s",
         (unsigned long long)(need_max >> 10), (unsigned long long)taken_over, (unsigned long long)free_low,
         (unsigned long long)(used_b >> 20), (unsigned long long)(cap_bytes >> 20), spare_sizes);
    logf("vk: process memory: private %llu MB, working set %llu MB (peak %llu); C heaps (%lu) %llu MB in use of %llu committed; "
         "captures: %zu queued, %zu buffers kept %llu MB, %llu spare buffers %llu MB (%llu made, %llu spares aged out of %llu asked), "
         "%zu spare records; texture shadow %zu pages "
         "(%llu MB); capture table %zu chunks (%llu MB)",
         (unsigned long long)(m.PrivateUsage >> 20), (unsigned long long)(m.WorkingSetSize >> 20),
         (unsigned long long)(m.PeakWorkingSetSize >> 20), (unsigned long)nh, (unsigned long long)(h_alloc >> 20),
         (unsigned long long)(h_commit >> 20), kept, cap_bufs.size(), (unsigned long long)(cap_bytes >> 20),
         (unsigned long long)free_n, (unsigned long long)(free_b >> 20), (unsigned long long)made,
         (unsigned long long)aged, (unsigned long long)gets, spares, shadow_n.load(),
         (unsigned long long)((uint64_t)shadow_n.load() * 4096 >> 20), chunks,
         (unsigned long long)((uint64_t)chunks * kCapChunkPages * sizeof(CapEntry) >> 20));
    size_t g = 0, used = 0;
    int64_t dropped = 0;
    res_pipeline_counts(&g, &used, &dropped);
    logf("vk: pipelines: %zu graphics made, %zu pipelines bound so far; %lld compute shader modules let go", g, used,
         (long long)dropped);
    mem_kinds_log();
}
/// (TICK-051, RX_VK_FRAME_TRACE) running totals for the frame trace: the captures' GPU waits for their late pages, and the
/// mirror syncs' scatter bytes
void memory_trace_counts(int64_t* late_ns, int64_t* late_n, int64_t* scatter_bytes) {
    *late_ns = st_tr_late_ns.load(std::memory_order_relaxed), *late_n = st_tr_late_n.load(std::memory_order_relaxed);
    *scatter_bytes = st_scatter_bytes.load(std::memory_order_relaxed);
}
void memory_stats(int64_t* mirrors, uint64_t* mirror_mb, int64_t* sync_bytes, int64_t* sync_copies, int64_t* wb_bytes) {
    *mirrors = st_mirrors, *mirror_mb = mirror_bytes >> 20, *sync_bytes = st_sync_bytes, *sync_copies = st_sync_copies;
    *wb_bytes = st_wb_bytes;
}
void memory_precopy_present() {   // (sprint 5, RX_VK_PRECOPY_KICK=2: a present begins)
    if (precopy_kick_mode() == 3 && precopy_on() && precopy_kick_event) SetEvent(precopy_kick_event), st_precopy_kicks.fetch_add(1, std::memory_order_relaxed);
}
} // namespace mkwvk

// (TICK-015) a second export, resolved by host/rx_backend.c if present: a game pool made, with its NVNmemoryPoolFlags (outside
// MkwVkRenderer, whose header (vk_api.h) is in the shader caches' stamp)
/// (TICK-054) the host's kick of the pre-copy thread (RX_VK_PRECOPY_KICK=1): host/rx_backend.c rx_precopy_kick
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_precopy_kick(void) {
    using namespace mkwvk;
    if (precopy_kick_event) SetEvent(precopy_kick_event), st_precopy_kicks.fetch_add(1, std::memory_order_relaxed);
}
#ifdef _WIN32
extern "C" __declspec(dllexport) void mkwvk_pool_flags(uint64_t gpu, uint64_t size, uint32_t flags) {
#else
extern "C" __attribute__((visibility("default"))) void mkwvk_pool_flags(uint64_t gpu, uint64_t size, uint32_t flags) {
#endif
    mkwvk::memory_pool_flags(gpu, size, flags);
}
