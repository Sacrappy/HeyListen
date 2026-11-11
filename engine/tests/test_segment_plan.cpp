#include "../gpu/vk/segment_plan.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::segment_plan::recorders;
    using mkwvk::segment_plan::rounds;
    using mkwvk::segment_plan::segment_count;
    const int kMaxBatch = 6;

    CHECK("4 workers are 4 recorders", recorders(4, false) == 4);
    CHECK("the replay thread is one more", recorders(4, true) == 5);
    CHECK("no workers", recorders(0, false) == 0 && recorders(-1, true) == 1);

    // (the base: one segment per 160, at most kMaxBatch, whatever the recorders)
    CHECK("base: 830 draws are 5 segments", segment_count(830, 160, 4, kMaxBatch, false) == 5);
    CHECK("base: 1200 draws are capped at 6", segment_count(1200, 160, 4, kMaxBatch, false) == 6);
    CHECK("base: below 2 x per is one segment", segment_count(319, 160, 4, kMaxBatch, false) == 1);
    CHECK("base: zero work is still one segment", segment_count(0, 160, 4, kMaxBatch, false) == 1);

    // (RX_VK_PARALLEL_ROUNDS: at most the recorders)
    CHECK("one round: 830 draws on 4 workers are 4", segment_count(830, 160, 4, kMaxBatch, true) == 4);
    CHECK("one round + self: 830 on 5 recorders are 5", segment_count(830, 160, recorders(4, true), kMaxBatch, true) == 5);
    CHECK("one round + self: 1200 on 5 recorders are 5", segment_count(1200, 160, 5, kMaxBatch, true) == 5);
    CHECK("one round: fewer draws than recorders x per keep their count", segment_count(500, 160, 4, kMaxBatch, true) == 3);
    CHECK("one round: below 2 x per is one segment", segment_count(319, 160, 5, kMaxBatch, true) == 1);
    CHECK("one round: the cap below the recorders wins", segment_count(2000, 160, 8, kMaxBatch, true) == kMaxBatch);
    CHECK("one round: a cap of 2", segment_count(2000, 160, 5, 2, true) == 2);
    CHECK("one round with no recorders: the cap alone", segment_count(2000, 160, 0, kMaxBatch, true) == kMaxBatch);

    // (RX_VK_PARALLEL_SEG: another segment size)
    CHECK("per 32 (the floor)", segment_count(830, 32, 4, kMaxBatch, false) == kMaxBatch && segment_count(830, 32, 4, kMaxBatch, true) == 4);
    CHECK("per 640: Kokiri's ~750 is one segment", segment_count(750, 640, 4, kMaxBatch, true) == 1);
    CHECK("a bad per or cap is not a division by zero", segment_count(830, 0, 4, kMaxBatch, false) == kMaxBatch &&
                                                          segment_count(830, 160, 4, 0, false) == 1);

    // (the rounds, a diagnosis)
    CHECK("5 segments on 4 are 2 rounds", rounds(5, 4) == 2);
    CHECK("4 on 4 are 1", rounds(4, 4) == 1);
    CHECK("5 on 5 are 1", rounds(5, 5) == 1);
    CHECK("6 on 4 are 2", rounds(6, 4) == 2);
    CHECK("none are 0", rounds(0, 4) == 0);
    // (RX_VK_PARALLEL_BALANCE: the boundaries by recording time)
    using mkwvk::segment_plan::balanced_targets;
    using mkwvk::segment_plan::Profile;
    using mkwvk::segment_plan::profile_add;
    {
        Profile p;
        int64_t out[5] = {};
        balanced_targets(p, 4, 800, out);
        CHECK("no profile yet: even counts", out[0] == 200 && out[1] == 400 && out[2] == 600);
        // (an even cost: even boundaries)
        const double f1[4] = {0, .25, .5, .75}, t1[4] = {.25, .5, .75, 1}, m1[4] = {1, 1, 1, 1};
        profile_add(p, f1, t1, m1, 4, 0.25);
        balanced_targets(p, 4, 800, out);
        CHECK("an even cost: even boundaries", out[0] == 200 && out[1] == 400 && out[2] == 600);
        // (the first half costs three times the second: a sample taken whole the first time, blended after)
        Profile q;
        const double f2[2] = {0, .5}, t2[2] = {.5, 1}, m2[2] = {3, 1};
        profile_add(q, f2, t2, m2, 2, 0.25);
        balanced_targets(q, 2, 800, out);
        CHECK("front-loaded: the boundary moves forward", out[0] > 250 && out[0] < 290);   // (2/3 of the first half: 267)
        balanced_targets(q, 4, 800, out);
        CHECK("front-loaded, 4 parts: equal time", out[0] > 120 && out[0] < 147 && out[1] > 250 && out[1] < 290 && out[2] > 380 && out[2] < 420);
        profile_add(q, f1, t1, m1, 4, 0.25);   // (then an even frame: moves a quarter of the way back)
        balanced_targets(q, 2, 800, out);
        CHECK("blended: between", out[0] > 267 && out[0] < 400);
        CHECK("boundaries in order", out[0] > 0 && out[0] < 800);
        // (bad samples change nothing)
        Profile r;
        const double fb[1] = {.5}, tb[1] = {.5}, mb[1] = {5};
        profile_add(r, fb, tb, mb, 1, 0.25);
        CHECK("an empty range is no sample", !r.valid);
        balanced_targets(r, 3, 900, out);
        CHECK("still even", out[0] == 300 && out[1] == 600);
    }
    std::puts("segment plan: all checks passed");
}
