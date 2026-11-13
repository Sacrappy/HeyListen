// HeyListen native engine: the stream module's path choice, counters and provenance table, run with no game
// (tests/test_native_engine.py).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hl/c/hl_stream.h"
#include "hl/stream/Decode.h"
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

}   // namespace

int main() {
    lookup();
    overlap();
    bounded();
    choose();
    c_surface();
    std::puts("stream_test: ok");
    return 0;
}
