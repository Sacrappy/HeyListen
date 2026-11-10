// Vulkan Memory Allocator (v3.3.0, from Eglantine's CPM cache: nx2/build.py adds its include path): images and
// device buffers are sub-allocated from large blocks (NVIDIA allows ~4096 allocations, and the imported pools
// already take about 2400 of them). Its Vulkan functions come from vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_internal.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace mkwvk {

VmaAllocator vma;

// ---- (TICK-038) allocations by kind and memory class, RX_VK_RES_LOG=1 ----
thread_local MemKind mem_kind_tag = MemKind::Other;
bool res_log_on() {
    static const bool v = getenv("RX_VK_RES_LOG") && atoi(getenv("RX_VK_RES_LOG")) != 0;
    return v;
}
namespace {
constexpr int kClasses = 4;   // (0 VRAM, 1 VRAM the CPU maps (BAR), 2 system cached, 3 system uncached)
std::mutex acct_mu;
struct AcctEntry { uint8_t kind, cls; uint64_t size; };
std::unordered_map<VmaAllocation, AcctEntry> acct;
uint64_t acct_bytes[(int)MemKind::Count][kClasses];
uint64_t acct_count[(int)MemKind::Count][kClasses];
void note_alloc(VmaAllocation a, MemKind kind) {
    if (!res_log_on() || !a) return;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, a, &info);
    const VkMemoryPropertyFlags f = dev.memory.memoryTypes[info.memoryType].propertyFlags;
    const uint8_t cls = (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 1 : 0)
                                                                    : ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2 : 3);
    std::lock_guard lk(acct_mu);
    acct[a] = {(uint8_t)kind, cls, info.size};
    acct_bytes[(int)kind][cls] += info.size, acct_count[(int)kind][cls]++;
}
void note_free(VmaAllocation a) {
    if (!res_log_on() || !a) return;
    std::lock_guard lk(acct_mu);
    auto it = acct.find(a);
    if (it == acct.end()) return;
    acct_bytes[it->second.kind][it->second.cls] -= it->second.size, acct_count[it->second.kind][it->second.cls]--;
    acct.erase(it);
}
MemKind image_kind(const VkImageCreateInfo& ci) {
    if (mem_kind_tag != MemKind::Other) return mem_kind_tag;
    return (ci.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ? MemKind::Target
                                                                                                           : MemKind::Texture;
}
}  // namespace
void mem_kinds_log() {
    if (!res_log_on()) return;
    static const char* const names[] = {"other", "textures", "render targets", "mirrors", "write-back shadows", "captures",
                                        "streams", "staging", "readback", "scratch"};
    char line[1600];
    int n = snprintf(line, sizeof line, "vk: allocations by kind, MB (VRAM / VRAM mapped / system cached / system uncached, count):");
    std::lock_guard lk(acct_mu);
    uint64_t tot[kClasses]{};
    for (int k = 0; k < (int)MemKind::Count && n < (int)sizeof line - 160; k++) {
        uint64_t c = 0;
        for (int j = 0; j < kClasses; j++) c += acct_count[k][j], tot[j] += acct_bytes[k][j];
        if (!c) continue;
        n += snprintf(line + n, sizeof line - n, " %s %llu / %llu / %llu / %llu (%llu);", names[k],
                      (unsigned long long)(acct_bytes[k][0] >> 20), (unsigned long long)(acct_bytes[k][1] >> 20),
                      (unsigned long long)(acct_bytes[k][2] >> 20), (unsigned long long)(acct_bytes[k][3] >> 20),
                      (unsigned long long)c);
    }
    snprintf(line + n, sizeof line - n, " all %llu / %llu / %llu / %llu", (unsigned long long)(tot[0] >> 20),
             (unsigned long long)(tot[1] >> 20), (unsigned long long)(tot[2] >> 20), (unsigned long long)(tot[3] >> 20));
    logf("%s", line);
}

