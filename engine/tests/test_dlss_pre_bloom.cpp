#include "../gpu/vk/pre_bloom.h"
#include "../gpu/vk/upscaler_rows.h"

#include <cstdio>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace pb = mkwvk::pre_bloom;

// One pass of a frame, as vk_temporal.cpp temporal_pre_bloom sees it: a draw into the scene ('D') or one that reads it ('R',
// with its program)
struct Pass {
    char kind;
    uint64_t key;
};

// The Market's frame (DLAA at 5120x2160; RX_VK_DUMP of main 7887c497, steps 240 to 295): scene draws, the game's copies of
// the scene at a quarter and at half the size, more scene draws (the shadowed transparents, the particles), a second half-size
// copy, a quarter-size one, the last scene draws, then the bloom's first draw (to an eighth of the size), and the scene
// mapping draw (the scene with the bloom through the tone curve)
constexpr uint64_t kQuarterCopy = 0xb7914ae65792d0c8ull, kHalfCopy = 0xed4819446f84dcb4ull, kQuarter2 = 0x1b464cb9d32aaae5ull;
constexpr uint64_t kBloom = 0x95937683d9f74e95ull, kMapping = 0xab3681946f93594aull;
static const std::vector<Pass> kMarket = {
    {'D', 0}, {'D', 0}, {'D', 0},
    {'R', kQuarterCopy}, {'R', kHalfCopy},
    {'D', 0}, {'D', 0},
    {'R', kHalfCopy},
    {'D', 0},
    {'R', kQuarter2},
    {'D', 0}, {'D', 0}, {'D', 0},
    {'R', kBloom},
    {'R', kMapping},
};

// Draws one frame's passes with the trigger t: where DLAA ran (-1: nowhere), and the frame for learn()
static int play(const std::vector<Pass>& passes, const pb::Trigger& t, pb::Frame& f) {
    f = {};
    int ran = -1;
    for (size_t i = 0; i < passes.size(); i++) {
        if (passes[i].kind == 'D') f.scene_draw();
        else if (f.sampler(passes[i].key, t)) ran = (int)i;
    }
    return ran;
}

