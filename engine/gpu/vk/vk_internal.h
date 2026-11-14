// The Vulkan back end's shared state (gpu/vk).
#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "vk_loader.h"
#include "vk_api.h"
#include "split_count.h"   // (TICK-052)

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define HOOKSHOT_RDTSC() ((uint64_t)__rdtsc())
#define HOOKSHOT_PAUSE() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define HOOKSHOT_RDTSC() ((uint64_t)__rdtsc())
#define HOOKSHOT_PAUSE() _mm_pause()
#endif

typedef struct VmaAllocation_T* VmaAllocation;   // (vk_mem_alloc.h's handle)

namespace mkwvk {

extern std::vector<std::string> gpu_device_names;
extern const MkwVkHost* host;
void logf(const char* fmt, ...);
void queue_flush_locked();   // (vk_replay.cpp, RX_VK_BATCH: held submits out; dev.present_mu held)
void queue_flush();          // (the same, taking dev.present_mu)
// (hookshot) held submits out, then vkDeviceWaitIdle with the present queue's lock taken too (dev.present_mu held): the device's
// every queue is synchronized by the host around it
void device_wait_idle_locked();

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    const char* present_mode_name = nullptr;   // (the swapchain's: the overlay shows it)
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceFeatures features{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    // extensions in use
    bool mesh_shader = false, external_memory_host = false, external_memory_fd = false, dma_buf_import = false, pipeline_library = false,
         graphics_pipeline_library = false, custom_border_color = false, dynamic_state3 = false,
         depth_clip_control = false, null_descriptor = false, index_uint8 = false, push_descriptor = false,
         conditional_rendering = false, provoking_vertex = false, vertex_divisor = false, stencil_export = false, memory_budget = false,
         checkpoints = false,   // (RX_VK_CHECKPOINTS: VK_NV_device_diagnostic_checkpoints)
         buffer_marker = false, // (RX_VK_CHECKPOINTS=1 without the NV extension: VK_AMD_buffer_marker, AMD and Intel)
         device_fault = false,  // (VK_EXT_device_fault: what a lost device faulted on)
         sparse_images = false, // (sparse resident 2D images on the queue: the terrain's huge arrays)
         sparse_buffers = false, // (sparse-bound buffers on the queue: a pool imported as several dma-bufs)
         view_2d_of_3d = false, // (VK_EXT_image_2d_view_of_3d: a 3D texture a 2D sample instruction reads)
         coop_matrix = false,   // (VK_KHR_cooperative_matrix with its feature)
         native_mma = false,    // (HMMA on it: the cooperative matrix test passed, vk_coopmat.cpp)
         pci_bus_info = false;  // (VK_EXT_pci_bus_info supported, not enabled: its properties name the card's sysfs node, vk_memory.cpp pcie_link_gbps)
    bool eds3_polygon = false, eds3_clamp = false, eds3_blend = false, eds3_a2c = false;
    bool swapchain_colorspace = false;   // (hookshot) VK_EXT_swapchain_colorspace: HDR10 surfaces are listed
    bool hdr = false;                    // (hookshot) the swapchain is HDR10 (ST 2084, Rec. 2020): framegen_hdr writes it
    bool mesh_fallback = false;   // (hookshot) no VK_EXT_mesh_shader: task and mesh programs as compute shaders (vk_shader.cpp)
    VkPhysicalDeviceVulkan12Features vk12{};
    VkPhysicalDeviceVulkan13Features vk13{};
    VkShaderStageFlags sg32_stages = 0;   // (stages that run at 32 lanes when asked: the size the translated programs assume)
    uint32_t max_push_descriptors = 0;
    // no window (HOS_HEADLESS): presents go to an image of their own, which RX_VK_SHOTS can write out
    bool offscreen = false;
    // the window
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;   // (hookshot) a view of each, for Dear ImGui's overlays (VK_NULL_HANDLE: none)
    VkFormat swap_view_format = VK_FORMAT_UNDEFINED;   // (their format: the UNORM one of an sRGB swapchain when it can be)
    bool swap_mutable = false;   // (VK_KHR_swapchain_mutable_format: an sRGB swapchain's images viewed as UNORM)
    std::vector<VkSemaphore> swap_rendered;   // present waits belong to acquired images, not frame slots
    std::mutex present_mu;   // (the swapchain: presents and resizes)
    std::atomic<uint64_t> swap_resize{0};   // (a new window size, width << 32 | height: the next present makes the swapchain)
    // (hookshot) a queue of a compute family for vkQueuePresentKHR alone (vk_main.cpp create_device; RX_VK_PRESENT_QUEUE=0 / 1).
    // VK_NULL_HANDLE: presents go on `queue` under present_mu. Its own lock: taken after present_mu, never before it
    VkQueue present_queue = VK_NULL_HANDLE;
    std::mutex present_queue_mu;
    uint32_t present_family = 0;   // (present_queue's family, the swapchain's images shared with it)
};
extern Device dev;

/// A range of guest memory as the GPU sees it: a buffer (an imported pool), the offset in it, and its device address
struct BufferRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0, size = 0;
    VkDeviceAddress address = 0;
    explicit operator bool() const {
        return buffer != VK_NULL_HANDLE;
    }
};
bool memory_init();                                   // vk_memory.cpp
void memory_cpu_owned(uint64_t guest, uint64_t size);   // (hookshot: words the replay writes on the CPU, left out of write-backs)
void memory_table_begin();                         // (hookshot: a recording's page table changes held back for its upload commands)
bool memory_table_flush();
uint64_t memory_guest_pages();                        // (hookshot: the device address of guest memory's page table, 0: none)
void memory_sync();                                   // (at each submit: pools the game made or let go)
BufferRef gpu_buffer(uint64_t gpu, uint64_t size);   // (empty when no pool holds the range)
/// A range as a binding of the replay sees it: the pool's VRAM mirror when it has one (vk_memory.cpp), else the
/// imported pool. written: the GPU may write it (copied back to guest memory at the submission's end).
BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written);
BufferRef stream_copy(const void* src, uint64_t n);   // vk_draw.cpp: bytes in the recording's stream, as a binding
/// Changes when a read binding remembered earlier must be looked up again (a pool left, a mirror was made, a new submission)
uint64_t gpu_bind_stamp();
/// Around each submission's recording: mirrors wanted since the last one are made (begin), what the GPU wrote into
/// mirrors goes back to guest memory (end, outside any pass, before the closing barrier)
void memory_begin_submission();
void memory_sync_mirrors_before_segments();   // (hookshot: vk_memory.cpp)
bool probe_on();                                // (hookshot: vk_draw.cpp, RX_VK_PROBE)
bool probe_serial();
void probe_dump_all(const char* why);
void probe_log(uint64_t frame);
void memory_end_submission(VkCommandBuffer cmd);
/// The game made a memory pool (gpu address, bytes): imported in the background, before it is used
/// RX_VK_CHECKPOINTS: the draws the GPU reached, logged once the device is lost (vk_draw.cpp)
void checkpoints_report();
/// The device is lost: that report once, then the process ends (vk_replay.cpp)
[[noreturn]] void device_lost();
/// VK_AMD_buffer_marker's buffer, for RX_VK_CHECKPOINTS on a device without the NV checkpoints (vk_draw.cpp)
void buffer_markers_init();
void coopmat_init();   // vk_coopmat.cpp (once the device is made)
/// A marker of another kind of GPU work (m: mirror sync, b: write-back, u: texture upload, g: GPU unswizzle; key: its
/// pool or texture), RX_VK_CHECKPOINTS
void checkpoint_mark(VkCommandBuffer cmd, uint64_t key, char kind);

