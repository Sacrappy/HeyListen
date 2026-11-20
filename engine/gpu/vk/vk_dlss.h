// NVIDIA DLSS Super Resolution, one of the temporal upscaler's engines (vk_temporal.h; vk_dlss.cpp). Built in only with
// NVIDIA's NGX SDK (HOOKSHOT_NGX, scripts/ngx_sdk.py: Linux and Windows); without it these say DLSS is not there and the
// game's network runs.
#pragma once

#include <vector>

#include "vk_internal.h"

namespace mkwvk {

struct TemporalEngine;

/// Whether this build has DLSS in it (HOOKSHOT_NGX)
bool dlss_built();
/// Why DLSS cannot be chosen here (the Insert menu's Upscaler row says it), DLSS_OK when it can or is not known yet
enum DlssWhy { DLSS_OK, DLSS_NOT_BUILT, DLSS_ENV_OFF, DLSS_NEEDS_RTX, DLSS_NEEDS_DRIVER, DLSS_NO_RUNTIME,
               DLSS_RUNTIME_NOT_ASCII, DLSS_WHY_COUNT };
DlssWhy dlss_why();
/// Asks NGX once whether DLSS runs on this device, so dlss_why knows before the first upscale (the menu, when it opens).
/// Nothing off an NVIDIA device with NGX's extensions, or in a build without DLSS
void dlss_probe();
/// The instance's extensions DLSS needs, added to `want` when the loader has them (all: what it has)
void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// The device's extensions DLSS needs, added to `want` when the device has them
void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// DLSS as the temporal upscaler's engine (DLSS and DLAA on the Upscaler row); nullptr in a build without it
const TemporalEngine* dlss_engine();

// NVIDIA DLSS Frame Generation through NGX (the same NGX session as DLSS, no Streamline: the renderer's display thread keeps
// presenting, DLSS-G only writes the made frames into the images it is given; vk_framegen.cpp calls it at a fixed multiplier).
// Off by default: the Frame generation by row (opts.framegen_by 1) or RX_VK_DLSS_FG=1 asks for it; RX_VK_DLSS_FG=0 never.

/// Why DLSS Frame Generation cannot run here, for the menu (FG_OK when it can or NGX has not answered yet)
enum DlssFgWhy { FG_OK, FG_NOT_BUILT, FG_OFF_ENV, FG_NO_DLSS, FG_NO_RUNTIME, FG_REFUSED, FG_WHY_COUNT };
DlssFgWhy dlss_fg_why();
/// DLSS Frame Generation is asked for now (the row, or RX_VK_DLSS_FG=1) and possible in this build and process
bool dlss_fg_wanted();
/// Asks NGX once whether it runs (after dlss_probe, on the same thread: the menu's), so the row knows before the first frame
void dlss_fg_probe();
/// The most made frames between two of the game's that NGX's runtime makes (0: not known yet, or it does not run here)
int dlss_fg_most();
/// The device's extensions DLSS Frame Generation needs, asked of NGX, added to `want` when the device has them
void dlss_fg_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);

/// One set's inputs: the scene's depth (reversed: 1 at the near plane) and motion (render pixels toward the frame before),
/// and its camera, as vk_fsr3_inputs.h made them
struct DlssFgInputs {
    VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE, motion_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED, motion_format = VK_FORMAT_UNDEFINED;
    uint32_t render_w = 0, render_h = 0;
    float projection[16]{};            // (the scene's, row major, clip = P * view position, no jitter)
    float view_projection[16]{};       // (this frame's, no jitter)
    float prev_view_projection[16]{};  // (the frame before's, no jitter; with have_prev)
    bool have_prev = false;            // (the frame before's camera is known: a moving camera's reprojection)
    float jitter[2]{};                 // (render pixels)
    float near_z = 0.1f, far_z = 1000.0f, fov_y = 1.0f;
    bool reset = false;                // (a cut, a load, a new size)
};
/// Records `count` made frames between the frame before the game's `color` (w x h, RGBA8, GENERAL) and it into out[0..count)
/// (RGBA8, GENERAL, w x h, with their views), from `in`. `sequence` counts the presents: a gap in it is a new history. Returns
/// count, or 0 when DLSS-G made none (not running, refused, more frames than its runtime makes: the renderer's own makes the
/// set) and wrote nothing
int dlss_fg_frames(VkCommandBuffer cmd, VkImage color, VkImageView color_view, uint32_t w, uint32_t h, int count,
                   const VkImage* out, const VkImageView* out_views, const DlssFgInputs& in, uint64_t sequence);
/// The feature and its memory let go when the row is not at DLSS FG any more (the menu's thread: the queue idle first)
void dlss_fg_idle();
/// The feature let go for the exit
void dlss_fg_close();

} // namespace mkwvk
