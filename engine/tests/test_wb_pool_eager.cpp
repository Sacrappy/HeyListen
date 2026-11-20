// (issue 50) gpu/vk/wb_pool_eager.h: when a CPU write makes a lazy pool eager. Built and run by test_wb_pool_eager.py.
#include "../gpu/vk/wb_pool_eager.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    using wb_pool_eager::cpu_write_makes_eager;
    CHECK(cpu_write_makes_eager(false, true));    // (a lazy pool, a CPU write, the fix on: eager)
    CHECK(!cpu_write_makes_eager(false, false));  // (RX_VK_WB_POOL_EAGER=0: as before)
    CHECK(!cpu_write_makes_eager(true, true));    // (already eager: nothing to do)
    CHECK(!cpu_write_makes_eager(true, false));
    using wb_pool_eager::may_defer_size;
    CHECK(!may_defer_size(0x2000, 4ull << 20));          // (the cut-grass debris' 8 KB pool: always written back)
    CHECK(!may_defer_size((4ull << 20) - 1, 4ull << 20));
    CHECK(may_defer_size(4ull << 20, 4ull << 20));       // (a big pool may be left lazy)
    CHECK(may_defer_size(0x2000, 0));                    // (RX_VK_WB_SMALL_EAGER_KB=0: as before)
    if (fails) std::printf("%d checks failed\n", fails);
    else std::printf("wb pool eager: all cases right\n");
    return fails ? 1 : 0;
}