bool vma_init(PFN_vkGetInstanceProcAddr gipa) {
    VmaVulkanFunctions f{};
    f.vkGetInstanceProcAddr = gipa;
    f.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
               (dev.memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    ci.physicalDevice = dev.physical;
    ci.device = dev.device;
    ci.instance = dev.instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &f;
    ci.preferredLargeHeapBlockSize = 64ull << 20;   // (64 MB blocks: Eglantine found 256 MB ones wasted VRAM)
    return vmaCreateAllocator(&ci, &vma) == VK_SUCCESS;
}

// Images come from a pool of 128 MB blocks: on their own, VMA gave most of them dedicated memory (the driver prefers it
// for render targets, and anything over half a default block), a vkAllocateMemory and a kernel call for each, and
// the dynamic resolution makes a new set of render targets every few frames. Images too large for a block, or of a
// memory type the pool's is not, are allocated as before.
VmaPool image_pool = VK_NULL_HANDLE;
bool image_pool_tried = false;
// (RX_VK_IMAGE_BLOCK_MB=<n>, 16 and up: the block's size. VMA keeps one block empty for the next image, 128 MB of VRAM that a 64 MB block
// would halve, at twice the memory objects: the driver allows ~4096, the imported pools take ~2400)
const uint64_t kImageBlock = (getenv("RX_VK_IMAGE_BLOCK_MB") ? (uint64_t)std::max(16, atoi(getenv("RX_VK_IMAGE_BLOCK_MB"))) : 128u) << 20;

// (hookshot) and the render targets' images in blocks of their own (texture_get's for a target: OoT's transient ones, made and let go
// all the time). In the same blocks as the textures that stay, each one gone left a hole beside a texture that kept its block: the
// pool's 47-54 blocks (6-6.9 GB) held 1.9-3.6 GB of images in Kokiri Forest, VRAM sat at 93-98% of what the driver allows, and an
// image made there then took 30-70 ms (RTX 5070). RX_VK_TARGET_BLOCKS=0: in the textures' blocks, as before
VmaPool target_image_pool = VK_NULL_HANDLE;
bool target_pool_tried = false;
VmaPool images_pool();
VmaPool target_images_pool() {
    if (target_pool_tried) return target_image_pool;
    target_pool_tried = true;
    static const bool off = getenv("RX_VK_TARGET_BLOCKS") && !atoi(getenv("RX_VK_TARGET_BLOCKS"));
    if (off || !images_pool()) return VK_NULL_HANDLE;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    if (vmaCreatePool(vma, &pi, &target_image_pool) != VK_SUCCESS) target_image_pool = VK_NULL_HANDLE;
    logf("vk: render targets' images from blocks of their own (%llu MB, memory type %u)%s", (unsigned long long)(kImageBlock >> 20),
         type, target_image_pool ? "" : ": not made");
    return target_image_pool;
}

VmaPool images_pool() {
    if (image_pool_tried) return image_pool;
    image_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (a representative image: the pool's memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    if (vmaCreatePool(vma, &pi, &image_pool) != VK_SUCCESS) image_pool = VK_NULL_HANDLE;
    logf("vk: images from a pool of %llu MB blocks (memory type %u)%s", (unsigned long long)(kImageBlock >> 20), type,
         image_pool ? "" : ": not made");
    return image_pool;
}

uint64_t big_image_bytes() {   // (TICK-023: RX_VK_BIG_IMAGE_MB, 16 by default; 0: none forced dedicated)
    static const uint64_t big = (uint64_t)(getenv("RX_VK_BIG_IMAGE_MB") ? std::max(0, atoi(getenv("RX_VK_BIG_IMAGE_MB"))) : 16) << 20;
    return big;
}
std::atomic<int> vram_alloc_failures{0};   // (TICK-023: images VRAM refused, read by the texture budget)
static bool create_image_(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // (in VRAM or not at all: an image VMA puts in system memory when VRAM is full is read over PCIe every frame,
    // and takes the RAM the game and everything else on the machine need)
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    // (TICK-023) a large image (16 MB and up at 4 bytes a texel: the 4096 x 4096 render targets, the terrain's arrays) gets a memory
    // object of its own: in the 128 MB blocks, the ones evicted left their space free in blocks still holding others (4 GB free in
    // the blocks after a walk in Kokiri, the driver's count unchanged), so evicting them gave nothing back. RX_VK_BIG_IMAGE_MB=<n>
    // (0: in the pool, as before)
    // (TICK-030) not a block-compressed image (BLOCK_TEXEL_VIEW_COMPATIBLE: the game's BC textures, which shaders write
    // through raw R32G32(B32A32)_UINT block views): Castle Town's ground (BC5/BC6H/BC7, 3064 x 2656, 2288 x 6112,
    // 1144 x 3056 x 3, 8-14 MB but 32-56 MB by the 4-bytes estimate) in memory of their own rendered flat teal from its
    // first frame, the same images in the pool right (bisected by RX_VK_BIG_IMAGE_MB: 16 and 32 teal, 64 and 0 right).
    // The re-encoded BC3 arrays (written by buffer copies) are asked for by vk_texture.cpp by their real size
    const uint64_t big = big_image_bytes();
    if (big && !(ci.flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) &&
        (uint64_t)ci.extent.width * ci.extent.height * ci.extent.depth * ci.arrayLayers * 4 >= big)
        dedicated = true;
    if (dedicated) {   // (a memory object of its own, which freeing gives back whole: a pool's block is kept by VMA when it empties)
        VmaAllocationCreateInfo dai = ai;
        dai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(vma, &ci, &dai, &image, &allocation, nullptr) == VK_SUCCESS) {
            static std::atomic<int> told{0};
            if (getenv("RX_VK_LOG_DEDICATED") && told++ < 200)
                logf("vk: dedicated image %ux%ux%u x%u levels %u format %d usage %#x flags %#x tiling %d", ci.extent.width,
                     ci.extent.height, ci.extent.depth, ci.arrayLayers, ci.mipLevels, (int)ci.format, ci.usage, ci.flags, (int)ci.tiling);
            return true;
        }
    }
    static const bool no_pool = getenv("RX_VK_NO_IMAGE_POOL") != nullptr;
    if (VmaPool pool = no_pool ? VK_NULL_HANDLE : target && target_images_pool() ? target_images_pool() : images_pool()) {
        VmaAllocationCreateInfo pai = ai;
        pai.pool = pool;
        pai.usage = VMA_MEMORY_USAGE_UNKNOWN;
        if (vmaCreateImage(vma, &ci, &pai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    }
    if (vmaCreateImage(vma, &ci, &ai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    // (TICK-023) noted for the texture budget: the next check (texture_end_frame, every 10 presents) evicts the least recently
    // used before the next image is refused too, instead of waiting for the driver's count
    vram_alloc_failures.fetch_add(1, std::memory_order_relaxed);
    // (hookshot) VRAM is full (a 4 GB card at a high resolution, #96): an image left unmade is a black screen, so it goes
    // to system memory instead, slow but drawn
    if (getenv("RX_VK_IMAGES_VRAM_ONLY")) return false;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vma, &ci, &sai, &image, &allocation, nullptr) != VK_SUCCESS) return false;
    static std::atomic<int> told{0};
    if (told++ < 4) logf("vk: VRAM full: a %ux%u image (format %d) in system memory", ci.extent.width, ci.extent.height, (int)ci.format);
    return true;
}
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target) {
    if (!create_image_(ci, image, allocation, dedicated, target)) return false;
    note_alloc(allocation, image_kind(ci));
    return true;
}
void destroy_image(VkImage image, VmaAllocation allocation) {
    note_free(allocation);
    vmaDestroyImage(vma, image, allocation);
}
uint64_t allocation_size(VmaAllocation allocation) {
    VmaAllocationInfo info{};
    if (allocation) vmaGetAllocationInfo(vma, allocation, &info);
    return info.size;
}
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer,
                   VmaAllocation& allocation, void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    // (a buffer with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT gets VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT from VMA:
    // vma_init sets VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT)
    static const bool blocks = getenv("RX_VK_BLOCK_BUFFERS") != nullptr;
    if (dedicated && !blocks) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    note_alloc(allocation, mem_kind_tag);
    return true;
}
/// A buffer in cached host memory, mapped: written on one thread, read back on the CPU too (the captures: the game's
/// threads copy pages into it, the replay reads texture pages out of it and the GPU copies the rest from it)
bool create_buffer_cached(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation,
                          void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;   // (its own memory object: no part of a 64 MB block left unused)
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    note_alloc(allocation, mem_kind_tag);
    return true;
}
void flush_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, 0, size);
}
void invalidate_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaInvalidateAllocation(vma, allocation, 0, size);
}
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation) {
    note_free(allocation);
    vmaDestroyBuffer(vma, buffer, allocation);
}
bool create_device_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, nullptr) != VK_SUCCESS) return false;
    note_alloc(allocation, mem_kind_tag);
    return true;
}
void log_alloc_where(const char* what, VmaAllocation allocation) {
    if (!allocation) return;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, allocation, &info);
    const VkMemoryType& t = dev.memory.memoryTypes[info.memoryType];
    const VkMemoryPropertyFlags f = t.propertyFlags;
    logf("vk: %s: %llu KB in memory type %u, heap %u (%s)%s%s%s", what, (unsigned long long)(info.size >> 10), info.memoryType,
         t.heapIndex, (dev.memory.memoryHeaps[t.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "device" : "system",
         (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " device-local" : "", (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " host-visible" : "",
         (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " cached" : "");
}
/// The image pools: their blocks, the images in them, and the free ranges between them (every 600 presents, with the heaps)
void vma_log_image_blocks() {
    for (VmaPool pool : {image_pool, target_image_pool}) {
        if (!pool) continue;
        VmaDetailedStatistics d{};
        vmaCalculatePoolStatistics(vma, pool, &d);
        logf("vk: image blocks%s: %u blocks %llu MB, %u images %llu MB; %u free ranges, the largest %llu MB",
             pool == target_image_pool ? " (render targets)" : "", d.statistics.blockCount,
             (unsigned long long)(d.statistics.blockBytes >> 20), d.statistics.allocationCount,
             (unsigned long long)(d.statistics.allocationBytes >> 20), d.unusedRangeCount,
             (unsigned long long)(d.unusedRangeSizeMax >> 20));
    }
}
/// (every heap: what VMA has in blocks and allocations there, what the process uses of it, its budget; one line)
void vma_log_heaps() {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    char line[1024];
    int n = snprintf(line, sizeof line, "vk: memory heaps:");
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount && n < 900; h++)
        n += snprintf(line + n, sizeof line - n, " heap %u%s: %u blocks %llu MB, %u allocations %llu MB; used %llu of %llu MB;", h,
                      (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : " (system)",
                      b[h].statistics.blockCount, (unsigned long long)(b[h].statistics.blockBytes >> 20),
                      b[h].statistics.allocationCount, (unsigned long long)(b[h].statistics.allocationBytes >> 20),
                      (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20));
    logf("%s", line);
}
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0, f = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            u += b[h].usage, t += b[h].budget;
            // (the free space in VMA's blocks: the driver counts it as the process's, new allocations take it first)
            f += b[h].statistics.blockBytes - std::min(b[h].statistics.blockBytes, b[h].statistics.allocationBytes);
        }
    // (a test: RX_VK_TEST_ALLOWED_MB=<n> reports what the driver allows as at most n MB, so a 16 GB card plays the eviction and
    // keep-rendered paths an 8 GB card takes when its VRAM fills (the rainbow textures of issue 993 were never seen here otherwise))
    static const uint64_t test_allowed = getenv("RX_VK_TEST_ALLOWED_MB") ? (uint64_t)atoi(getenv("RX_VK_TEST_ALLOWED_MB")) << 20 : 0;
    if (test_allowed && t > test_allowed) t = test_allowed;
    *used = u, *budget = t;
    if (block_free) *block_free = f;
    return t != 0;
}

/// The system-memory heaps the GPU reaches: what the process uses there (the imported guest memory too: pinned by the
/// driver) and what the driver allows it, asked now (a new frame index makes VMA ask the driver again); false when unknown
namespace {
std::atomic<uint32_t> budget_frame{0};   // (VMA's frame index: a new one makes it ask the driver for the budgets again)
}
bool vram_usage_now(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    return vram_usage(used, budget) && dev.memory_budget;
}
bool system_heap_usage(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (!(dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) u += b[h].usage, t += b[h].budget;
    *used = u, *budget = t;
    return dev.memory_budget && t != 0;
}

} // namespace mkwvk
