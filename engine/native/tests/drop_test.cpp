// HeyListen native engine: the drop policy and its touch hook, run with no game against a stand-in substrate
// (tests/test_native_engine.py).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "hl/mem/Drop.h"

using hl::mem::Drop;
using hl::mem::DropSource;
using hl::mem::Refuse;
using hl::mem::TouchPath;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

constexpr uint64_t G = 4096;          // (a small granule for the test)
constexpr uint64_t BASE = 0x100000;   // the stand-in window's first address
constexpr uint64_t GRANULES = 64;
constexpr int64_t S = 1'000'000'000;

// The stand-in substrate: memory, a dropped bit and a read-only bit a granule, the working set, the source files. A
// restore fills a private buffer, then makes it visible under the window's lock in one step (as the real substrate maps
// a filled view in one call); a dropped granule reads as garbage to anyone who looks without the lock.
struct Window {
    std::mutex lock;
    std::vector<uint8_t> mem = std::vector<uint8_t>(GRANULES * G);
    std::atomic<uint8_t> dropped[GRANULES] = {}, read_only[GRANULES] = {};
    uint32_t resident[GRANULES] = {};
    std::vector<uint8_t> file;            // the source's bytes (one source: every range decodes from `file` at its dest)
    std::atomic<int> sources{0}, released{0}, restores{0};
    std::atomic<int> protect_calls{0};
    bool source_ok = true;
    std::atomic<bool> slow_fill{false};

    static uint64_t gi(uint64_t a) { return (a - BASE) / G; }
    uint8_t* at(uint64_t a) { return mem.data() + (a - BASE); }

    // a reader as a guest thread is: a dropped granule faults into the owner, then reads on
    uint8_t read(Drop& d, uint64_t a) {
        for (;;) {
            {
                std::lock_guard<std::mutex> l(lock);
                if (!dropped[gi(a)]) return *at(a);
            }
            CHECK(d.touch(a, TouchPath::GuestRead, 0));
        }
    }
    // a writer: a read-only granule faults and waits for the decision; a dropped one is restored first
    void write(Drop& d, uint64_t a, uint8_t v) {
        for (;;) {
            {
                std::lock_guard<std::mutex> l(lock);
                if (!dropped[gi(a)] && !read_only[gi(a)]) { *at(a) = v; return; }
            }
            if (read_only[gi(a)]) {
                if (!d.wait_pending(a)) std::this_thread::yield();
                continue;
            }
            CHECK(d.touch(a, TouchPath::GuestWrite, 0));
        }
    }
};


Drop::Host host_of(Window& w) {
    Drop::Host h;
    h.ctx = &w;
    h.is_dropped = [](void* c, uint64_t a) -> int { return static_cast<Window*>(c)->dropped[Window::gi(a)]; };
    h.drop = [](void* c, uint64_t a, uint64_t n) -> int {
        auto* w = static_cast<Window*>(c);
        std::lock_guard<std::mutex> l(w->lock);
        for (uint64_t x = a; x < a + n; x += G) {
            std::memset(w->at(x), 0xCD, G);
            w->dropped[Window::gi(x)] = 1;
            w->read_only[Window::gi(x)] = 0;
        }
        return 0;
    };
    h.restore = [](void* c, uint64_t a, uint64_t n, int (*fill)(void*, uint8_t*, uint64_t, uint64_t), void* fc) -> int {
        auto* w = static_cast<Window*>(c);
        std::vector<uint8_t> buf(n);
        if (w->slow_fill) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (fill(fc, buf.data(), a, n)) return 1;
        std::lock_guard<std::mutex> l(w->lock);
        std::memcpy(w->at(a), buf.data(), n);
        for (uint64_t x = a; x < a + n; x += G) w->dropped[Window::gi(x)] = 0;
        w->restores++;
        return 0;
    };
    h.source = [](void* c, const DropSource& s) -> const uint8_t* {
        auto* w = static_cast<Window*>(c);
        if (!w->source_ok) return nullptr;
        w->sources++;
        auto* b = new uint8_t[s.size];
        std::memcpy(b, w->file.data() + (s.dest - BASE), s.size);
        return b;
    };
    h.release = [](void* c, const uint8_t* b) { static_cast<Window*>(c)->released++, delete[] b; };
    h.view = [](void* c, uint64_t a) -> const uint8_t* { return static_cast<Window*>(c)->at(a); };
    h.resident = [](void* c, uint64_t a) -> uint32_t { return static_cast<Window*>(c)->resident[Window::gi(a)]; };
    h.protect = [](void* c, uint64_t a, uint64_t n, int ro) -> int {
        auto* w = static_cast<Window*>(c);
        w->protect_calls++;
        for (uint64_t x = a; x < a + n; x += G) w->read_only[Window::gi(x)] = (uint8_t)ro;
        return 0;
    };
    return h;
}

Drop::Config config() {
    Drop::Config c;
    c.granule = G;
    c.age_ns = 30 * S;
    c.retry_ns = 10 * S;
    c.hold_ns = 60 * S;
    c.give_up = 3;
    c.kernel_ns = 5 * S;
    c.check = true;
    return c;
}