/// Parallel replay (vk_replay.cpp, RX_VK_PARALLEL): while segments of a submission are recorded on several threads,
/// what they share (textures and their uploads, programs, pipelines, samplers, pools and memory stamps) is used under
/// one lock; the per-draw caches are each thread's own
extern std::atomic<bool> par_active;
/// (contention: how often the lock was taken and the time threads waited for it, summed over the threads' own counters)
void par_counts_sum(int64_t* locks, int64_t* wait_ns);
/// (TICK-052, RX_VK_PARLOCK_STATS) a recording thread starts or ends a segment; a draw or dispatch starts
void par_stats_segment(bool begin);
void par_stats_draw();
void par_stats_log();
/// (TICK-052, RX_VK_PARLOCK_STATS) why a recording thread's lookup went to the lock: par_why(kind) while segments are recorded
enum ParWhy : int {
    PW_BIND_WRITTEN, PW_BIND_NO_TL, PW_BIND_TL_GEN, PW_BIND_TL_RANGE, PW_BIND_MIRROR_STATE, PW_BIND_SYNC_EPOCH, PW_BIND_SYNC_SEG, PW_BIND_SIZE,
    PW_VIEW_NEW, PW_VIEW_VERSION, PW_VIEW_DGEN, PW_TOUCH_VIEW, PW_TOUCH_SLOT, PW_SLOT_NEW, PW_SLOT_HANDLE, PW_SLOT_VERSION,
    PW_SLOT_DGEN, PW_QUICK_NOT_UPLOADED, PW_QUICK_LATER_SEG, PW_QUICK_EPOCH, PW_COUNT
};
void par_why(int kind);
/// (TICK-052, vk_texture.cpp) RX_VK_GONE_RING=1 (off by default): whether a texture resolved at destroy generation `from` is still
/// alive at `to`: no texture_destroy_generation bump between them was its own (or a sweep of all, the deferred ASTC uploads').
/// A recording thread's view and slot caches then outlive another texture's eviction instead of all going stale at each one
bool texture_gone_ring_on();
struct Texture;
bool texture_kept_since(const Texture* tex, uint64_t from, uint64_t to);
/// (TICK-052, RX_VK_PARLOCK_STATS) a segment's recording in parts (ns): set up, before its first draw, its draws, after its last
/// draw's start, and its command buffers ended
void par_stats_segment_parts(int64_t t0, int64_t ta, int64_t tc, int64_t t1);
std::recursive_mutex& par_mutex();
int64_t par_now_ns();
/// (TICK-052) RX_VK_PARLOCK_STATS=1: each ParLock place's takings, waits and holds while segments are recorded, and the
/// recording threads' draws by their place in a segment, logged with the parallel replay's line (vk_replay.cpp)
extern bool par_stats_on;
int par_stats_site(const char* file, unsigned line);
void par_stats_note(int site, int64_t wait_ns, int64_t hold_ns, bool released);
/// (TICK-052) the shared counters each ParLock bumped (par_locks, par_lock_wait_ns) went to one cache line that every recording
/// thread wrote ~1,000 times a command buffer: each thread counts its own now (par_counts_add), summed when the line is logged
void par_counts_add(int64_t locks, int64_t wait_ns);
struct ParLock {
    bool on;
    int site = -1;
    int64_t t_held = 0;
    explicit ParLock(bool want = true, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE())
        : on(want && par_active.load(std::memory_order_relaxed)) {
        if (!on) return;
        if (par_mutex().try_lock()) {
            par_counts_add(1, 0);
            if (par_stats_on) site = par_stats_site(file, line), t_held = par_now_ns();
            return;
        }
        const int64_t t0 = par_now_ns();
        par_mutex().lock();
        t_held = par_now_ns();
        par_counts_add(1, t_held - t0);
        if (par_stats_on) site = par_stats_site(file, line), par_stats_note(site, t_held - t0, 0, false);
    }
    ~ParLock() {
        if (!on) return;
        if (site >= 0) par_stats_note(site, 0, par_now_ns() - t_held, true);
        par_mutex().unlock();
    }
    ParLock(const ParLock&) = delete;
    ParLock& operator=(const ParLock&) = delete;
};
void memory_pool_created(uint64_t gpu, uint64_t size);
/// (TICK-015) a game pool made, with its NVNmemoryPoolFlags (the export mkwvk_pool_flags)
void memory_pool_flags(uint64_t gpu, uint64_t size, uint32_t flags);
/// (TICK-015) the CPU is about to read [guest, guest + size) (invalidate): its game pool's write-backs are made from now on
void memory_cpu_reads(uint64_t guest, uint64_t size);
/// (HOS_NOACCESS_RAM, on unless =0) the pools the game made without CPU access keep no zero pages in RAM (vk_memory.cpp)
bool noaccess_ram_on();
/// whether [guest, guest + size) lies in one game pool made with NVN's CPU_NO_ACCESS flag
bool memory_in_noaccess_pool(uint64_t guest, uint64_t size);
/// whether every page of [guest, guest + size) is only reserved: nobody wrote it, it reads as zeros (no page is touched)
bool memory_all_reserved(uint64_t guest, uint64_t size);
/// (vk_replay.cpp) whether a live texture's storage lies in [gpu, gpu + size)
bool replay_tex_storage_in(uint64_t gpu, uint64_t size);
/// Whether guest memory in [lo, hi) is in an imported pool, live or retired (the GPU may read or write it)
int memory_guest_imported(uint64_t lo, uint64_t hi);
void memory_stats(int64_t* mirrors, uint64_t* mirror_mb, int64_t* sync_bytes, int64_t* sync_copies, int64_t* wb_bytes);
// Page write generations (vk_memory.cpp, after totk-pc-recomp's vk_memory.c): at each submit the guest pages the CPU
// wrote since the last one (the soft write watch) are stamped with a new generation; a texture uploaded at
// generation g is stale when a page of its storage carries a newer one. GPU writes into guest memory that textures
// may be read from (storage buffers, buffer copies) are stamped the same way.
void memory_collect_written();
/// RX_VK_CAPTURE (on with the mirrors; =0 off): at a submit, on the submitting thread, the pages the CPU wrote since the
/// last one, and a copy of them as they are then; the replay stamps them and fills the mirrors from that copy
void memory_capture_submit();
/// The replay of the next submit: its capture (false: none, the pages are collected now as before)
bool memory_take_capture(uint64_t value, uint64_t completed);
void memory_trace_counts(int64_t* late_ns, int64_t* late_n, int64_t* scatter_bytes);   // (TICK-051, RX_VK_FRAME_TRACE)
bool memory_capture_now(uint64_t value, uint64_t completed);   // (hookshot: made and taken at once)
/// The submission the replay records next (bindings note it: a pool's mirror is in use until it is done)
void memory_recording_value(uint64_t value, uint64_t completed);
/// RX_VK_TEX_CAPTURE (on with the capture; =0 off): the storage of a texture uploaded from guest memory joins the
/// capture (its pool up to 256 MB, else its 2 MB regions), and what the capture took of it is kept for the uploads
void memory_note_texture_upload(uint64_t guest, uint64_t size);
/// Whether [guest, guest + n) has pages kept as a capture took them
bool memory_has_shadow(uint64_t guest, uint64_t n);
/// A read of guest memory by the replay as the submission being recorded had it: the pages a kept capture holds from
/// there (the game writes several submits ahead of the replay), the rest from guest memory. False when unreadable
bool memory_read_as_submitted(uint64_t gpu, void* dst, uint64_t n);
/// (HL_UBO_CHECK) the game pool holding guest address g: its range and NVN flags (false: in none)
bool memory_nvn_pool_of(uint64_t guest, uint64_t* lo, uint64_t* hi, uint32_t* flags);
/// (RX_VK_CAP_ONLY_GPU) a present: the capture's per-frame counts logged every 10 s (vk_memory.cpp)
void memory_cap_present();
/// (HL_UBO_CHECK) the texture storage the captures take, sorted and merged
std::vector<std::pair<uint64_t, uint64_t>> memory_tex_capture_ranges();
// ---- (HeyListen) HL_UBO_CHECK=1 (vk_ubo_check.cpp; docs/native-engine/direct-emission.md section 9): a measurement, no
// behaviour change. Each uniform-buffer bind's bytes as hashed at the bind, the recording's end and the submit
// (NOP_UBO_CHECK, host/nvn_native.c) against what the replay reads as submitted; and of the pages the capture copies in a
// frame, which the frame's records read and how ----
bool ubo_check_on();
enum UboRefKind : uint32_t { kRefUboEqual = 1, kRefUboOther = 2, kRefVertex = 4, kRefIndex = 8, kRefStorage = 16,
                             kRefIndirect = 32, kRefOther = 64 };
