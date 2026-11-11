#include "../gpu/vk/gone_ring.h"

#include <cstdio>
#include <thread>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

struct Obj { int x; };

int main() {
    using mkwvk::gone_ring::Ring;
    using mkwvk::gone_ring::page_slot;
    Obj a{1}, b{2}, c{3};
    {
        Ring<Obj, 16, 8> r(1);
        CHECK("first generation", r.generation() == 1);
        CHECK("nothing went: kept", r.kept(&a, 1, 1));
        const uint64_t g = r.bump(&b);
        CHECK("bump returns the generation", g == 2 && r.generation() == 2);
        CHECK("another went: a kept", r.kept(&a, 1, 2));
        CHECK("b itself went: not kept", !r.kept(&b, 1, 2));
        CHECK("nothing resolved (null): not kept", !r.kept(nullptr, 1, 2));
        r.bump(&c);
        r.bump(&a);
        CHECK("a went at 4: stale from 1", !r.kept(&a, 1, 4));
        CHECK("a resolved after it went: kept", r.kept(&a, 4, 4));
        CHECK("b resolved at 2: kept through c and a", r.kept(&b, 2, 4));
        r.bump(nullptr);   // (all of them: the deferred uploads' sweep)
        CHECK("a sweep of all: stale", !r.kept(&b, 4, 5));
        CHECK("after the sweep: kept", r.kept(&b, 5, 5));
        CHECK("backwards: stale", !r.kept(&b, 5, 4));
    }
    {
        Ring<Obj, 16, 8> r(1);
        for (int i = 0; i < 8; i++) r.bump(&b);
        CHECK("a gap of the window: kept", r.kept(&a, 1, 9));
        r.bump(&b);
        CHECK("a gap past the window: stale", !r.kept(&a, 1, 10));
        CHECK("within the window again: kept", r.kept(&a, 2, 10));
        for (int i = 0; i < 16; i++) r.bump(&c);   // (the ring written over)
        CHECK("the last window still held: kept", r.kept(&a, r.generation() - 8, r.generation()));
    }
    {   // (a writer bumping while readers check: a reader never says "kept" for its own object once it went)
        Ring<Obj, 1024, 256> r(1);
        Obj objs[64];
        std::atomic<bool> stop{false}, bad{false};
        std::atomic<uint64_t> went_at{0};   // (the generation objs[0] went at; 0: not yet)
        std::thread reader([&] {
            while (!stop.load()) {
                const uint64_t from = r.generation();
                std::this_thread::yield();
                const uint64_t to = r.generation();
                const uint64_t w = went_at.load();
                if (w && w > from && w <= to && r.kept(&objs[0], from, to)) bad = true;
            }
        });
        for (int i = 0; i < 200000; i++) {
            if (i == 100000) went_at = r.bump(&objs[0]);
            else r.bump(&objs[1 + i % 63]);
        }
        stop = true;
        reader.join();
        CHECK("concurrent: never kept once gone", !bad.load());
    }
    CHECK("page slot: 64 KB pages", page_slot(0x10000, 1024) == 1 && page_slot(0x1FFFF, 1024) == 1);
    CHECK("1024 entries: 64 MB apart share", page_slot(0x80000000, 1024) == page_slot(0x84000000, 1024));
    CHECK("16384 entries: 64 MB apart do not", page_slot(0x80000000, 16384) != page_slot(0x84000000, 16384));
    CHECK("16384 entries: 1 GB apart share", page_slot(0x80000000, 16384) == page_slot(0xC0000000, 16384));
    std::puts("gone_ring: ok");
    return 0;
}