void fresh(Window& w) {
    w.file.resize(GRANULES * G);
    for (size_t i = 0; i < w.file.size(); i++) w.file[i] = (uint8_t)(i * 131 + (i >> 12) * 7 + 1);
    w.mem = w.file;   // (memory holds what the decodes wrote)
}

DropSource range(uint64_t first_granule, uint64_t granules, uint64_t lead = 100, uint64_t tail = 200) {
    DropSource s;
    s.dest = BASE + first_granule * G - lead;   // (not granule aligned: the two partial ends are not candidates)
    s.size = granules * G + lead + tail;
    s.name = "Tex/a.bntx";
    return s;
}

// A range is looked at only once old enough; its whole granules that are cold and unchanged are dropped; a touch
// restores them all, byte-exact, and the check's hashes agree.
void drop_and_restore() {
    Window w;
    fresh(w);
    Drop d(config(), host_of(w));
    d.decoded(range(2, 4), 0);   // whole granules 2..5
    w.resident[3] = 2;           // (in use: two pages in the working set)
    CHECK(d.pass(10 * S) == 0);  // (too young)
    CHECK(d.pass(30 * S) == 3);
    CHECK(w.dropped[2] && !w.dropped[3] && w.dropped[4] && w.dropped[5] && !w.dropped[1] && !w.dropped[6]);
    CHECK(d.stats().refused[int(Refuse::Resident)] == 1 && d.stats().dropped_now == 3);
    CHECK(!w.read_only[2] && !w.read_only[3]);
    w.resident[3] = 0;
    CHECK(d.pass(35 * S) == 0);   // (not before retry_ns)
    CHECK(d.pass(40 * S) == 1);   // (cold now)
    CHECK(d.stats().dropped_now == 4);
    for (uint64_t a = BASE + 2 * G; a < BASE + 6 * G; a += 97) CHECK(w.read(d, a) == w.file[a - BASE]);
    CHECK(w.restores == 1);   // (one run, one restore: the whole range came back at the first touch)
    CHECK(d.stats().restores == 1 && d.stats().restored_granules == 4 && d.stats().dropped_now == 0);
    CHECK(d.stats().checked == 4 && d.stats().mismatches == 0);
    CHECK(d.stats().by_path[int(TouchPath::GuestRead)] == 1);
    CHECK(w.mem == w.file);
    CHECK(w.sources == w.released);
}

// A granule written since its decode is never dropped; one imported never; one the kernel was given not for a while.
void refusals() {
    Window w;
    fresh(w);
    Drop d(config(), host_of(w));
    d.decoded(range(10, 6), 0);   // 10..15
    w.mem[(11 - 0) * G + 5] ^= 1;   // (written)
    d.mark_imported(BASE + 12 * G + 10, BASE + 12 * G + 20);
    d.kernel_use(BASE + 13 * G, BASE + 13 * G + 1, 26 * S);   // (until 31 s)
    CHECK(d.pass(30 * S) == 3);   // 10, 14, 15
    CHECK(d.stats().refused[int(Refuse::Changed)] == 1);
    CHECK(d.stats().refused[int(Refuse::Imported)] == 1);
    CHECK(d.stats().refused[int(Refuse::KernelUse)] == 1);
    CHECK(!w.read_only[11] && !w.read_only[13]);
    CHECK(d.pass(40 * S) == 1);   // 13 now; 11 never; 12 while it is imported
    CHECK(d.pass(50 * S) == 0 && d.pass(60 * S) == 0);
    CHECK(w.dropped[13] && !w.dropped[11] && !w.dropped[12]);
    CHECK(d.stats().refused[int(Refuse::Changed)] == 1 && d.stats().refused[int(Refuse::Imported)] == 4);
    // (the import given back: only whole granules leave the list, so a range cutting 12 keeps it; the whole of it frees it)
    d.unmark_imported(BASE + 12 * G + 10, BASE + 13 * G);
    CHECK(d.pass(70 * S) == 0);
    d.unmark_imported(BASE + 12 * G, BASE + 13 * G);
    CHECK(d.pass(80 * S) == 1 && w.dropped[12]);
    // (no source: refused for good)
    Window w2;
    fresh(w2);
    w2.source_ok = false;
    Drop d2(config(), host_of(w2));
    d2.decoded(range(1, 2), 0);
    CHECK(d2.pass(30 * S) == 0 && d2.stats().refused[int(Refuse::NoSource)] == 2);
    CHECK(d2.pass(100 * S) == 0 && d2.stats().refused[int(Refuse::NoSource)] == 2);
}

// Eight threads touch one dropped range at once while the restore is slow: each reads only the source's bytes, and the
// range is restored once.
void threads() {
    Window w;
    fresh(w);
    Drop d(config(), host_of(w));
    d.decoded(range(20, 8), 0);
    CHECK(d.pass(30 * S) == 8);
    w.slow_fill = true;
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; t++)
        ts.emplace_back([&, t] {
            for (uint64_t a = BASE + 20 * G + t * 501; a < BASE + 28 * G; a += 3001)
                if (w.read(d, a) != w.file[a - BASE]) bad++;
        });
    for (auto& t : ts) t.join();
    CHECK(bad == 0);
    CHECK(w.restores == 1 && d.stats().restores == 1);
    CHECK(d.stats().mismatches == 0);
}