/// a command buffer's records begin (the replay thread's pass over them, the dry pass included; never a worker's)
void ubo_check_begin_cmd();
/// a NOP_UBO_CHECK record met in that pass; updated: an earlier UpdateUniformBuffer of the stream overlaps the range
void ubo_check_bind(const nvn_p_ubo_check& k, bool updated);
/// an UpdateUniformBuffer in that pass that overlaps a bound uniform range
void ubo_check_update_into_bound();
/// [gpu, gpu + size) read by a record of that pass (a page of it captured this frame is read by it)
void ubo_check_ref(uint64_t gpu, uint64_t size, uint32_t kind);
/// the pages a capture copies (capture_make)
void ubo_check_captured(const uint32_t* pages, int64_t n);
/// a present: the frame's pages classified, the tables logged every 10 s
void ubo_check_frame();
uint64_t memory_capture_view();          // (bumped when a capture is published or any page is stamped)
uint32_t memory_page_stamp(uint64_t gpu); // (that address's page only)
uint64_t gpu_pool_end(uint64_t gpu);   // (hookshot) where the memory pool holding that address ends, 0: in none (vk_memory.cpp)
uint64_t gpu_pool_start(uint64_t gpu); // (EvilOOT) and where it begins
bool create_buffer_cached(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation,
                          void** mapped, bool dedicated = false);   // (vk_vma.cpp: cached host memory, mapped)
