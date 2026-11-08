// GPU profile (diagnostics): RX_VK_GPU_PROFILE=1 times every pass and dispatch on the GPU (a timestamp after all the
// work before it and one after its own), labelled by what it renders or dispatches, and every 120 presents logs the
// labels that took the most GPU time a frame.
#pragma once

#include <string>

#include "vk_internal.h"

namespace mkwvk {

bool gprof_on();
/// A recording begins in slot (its earlier work is done: its timings are collected first)
void gprof_begin_recording(int slot, VkCommandBuffer cmd);
/// A timed span begins (outside a pass, or right after it began) / ends (its label: what it was)
void gprof_begin(VkCommandBuffer cmd);
void gprof_end(VkCommandBuffer cmd, const std::string& label);
/// The slot's work completed: its spans are added to the totals
void gprof_collect(int slot);
/// Every 120 presents: the top labels a frame
void gprof_report(int frames);

} // namespace mkwvk