// A writer that faults on a granule being decided waits, then writes on: after the decision the granule holds the
// source's bytes and the write, whichever way it went. The allocator reusing a dropped range: the old bytes come back
// and the new owner's write lands on them.
void writer_during_decision() {
    Window w;
    fresh(w);
    Drop::Host h = host_of(w);
    // (the writer starts as the pages are made read-only for the decision, and must wait for it)
    static std::thread* writer;
    static Drop* dp;
    static std::atomic<bool> started{false};
    h.protect = [](void* c, uint64_t a, uint64_t n, int ro) -> int {
        auto* w = static_cast<Window*>(c);
        for (uint64_t x = a; x < a + n; x += G) w->read_only[Window::gi(x)] = (uint8_t)ro;
        if (ro && Window::gi(a) == 31 && !started.exchange(true)) {
            writer = new std::thread([w] { w->write(*dp, BASE + 31 * G + 7, 0x5A); });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));   // (the writer is waiting by now)
        }
        return 0;
    };
    Drop d(config(), h);
    dp = &d;
    d.decoded(range(30, 3), 0);
    CHECK(d.pass(30 * S) == 3);
    writer->join();
    delete writer;
    CHECK(d.stats().waited >= 1);
    CHECK(w.read(d, BASE + 31 * G + 7) == 0x5A);
    std::vector<uint8_t> want = w.file;
    want[31 * G + 7] = 0x5A;
    for (uint64_t a = BASE + 30 * G; a < BASE + 33 * G; a++) CHECK(w.read(d, a) == want[a - BASE]);
    CHECK(d.stats().mismatches == 0);
}

// A restored range is held back (60 s, then 120 s) and given up after give_up restores.
void hold_and_give_up() {
    Window w;
    fresh(w);
    Drop d(config(), host_of(w));
    d.decoded(range(40, 2), 0);
    CHECK(d.pass(30 * S) == 2);
    CHECK(d.touch(BASE + 40 * G, TouchPath::HostRead, 31 * S));
    CHECK(!w.dropped[40] && !w.dropped[41]);
    CHECK(d.pass(80 * S) == 0);    // (held until 91 s)
    CHECK(d.pass(91 * S) == 2);
    CHECK(d.touch(BASE + 41 * G + 9, TouchPath::Kernel, 92 * S));
    CHECK(d.pass(200 * S) == 0);   // (held until 212 s)
    CHECK(d.pass(212 * S) == 2);
    CHECK(d.touch(BASE + 41 * G, TouchPath::HostWrite, 213 * S));
    CHECK(d.stats().refused[int(Refuse::GivenUp)] == 1);
    CHECK(d.pass(10000 * S) == 0);
    CHECK(d.stats().by_path[int(TouchPath::Kernel)] == 1 && d.stats().restores == 3);
    CHECK(!d.touch(BASE + 41 * G, TouchPath::HostRead, 0));   // (not dropped: not ours)
}

// A new decode over a candidate replaces it; granules of the old range still dropped keep their source. Forgetting an
// unmapped range, and granules the substrate no longer calls dropped.
void replace_and_forget() {
    Window w;
    fresh(w);
    Drop d(config(), host_of(w));
    d.decoded(range(50, 4), 0);   // 50..53
    CHECK(d.pass(30 * S) == 4);
    // (the allocator gave 52..53 to a new file: the decoder's write restored them first, then wrote)
    w.write(d, BASE + 52 * G, 0x11);
    DropSource s = range(52, 2, 0, 0);
    s.name = "Model/b.bfres";
    std::memcpy(w.file.data() + 52 * G, w.mem.data() + 52 * G, 2 * G);   // (the new file's bytes are what is there now)
    d.decoded(s, 40 * S);
    CHECK(d.stats().replaced == 1);
    // (the old range was restored whole by that write: dropped again only through the new range)
    CHECK(d.pass(80 * S) == 2);
    CHECK(w.dropped[52] && w.dropped[53] && !w.dropped[50]);
    CHECK(w.read(d, BASE + 52 * G) == 0x11);
    CHECK(d.pass(200 * S) == 2);
    d.forget(BASE + 52 * G, BASE + 54 * G);
    CHECK(d.stats().forgotten == 2 && d.stats().dropped_now == 0);
    w.dropped[52] = w.dropped[53] = 0;
    // (reconcile: a granule the substrate let go of behind our back)
    d.decoded(range(56, 1, 0, 0), 300 * S);
    CHECK(d.pass(330 * S) == 1);
    w.dropped[56] = 0;
    CHECK(d.reconcile() == 1 && d.stats().dropped_now == 0);
    char line[1024];
    CHECK(d.report(line, sizeof line, 340 * S) > 0);
    std::printf("%s\n", line);
}

}   // namespace

int main() {
    drop_and_restore();
    refusals();
    threads();
    writer_during_decision();
    hold_and_give_up();
    replace_and_forget();
    std::printf("drop_test: ok\n");
    return 0;
}