void flush_mapped(VmaAllocation allocation, uint64_t size);
void invalidate_mapped(VmaAllocation allocation, uint64_t size);   // (the CPU is to read what the GPU wrote: a no-op in coherent memory)
void memory_stamp_gpu(uint64_t guest, uint64_t size);   // (a GPU write: memory_stamp, and the pages noted)
void memory_protect_gpu(uint64_t guest, uint64_t size);  // (RX_VK_PROTECT_BARE=1: pages a shader writes through a table pointer)
/// (vk_replay.cpp) guest memory as the stream's place sees it: as submitted, the stream's uniform updates before it over it
bool ub_read(uint64_t addr, void* dst, uint64_t n);
/// [guest, guest + n) as of the submit being replayed: live itself, or scratch holding it with the kept pages over it
const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, std::vector<uint8_t>& scratch);
void memory_drop_shadow(uint64_t guest, uint64_t size);   // (a write the capture did not take: the copies go)
/// (EvilBoyce) the newest submission value whose write-back reaches into [guest, guest + size) (0: none ever); *ever set
/// when any write-back ever did
uint64_t memory_writeback_value(uint64_t guest, uint64_t size, bool* ever);
void memory_note_readback(uint64_t guest, uint64_t size);   // (PR 430) a CPU read of GPU results: see memory_rb_refresh
bool memory_rb_refresh(VkCommandBuffer up);
void memory_rb_recording_begun();   // (hookshot) at a recording's start, on its thread: the submission it joins (memory_rb_refresh)
void memory_rb_segments_begin(uint64_t base, uint64_t n);   // (and a parallel replay's segments: their order, base on)
/// (hookshot) a range a program may write in the mirror that is not bound as written (not copied back): memory_rb_refresh
void memory_note_gpu_write(uint64_t gpu, uint64_t size);
/// (vk_replay.cpp) The GPU's progress on the recordings' timeline, and a wait for it to reach v
uint64_t vk_timeline_completed();
/// (HeyListen, HL_TRACE) one of the back end's writes into guest memory, or a mirror's or an import's life, for the crash
/// recorder (vk_api.h trace_gpu; nothing when it is off)
inline void trace_gpu(uint32_t what, uint64_t guest, uint64_t size, uint64_t pool, uint64_t value) {
    if (host && host->trace_gpu) host->trace_gpu(what, guest, size, pool, value, vk_timeline_completed());
}
void vk_timeline_wait(uint64_t v);
void gpu_tail_note_late(uint64_t v);   // (TICK-057, vk_replay.cpp: a capture waited for v's late pages: the GPU's busy tail before it)
/// (TICK-060, RX_VK_LATE_DEFER) vk_memory.cpp: the highest late value a pending fix waits for (0: none); the fixes whose value done
/// reached, applied (the late slots copied again from guest memory). vk_replay.cpp: the highest timeline value a submit sent to
/// the queue signals; the fix thread woken for a fix of value v
uint64_t memory_late_fix_pending();
void memory_late_fix_apply(uint64_t done);
void memory_late_fix_wait_range(uint64_t guest, uint64_t n);   // (TICK-060m: before a CPU upload reads guest memory: a pending late page's value)
uint64_t vk_timeline_signal_sent();
void vk_timeline_wait_sent(uint64_t v);   // (TICK-060: a wait for a value already sent: no flush, safe under any lock)
bool late_fix_wait_unlocked();   // (TICK-060, vk_replay.cpp: a pending late fix's value waited for before the queue's lock; false: a poll)
void late_fix_wake(uint64_t v);
uint32_t memory_generation();
/// 1: a page of [guest, guest + size) was written after generation gen; 0: none was
bool memory_written_since(uint64_t guest, uint64_t size, uint32_t gen);
void memory_stamp(uint64_t guest, uint64_t size);   // a write the watch does not see (GPU writes, host writes)
/// The page runs of [guest, guest + size) written after gen, as byte ranges from guest (appended to out)
void memory_written_runs(uint64_t guest, uint64_t size, uint32_t gen, std::vector<std::pair<uint64_t, uint64_t>>& out);
uint64_t memory_pages_collected();                  // (statistics)

