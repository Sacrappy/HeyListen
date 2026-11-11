// (HeyListen TICK-052) A counter every recording thread adds to, split into one cache line a thread: the shared counters the
// draws bump (st_draws, st_desc_reused, the phase times prof_ns, ...) were plain int64_t that the replay workers and the
// Presentation Thread wrote at every draw, so the line went from core to core at each one (and racy adds lost counts). Each thread
// adds to its own slot (a thread past the slots shares one, atomically); a read sums the slots. Pure: tests/test_split_count.cpp.
#pragma once
#include <atomic>
#include <cstdint>

namespace mkwvk {

constexpr int kSplitSlots = 16;
inline int split_slot() {
    static std::atomic<int> next{0};
    static thread_local const int slot = next.fetch_add(1, std::memory_order_relaxed) % kSplitSlots;
    return slot;
}

struct SplitCount {
    struct alignas(64) Slot {
        std::atomic<int64_t> v{0};
    };
    Slot slots[kSplitSlots];
    void add(int64_t d) { slots[split_slot()].v.fetch_add(d, std::memory_order_relaxed); }
    void operator++(int) { add(1); }
    void operator+=(int64_t d) { add(d); }
    int64_t get() const {
        int64_t s = 0;
        for (const Slot& x : slots) s += x.v.load(std::memory_order_relaxed);
        return s;
    }
    operator int64_t() const { return get(); }
};

} // namespace mkwvk