int main() {
    pb::Frame f;
    // ---- the first frame: no place known, DLAA at the network; it teaches the bloom's first draw ----
    CHECK("first frame: nowhere", play(kMarket, {}, f) == -1);
    const pb::Trigger t = pb::learn(f);
    CHECK("learned", t.valid);
    CHECK("the bloom's first draw", t.key == kBloom);
    CHECK("its place: the fifth reader", t.place == 4);
    CHECK("six readers", f.samplers == 6);
    // ---- the next frame: DLAA before the bloom's first draw, after the last scene draw, and it teaches the same ----
    CHECK("next frame: before the bloom", play(kMarket, t, f) == 13);
    CHECK("no late draw", !f.late);
    CHECK("the same place again", pb::learn(f) == t);
    // ---- never at the first reader (a copy the scene is drawn into after) ----
    {
        const pb::Trigger first{kQuarterCopy, 0, true};
        CHECK("a copy's place: DLAA runs there", play(kMarket, first, f) == 3);
        CHECK("the draws after it: late", f.late);
        CHECK("late teaches nothing", !pb::learn(f).valid);
    }
    // ---- the same program at another place does not fire (the copies' program at the bloom's place) ----
    {
        std::vector<Pass> other = kMarket;
        other[13].key = kHalfCopy;
        CHECK("another program at the place: nowhere", play(other, t, f) == -1);
        CHECK("it teaches that one", pb::learn(f).key == kHalfCopy && pb::learn(f).place == 4);
    }
    // ---- the bloom's program, but one reader more before it (another area's passes): nowhere, then it learns ----
    {
        std::vector<Pass> more = kMarket;
        more.insert(more.begin() + 9, Pass{'R', kHalfCopy});
        // ((HeyListen) matched by its program: it runs before the bloom there too, and the place kept: keep())
        CHECK("one reader more: still before the bloom", play(more, t, f) == 14);
        CHECK("kept", pb::keep(f, t) == t);
        const pb::Trigger m = pb::learn(f);
        CHECK("it would teach the new place", m.valid && m.key == kBloom && m.place == 5);
        CHECK("then before the bloom", play(more, m, f) == 14);
    }
    // ---- no reader after the last scene draw (a menu over the scene, no bloom): nothing learned ----
    {
        const std::vector<Pass> none = {{'D', 0}, {'R', kQuarterCopy}, {'D', 0}};
        CHECK("none: nowhere", play(none, t, f) == -1);
        CHECK("none: nothing learned", !pb::learn(f).valid);
        const std::vector<Pass> empty = {};
        CHECK("an empty frame: nothing learned", play(empty, t, f) == -1 && !pb::learn(f).valid);
    }
    // ---- it runs once a frame: the same program at the same place cannot come twice, and later readers do not fire ----
    {
        pb::Frame g;
        const pb::Trigger at0{kBloom, 0, true};
        g.scene_draw();
        CHECK("fires", g.sampler(kBloom, at0));
        CHECK("then not again", !g.sampler(kBloom, at0) && !g.sampler(kBloom, at0));
        CHECK("ran at 0", g.ran && g.ran_at == 0);
    }
    // ---- (HeyListen) the place kept: keep() ----
    {
        // HeyListen's frame: draws into the scene after the bloom's first read and after the tone map's (from its log)
        static const std::vector<Pass> kHeyListen = {
            {'D', 0}, {'R', kQuarterCopy}, {'D', 0}, {'R', kHalfCopy}, {'D', 0}, {'R', kHalfCopy}, {'D', 0},
            {'R', kQuarter2}, {'D', 0}, {'R', kBloom}, {'D', 0}, {'R', kMapping}, {'D', 0},
        };
        CHECK("HeyListen's frame teaches nothing", play(kHeyListen, {}, f) == -1 && !pb::learn(f).valid);
        CHECK("...and keeps nothing", !pb::keep(f, {}).valid);
        CHECK("with the place: before the bloom", play(kHeyListen, t, f) == 9);
        CHECK("late", f.late);
        CHECK("late: the place kept", pb::keep(f, t) == t);
        for (int i = 0; i < 100; i++) {   // (frame after frame: no change, no flicker)
            CHECK("every frame before the bloom", play(kHeyListen, t, f) == 9);
            CHECK("kept", pb::keep(f, t) == t);
        }
        // a frame without the bloom (a menu over the scene): nowhere, the place kept for after it
        const std::vector<Pass> menu = {{'D', 0}, {'R', kQuarterCopy}, {'D', 0}};
        CHECK("a menu: nowhere", play(menu, t, f) == -1);
        CHECK("a menu: kept", pb::keep(f, t) == t);
        // the tone map's read learned first (a frame drawing into the scene up to it): an earlier place learned replaces it
        const pb::Trigger mapping{kMapping, 5, true};
        CHECK("at the tone map", play(kMarket, mapping, f) == 14);
        CHECK("not late", !f.late);
        CHECK("the bloom's place taught: taken", pb::keep(f, mapping) == t);
        // where it ran before the bloom, the tone map's place a frame teaches does not move it later
        pb::Frame g;
        g.scene_draw(), (void)g.sampler(kQuarterCopy, t);   // (a frame: the bloom's read missing, the tone map after a draw)
        g.scene_draw(), (void)g.sampler(kMapping, t);
        CHECK("not fired", !g.ran);
        CHECK("another area's place: taken", pb::keep(g, t).key == kMapping);
        pb::Frame h;
        h.scene_draw(), (void)h.sampler(kBloom, t);
        h.scene_draw(), (void)h.sampler(kMapping, t);
        CHECK("fired, then late", h.ran && h.late && pb::keep(h, t) == t);
    }
    // ---- more readers than the list holds: the place past it is not learned, one inside it is ----
    {
        pb::Frame g;
        for (uint32_t i = 0; i < pb::kSamplers + 4; i++) (void)g.sampler(i + 1, {});
        g.scene_draw();
        (void)g.sampler(0x77, {});
        CHECK("past the list: nothing learned", !pb::learn(g).valid);
        pb::Frame h;
        for (uint32_t i = 0; i < pb::kSamplers - 1; i++) (void)h.sampler(i + 1, {});
        h.scene_draw();
        (void)h.sampler(0x77, {});
        const pb::Trigger last = pb::learn(h);
        CHECK("the list's last place: learned", last.valid && last.key == 0x77 && last.place == pb::kSamplers - 1);
    }
    // ---- the menu's DLAA before the bloom row (upscaler_rows.h): shown with NVIDIA DLSS where it runs, changeable at DLAA
    // alone (DLSS's other modes draw the scene smaller than DLSS's picture), fixed while RX_VK_DLSS_PRE_BLOOM sets it ----
    {
        namespace ur = mkwvk::upscaler_rows;
        CHECK("shown: DLAA", ur::pre_bloom_shown(true, ur::DLAA));
        CHECK("shown: DLSS", ur::pre_bloom_shown(true, ur::DLSS));
        CHECK("hidden: no DLSS here", !ur::pre_bloom_shown(false, ur::DLAA));
        for (int u : {ur::GAME, ur::FSR3, ur::FSR4, ur::FSR4_AA, ur::OFF})   // (HeyListen has no XeSS)
            CHECK("hidden: another upscaler", !ur::pre_bloom_shown(true, u));
        CHECK("changeable: DLAA", !ur::pre_bloom_fixed(ur::DLAA, false));
        CHECK("fixed: DLSS below Native", ur::pre_bloom_fixed(ur::DLSS, false));
        CHECK("fixed: the switch set", ur::pre_bloom_fixed(ur::DLAA, true));
        CHECK("fixed: the parallel replay", ur::pre_bloom_fixed(ur::DLAA, false, true));
        // (the bug: the menu asked the Upscaler row's choice, which is DLSS for DLAA too (ur::engine), so the row never came
        // usable; it asks the mode the chain runs, which is DLAA for DLSS at the Render resolution's native choice)
        CHECK("the row's choice is never DLAA", ur::engine(ur::DLAA) == ur::DLSS);
        CHECK("DLSS at Native is DLAA", ur::mode_for(ur::DLSS, ur::kNativeAa) == ur::DLAA);
        CHECK("usable: DLSS at Native", !ur::pre_bloom_fixed(ur::mode_for(ur::DLSS, ur::kNativeAa), false) &&
                                          ur::pre_bloom_shown(true, ur::mode_for(ur::DLSS, ur::kNativeAa)));
        for (int res : {0, 1, 2, 3, 4, 5, 6}) {   // (Auto, 100% to Ultra Performance: DLSS below Native)
            CHECK("greyed, still shown: DLSS below Native", ur::pre_bloom_fixed(ur::mode_for(ur::DLSS, res), false) &&
                                                              ur::pre_bloom_shown(true, ur::mode_for(ur::DLSS, res)));
        }
        CHECK("FSR 4 at Native is not DLAA", ur::pre_bloom_fixed(ur::mode_for(ur::FSR4, ur::kNativeAa), false) &&
                                                !ur::pre_bloom_shown(true, ur::mode_for(ur::FSR4, ur::kNativeAa)));
    }
    std::puts("pre_bloom: ok");
    return 0;
}