/// An NVN format as Vulkan stores it (vk_formats.cpp): ASTC is decoded to RGBA8, block_w x block_h
/// Bytes per block and block size of an NVN format (ASTC blocks are 16 bytes)
int nvn_block_size(int format, int* bw, int* bh);

struct VkFormatInfo {
    VkFormat format;
    bool astc;
    int block_w, block_h;
};
VkFormatInfo vk_format(int nvn_format);
bool is_depth_format(VkFormat f);
bool has_stencil(VkFormat f);

// (TICK-038) what each allocation is for, by kind and where it landed (RX_VK_RES_LOG=1: counted, and logged every 600 presents).
// A call site sets the kind for the allocations it makes with MemKindScope; images are told apart by their usage
enum class MemKind : uint8_t { Other, Texture, Target, Mirror, WbShadow, Capture, Stream, Staging, Readback, Scratch, BcCache, Count };
extern thread_local MemKind mem_kind_tag;
// (HOS_HOST_SPLIT, host/hos_hostsplit.c: vk_vma.cpp) name a range of this process's memory for the census of what is outside the game's;
// nothing when the executable lacks the export or the switch is off
void host_tag(const void* ptr, uint64_t size, const char* name);
void host_untag(const void* ptr);
struct MemKindScope {
    MemKind prev;
    explicit MemKindScope(MemKind k) : prev(mem_kind_tag) { mem_kind_tag = k; }
    ~MemKindScope() { mem_kind_tag = prev; }
};
bool res_log_on();
/// one line: bytes by kind and memory class (VRAM, VRAM the CPU maps, system cached, system uncached)
void mem_kinds_log();
void memory_res_log();   // (vk_memory.cpp: the process line, then mem_kinds_log)
void res_note_pipeline_use(VkPipeline pl);   // (vk_draw.cpp, RX_VK_RES_LOG: a pipeline bound)
/// (vk_draw.cpp) graphics pipelines made, pipelines ever bound, compute shader modules let go (RX_VK_CS_MODULE_DROP)
void res_pipeline_counts(size_t* graphics, size_t* used, int64_t* modules_dropped);
// device memory (vk_vma.cpp, Vulkan Memory Allocator)
bool vma_init(PFN_vkGetInstanceProcAddr gipa);
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated = false, bool target = false);
void destroy_image(VkImage image, VmaAllocation allocation);
/// What an allocation really takes (VkMemoryRequirements.size: padding, alignment, the driver's metadata), 0 for none
uint64_t allocation_size(VmaAllocation allocation);
/// (TICK-023/030) the size from which an image gets memory of its own (RX_VK_BIG_IMAGE_MB, 16 MB; 0: none)
uint64_t big_image_bytes();
/// (dedicated: its own memory object, not a share of a 64 MB block: for the 16 to 32 MB ones that fit two to a block and leave
/// the rest of it free; RX_VK_BLOCK_BUFFERS=1: as before)
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer,
                   VmaAllocation& allocation, void** mapped, bool dedicated = false);
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation);
/// A buffer in VRAM only (within the driver's budget), or false
bool create_device_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, bool dedicated = false);
/// Device-local memory this process uses and what the driver allows it now (VK_EXT_memory_budget), summed over the
/// device-local heaps; false when unknown
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free = nullptr);
/// vram_usage asked of the driver now (VMA asks again for a new frame index), for the status line; false when the driver
/// gives no budget (VK_EXT_memory_budget): used is then the game's own allocations
bool vram_usage_now(uint64_t* used, uint64_t* budget);
void vma_log_heaps();
/// (TICK-019) one log line: which memory type and heap an allocation landed in (device-local / host-visible / cached)
void log_alloc_where(const char* what, VmaAllocation allocation);
/// (TICK-019) GPU reads straight from imported guest memory outside the buffer bindings' count: texel buffer views, GPU unswizzles
extern std::atomic<int64_t> st_import_texel_views, st_import_texel_bytes, st_import_unswizzle_bytes;
void vma_log_image_blocks();   // (the image pools' blocks: what they hold, and their free ranges)
PFN_vkGetInstanceProcAddr instance_proc();   // vk_main.cpp

