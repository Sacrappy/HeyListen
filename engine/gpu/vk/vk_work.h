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
// (TICK-057) Every command that does or orders GPU work, and every begin of a command buffer, by the command buffer it went into
// (a slot of 64 by its handle, as above: two command buffers in one slot count each other's, a barrier kept, never one left out).
// full_barrier's dedupe inside a parallel replay (RX_VK_BARRIER_DEDUPE_PAR) compares this count, not work_epoch, which the
// other recording threads move all the time
extern std::atomic<uint64_t> all_work_by_cmd[64];
inline std::atomic<uint64_t>& all_work_slot(VkCommandBuffer c) {
    const uintptr_t v = (uintptr_t)c;
    return all_work_by_cmd[((v >> 4) ^ (v >> 10) ^ (v >> 16)) & 63];
}
inline void note_other_work(VkCommandBuffer c) {
    other_work_slot(c).fetch_add(1, std::memory_order_relaxed);
    all_work_slot(c).fetch_add(1, std::memory_order_relaxed);
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
inline void note_work_cmd(VkCommandBuffer c) {
    all_work_slot(c).fetch_add(1, std::memory_order_relaxed);
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
extern std::atomic<uint64_t> foreign_work;   // (TICK-057: note_work's calls with no command buffer, NGX's: the dedupe by command buffer keeps the next barrier)
inline void note_work() {
    foreign_work.fetch_add(1, std::memory_order_relaxed);
    // (an add, not a load and a store: a thread preempted between the two would store a stale count over another thread's, which could
    // make the count equal the one a barrier was recorded at with work recorded since, and the next barrier is left out for it)
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
} // namespace mkwvk

#define vkBeginCommandBuffer(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkBeginCommandBuffer(c_, __VA_ARGS__))
#define vkCmdPipelineBarrier2(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdPipelineBarrier2(c_, __VA_ARGS__))
#define vkCmdBeginRendering(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdBeginRendering(c_, __VA_ARGS__))
#define vkCmdEndRendering(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdEndRendering(c_ __VA_OPT__(,) __VA_ARGS__))
#define vkCmdDraw(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDraw(c_, __VA_ARGS__))
#define vkCmdDrawIndexed(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawIndexed(c_, __VA_ARGS__))
#define vkCmdDrawIndirect(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawIndirect(c_, __VA_ARGS__))
#define vkCmdDrawIndexedIndirect(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawIndexedIndirect(c_, __VA_ARGS__))
#define vkCmdDrawIndirectCount(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawIndirectCount(c_, __VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawIndexedIndirectCount(c_, __VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawMeshTasksEXT(c_, __VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawMeshTasksIndirectEXT(c_, __VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdDrawMeshTasksIndirectCountEXT(c_, __VA_ARGS__))
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
