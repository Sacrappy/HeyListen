#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#define VK_NO_PROTOTYPES
#define VK_USE_64_BIT_PTR_DEFINES 0
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
namespace fixture {
using VmaAllocation = void*;
constexpr int kMaxMult = 8;
struct Device { VkDevice device{}; bool push_descriptor = true, offscreen = false; std::mutex present_mu; } dev;
struct Options { int framegen = 2; } opts;
auto vkCmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(uintptr_t(1));
constexpr uint32_t FG_COPY_COMP_SPV[] = {0}, FG_LUMA_COMP_SPV[] = {0}, FG_FLOW_COMP_SPV[] = {0},
                   FG_MEDIAN_COMP_SPV[] = {0}, FG_SYNTH_COMP_SPV[] = {0}, FG_SCALE_COMP_SPV[] = {0},
                   FG_COPY_PQ_COMP_SPV[] = {0}, FG_SYNTH_PQ_COMP_SPV[] = {0};
bool dlss_on = false;
bool dlss_fg_wanted() { return dlss_on; }   /* (vk_dlss.cpp: NVIDIA DLSS Frame Generation makes the frames; they stay RGBA8) */
/* ACTUAL_STRUCTS */
int multiplier() { return opts.framegen; }
int framegen_now() { return opts.framegen; }   /* (vk_framegen.cpp: the Frame generation row as it runs) */
int display_refresh_mhz() { return 60000; }
void logf(const char*, ...) {}
struct Object { char kind; uint64_t parent; };
std::map<uint64_t, Object> live;
uint64_t serial = 0;
int attempts = 0, fail_at = 0, barriers = 0, waits = 0;
bool idle = true, submitted = false;
bool fails() { return ++attempts == fail_at; }
uint64_t allocate(char kind, uint64_t parent = 0) { live[++serial] = {kind, parent}; return serial; }
void release(uint64_t handle, char kind) {
    if (!handle) return;
    CHECK(live.count(handle) && live.at(handle).kind == kind);
    CHECK(!submitted || idle);
    for (const auto& [id, obj] : live) { (void)id; CHECK(obj.parent != handle); }
    live.erase(handle);
}
VkResult vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* p) {
    *p = 0; if (fails()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *p = allocate('D'); return VK_SUCCESS;
}
VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* info, const VkAllocationCallbacks*, VkPipelineLayout* p) {
    *p = 0; if (fails()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *p = allocate('L', *info->pSetLayouts); return VK_SUCCESS;
}
VkResult vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* p) {
    *p = 0; if (fails()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *p = allocate('M'); return VK_SUCCESS;
}
VkResult vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t count, const VkComputePipelineCreateInfo* info,
                                  const VkAllocationCallbacks*, VkPipeline* p) {
    CHECK(count == 1); const bool fail = fails();
    // Vulkan may return owned pipeline handles together with an error: cleanup must inspect the output.
    *p = allocate('P', info->layout); return fail ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
// (longshot #252: the code cut out creates through the fault guards, held by Intel's compiler gate; here the gate holds nothing and
// the guards are the same fakes, so every injected failure still reaches the code under test)
struct IntelDriverGate { IntelDriverGate() {} };
VkResult extern_guarded_shader_module(const VkShaderModuleCreateInfo* ci, VkShaderModule* m) {
    return vkCreateShaderModule(dev.device, ci, nullptr, m);
}
VkResult extern_guarded_compute(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* p) {
    return vkCreateComputePipelines(dev.device, cache, 1, ci, nullptr, p);
}
void vkDestroyPipeline(VkDevice, VkPipeline h, const VkAllocationCallbacks*) { release(h, 'P'); }
void vkDestroyPipelineLayout(VkDevice, VkPipelineLayout h, const VkAllocationCallbacks*) { release(h, 'L'); }
void vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout h, const VkAllocationCallbacks*) { release(h, 'D'); }
void vkDestroyShaderModule(VkDevice, VkShaderModule h, const VkAllocationCallbacks*) { release(h, 'M'); }
VkResult vkCreateSampler(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler* p) {
    *p = 0; if (fails()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *p = allocate('S'); return VK_SUCCESS;
}
void vkDestroySampler(VkDevice, VkSampler h, const VkAllocationCallbacks*) { release(h, 'S'); }
std::map<VkFormat, int> made;   /* (the formats the images were made in) */
bool create_image(const VkImageCreateInfo& info, VkImage& image, VmaAllocation& memory) {
    if (fails()) return false;
    ++made[info.format];
    image = allocate('I'); memory = reinterpret_cast<void*>(uintptr_t(allocate('A'))); return true;
}
void destroy_image(VkImage image, VmaAllocation memory) { release(image, 'I'); release(uintptr_t(memory), 'A'); }
VkResult vkCreateImageView(VkDevice, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*, VkImageView* p) {
    *p = 0; if (fails()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *p = allocate('V', info->image); return VK_SUCCESS;
}
void vkDestroyImageView(VkDevice, VkImageView h, const VkAllocationCallbacks*) { release(h, 'V'); }
void device_wait_idle_locked() { idle = true; ++waits; }
void to_general(VkCommandBuffer, VkImage image) { CHECK(image && live.count(image)); ++barriers; }
/* ACTUAL_FUNCTIONS */

void clear_model() {
    CHECK(live.empty()); fg.init = fg.failed = false; fg.pq = fg.pq_tried = false; fg.copy_pq = fg.synth_pq = {}; dlss_on = false; made.clear(); fg.w = fg.h = 0; fg.mult = 0;
    fg.have_prev = false; fg.recorded = nullptr; fg.nearest = fg.linear = 0;
    fg.copy = fg.luma = fg.flow = fg.median = fg.synth = {};
    fg.cur = fg.next_set = 0; fg.jobs.clear();
    for (int i = 0; i < 2; ++i) fg.frames[i] = fg.lumas[i] = fg.flows[i] = {};
    for (auto& set : fg.sets) { set.busy = false; for (auto& image : set.frame) image = {}; }
    scale.init = scale.failed = false; scale.pipe = {}; scale.linear = 0;
    attempts = fail_at = barriers = waits = 0; submitted = false; idle = true;
}
void reset() { drop_size(); drop_setup(); clear_model(); }   /* (what is left of a successful setup released first) */
void inspect_disabled() {
    CHECK(fg.failed && !framegen_on()); CHECK(live.empty());
    const int before = attempts; CHECK(!init() && attempts == before);
}
int run(const std::string& phase) {
    clear_model();
    if (phase == "samplers") {
        fail_at = 21; CHECK(!init()); inspect_disabled(); return 0;
    }
    if (phase == "all") {
        for (int failure = 1; failure <= 22; ++failure) {
            clear_model(); fail_at = failure; CHECK(!init()); inspect_disabled();
        }
        clear_model(); dev.push_descriptor = false; CHECK(!init() && live.empty()); dev.push_descriptor = true;
    }
    // Every image or view failure at fixed2x and max8x, after successful pipeline setup.
    for (int mult : {2, 8}) for (int failed = 1; failed <= 2 * (6 + 3 * mult); ++failed) {
        clear_model(); opts.framegen = mult; CHECK(init() && live.size() == 17);
        fail_at = attempts + failed; CHECK(!ensure_size({}, 3440, 1440, false));
        CHECK(barriers == 0 && !fg.w && !fg.h && !fg.mult && !fg.have_prev && !fg.recorded);
        inspect_disabled();
    }
    // Resize must drain old work before releasing it, then roll back a partially allocated replacement.
    clear_model(); opts.framegen = 2; CHECK(init()); CHECK(ensure_size({}, 1720, 720, false));
    CHECK(live.size() == 53 && barriers == 12 && framegen_on());
    const int count = attempts; CHECK(ensure_size({}, 1720, 720, false) && attempts == count);
    submitted = true; idle = false; fail_at = attempts + 8;
    CHECK(!ensure_size({}, 3440, 1440, false)); CHECK(waits == 2 && idle);
    CHECK(!fg.w && !fg.h && !fg.mult && !fg.have_prev && !fg.recorded); inspect_disabled();
    // The 10-bit frames (PQ, Native HDR): the copy and blend pipelines are made at the first use, the frames follow the flag, and every
    // failure leaves either 8-bit frames (the 10-bit pipelines failed) or nothing (the images failed); DLSS Frame Generation keeps RGBA8.
    const VkFormat pq10 = VK_FORMAT_A2B10G10R10_UNORM_PACK32, sdr8 = VK_FORMAT_R8G8B8A8_UNORM;
    reset(); opts.framegen = 2; CHECK(init()); CHECK(!fg.pq_tried && !fg.copy_pq.pipe);
    CHECK(ensure_size({}, 1720, 720, false) && !fg.pq && !fg.pq_tried && made[sdr8] == 8 && !made[pq10]);   /* (SDR: no 10-bit pipelines made) */
    const size_t sdr_live = live.size();
    CHECK(ensure_size({}, 1720, 720, true) && fg.pq && fg.pq_tried && fg.copy_pq.pipe && fg.synth_pq.pipe);
    CHECK(made[pq10] == 8 && made[sdr8] == 8 && live.size() == sdr_live + 6);
    const int pq_count = attempts; CHECK(ensure_size({}, 1720, 720, true) && attempts == pq_count);   /* (the same flag: nothing made again) */
    CHECK(ensure_size({}, 1720, 720, false) && !fg.pq && made[sdr8] == 16 && live.size() == sdr_live + 6);   /* (back to 8-bit: the pipelines stay) */
    CHECK(waits >= 2);
    reset(); opts.framegen = 2; CHECK(init()); dlss_on = true;
    CHECK(ensure_size({}, 1720, 720, true) && !fg.pq && !fg.pq_tried && made[sdr8] == 8 && !made[pq10]);   /* (DLSS FG: RGBA8 even for PQ) */
    dlss_on = false; CHECK(ensure_size({}, 1720, 720, true) && fg.pq && made[pq10] == 8);
    for (int failed = 1; failed <= 8 + 2 * (6 + 3 * 2); ++failed) {   // (8: the 10-bit pipelines' calls; then the images')
        reset(); opts.framegen = 2; CHECK(init());
        fail_at = attempts + failed; const bool ok = ensure_size({}, 1720, 720, true);
        if (failed <= 8) {   // the 10-bit pipelines failed: the frames are 8-bit and the 10-bit pipelines gone, not tried again
            CHECK(ok && !fg.pq && fg.pq_tried && !fg.copy_pq.pipe && !fg.synth_pq.pipe && made[sdr8] == 8 && !made[pq10]);
            CHECK(live.size() == 53);
            const int again = attempts; CHECK(ensure_size({}, 1720, 720, true) && attempts == again);
        } else {
            CHECK(!ok); CHECK(barriers == 0 && !fg.w && !fg.h && !fg.mult && !fg.have_prev && !fg.recorded);
            inspect_disabled();
        }
    }
    // The independent scale setup must also release each partial pipeline/sampler on failure.
    for (int failure = 1; failure <= 5; ++failure) {
        clear_model(); fail_at = failure; CHECK(!init_scale() && scale.failed && live.empty());
    }
    std::puts("PASS: framegen setup/image rollback, 10-bit PQ frames (failures, DLSS FG keeps RGBA8), checked samplers, no stale size, queue drain and direct fallback gate");
    return 0;
}
}
int main(int argc, char** argv) { return fixture::run(argc > 1 ? argv[1] : "all"); }
