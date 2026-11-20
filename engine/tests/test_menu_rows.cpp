#include "../gpu/vk/menu_rows.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace mr = mkwvk::menu_rows;
    // HDR: without a display that lists HDR10 (and no RX_VK_HDR_FORCE) the row's value does nothing
    CHECK("SDR screen: Native is nothing", mr::hdr_effective(2, false, false) == 0 && mr::hdr_effective(1, false, false) == 0);
    CHECK("HDR screen: the row's value", mr::hdr_effective(2, true, false) == 2 && mr::hdr_effective(1, true, false) == 1 &&
                                         mr::hdr_effective(0, true, false) == 0);
    CHECK("forced: as an HDR screen", mr::hdr_effective(2, false, true) == 2);
    // the rows under it
    CHECK("levels with HDR on only", !mr::hdr_levels_shown(0) && mr::hdr_levels_shown(1) && mr::hdr_levels_shown(2));
    CHECK("tone mapper with Native only", !mr::tonemap_shown(0) && !mr::tonemap_shown(1) && mr::tonemap_shown(2));
    CHECK("PsychoV35 with Native and PsychoV35 only", mr::psychov35_shown(2, 1) && !mr::psychov35_shown(2, 0) &&
                                                     !mr::psychov35_shown(1, 1) && !mr::psychov35_shown(0, 1));
    CHECK("contrast: HDR on, not PsychoV35", !mr::contrast_shown(0, 0) && mr::contrast_shown(1, 0) && mr::contrast_shown(1, 1) &&
                                             mr::contrast_shown(2, 0) && !mr::contrast_shown(2, 1));
    // as soon as HDR is there: the owner's PC, Native with the display listing HDR10, shows the tone mapper, then PsychoV35's rows
    CHECK("tone mapper appears with HDR available", mr::tonemap_shown(mr::hdr_effective(2, true, false)) &&
                                                    !mr::tonemap_shown(mr::hdr_effective(2, false, false)));
    // frame generation's sub-rows, V-Sync mode
    CHECK("frame generation sub-rows: not while Off", !mr::framegen_sub_shown(0) && mr::framegen_sub_shown(1) && mr::framegen_sub_shown(8));
    CHECK("V-Sync mode hidden with V-Sync Off", !mr::vsync_mode_shown(0, false) && mr::vsync_mode_shown(1, false));
    CHECK("V-Sync mode greyed where the environment decides", mr::vsync_mode_shown(0, true) && mr::vsync_mode_fixed(0, true) &&
                                                              mr::vsync_mode_fixed(1, true));
    CHECK("V-Sync mode changeable with V-Sync On", !mr::vsync_mode_fixed(1, false) && mr::vsync_mode_fixed(0, false));
    // the cursor stays on its row when rows above it are hidden or shown
    {
        bool shown[6] = {true, true, false, true, true, true};
        auto f = [&](int i) { return shown[i]; };
        CHECK("local index", mr::local_index(6, 4, f) == 3 && mr::local_index(6, 2, f) == -1 && mr::local_index(6, 0, f) == 0);
        shown[1] = false;   // (a row above the cursor hides: the same row is one place up)
        CHECK("local index after a hide", mr::local_index(6, 4, f) == 2);
        shown[2] = true, shown[1] = true;   // (rows above show)
        CHECK("local index after a show", mr::local_index(6, 4, f) == 4);
    }
    std::puts("menu_rows: ok");
    return 0;
}
