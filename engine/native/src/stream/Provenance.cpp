// HeyListen native engine: the provenance table of decompressed resources (hl/stream/Provenance.h).
#include "hl/stream/Provenance.h"

#include <iterator>
#include <utility>

namespace hl::stream {

void Provenance::drop_overlapping(uint64_t lo, uint64_t hi, uint64_t* counter) {
    if (hi <= lo) return;
    auto it = by_dest_.upper_bound(lo);
    if (it != by_dest_.begin()) {
        auto prev = std::prev(it);
        if (prev->second.d.dest + prev->second.d.size > lo) it = prev;
    }
    while (it != by_dest_.end() && it->first < hi) {
        const Entry& e = it->second;
        if (e.d.dest + e.d.size > lo) {
            by_age_.erase(e.seq);
            stats_.live_bytes -= e.d.size;
            stats_.live--;
            if (counter) (*counter)++;
            it = by_dest_.erase(it);
        } else {
            ++it;
        }
    }
}

void Provenance::record(Decode d) {
    if (!d.size) return;
    std::lock_guard<std::mutex> g(lock_);
    drop_overlapping(d.dest, d.dest + d.size, &stats_.dropped_overlap);
    while (by_dest_.size() >= capacity_ && !by_age_.empty()) {
        auto oldest = by_age_.begin();
        auto e = by_dest_.find(oldest->second);
        if (e != by_dest_.end()) {
            stats_.live_bytes -= e->second.d.size;
            stats_.live--;
            by_dest_.erase(e);
        }
        by_age_.erase(oldest);
        stats_.dropped_old++;
    }
    const uint64_t seq = ++seq_;
    const uint64_t dest = d.dest, size = d.size;
    by_dest_[dest] = Entry{std::move(d), seq};
    by_age_[seq] = dest;
    stats_.recorded++;
    stats_.live++;
    stats_.live_bytes += size;
}

bool Provenance::find(uint64_t addr, Decode* out) const {
    std::lock_guard<std::mutex> g(lock_);
    auto it = by_dest_.upper_bound(addr);
    if (it == by_dest_.begin()) return false;
    --it;
    const Decode& d = it->second.d;
    if (addr >= d.dest + d.size) return false;
    if (out) *out = d;
    return true;
}

void Provenance::forget(uint64_t lo, uint64_t hi) {
    std::lock_guard<std::mutex> g(lock_);
    drop_overlapping(lo, hi, nullptr);
}

Provenance::Stats Provenance::stats() const {
    std::lock_guard<std::mutex> g(lock_);
    return stats_;
}

uint64_t fingerprint(const void* data, size_t n, uint64_t seed) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

}   // namespace hl::stream
