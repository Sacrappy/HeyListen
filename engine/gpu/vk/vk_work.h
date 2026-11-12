// (hookshot) Every Vulkan command that does GPU work, or orders it, counts in work_epoch. full_barrier (vk_texture.cpp) leaves out a
// barrier recorded right after another one on the same command buffer with nothing in between: the second orders nothing the first
// did not. Include this header AFTER the file's other includes: it turns the calls below into a count and the call (a header
// included after it that names one of them with a '(' after it would be changed too).
#pragma once

#include <atomic>

#include "vk_loader.h"
#include "vk_recorder.h"   // (RX_VK_RECORDER: each macro below calls its packer, which calls the driver directly when the recorder is off)

namespace mkwvk {
extern std::atomic<uint64_t> work_epoch;   // (vk_texture.cpp)
// (TICK-016, vk_texture.cpp) Work that is not a draw (dispatches, copies, clears, updates, queries), counted by the command buffer it
// went into: a slot of 64 by its handle (two command buffers in one slot count each other's: a barrier kept, never one left out)
extern std::atomic<uint64_t> other_work_by_cmd[64];
inline std::atomic<uint64_t>& other_work_slot(VkCommandBuffer c) {
    const uintptr_t v = (uintptr_t)c;
    return other_work_by_cmd[((v >> 4) ^ (v >> 10) ^ (v >> 16)) & 63];
}
extern std::atomic<uint64_t> xfer_work_by_cmd[64];   // (the same without dispatches: copies, clears, updates, queries)
inline void note_xfer_work(VkCommandBuffer c) {
    const uintptr_t v = (uintptr_t)c;
    xfer_work_by_cmd[((v >> 4) ^ (v >> 10) ^ (v >> 16)) & 63].fetch_add(1, std::memory_order_relaxed);
}
// (TICK-057) Every command that does or orders GPU work, and every begin of a command buffer, by the command buffer it went into
// (a slot of 64 by its handle, as above: two command buffers in one slot count each other's, a barrier kept, never one left out).
// full_barrier's dedupe inside a parallel replay (RX_VK_BARRIER_DEDUPE_PAR) compares this count, not work_epoch, which the
// other recording threads move all the time
extern std::atomic<uint64_t> all_work_by_cmd[64];
inline std::atomic<uint64_t>& all_work_slot(VkCommandBuffer c) {
    const uintptr_t v = (uintptr_t)c;
    return all_work_by_cmd[((v >> 4) ^ (v >> 10) ^ (v >> 16)) & 63];
}
// (TICK-061, D6, RX_VK_WORK_LOCAL=1, off by default) Inside a parallel replay's segment (vk_replay.cpp record_job, while par_active)
// the five recording threads bumped work_epoch (one cache line) and all_work_by_cmd (8 slots a line) at every draw, pass edge and
// barrier: ~2 contended lock xadd a draw. There work_seg_local is set (work_seg_begin / work_seg_end, vk_texture.cpp) and:
// - work_epoch is not bumped. It is read only by the serial barrier dedupe (full_barrier) and RX_VK_PASS_BARRIER=game, both only
//   while par_active is false, i.e. never while a segment records; the segment's end bumps it once (work_seg_end), so a thread whose
//   last barrier came before or during the segments sees it moved and keeps its next barrier, as it would have;
// - all_work_by_cmd is not bumped when nothing reads it: only RX_VK_BARRIER_DEDUPE_PAR and RX_VK_BARRIER_TRUST decide from it (both
//   off by default); with either on it is bumped as before (work_seg_skip_all false).
// other_work_by_cmd / xfer_work_by_cmd (dispatches, copies, clears: ~130 a frame) are kept as they were.
inline thread_local bool work_seg_local = false;
inline thread_local bool work_seg_skip_all = false;
void work_seg_begin();   // (vk_texture.cpp)
void work_seg_end();
inline void note_other_work(VkCommandBuffer c) {
    other_work_slot(c).fetch_add(1, std::memory_order_relaxed);
    if (!work_seg_skip_all) all_work_slot(c).fetch_add(1, std::memory_order_relaxed);
    if (!work_seg_local) work_epoch.fetch_add(1, std::memory_order_relaxed);
}
inline void note_work_cmd(VkCommandBuffer c) {
    if (!work_seg_skip_all) all_work_slot(c).fetch_add(1, std::memory_order_relaxed);
    if (!work_seg_local) work_epoch.fetch_add(1, std::memory_order_relaxed);
}
extern std::atomic<uint64_t> foreign_work;   // (TICK-057: note_work's calls with no command buffer, NGX's: the dedupe by command buffer keeps the next barrier)
inline void note_work() {
    foreign_work.fetch_add(1, std::memory_order_relaxed);
    // (an add, not a load and a store: a thread preempted between the two would store a stale count over another thread's, which could
    // make the count equal the one a barrier was recorded at with work recorded since, and the next barrier is left out for it)
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
} // namespace mkwvk

#define vkCmdPipelineBarrier2(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdPipelineBarrier2(c_, __VA_ARGS__))
#define vkCmdBeginRendering(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdBeginRendering(c_, __VA_ARGS__))
#define vkCmdEndRendering(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdEndRendering(c_ __VA_OPT__(,) __VA_ARGS__))
#define vkCmdDraw(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDraw(c_, __VA_ARGS__))
#define vkCmdDrawIndexed(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawIndexed(c_, __VA_ARGS__))
#define vkCmdDrawIndirect(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawIndirect(c_, __VA_ARGS__))
#define vkCmdDrawIndexedIndirect(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawIndexedIndirect(c_, __VA_ARGS__))
#define vkCmdDrawIndirectCount(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawIndirectCount(c_, __VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawIndexedIndirectCount(c_, __VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawMeshTasksEXT(c_, __VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawMeshTasksIndirectEXT(c_, __VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::rx_vkCmdDrawMeshTasksIndirectCountEXT(c_, __VA_ARGS__))
#define vkCmdDispatch(c_, ...) (::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdDispatch(c_, __VA_ARGS__))
#define vkCmdDispatchIndirect(c_, ...) (::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdDispatchIndirect(c_, __VA_ARGS__))
#define vkCmdCopyBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdCopyBuffer(c_, __VA_ARGS__))
#define vkCmdCopyBufferToImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdCopyBufferToImage(c_, __VA_ARGS__))
#define vkCmdCopyImageToBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdCopyImageToBuffer(c_, __VA_ARGS__))
#define vkCmdCopyImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdCopyImage(c_, __VA_ARGS__))
#define vkCmdBlitImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdBlitImage(c_, __VA_ARGS__))
#define vkCmdResolveImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdResolveImage(c_, __VA_ARGS__))
#define vkCmdClearColorImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdClearColorImage(c_, __VA_ARGS__))
#define vkCmdClearDepthStencilImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdClearDepthStencilImage(c_, __VA_ARGS__))
#define vkCmdClearAttachments(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdClearAttachments(c_, __VA_ARGS__))
#define vkCmdFillBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdFillBuffer(c_, __VA_ARGS__))
#define vkCmdUpdateBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdUpdateBuffer(c_, __VA_ARGS__))
#define vkCmdBeginQuery(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdBeginQuery(c_, __VA_ARGS__))
#define vkCmdEndQuery(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::mkwvk::rec::rx_vkCmdEndQuery(c_, __VA_ARGS__))

// (RX_VK_RECORDER) the calls that set state, bind or mark: not work (no count), but in the command buffer in order with the work, so
// through the recorder too
#define vkCmdBindPipeline(...) (::mkwvk::rec::rx_vkCmdBindPipeline(__VA_ARGS__))
#define vkCmdBindDescriptorSets(...) (::mkwvk::rec::rx_vkCmdBindDescriptorSets(__VA_ARGS__))
#define vkCmdBindIndexBuffer(...) (::mkwvk::rec::rx_vkCmdBindIndexBuffer(__VA_ARGS__))
#define vkCmdBindVertexBuffers2(...) (::mkwvk::rec::rx_vkCmdBindVertexBuffers2(__VA_ARGS__))
#define vkCmdPushConstants(...) (::mkwvk::rec::rx_vkCmdPushConstants(__VA_ARGS__))
#define vkCmdPushDescriptorSetKHR(...) (::mkwvk::rec::rx_vkCmdPushDescriptorSetKHR(__VA_ARGS__))
#define vkCmdSetViewport(...) (::mkwvk::rec::rx_vkCmdSetViewport(__VA_ARGS__))
#define vkCmdSetScissor(...) (::mkwvk::rec::rx_vkCmdSetScissor(__VA_ARGS__))
#define vkCmdSetViewportWithCount(...) (::mkwvk::rec::rx_vkCmdSetViewportWithCount(__VA_ARGS__))
#define vkCmdSetScissorWithCount(...) (::mkwvk::rec::rx_vkCmdSetScissorWithCount(__VA_ARGS__))
#define vkCmdSetBlendConstants(...) (::mkwvk::rec::rx_vkCmdSetBlendConstants(__VA_ARGS__))
#define vkCmdSetStencilReference(...) (::mkwvk::rec::rx_vkCmdSetStencilReference(__VA_ARGS__))
#define vkCmdSetStencilCompareMask(...) (::mkwvk::rec::rx_vkCmdSetStencilCompareMask(__VA_ARGS__))
#define vkCmdSetStencilWriteMask(...) (::mkwvk::rec::rx_vkCmdSetStencilWriteMask(__VA_ARGS__))
#define vkCmdSetStencilOp(...) (::mkwvk::rec::rx_vkCmdSetStencilOp(__VA_ARGS__))
#define vkCmdSetDepthBias(...) (::mkwvk::rec::rx_vkCmdSetDepthBias(__VA_ARGS__))
#define vkCmdSetDepthBiasEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthBiasEnable(__VA_ARGS__))
#define vkCmdSetLineWidth(...) (::mkwvk::rec::rx_vkCmdSetLineWidth(__VA_ARGS__))
#define vkCmdSetDepthBoundsTestEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthBoundsTestEnable(__VA_ARGS__))
#define vkCmdSetCullMode(...) (::mkwvk::rec::rx_vkCmdSetCullMode(__VA_ARGS__))
#define vkCmdSetFrontFace(...) (::mkwvk::rec::rx_vkCmdSetFrontFace(__VA_ARGS__))
#define vkCmdSetPrimitiveTopology(...) (::mkwvk::rec::rx_vkCmdSetPrimitiveTopology(__VA_ARGS__))
#define vkCmdSetPrimitiveRestartEnable(...) (::mkwvk::rec::rx_vkCmdSetPrimitiveRestartEnable(__VA_ARGS__))
#define vkCmdSetDepthTestEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthTestEnable(__VA_ARGS__))
#define vkCmdSetDepthWriteEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthWriteEnable(__VA_ARGS__))
#define vkCmdSetDepthCompareOp(...) (::mkwvk::rec::rx_vkCmdSetDepthCompareOp(__VA_ARGS__))
#define vkCmdSetStencilTestEnable(...) (::mkwvk::rec::rx_vkCmdSetStencilTestEnable(__VA_ARGS__))
#define vkCmdSetRasterizerDiscardEnable(...) (::mkwvk::rec::rx_vkCmdSetRasterizerDiscardEnable(__VA_ARGS__))
#define vkCmdSetPolygonModeEXT(...) (::mkwvk::rec::rx_vkCmdSetPolygonModeEXT(__VA_ARGS__))
#define vkCmdSetDepthClampEnableEXT(...) (::mkwvk::rec::rx_vkCmdSetDepthClampEnableEXT(__VA_ARGS__))
#define vkCmdSetAlphaToCoverageEnableEXT(...) (::mkwvk::rec::rx_vkCmdSetAlphaToCoverageEnableEXT(__VA_ARGS__))
#define vkCmdSetColorBlendEnableEXT(...) (::mkwvk::rec::rx_vkCmdSetColorBlendEnableEXT(__VA_ARGS__))
#define vkCmdSetColorBlendEquationEXT(...) (::mkwvk::rec::rx_vkCmdSetColorBlendEquationEXT(__VA_ARGS__))
#define vkCmdSetColorWriteMaskEXT(...) (::mkwvk::rec::rx_vkCmdSetColorWriteMaskEXT(__VA_ARGS__))
#define vkCmdWriteBufferMarkerAMD(...) (::mkwvk::rec::rx_vkCmdWriteBufferMarkerAMD(__VA_ARGS__))
#define vkCmdSetCheckpointNV(...) (::mkwvk::rec::rx_vkCmdSetCheckpointNV(__VA_ARGS__))
#define vkCmdWriteTimestamp2(...) (::mkwvk::rec::rx_vkCmdWriteTimestamp2(__VA_ARGS__))
#define vkCmdResetQueryPool(...) (::mkwvk::rec::rx_vkCmdResetQueryPool(__VA_ARGS__))

// (RX_VK_RECORDER=defer) the calls that begin, end or reset command buffers, submit, present or wait, read back what the GPU did,
// or let go of what recorded commands use: the thread's held packets reach the driver first (rec::boundary(): a relaxed load and
// a branch unless deferred; RX_VK_RECORDER=thread: the replay lock's holder waits for the recorder thread to run all it put in its ring;
// the kind is for the recorder's stats line). RX_REC_BOUNDARIES in vk_recorder.h lists them
#define vkBeginCommandBuffer(c_, ...) (::mkwvk::note_work_cmd(c_), ::mkwvk::rec::boundary(::mkwvk::rec::B_vkBeginCommandBuffer), ::vkx::vkBeginCommandBuffer(c_, __VA_ARGS__))
#define vkEndCommandBuffer(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkEndCommandBuffer), ::vkx::vkEndCommandBuffer(__VA_ARGS__))
#define vkResetCommandPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetCommandPool), ::vkx::vkResetCommandPool(__VA_ARGS__))
#define vkDestroyCommandPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyCommandPool), ::vkx::vkDestroyCommandPool(__VA_ARGS__))
#define vkQueueSubmit2(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueueSubmit2), ::vkx::vkQueueSubmit2(__VA_ARGS__))
#define vkQueueWaitIdle(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueueWaitIdle), ::vkx::vkQueueWaitIdle(__VA_ARGS__))
#define vkQueueBindSparse(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueueBindSparse), ::vkx::vkQueueBindSparse(__VA_ARGS__))
#define vkQueuePresentKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueuePresentKHR), ::vkx::vkQueuePresentKHR(__VA_ARGS__))
#define vkAcquireNextImageKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkAcquireNextImageKHR), ::vkx::vkAcquireNextImageKHR(__VA_ARGS__))
#define vkDeviceWaitIdle(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDeviceWaitIdle), ::vkx::vkDeviceWaitIdle(__VA_ARGS__))
#define vkWaitForFences(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkWaitForFences), ::vkx::vkWaitForFences(__VA_ARGS__))
#define vkResetFences(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetFences), ::vkx::vkResetFences(__VA_ARGS__))
#define vkWaitSemaphores(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkWaitSemaphores), ::vkx::vkWaitSemaphores(__VA_ARGS__))
#define vkGetSemaphoreCounterValue(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetSemaphoreCounterValue), ::vkx::vkGetSemaphoreCounterValue(__VA_ARGS__))
#define vkGetQueryPoolResults(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetQueryPoolResults), ::vkx::vkGetQueryPoolResults(__VA_ARGS__))
#define vkResetQueryPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetQueryPool), ::vkx::vkResetQueryPool(__VA_ARGS__))
#define vkGetQueueCheckpointDataNV(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetQueueCheckpointDataNV), ::vkx::vkGetQueueCheckpointDataNV(__VA_ARGS__))
#define vkGetDeviceFaultInfoEXT(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetDeviceFaultInfoEXT), ::vkx::vkGetDeviceFaultInfoEXT(__VA_ARGS__))
#define vkResetDescriptorPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetDescriptorPool), ::vkx::vkResetDescriptorPool(__VA_ARGS__))
#define vkDestroyBuffer(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyBuffer), ::vkx::vkDestroyBuffer(__VA_ARGS__))
#define vkDestroyImage(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyImage), ::vkx::vkDestroyImage(__VA_ARGS__))
#define vkDestroyImageView(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyImageView), ::vkx::vkDestroyImageView(__VA_ARGS__))
#define vkDestroyBufferView(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyBufferView), ::vkx::vkDestroyBufferView(__VA_ARGS__))
#define vkDestroySampler(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySampler), ::vkx::vkDestroySampler(__VA_ARGS__))
#define vkDestroyPipeline(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyPipeline), ::vkx::vkDestroyPipeline(__VA_ARGS__))
#define vkDestroyPipelineLayout(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyPipelineLayout), ::vkx::vkDestroyPipelineLayout(__VA_ARGS__))
#define vkDestroyDescriptorSetLayout(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyDescriptorSetLayout), ::vkx::vkDestroyDescriptorSetLayout(__VA_ARGS__))
#define vkDestroyDescriptorPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyDescriptorPool), ::vkx::vkDestroyDescriptorPool(__VA_ARGS__))
#define vkDestroyQueryPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyQueryPool), ::vkx::vkDestroyQueryPool(__VA_ARGS__))
#define vkDestroyShaderModule(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyShaderModule), ::vkx::vkDestroyShaderModule(__VA_ARGS__))
#define vkDestroyPipelineCache(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyPipelineCache), ::vkx::vkDestroyPipelineCache(__VA_ARGS__))
#define vkDestroyFence(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyFence), ::vkx::vkDestroyFence(__VA_ARGS__))
#define vkDestroySemaphore(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySemaphore), ::vkx::vkDestroySemaphore(__VA_ARGS__))
#define vkDestroySwapchainKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySwapchainKHR), ::vkx::vkDestroySwapchainKHR(__VA_ARGS__))
#define vkDestroySurfaceKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySurfaceKHR), ::vkx::vkDestroySurfaceKHR(__VA_ARGS__))
#define vkDestroyDevice(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyDevice), ::vkx::vkDestroyDevice(__VA_ARGS__))
#define vkDestroyInstance(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyInstance), ::vkx::vkDestroyInstance(__VA_ARGS__))
#define vkFreeMemory(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkFreeMemory), ::vkx::vkFreeMemory(__VA_ARGS__))
#define vmaDestroyBuffer(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vmaDestroyBuffer), ::vmaDestroyBuffer(__VA_ARGS__))
#define vmaDestroyImage(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vmaDestroyImage), ::vmaDestroyImage(__VA_ARGS__))
#define vmaInvalidateAllocation(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vmaInvalidateAllocation), ::vmaInvalidateAllocation(__VA_ARGS__))
