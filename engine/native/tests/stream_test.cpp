// HeyListen native engine: the stream module's path choice, counters and provenance table, run with no game
// (tests/test_native_engine.py).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hl/c/hl_stream.h"
#include "hl/stream/Decode.h"
#include "hl/stream/Pages.h"
#include "hl/stream/Provenance.h"

using hl::stream::Decode;
using hl::stream::Provenance;
using hl::stream::Reason;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

Decode decode(uint64_t dest, uint64_t size, const char* name) {
    Decode d;
    d.dest = dest;
    d.size = size;
    d.compressed = size / 3;
    d.name = name;
    return d;
}

// A range is found by any address inside it and by none outside.
void lookup() {
    Provenance p(16);
    p.record(decode(0x1000, 0x800, "Tex/a.bntx"));
    p.record(decode(0x3000, 0x100, "Model/b.bfres"));
    Decode d;
    CHECK(p.find(0x1000, &d) && d.name == "Tex/a.bntx");
    CHECK(p.find(0x17ff, &d) && d.dest == 0x1000);
    CHECK(!p.find(0x1800, &d));
    CHECK(!p.find(0xfff, &d));
    CHECK(p.find(0x30ff, &d) && d.name == "Model/b.bfres");
    CHECK(!p.find(0x3100, &d));
    CHECK(p.stats().live == 2 && p.stats().live_bytes == 0x900);
}

// A new decode over a range that was freed and given again replaces every older range it overlaps.
void overlap() {
    Provenance p(16);
    p.record(decode(0x1000, 0x1000, "a"));
    p.record(decode(0x2000, 0x1000, "b"));
    p.record(decode(0x3000, 0x1000, "c"));
    p.record(decode(0x1800, 0x1000, "d"));   // (over the end of a and the start of b)
    Decode d;
    CHECK(!p.find(0x1000, &d));
    CHECK(p.find(0x1800, &d) && d.name == "d");
    CHECK(p.find(0x27ff, &d) && d.name == "d");
    CHECK(!p.find(0x2800, &d));
    CHECK(p.find(0x3000, &d) && d.name == "c");
    CHECK(p.stats().dropped_overlap == 2 && p.stats().live == 2);
    p.forget(0x3800, 0x3900);
    CHECK(!p.find(0x3000, &d));
    CHECK(p.stats().live == 1);
}

// Bounded: past its capacity the oldest range goes.
void bounded() {
    Provenance p(3);
    for (uint64_t i = 0; i < 5; i++) p.record(decode(0x10000 * (i + 1), 0x100, "x"));
    Decode d;
    CHECK(!p.find(0x10000, &d) && !p.find(0x20000, &d));
    CHECK(p.find(0x30000, &d) && p.find(0x40000, &d) && p.find(0x50000, &d));
    CHECK(p.stats().live == 3 && p.stats().dropped_old == 2 && p.stats().recorded == 5);
    p.record(decode(0x60000, 0, "empty"));   // (nothing decoded: not kept)
    CHECK(p.stats().recorded == 5);
}

// The path choice: every rare path goes to the translated function, decided before anything is done.
void choose() {
    hl::stream::Request r;
    r.size_step = 0;
    r.has_file = r.file_kind_ok = r.has_read_buffer = r.has_heap = true;
    CHECK(hl::stream::choose(r) == Reason::Native);
    r.size_step = 64;
    CHECK(hl::stream::choose(r) == Reason::Native);
    r.size_step = 48;
    CHECK(hl::stream::choose(r) == Reason::SizeStep);
    r.size_step = 0;
    r.has_file = false;
    CHECK(hl::stream::choose(r) == Reason::PathOpen);
    r.has_file = true;
    r.file_kind_ok = false;
    CHECK(hl::stream::choose(r) == Reason::FileKind);
    r.file_kind_ok = true;
    r.has_read_buffer = false;
    CHECK(hl::stream::choose(r) == Reason::NoReadBuffer);
    r.has_read_buffer = true;
    r.has_heap = false;
    CHECK(hl::stream::choose(r) == Reason::NoHeap);
}

// The C surface the OS layer uses: counters, the check's counts, provenance and the report line.
void c_surface() {
    CHECK(hl_stream_start(64) == 1);
    hl_stream_request q = {0, 1, 1, 1, 1};
    CHECK(hl_stream_choose(&q) == HL_STREAM_NATIVE);
    hl_stream_count(HL_STREAM_NATIVE, 3 << 20);
    hl_stream_count(HL_STREAM_CHUNKED, 1 << 20);
    hl_stream_checked(1, 0, 4096, 0);
    hl_stream_checked(1, 1, 4096, 1);
    hl_stream_checked(0, 0, 0, 0);
    const char bytes[] = "decoded bytes";
    hl_stream_decode d = {0x5000, 0x2000, 0x900, 0x77, hl_stream_fingerprint(bytes, sizeof bytes, 1), 3, 0, "Pack/c.pack"};
    hl_stream_record(&d);
    hl_stream_decode o;
    char name[32];
    CHECK(hl_stream_find(0x6fff, &o, name, sizeof name) && o.dest == 0x5000 && o.dict_id == 3 && !std::strcmp(name, "Pack/c.pack"));
    CHECK(o.fingerprint == hl_stream_fingerprint(bytes, sizeof bytes, 1) && o.fingerprint != hl_stream_fingerprint(bytes, 4, 1));
    char line[512];
    CHECK(hl_stream_report(line, sizeof line) > 0);
    CHECK(std::strstr(line, "native 1 (3.0 MB)") && std::strstr(line, "chunked 1 (1.0 MB)"));
    CHECK(std::strstr(line, "mismatches 1") && std::strstr(line, "not checked 1") && std::strstr(line, "1 live"));
    char tiny[8];
    CHECK(hl_stream_report(tiny, sizeof tiny) == 7);
    hl_stream_forget(0x5000, 0x5001);
    CHECK(!hl_stream_find(0x5000, &o, name, sizeof name));
}

