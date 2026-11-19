// The prediction's limits (gpu/vk/predict_limits.h): which programs are left to the draw that needs them, and when the prediction
// stops for want of commit. The sizes are the ones the PC 3050's run measured: 1014k and 502k words took the driver's compiler over three
// minutes and past 11 GB; the largest of the others, 133k words, 25 s.
#include <cstdint>
#include <cstdio>

#include "../gpu/vk/predict_limits.h"

using namespace mkwvk::predictlimits;

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main() {
    // (the two programs that took the PC's memory are over the default limit, and every other program of the game is under it)
    CHECK(too_big(1014000, kDefaultMaxKWords));
    CHECK(too_big(502000, kDefaultMaxKWords));
    CHECK(too_big(144000, kDefaultMaxKWords));   // (the program that hung a Turing driver for 2 hours)
    CHECK(!too_big(40000, kDefaultMaxKWords));
    CHECK(!too_big(100999, kDefaultMaxKWords));   // (100k words and some: still 100 thousand)
    CHECK(too_big(101000, kDefaultMaxKWords));
    CHECK(!too_big(0, kDefaultMaxKWords));
    // (0: no limit)
    CHECK(!too_big(5000000, 0));
    // (a limit set by the user)
    CHECK(too_big(100000, 50));
    CHECK(!too_big(50000, 50));
    // (commit: the reserve is what the prediction leaves, and 0 is no reserve)
    CHECK(low_commit(0, kDefaultReserveMB));
    CHECK(low_commit(3071, kDefaultReserveMB));
    CHECK(!low_commit(3072, kDefaultReserveMB));
    CHECK(!low_commit(0, 0));
    std::printf("predict_limits: %d failures\n", bad);
    return bad ? 1 : 0;
}
