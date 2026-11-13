// HeyListen native engine: the C surface (hl/c/hl_stream.h) over the stream module.
#include "hl/c/hl_stream.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#include "hl/stream/Decode.h"
#include "hl/stream/Provenance.h"

namespace {
// (made once and never destroyed: loading threads use them until the process ends)
std::atomic<hl::stream::Provenance*> g_prov{nullptr};
hl::stream::Counters g_counters;
}   // namespace

extern "C" int hl_stream_start(uint32_t capacity) {
    if (g_prov.load()) return 1;
    auto* p = new hl::stream::Provenance(capacity);
    hl::stream::Provenance* none = nullptr;
    if (!g_prov.compare_exchange_strong(none, p)) delete p;
    return 1;
}

extern "C" int hl_stream_choose(const hl_stream_request* r) {
    if (!r) return HL_STREAM_PATH_OPEN;
    hl::stream::Request q;
    q.size_step = r->size_step;
    q.has_file = r->has_file != 0;
    q.file_kind_ok = r->file_kind_ok != 0;
    q.has_read_buffer = r->has_read_buffer != 0;
    q.has_heap = r->has_heap != 0;
    return static_cast<int>(hl::stream::choose(q));
}

extern "C" void hl_stream_count(int reason, uint64_t bytes) { g_counters.add(static_cast<hl::stream::Reason>(reason), bytes); }
extern "C" void hl_stream_error(void) { g_counters.errors.fetch_add(1, std::memory_order_relaxed); }

extern "C" void hl_stream_checked(int checked, int chunked, uint64_t bytes, int mismatch) {
    if (!checked) {
        g_counters.unchecked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_counters.checked_files.fetch_add(1, std::memory_order_relaxed);
    g_counters.checked_bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (chunked) g_counters.checked_chunked.fetch_add(1, std::memory_order_relaxed);
    if (mismatch) g_counters.mismatches.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void hl_stream_record(const hl_stream_decode* d) {
    auto* p = g_prov.load();
    if (!p || !d) return;
    hl::stream::Decode e;
    e.dest = d->dest;
    e.size = d->size;
    e.compressed = d->compressed;
    e.file = d->file;
    e.dict_id = d->dict_id;
    e.chunked = d->chunked != 0;
    e.fingerprint = d->fingerprint;
    if (d->name) e.name = d->name;
    p->record(std::move(e));
}

extern "C" int hl_stream_find(uint64_t addr, hl_stream_decode* out, char* name, size_t cap) {
    auto* p = g_prov.load();
    hl::stream::Decode e;
    if (!p || !p->find(addr, &e)) return 0;
    if (out) {
        out->dest = e.dest;
        out->size = e.size;
        out->compressed = e.compressed;
        out->file = e.file;
        out->dict_id = e.dict_id;
        out->chunked = e.chunked;
        out->fingerprint = e.fingerprint;
        out->name = nullptr;
    }
    if (name && cap) std::snprintf(name, cap, "%s", e.name.c_str());
    return 1;
}

extern "C" void hl_stream_forget(uint64_t lo, uint64_t hi) {
    if (auto* p = g_prov.load()) p->forget(lo, hi);
}

extern "C" uint64_t hl_stream_fingerprint(const void* data, size_t n, uint64_t seed) {
    return hl::stream::fingerprint(data, n, seed);
}

extern "C" int hl_stream_report(char* out, size_t cap) {
    int n = g_counters.report(out, cap);
    auto* p = g_prov.load();
    if (p && n >= 0 && static_cast<size_t>(n) < cap) {
        const auto s = p->stats();
        n += std::snprintf(out + n, cap - n, "; provenance: %llu recorded, %llu live (%.1f MB), dropped %llu overlapped, %llu old",
                           static_cast<unsigned long long>(s.recorded), static_cast<unsigned long long>(s.live),
                           s.live_bytes / 1048576.0, static_cast<unsigned long long>(s.dropped_overlap),
                           static_cast<unsigned long long>(s.dropped_old));
        if (static_cast<size_t>(n) >= cap) n = static_cast<int>(cap) - 1;
    }
    return n;
}