// The page hash: one changed word anywhere changes it; equal bytes give equal hashes.
void page_hash() {
    std::vector<uint8_t> a(4096);
    for (size_t i = 0; i < a.size(); i++) a[i] = static_cast<uint8_t>(i * 7);
    const uint64_t h = hl::stream::page_hash(a.data(), a.size());
    std::vector<uint8_t> b = a;
    CHECK(hl::stream::page_hash(b.data(), b.size()) == h);
    for (size_t at : {size_t(0), size_t(8), size_t(4088), size_t(2049)}) {
        b = a;
        b[at] ^= 1;
        CHECK(hl::stream::page_hash(b.data(), b.size()) != h);
    }
    CHECK(hl::stream::file_kind("Tex/a.bntx") == "Tex/*.bntx");
    CHECK(hl::stream::file_kind("Model/b.anim.bfres") == "Model/*.bfres");
    CHECK(hl::stream::file_kind("c.pack") == "*.pack");
    CHECK(hl::stream::file_kind("Dir.x/noext") == "Dir.x/*");
    CHECK(hl::stream::file_kind("") == "(no name)");
}

// The two checks: whole pages only, a page changed after the decode counted apart, a page gone counted as not
// readable, a range replaced by a new decode before its second check counted as replaced.
std::vector<uint8_t> g_mem;
const uint8_t* view(uint64_t addr, size_t n, void*) {
    if (addr >= 0x30000 && addr < 0x31000) return nullptr;   // (a page decommitted)
    return addr + n <= g_mem.size() ? g_mem.data() + addr : nullptr;
}

void pages() {
    using hl::stream::Pages;
    g_mem.assign(0x80000, 0);
    for (size_t i = 0; i < g_mem.size(); i++) g_mem[i] = static_cast<uint8_t>(i * 13 + (i >> 12));
    Pages::Config c;
    c.first_ns = 30, c.second_ns = 60;
    Pages p(c);
    // [0x10800, 0x15800): whole pages 0x11000..0x15000 (4)
    CHECK(p.track(0x10800, g_mem.data() + 0x10800, 0x5000, "Tex/a.bntx", 0));
    CHECK(p.track(0x30000, g_mem.data() + 0x30000, 0x4000, "Tex/b.bntx", 0));
    CHECK(p.track(0x50000, g_mem.data() + 0x50000, 0x2000, "Model/c.bfres", 0));
    CHECK(!p.track(0x70010, g_mem.data() + 0x70010, 0x100, "Model/d.bfres", 0));   // (no whole page)
    g_mem[0x12000 + 40] ^= 0xff;                                                     // (patched in place)
    std::vector<Pages::Checked> got;
    CHECK(p.check(29, view, nullptr, 1000, &got) == 0);
    CHECK(p.check(30, view, nullptr, 1000, &got) == 3 && got.size() == 3);
    for (const auto& k : got) {
        CHECK(k.stage == 1);
        if (k.dest == 0x10800) {
            CHECK(k.first_page == 0x11000 && k.equal.size() == 4);
            CHECK(k.equal[0] == 1 && k.equal[1] == 0 && k.equal[2] == 1 && k.equal[3] == 1);
        }
        if (k.dest == 0x30000) CHECK(k.equal[0] == 2 && k.equal[1] == 1);
    }
    CHECK(p.track(0x50000, g_mem.data() + 0x50000, 0x1000, "Model/e.bfres", 40));   // (c replaced)
    got.clear();
    CHECK(p.check(60, view, nullptr, 1000, &got) == 2);
    CHECK(p.check(100, view, nullptr, 1000, &got) == 1);   // (e's two checks are later: 70 then 100)
    const auto s = p.stats();
    uint64_t replaced = 0, equal60 = 0, checked60 = 0, gone30 = 0;
    for (const auto& k : s) {
        replaced += k.replaced;
        equal60 += k.equal[1];
        checked60 += k.checked[1];
        gone30 += k.unreadable[0];
        if (k.kind == "Tex/*.bntx") CHECK(k.ranges == 2 && k.pages == 8 && k.equal[0] == 6);
    }
    CHECK(replaced == 1 && checked60 == 2 && equal60 == 6 && gone30 == 1);
}

// The C surface of the measurement: a range tracked, checked, given to the callback, and the table's lines.
size_t g_seen;
void seen(void*, uint64_t dest, uint64_t, uint64_t first_page, size_t pages, const uint64_t* hashes, const uint8_t* equal,
          int stage, const char* name) {
    CHECK(dest == 0x20000 && first_page == 0x20000 && pages == 2 && stage == 1 && !std::strcmp(name, "Pack/p.pack"));
    CHECK(equal[0] == 1 && equal[1] == 1 && hashes[0] == hl_stream_page_hash(g_mem.data() + 0x20000, 4096));
    g_seen++;
}

void pages_c() {
    CHECK(hl_stream_pages_start(10, 20) == 1);
    hl_stream_pages_track(0x20000, g_mem.data() + 0x20000, 0x2000, "Pack/p.pack", 0);
    CHECK(hl_stream_pages_check(10, view, nullptr, 100, seen, nullptr) == 1 && g_seen == 1);
    char out[2048];
    CHECK(hl_stream_pages_report(out, sizeof out) > 0 && std::strstr(out, "Pack/*.pack"));
}

}   // namespace

int main() {
    lookup();
    overlap();
    bounded();
    choose();
    c_surface();
    page_hash();
    pages();
    pages_c();
    std::puts("stream_test: ok");
    return 0;
}
