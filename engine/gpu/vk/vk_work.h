// (hookshot) Every Vulkan command that does GPU work, or orders it, counts in work_epoch. full_barrier (vk_texture.cpp) leaves out a
// barrier recorded right after another one on the same command buffer with nothing in between: the second orders nothing the first
// did not. Include this header AFTER the file's other includes: it turns the calls below into a count and the call (a header
// included after it that names one of them with a '(' after it would be changed too).
#pragma once

#include <atomic>

#include "vk_loader.h"

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
inline void note_other_work(VkCommandBuffer c) {
    other_work_slot(c).fetch_add(1, std::memory_order_relaxed);
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
inline void note_work() {
    // (an add, not a load and a store: a thread preempted between the two would store a stale count over another thread's, which could
    // make the count equal the one a barrier was recorded at with work recorded since, and the next barrier is left out for it)
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
} // namespace mkwvk

#define vkCmdPipelineBarrier2(...) (::mkwvk::note_work(), ::vkx::vkCmdPipelineBarrier2(__VA_ARGS__))
#define vkCmdBeginRendering(...) (::mkwvk::note_work(), ::vkx::vkCmdBeginRendering(__VA_ARGS__))
#define vkCmdEndRendering(...) (::mkwvk::note_work(), ::vkx::vkCmdEndRendering(__VA_ARGS__))
#define vkCmdDraw(...) (::mkwvk::note_work(), ::vkx::vkCmdDraw(__VA_ARGS__))
#define vkCmdDrawIndexed(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndexed(__VA_ARGS__))
#define vkCmdDrawIndirect(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndirect(__VA_ARGS__))
#define vkCmdDrawIndexedIndirect(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndexedIndirect(__VA_ARGS__))
#define vkCmdDrawIndirectCount(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawIndexedIndirectCount(__VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawMeshTasksEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawMeshTasksIndirectEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(...) (::mkwvk::note_work(), ::vkx::vkCmdDrawMeshTasksIndirectCountEXT(__VA_ARGS__))
#define vkCmdDispatch(c_, ...) (::mkwvk::note_other_work(c_), ::vkx::vkCmdDispatch(c_, __VA_ARGS__))
#define vkCmdDispatchIndirect(c_, ...) (::mkwvk::note_other_work(c_), ::vkx::vkCmdDispatchIndirect(c_, __VA_ARGS__))
#define vkCmdCopyBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdCopyBuffer(c_, __VA_ARGS__))
#define vkCmdCopyBufferToImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdCopyBufferToImage(c_, __VA_ARGS__))
#define vkCmdCopyImageToBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdCopyImageToBuffer(c_, __VA_ARGS__))
#define vkCmdCopyImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdCopyImage(c_, __VA_ARGS__))
#define vkCmdBlitImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdBlitImage(c_, __VA_ARGS__))
#define vkCmdResolveImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdResolveImage(c_, __VA_ARGS__))
#define vkCmdClearColorImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdClearColorImage(c_, __VA_ARGS__))
#define vkCmdClearDepthStencilImage(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdClearDepthStencilImage(c_, __VA_ARGS__))
#define vkCmdClearAttachments(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdClearAttachments(c_, __VA_ARGS__))
#define vkCmdFillBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdFillBuffer(c_, __VA_ARGS__))
#define vkCmdUpdateBuffer(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdUpdateBuffer(c_, __VA_ARGS__))
#define vkCmdBeginQuery(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdBeginQuery(c_, __VA_ARGS__))
#define vkCmdEndQuery(c_, ...) (::mkwvk::note_xfer_work(c_), ::mkwvk::note_other_work(c_), ::vkx::vkCmdEndQuery(c_, __VA_ARGS__))
