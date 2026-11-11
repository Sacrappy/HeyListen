// (HeyListen TICK-052) Which objects each bump of a destroy generation was for, so that a cache entry resolved at one generation
// can tell, at a later one, whether its own object went in between (it is stale) or only others did (it stands). Used for the
// textures' destroy generation (vk_texture.cpp destroy_later): each eviction made every recording thread's view and slot caches
// stale, and their next lookups went to the shared lock. Pure, no Vulkan: tests/test_gone_ring.cpp.
//
// One writer at a time (the generation's owner bumps it); any number of readers, without a lock. A bump writes its entry before
// it publishes the generation; a reader that finds an entry of another generation (written over by a later bump, or one not
// published yet) or one for all (a null object) answers "stale", as the plain generation compare did.
#pragma once
#include <atomic>
#include <cstdint>

namespace mkwvk::gone_ring {

template <typename T, uint64_t N = 1024, uint64_t Window = 256>
class Ring {
    static_assert(N && (N & (N - 1)) == 0, "a power of two");
    static_assert(Window < N, "the window within the ring");
    struct Entry {
        std::atomic<uint64_t> gen{0};
        std::atomic<const T*> obj{nullptr};   // (null: every object)
    };
    Entry ring_[N];
    std::atomic<uint64_t> gen_;

public:
    explicit Ring(uint64_t first = 1) : gen_(first) {}
    uint64_t generation() const { return gen_.load(std::memory_order_acquire); }
    /// The generation bumped for obj going (null: for all of them); returns the new generation
    uint64_t bump(const T* obj) {
        const uint64_t g = gen_.load(std::memory_order_relaxed) + 1;
        Entry& e = ring_[g & (N - 1)];
        e.gen.store(0, std::memory_order_relaxed);
        e.obj.store(obj, std::memory_order_relaxed);
        e.gen.store(g, std::memory_order_release);
        gen_.store(g, std::memory_order_release);
        return g;
    }
    /// Whether obj, resolved at generation from, still stands at generation to: no bump in (from, to] was for it or for all.
    /// A null obj (nothing was resolved) never stands, nor a gap wider than the window or one the ring no longer holds
    bool kept(const T* obj, uint64_t from, uint64_t to) const {
        if (from == to) return true;
        if (!obj || to < from || to - from > Window) return false;
        for (uint64_t g = from + 1; g <= to; g++) {
            const Entry& e = ring_[g & (N - 1)];
            if (e.gen.load(std::memory_order_acquire) != g) return false;
            const T* x = e.obj.load(std::memory_order_relaxed);
            if (!x || x == obj) return false;
            if (e.gen.load(std::memory_order_acquire) != g) return false;   // (written over while read)
        }
        return true;
    }
};

/// (TICK-052) the recording threads' table of the pools they found, by 64 KB page: entries in a table of `entries` (a power of
/// two). 1024 entries put pages 64 MB apart in one entry, and the game's pools are further apart than that
inline uint64_t page_slot(uint64_t guest, uint64_t entries) { return (guest >> 16) & (entries - 1); }

} // namespace mkwvk::gone_ring
