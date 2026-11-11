#include "../gpu/vk/split_count.h"

#include <cstdio>
#include <thread>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

mkwvk::SplitCount total, other;

int main() {
    CHECK("starts at 0", (int64_t)total == 0);
    total++;
    total += 41;
    CHECK("one thread", total.get() == 42);
    CHECK("a cache line a slot", sizeof(mkwvk::SplitCount::Slot) == 64 && alignof(mkwvk::SplitCount) == 64);
    // (more threads than slots: those past them share one, and nothing is lost)
    std::vector<std::thread> ts;
    for (int t = 0; t < mkwvk::kSplitSlots + 7; t++)
        ts.emplace_back([] {
            for (int i = 0; i < 100000; i++) total++, other += 2;
        });
    for (auto& t : ts) t.join();
    CHECK("threads: no count lost", total.get() == 42 + (int64_t)(mkwvk::kSplitSlots + 7) * 100000);
    CHECK("another counter apart", other.get() == 2 * (int64_t)(mkwvk::kSplitSlots + 7) * 100000);
    const int64_t as_int = total;
    CHECK("reads as an integer", as_int == total.get());
    std::puts("split_count: ok");
    return 0;
}
