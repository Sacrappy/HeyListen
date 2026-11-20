#include "../gpu/vk/framegen_rows.h"

#include <cstdio>
#include <cstring>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace fr = mkwvk::framegen_rows;
    using fr::BY_DLSS;
    using fr::BY_FSR3;
    using fr::BY_OWN;

    // the saved modes keep their numbers; AMD FSR 3.1 came after them
    CHECK("modes", BY_OWN == 0 && BY_DLSS == 1 && BY_FSR3 == 2 && fr::kBy == 3);

    // the highest multiplier each mode makes
    CHECK("own: up to 8x", fr::top(BY_OWN, 0) == 8 && fr::top(BY_OWN, 5) == 8 && fr::top(BY_OWN, 1) == 8);
    CHECK("FSR 3.1: 2x", fr::top(BY_FSR3, 0) == 2 && fr::top(BY_FSR3, 5) == 2);
    CHECK("DLSS FG: its runtime's most + 1 (RTX 50: 5 made frames, 6x; RTX 40: 1, 2x)", fr::top(BY_DLSS, 5) == 6 && fr::top(BY_DLSS, 1) == 2);
    CHECK("DLSS FG: 8x while the most is not known or it does not make the frames", fr::top(BY_DLSS, 0) == 8 && fr::top(BY_DLSS, -1) == 8);
    CHECK("DLSS FG: a runtime of 7 or more made frames is 8x at the most", fr::top(BY_DLSS, 7) == 8 && fr::top(BY_DLSS, 9) == 8);

    // the value as the mode makes it
    CHECK("Off stays Off", fr::clamp(BY_OWN, 0, 0) == 0 && fr::clamp(BY_DLSS, 5, 0) == 0 && fr::clamp(BY_FSR3, 0, 0) == 0);
    CHECK("Auto: the renderer's own only", fr::clamp(BY_OWN, 0, 1) == 1 && fr::clamp(BY_DLSS, 5, 1) == 2 && fr::clamp(BY_FSR3, 0, 1) == 2);
    CHECK("own: the multiplier as it is", fr::clamp(BY_OWN, 0, 2) == 2 && fr::clamp(BY_OWN, 0, 8) == 8 && fr::clamp(BY_OWN, 5, 7) == 7);
    CHECK("DLSS FG on an RTX 50: 6x at most", fr::clamp(BY_DLSS, 5, 8) == 6 && fr::clamp(BY_DLSS, 5, 7) == 6 && fr::clamp(BY_DLSS, 5, 6) == 6 &&
                                                  fr::clamp(BY_DLSS, 5, 5) == 5 && fr::clamp(BY_DLSS, 5, 2) == 2);
    CHECK("DLSS FG on an RTX 40: 2x at most", fr::clamp(BY_DLSS, 1, 8) == 2 && fr::clamp(BY_DLSS, 1, 3) == 2 && fr::clamp(BY_DLSS, 1, 2) == 2);
    CHECK("FSR 3.1: 2x at most", fr::clamp(BY_FSR3, 0, 8) == 2 && fr::clamp(BY_FSR3, 0, 3) == 2 && fr::clamp(BY_FSR3, 0, 2) == 2);
    CHECK("a value past the row", fr::clamp(BY_OWN, 0, 99) == 8 && fr::clamp(BY_OWN, 0, -4) == 0 && fr::clamp(BY_FSR3, 0, 99) == 2);
    for (int by = 0; by < fr::kBy; by++)
        for (int most : {0, 1, 5})
            for (int v = -2; v < 12; v++) {
                const int c = fr::clamp(by, most, v);
                CHECK("a clamped value is never above the mode's top", c == 0 || c == 1 || (c >= 2 && c <= fr::top(by, most)));
                CHECK("a clamped value is never raised", c <= (v < 0 ? 0 : v) || (v == 1 && c == 2));
                CHECK("clamp is idempotent", fr::clamp(by, most, c) == c);
                CHECK("Auto only where offered", c != 1 || fr::auto_offered(by));
            }

    // what each mode offers, in order, and the arrows over it
    {
        int list[16];
        int n = fr::offered(BY_OWN, 0, list);
        CHECK("own offers Off, Auto, 2x to 8x", n == 9 && list[0] == 0 && list[1] == 1 && list[2] == 2 && list[8] == 8);
        n = fr::offered(BY_DLSS, 5, list);
        CHECK("DLSS FG on an RTX 50 offers Off, 2x to 6x", n == 6 && list[0] == 0 && list[1] == 2 && list[5] == 6);
        n = fr::offered(BY_DLSS, 1, list);
        CHECK("DLSS FG on an RTX 40 offers Off, 2x", n == 2 && list[0] == 0 && list[1] == 2);
        n = fr::offered(BY_FSR3, 0, list);
        CHECK("FSR 3.1 offers Off, 2x", n == 2 && list[0] == 0 && list[1] == 2);
    }
    CHECK("own steps: Off, Auto, 2x ... 8x, round", fr::step(BY_OWN, 0, 0, 1) == 1 && fr::step(BY_OWN, 0, 1, 1) == 2 && fr::step(BY_OWN, 0, 8, 1) == 0 &&
                                                       fr::step(BY_OWN, 0, 0, -1) == 8 && fr::step(BY_OWN, 0, 2, -1) == 1 && fr::step(BY_OWN, 0, 1, -1) == 0);
    CHECK("DLSS FG on an RTX 50 steps: Off, 2x ... 6x", fr::step(BY_DLSS, 5, 0, 1) == 2 && fr::step(BY_DLSS, 5, 5, 1) == 6 &&
                                                          fr::step(BY_DLSS, 5, 6, 1) == 0 && fr::step(BY_DLSS, 5, 0, -1) == 6 &&
                                                          fr::step(BY_DLSS, 5, 2, -1) == 0);
    CHECK("DLSS FG on an RTX 50: a saved 8x steps from the 6x shown", fr::step(BY_DLSS, 5, 8, 1) == 0 && fr::step(BY_DLSS, 5, 8, -1) == 5 &&
                                                                        fr::step(BY_DLSS, 5, 7, -1) == 5);
    CHECK("a saved Auto with DLSS FG is 2x to the arrows", fr::step(BY_DLSS, 5, 1, 1) == 3 && fr::step(BY_DLSS, 5, 1, -1) == 0);
    CHECK("DLSS FG on an RTX 40 steps: Off, 2x", fr::step(BY_DLSS, 1, 0, 1) == 2 && fr::step(BY_DLSS, 1, 2, 1) == 0 && fr::step(BY_DLSS, 1, 0, -1) == 2);
    CHECK("FSR 3.1 steps: Off, 2x", fr::step(BY_FSR3, 0, 0, 1) == 2 && fr::step(BY_FSR3, 0, 2, 1) == 0 && fr::step(BY_FSR3, 0, 6, 1) == 0 &&
                                      fr::step(BY_FSR3, 0, 2, -1) == 0);
    for (int by = 0; by < fr::kBy; by++)
        for (int most : {0, 1, 5})
            for (int v = 0; v <= 8; v++)
                for (int dir : {1, -1}) {
                    const int next = fr::step(by, most, v, dir);
                    CHECK("an arrow lands on what the mode offers", next == fr::clamp(by, most, next));
                    CHECK("an arrow moves", next != fr::clamp(by, most, v) || fr::top(by, most) < 2);
                }

    // switching the mode keeps the saved multiplier within the new mode's, never raising it
    CHECK("mode change", fr::on_mode_change(BY_DLSS, 5, 8) == 6 && fr::on_mode_change(BY_DLSS, 1, 4) == 2 && fr::on_mode_change(BY_FSR3, 0, 8) == 2 &&
                             fr::on_mode_change(BY_OWN, 0, 6) == 6 && fr::on_mode_change(BY_DLSS, 5, 1) == 2 &&
                             fr::on_mode_change(BY_FSR3, 0, 1) == 2 && fr::on_mode_change(BY_OWN, 0, 1) == 1 && fr::on_mode_change(BY_DLSS, 5, 0) == 0);

    // the Frame rate limit row: the values from before keep their numbers, Unlimited is the new last one
    CHECK("limit values from before", fr::limit_fps(0) == 60 && fr::limit_fps(1) == 90 && fr::limit_fps(2) == 120 && fr::limit_fps(3) == 144 &&
                                          fr::limit_fps(4) == 165 && fr::limit_fps(5) == 240);
    CHECK("Unlimited is 6, the last, and 0 frames a second (no limit)", fr::kLimitUnlimited == 6 && fr::kLimits == 7 && fr::limit_fps(6) == 0);
    CHECK("a limit past the row is the console's 60", fr::limit_fps(7) == 60 && fr::limit_fps(-1) == 60);

    // the limit is the rate presented: the game runs at the limit over the multiplier
    CHECK("no frame generation: the game at the limit", fr::game_fps(120, 0) == 120.0 && fr::game_fps(120, 1) == 120.0 && fr::game_fps(60, 1) == 60.0);
    CHECK("fixed multiplier: the limit over it", fr::game_fps(120, 2) == 60.0 && fr::game_fps(120, 3) == 40.0 && fr::game_fps(240, 4) == 60.0 &&
                                                     fr::game_fps(144, 6) == 24.0);
    CHECK("Unlimited: no rate to hold", fr::game_fps(0, 4) == 0.0 && fr::game_fps(0, 1) == 0.0);
    CHECK("shown", fr::shown_fps(40.0, 3) == 120.0 && fr::shown_fps(60.0, 1) == 60.0 && fr::shown_fps(60.0, 0) == 60.0);
    // the multiplier that leaves the game its 20 frames a second
    CHECK("the minimum is 20", fr::kMinGameFps == 20);
    CHECK("limited at 60", fr::limited(60, 2) == 2 && fr::limited(60, 3) == 3 && fr::limited(60, 4) == 3 && fr::limited(60, 8) == 3);
    CHECK("limited at 90", fr::limited(90, 4) == 4 && fr::limited(90, 5) == 4);
    CHECK("limited at 120", fr::limited(120, 6) == 6 && fr::limited(120, 7) == 6 && fr::limited(120, 8) == 6);
    CHECK("limited at 144, 165, 240", fr::limited(144, 7) == 7 && fr::limited(144, 8) == 7 && fr::limited(165, 8) == 8 && fr::limited(240, 8) == 8);
    CHECK("Off, Auto and Unlimited are not lowered", fr::limited(60, 0) == 0 && fr::limited(60, 1) == 1 && fr::limited(0, 8) == 8 && fr::limited(0, 2) == 2 &&
                                                         fr::limited(-1, 8) == 8);
    CHECK("a fixed multiplier stays at least 2x", fr::limited(30, 8) == 2 && fr::limited(40, 4) == 2);
    for (int row = 0; row < 6; row++) {   // (every limit and every multiplier: what is shown is the limit, and the game keeps its 20)
        const int limit = fr::limit_fps(row);
        for (int m = 2; m <= 8; m++) {
            const int run = fr::limited(limit, m);
            CHECK("the multiplier is never raised", run <= m && run >= 2);
            CHECK("the game keeps at least 20 frames a second", fr::game_fps(limit, run) >= fr::kMinGameFps);
            CHECK("the frames shown are the limit", fr::shown_fps(fr::game_fps(limit, run), run) > limit - 0.001 &&
                                                        fr::shown_fps(fr::game_fps(limit, run), run) < limit + 0.001);
        }
    }
    std::puts("framegen rows: all checks passed");
}
