// HeyListen native engine: the stream module's path choice and counters (hl/stream/Decode.h).
#include "hl/stream/Decode.h"

#include <cstdio>

namespace hl::stream {

const char* reason_name(Reason r) {
    switch (r) {
    case Reason::Native: return "native";
    case Reason::SizeStep: return "size-step";
    case Reason::PathOpen: return "path-open";
    case Reason::FileKind: return "file-kind";
    case Reason::NoReadBuffer: return "no-read-buffer";
    case Reason::NoHeap: return "no-heap";
    case Reason::Chunked: return "chunked";
    default: return "?";
    }
}

Reason choose(const Request& r) {
    if (r.size_step & 31) return Reason::SizeStep;
    if (!r.has_file) return Reason::PathOpen;
    if (!r.file_kind_ok) return Reason::FileKind;
    if (!r.has_read_buffer) return Reason::NoReadBuffer;
    if (!r.has_heap) return Reason::NoHeap;
    return Reason::Native;
}

void Counters::add(Reason r, uint64_t decoded_bytes) {
    const int i = static_cast<int>(r);
    if (i < 0 || i >= static_cast<int>(Reason::Count)) return;
    files[i].fetch_add(1, std::memory_order_relaxed);
    bytes[i].fetch_add(decoded_bytes, std::memory_order_relaxed);
}

int Counters::report(char* out, size_t cap) const {
    if (!out || !cap) return 0;
    uint64_t all_files = 0, all_bytes = 0;
    for (int i = 0; i < static_cast<int>(Reason::Count); i++) {
        all_files += files[i].load(std::memory_order_relaxed);
        all_bytes += bytes[i].load(std::memory_order_relaxed);
    }
    int n = std::snprintf(out, cap, "files %llu (%.1f MB)", static_cast<unsigned long long>(all_files), all_bytes / 1048576.0);
    for (int i = 0; i < static_cast<int>(Reason::Count) && n > 0 && static_cast<size_t>(n) < cap; i++) {
        const uint64_t f = files[i].load(std::memory_order_relaxed);
        if (!f) continue;
        n += std::snprintf(out + n, cap - n, ", %s %llu (%.1f MB)", reason_name(static_cast<Reason>(i)),
                           static_cast<unsigned long long>(f), bytes[i].load(std::memory_order_relaxed) / 1048576.0);
    }
    if (n > 0 && static_cast<size_t>(n) < cap && errors.load())
        n += std::snprintf(out + n, cap - n, ", error paths %llu", static_cast<unsigned long long>(errors.load()));
    if (n > 0 && static_cast<size_t>(n) < cap && (checked_files.load() || unchecked.load()))
        n += std::snprintf(out + n, cap - n, "; check: files %llu (%llu chunked), %.1f MB, mismatches %llu, not checked %llu",
                           static_cast<unsigned long long>(checked_files.load()),
                           static_cast<unsigned long long>(checked_chunked.load()), checked_bytes.load() / 1048576.0,
                           static_cast<unsigned long long>(mismatches.load()), static_cast<unsigned long long>(unchecked.load()));
    return n < 0 ? 0 : (static_cast<size_t>(n) >= cap ? static_cast<int>(cap) - 1 : n);
}

}   // namespace hl::stream
