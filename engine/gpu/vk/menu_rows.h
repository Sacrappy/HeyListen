/* The Insert menu's dependent rows (vk_menu.cpp row_shown / row_fixed), as plain functions: which rows show only while the setting
   they depend on is on (the others are greyed, with a help line that says why). No Vulkan, no menu: tests/test_menu_rows.cpp checks
   them. */
#pragma once

namespace mkwvk::menu_rows {

/// The HDR row (opts.hdr: 0 Off, 1 Expanded, 2 Native) as it works on this PC: Expanded and Native need an HDR10 surface, which the
/// display lists only with Windows' "Use HDR" on (listed: dev.hdr_listed, found whenever the swapchain is made; forced:
/// RX_VK_HDR_FORCE=1, for a PC with no HDR display). Without one the row's value does nothing and the HDR rows under it are hidden
inline int hdr_effective(int hdr, bool listed, bool forced) { return hdr != 0 && (listed || forced) ? hdr : 0; }
/// Paper white and peak brightness: with HDR on
inline bool hdr_levels_shown(int effective) { return effective != 0; }
/// The tone mapper row: with Native HDR
inline bool tonemap_shown(int effective) { return effective == 2; }
/// PsychoV35's grading rows: with Native HDR on PsychoV35 (hdr_tonemap 1)
inline bool psychov35_shown(int effective, int tonemap) { return effective == 2 && tonemap == 1; }
/// HDR contrast: with HDR on, and not with PsychoV35, which has its own
inline bool contrast_shown(int effective, int tonemap) { return effective != 0 && !psychov35_shown(effective, tonemap); }
/// Frame generation's sub-rows (Frame generation by, Frame pacing): while the multiplier row is not Off
inline bool framegen_sub_shown(int framegen) { return framegen != 0; }
/// The V-Sync mode row: with V-Sync On (or while RX_VK_FIFO / RX_VK_IMMEDIATE decide, where it stays greyed to say so)
inline bool vsync_mode_shown(int vsync, bool env_decides) { return vsync != 0 || env_decides; }
inline bool vsync_mode_fixed(int vsync, bool env_decides) { return vsync == 0 || env_decides; }
/// The cursor's row after the rows above it were hidden or shown: the position of global row g among the shown ones, -1 if g is hidden
template <class Shown>
inline int local_index(int rows, int g, Shown shown) {
    int at = 0;
    for (int i = 0; i < rows; i++) {
        if (!shown(i)) continue;
        if (i == g) return at;
        at++;
    }
    return -1;
}

} // namespace mkwvk::menu_rows
