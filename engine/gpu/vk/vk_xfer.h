// Texture <-> buffer and texture <-> texture copies, and texture clears.
//
// These are totk-pc-recomp's, from runtime/vk_exec_xfer.c (sub_resolve, buffer_side, copy_buffer_image, copy_t2t,
// clear_tex), whose comments explain each case; adapted to this back end, where an image is one level and one layer
// of a texture found by its storage address (vk_image.cpp image_for) instead of one image holding every level.
//
// What they are for here: MKW renders into its own targets and copies them into the 4K window texture it presents,
// so without these the present blits an image nothing ever wrote.
#pragma once

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {

/// buffer <-> texture level (nvn_p_copy_bt); to_image: buffer to texture, else texture to buffer
void copy_b2t(VkCommandBuffer cmd, NvnState& s, const nvn_p_copy_bt& p, bool to_image);
/// texture level -> texture level (nvn_p_copy_tt): copy, blit when scaled or converting, resolve when multisampled
void copy_t2t(VkCommandBuffer cmd, NvnState& s, const nvn_p_copy_tt& p);
/// a whole level to a value (nvn_p_clear_tex)
void clear_tex(VkCommandBuffer cmd, NvnState& s, const nvn_p_clear_tex& p);

} // namespace mkwvk
