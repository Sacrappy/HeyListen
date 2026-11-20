/* The Insert menu's frame rows (vk_menu.cpp, vk_framegen.cpp, host/nn_vi.c), as plain functions: what the Frame generation
   multiplier row offers for each Frame generation by, and how the Frame rate limit divides between the game and the made
   frames. No Vulkan, no menu: tests/test_framegen_rows.cpp checks them. */
#pragma once

#include <algorithm>

namespace mkwvk::framegen_rows {

/// Frame generation by (opts.framegen_by, saved as frame_generation_by): 0 the renderer's own (its block matching, and AMD FSR 3.1's
/// frame interpolation at 2x where it runs), 1 NVIDIA DLSS Frame Generation, 2 AMD FSR 3.1's frame interpolation alone. The saved 0
/// and 1 keep their meaning; 2 came after them
enum { BY_OWN = 0, BY_DLSS = 1, BY_FSR3 = 2, kBy = 3 };
/// The Frame generation row (opts.framegen): 0 Off, 1 Auto (a frame for each refresh of the display), 2 to 8 a fixed multiplier
constexpr int kOff = 0, kAuto = 1, kFixedFrom = 2, kMaxMult = 8;

/// The highest fixed multiplier the mode makes. DLSS FG: its runtime's most made frames + 1 (dlss_most: vk_dlss.h dlss_fg_most,
/// 5 on an RTX 50's runtime and 1 on an RTX 40's: 6x and 2x; 0 while not known, or while DLSS FG does not make the frames: the
/// renderer's own makes them, up to 8x). AMD FSR 3.1: 2x, the one made frame between two of the game's. The renderer's own: 8x
inline int top(int by, int dlss_most) {
    if (by == BY_FSR3) return 2;
    if (by == BY_DLSS && dlss_most > 0) return std::min(dlss_most + 1, (int)kMaxMult);
    return kMaxMult;
}
/// Whether Auto is one of the mode's values: the renderer's own makes it (DLSS FG and FSR 3.1 make a fixed 2x and up)
inline bool auto_offered(int by) { return by != BY_DLSS && by != BY_FSR3; }
/// The value as the mode makes it: Off stays Off; Auto, where the mode has none, is its lowest fixed multiplier (2x); a
/// multiplier past the top is the top. A value past the row is Off or the top
inline int clamp(int by, int dlss_most, int v) {
    if (v <= kOff) return kOff;
    if (v == kAuto) return auto_offered(by) ? kAuto : kFixedFrom;
    return std::min(v, top(by, dlss_most));
}
/// The values the mode offers, in order: Off, Auto (where offered), 2x to the top. Writes them to out (room for 9), returns how
/// many
inline int offered(int by, int dlss_most, int* out) {
    int n = 0;
    out[n++] = kOff;
    if (auto_offered(by)) out[n++] = kAuto;
    for (int v = kFixedFrom; v <= top(by, dlss_most); v++) out[n++] = v;
    return n;
}
/// The arrows' next value from v (dir +1 Right, -1 Left), wrapping over what the mode offers. A saved value the mode does not
/// offer is stepped from as the mode makes it (clamp), so the arrow moves from the value the row shows
inline int step(int by, int dlss_most, int v, int dir) {
    int list[16];
    const int n = offered(by, dlss_most, list);
    v = clamp(by, dlss_most, v);
    int at = 0;
    while (at < n && list[at] != v) at++;
    return list[(at + (dir > 0 ? 1 : n - 1)) % n];
}
/// The value the mode makes of v when the mode changes (the saved multiplier clamped to the new mode): never raised
inline int on_mode_change(int by, int dlss_most, int v) { return clamp(by, dlss_most, v); }

/// The Frame rate limit row (opts.fps_limit, saved as frame_rate_limit): 0 60, 1 90, 2 120, 3 144, 4 165, 5 240 (the values
/// from before keep their numbers), 6 Unlimited. The limit is the rate PRESENTED: the game's own frames and the frames made
/// between them. Off or Auto, the game runs at the limit (Auto makes frames only to fill what the game falls short of, up to
/// the limit and the display's refresh); at a fixed multiplier m the game runs at the limit over m, and the made frames fill the
/// rest. Unlimited: no limit from the game or the frame generation (the display and the driver's present mode may still hold it)
enum { kLimit60, kLimit90, kLimit120, kLimit144, kLimit165, kLimit240, kLimitUnlimited, kLimits };
constexpr int kLimitFps[kLimits] = {60, 90, 120, 144, 165, 240, 0};
/// The limit in frames a second: 0 for Unlimited. A value past the row is the console's 60
inline int limit_fps(int v) { return v >= 0 && v < kLimits ? kLimitFps[v] : 60; }
/// The slowest the game runs for a fixed multiplier: 20 frames a second, Ocarina of Time's own rate. Below it the game's steps
/// are too coarse (its input, its physics): the multiplier is lowered instead, so that the limit still holds
constexpr int kMinGameFps = 20;
/// The multiplier at a limit (limit: limit_fps, 0 Unlimited): v, or the highest that leaves the game its 20 frames a second
/// (60: 3x at the most, 90: 4x, 120: 6x, 144: 7x, 165: 8x, 240: 8x). Off, Auto and Unlimited as they are; a fixed multiplier
/// is at least 2x
inline int limited(int limit, int v) {
    if (v < kFixedFrom || limit <= 0) return v;
    return std::max((int)kFixedFrom, std::min(v, limit / kMinGameFps));
}
/// The game's own rate (frames a second) for a limit and the multiplier that runs (1: no fixed frame generation): the limit
/// over it. 0 for Unlimited (no rate to hold)
inline double game_fps(int limit, int mult) { return limit <= 0 ? 0.0 : (double)limit / (mult > 1 ? mult : 1); }
/// The frames a second shown for the game's rate g (a fixed multiplier shows each game frame and its made frames)
inline double shown_fps(double game, int mult) { return game * (mult > 1 ? mult : 1); }

} // namespace mkwvk::framegen_rows
