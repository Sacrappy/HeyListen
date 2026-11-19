// What the shaders' preparation is made of (gpu/vk/prepare_work.h): tests/test_prepare_speed.py compiles and runs this.
#include "../gpu/vk/prepare_work.h"
#include <cstdio>

static int failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failed++; } } while (0)

int main() {
    using namespace prepare_work;
    // workers before the game: all the CPUs but an eighth (one at least)
    CHECK(workers(false, 1) == 1);
    CHECK(workers(false, 2) == 2);
    CHECK(workers(false, 4) == 4);
    CHECK(workers(false, 8) == 7);
    CHECK(workers(false, 12) == 11);
    CHECK(workers(false, 16) == 14);
    CHECK(workers(false, 32) == 28);
    CHECK(workers(false, 128) == 48);   // (never more than 48)
    CHECK(workers(false, 0) == 1);      // (a CPU count the system could not give)
    // the old count and a number of one's own
    CHECK(workers(false, 12, "old") == 6);
    CHECK(workers(false, 16, "old") == 8);
    CHECK(workers(false, 12, "3") == 3);
    CHECK(workers(false, 12, "500") == 64);
    CHECK(workers(false, 12, "0") == 11);      // (not a number of workers: the default)
    CHECK(workers(false, 12, "nonsense") == 11);
    // in play: a quarter, two to four, whatever the others do
    CHECK(workers(true, 4) == 2);
    CHECK(workers(true, 16) == 4);
    CHECK(workers(true, 64) == 4);
    // the bar: a compute program the driver builds is worth many graphics programs, one left to play few
    CHECK(cost(false, false) == kGraphics);
    CHECK(cost(true, false) >= 50 * cost(false, false));
    CHECK(cost(true, true) < cost(true, false) && cost(true, true) > cost(false, false));
    if (failed) return 1;
    std::printf("prepare_speed: ok\n");
    return 0;
}