bool create_swapchain(uint32_t width, uint32_t height);
bool replay_start();                          // vk_replay.cpp
bool precompile_wanted();                     // vk_draw.cpp
bool precompile_blocking();                   // vk_draw.cpp: RX_VK_PRECOMPILE_WAIT=1 (else the pipelines are built while the game runs)
void precompile_run(void (*progress)(size_t done, size_t total), bool wait = true);
bool prepare_run(void (*progress)(size_t done, size_t total), bool background);
bool prepare_is_update();
void prepare_background();
void state_list_run();                        // (an update: the state list's pipelines noted before precompile_run)
void predict_run(void (*progress)(size_t done, size_t total));   // (after a prepare_run that prepared: the notes' predictions)
/// (hookshot) The window was closed while the shaders were prepared: prepare_run and precompile_run take nothing more and return,
/// what is done kept (the program cache, the driver's) and not counted as a start that ended inside them
void prepare_stop();
bool prepare_stopped();
void fill_replay_table(MkwVkRenderer& t);     // vk_replay.cpp

} // namespace mkwvk

// ---- a scoped timer per phase of the replay (logged per frame with the frame statistics) ----
namespace mkwvk {
enum ProfPhase { PROF_SUBMIT, PROF_PRESENT, PROF_DRAW, PROF_DESC, PROF_TEXGET, PROF_UPLOAD, PROF_PIPE, PROF_PROGRAM,
                 PROF_CMDS, PROF_DISPATCH, PROF_MEMSYNC, PROF_QSUBMIT, PROF_SLOTWAIT, PROF_COUNT };
extern SplitCount prof_ns[PROF_COUNT];   // (TICK-052: a cache line a thread, split_count.h)
/// Nanoseconds from a performance-counter reading. An integer number of nanoseconds per tick is a multiply, so a
/// reading past 2^53 does not lose its low bits the way the double divide does.
inline int64_t perf_ticks_to_ns(int64_t ticks, int64_t freq) {
    if (freq > 0 && 1000000000LL % freq == 0)
        return ticks * (1000000000LL / freq);
    if (freq <= 0) return 0;
    return (int64_t)((double)ticks * 1e9 / (double)freq);
}
// A cheap nanosecond clock for the per-phase replay timers. Ten QueryPerformanceCounter reads a draw were ~4% of
// the GPU thread (gpu/render/rx_internal.h). The hot path reads the invariant TSC; the counter calibrates the scale once.
inline int64_t prof_now() {
#if defined(HOOKSHOT_RDTSC)
    struct Scale { double ns_per; bool ok; };
    static const Scale scale = [] {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        const uint64_t c0 = HOOKSHOT_RDTSC();
        QueryPerformanceCounter(&a);
        volatile int sink = 0;
        for (int i = 0; i < 400000; i++) { HOOKSHOT_PAUSE(); sink += 1; }
        const uint64_t c1 = HOOKSHOT_RDTSC();
        QueryPerformanceCounter(&b);
        const int64_t dns = perf_ticks_to_ns(b.QuadPart - a.QuadPart, f.QuadPart);
        Scale s;
        s.ok = c1 > c0 && dns > 0 && sink > 0;
        s.ns_per = s.ok ? (double)dns / (double)(c1 - c0) : 0.0;
        return s;
    }();
    if (scale.ok) return (int64_t)((double)HOOKSHOT_RDTSC() * scale.ns_per);
#endif
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return perf_ticks_to_ns(t.QuadPart, freq);
}
// (hookshot) The per-draw phases (PROF_HOT) run a few thousand times a frame, and each timer is two counter reads: ~25k
// reads a frame were ~2% of the replay thread. One draw in eight is timed, picked at random per thread, and counted eight
// times: the outermost PROF_HOT on the thread picks, and those nested in it follow, so a phase never reads more than the
// one around it. What is slow and rare inside (a program or pipeline made, a texture created or uploaded, a wait) is timed
// by PROF / PROF_EXACT and counted once as it is, sampled draw or not: the PROF_HOT scopes around it weigh only the rest.
// Every scope timed (prof_full): RX_VK_PROF_ALL=1, RX_VK_SLOW_LOG, and while RX_VK_BENCH replays its frame.
inline std::atomic<int> prof_full{getenv("RX_VK_PROF_ALL") || getenv("RX_VK_SLOW_LOG") ? 1 : 0};
struct ProfThread {
    uint32_t rng;    // (xorshift32, seeded at the thread's first pick)
    int open, k;     // (the PROF_HOT scopes open, and the outermost one's weight: 0 not timed)
    int64_t exact;   // (the nanoseconds PROF / PROF_EXACT timed on this thread so far)
};
inline thread_local ProfThread prof_t;
struct ProfScope {
    int phase;   // (-1: PROF_EXACT, no phase of its own)
    int64_t t0, x0;
    explicit ProfScope(int p) : phase(p), t0(prof_now()), x0(prof_t.exact) {}
    ~ProfScope() {
        const int64_t dt = prof_now() - t0;
        if (phase >= 0) prof_ns[phase] += dt;
        prof_t.exact = x0 + dt;   // (what was exact inside is part of dt: not counted twice)
    }
};
/// A thread's first state: splitmix64 of a counter, so the threads' sequences are unrelated; never 0, xorshift's fixed point
inline uint32_t prof_seed() {
    static std::atomic<uint64_t> threads{0};
    uint64_t z = (threads.fetch_add(1, std::memory_order_relaxed) + 1) * 0x9E3779B97F4A7C15ull;
    z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ z >> 27) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ z >> 31) >> 32) | 1u;
}
inline int prof_pick(ProfThread& t) {   // (the weight: 8 one time in eight, else 0; 1 when every scope is timed)
    if (prof_full.load(std::memory_order_relaxed)) return 1;
    if (!t.rng) t.rng = prof_seed();
    t.rng ^= t.rng << 13, t.rng ^= t.rng >> 17, t.rng ^= t.rng << 5;
    return t.rng >> 29 ? 0 : 8;   // (the top bits: xorshift's low ones are its weakest)
}
struct ProfScopeHot {
    int phase, k;
    int64_t t0, x0;
    explicit ProfScopeHot(int p) : phase(p) {
        ProfThread& t = prof_t;
        if (!t.open++) t.k = prof_pick(t);
        k = t.k, x0 = t.exact, t0 = k ? prof_now() : 0;
    }
    ~ProfScopeHot() {
        ProfThread& t = prof_t;
        const int64_t x = t.exact - x0;   // (timed exactly inside: once, as it is)
        if (k) prof_ns[phase] += (prof_now() - t0 - x) * k + x;
        else if (x) prof_ns[phase] += x;
        t.open--;
    }
};
} // namespace mkwvk
#define PROF(phase) ::mkwvk::ProfScope prof_scope_##phase(::mkwvk::phase)
#define PROF_HOT(phase) ::mkwvk::ProfScopeHot prof_scope_##phase(::mkwvk::phase)   // (the per-draw phases: sampled, above)
#define PROF_EXACT() ::mkwvk::ProfScope prof_scope_exact(-1)   // (a slow, rare path inside them: timed exactly, above)
