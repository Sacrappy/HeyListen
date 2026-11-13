// HeyListen native engine: the decoded pages that still hold the decoder's bytes (hl/stream/Pages.h).
#include "hl/stream/Pages.h"

#include <cstring>
#include <iterator>
#include <utility>

namespace hl::stream {

uint64_t page_hash(const void* data, size_t n) {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ULL;
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h[4] = {k, k ^ 1, k ^ 2, k ^ 3};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; l++) {
            uint64_t w;
            std::memcpy(&w, p + i + 8 * l, 8);
            h[l] = (h[l] ^ w) * k;   // (a bijection of h for a given w: a changed word gives a changed lane)
            h[l] ^= h[l] >> 29;
        }
    uint64_t r = n;
    for (int l = 0; l < 4; l++) r = (r ^ h[l]) * k, r ^= r >> 31;
    for (; i < n; i++) r = (r ^ p[i]) * k;
    return r ^ (r >> 33);
}

std::string file_kind(const std::string& name) {
    if (name.empty()) return "(no name)";
    const size_t slash = name.find('/');
    const size_t base = name.rfind('/');
    const size_t dot = name.rfind('.');
    const std::string folder = slash == std::string::npos ? "" : name.substr(0, slash + 1);
    const std::string ext = dot == std::string::npos || (base != std::string::npos && dot < base) ? "" : name.substr(dot);
    return folder + "*" + ext;
}

size_t Pages::kind_of(const std::string& name) {
    const std::string k = file_kind(name);
    for (size_t i = 0; i < kinds_.size(); i++)
        if (kinds_[i].kind == k) return i;
    if (kinds_.size() + 1 >= cfg_.max_kinds) {
        for (size_t i = 0; i < kinds_.size(); i++)
            if (kinds_[i].kind == "(other)") return i;
        kinds_.push_back(KindStats{});
        kinds_.back().kind = "(other)";
        return kinds_.size() - 1;
    }
    kinds_.push_back(KindStats{});
    kinds_.back().kind = k;
    return kinds_.size() - 1;
}

void Pages::drop_overlapping(uint64_t lo, uint64_t hi) {
    auto it = by_dest_.lower_bound(lo);
    if (it != by_dest_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second.size > lo) it = prev;
    }
    while (it != by_dest_.end() && it->first < hi) {
        if (it->first + it->second.size > lo) {
            kinds_[it->second.kind].replaced++;
            held_pages_ -= it->second.hashes.size();
            it = by_dest_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Pages::track(uint64_t dest, const uint8_t* bytes, uint64_t size, const std::string& name, int64_t now) {
    const uint64_t first = (dest + kPage - 1) & ~(kPage - 1), end = (dest + size) & ~(kPage - 1);
    const size_t n = end > first ? static_cast<size_t>((end - first) / kPage) : 0;
    Entry e;
    e.size = size, e.first_page = first, e.t0 = now, e.name = name;
    if (n) {
        e.hashes.resize(n);
        for (size_t i = 0; i < n; i++) e.hashes[i] = page_hash(bytes + (first - dest) + i * kPage, kPage);
    }
    std::lock_guard<std::mutex> g(lock_);
    drop_overlapping(dest, dest + size);
    e.kind = kind_of(name);
    KindStats& k = kinds_[e.kind];
    if (!n || held_pages_ + n > cfg_.max_pages) {
        if (n) not_tracked_++;
        return false;
    }
    k.ranges++, k.bytes += size, k.pages += n;
    held_pages_ += n;
    by_dest_[dest] = std::move(e);
    return true;
}

size_t Pages::check(int64_t now, View view, void* ctx, size_t budget_pages, std::vector<Checked>* out) {
    std::vector<std::pair<uint64_t, Entry>> due;
    {
        std::lock_guard<std::mutex> g(lock_);
        size_t pages = 0;
        for (auto it = by_dest_.begin(); it != by_dest_.end() && pages < budget_pages;) {
            const Entry& e = it->second;
            const int64_t at = e.t0 + (e.stage == 0 ? cfg_.first_ns : cfg_.second_ns);
            if (now < at) { ++it; continue; }
            pages += e.hashes.size();
            due.emplace_back(it->first, std::move(it->second));
            it = by_dest_.erase(it);
        }
    }
    for (auto& [dest, e] : due) {
        Checked c;
        c.dest = dest, c.size = e.size, c.first_page = e.first_page, c.name = e.name, c.stage = e.stage + 1;
        c.equal.resize(e.hashes.size());
        for (size_t i = 0; i < e.hashes.size(); i++) {
            const uint8_t* p = view(e.first_page + i * kPage, kPage, ctx);
            c.equal[i] = !p ? 2 : page_hash(p, kPage) == e.hashes[i] ? 1 : 0;
        }
        std::lock_guard<std::mutex> g(lock_);
        KindStats& k = kinds_[e.kind];
        const int s = e.stage;
        k.checked[s]++;
        k.pages_checked[s] += e.hashes.size();
        for (uint8_t v : c.equal) k.equal[s] += v == 1, k.unreadable[s] += v == 2;
        e.stage++;
        // (kept for its second check unless a decode took its place while it was being checked)
        auto next = by_dest_.lower_bound(dest);
        const bool taken = (next != by_dest_.end() && next->first < dest + e.size) ||
                           (next != by_dest_.begin() && std::prev(next)->first + std::prev(next)->second.size > dest);
        if (e.stage == 1 && !taken) {
            if (out) c.hashes = e.hashes, out->push_back(std::move(c));
            by_dest_[dest] = std::move(e);
            continue;
        }
        if (e.stage == 1) k.replaced++;
        held_pages_ -= e.hashes.size();
        if (out) c.hashes = std::move(e.hashes), out->push_back(std::move(c));
    }
    return due.size();
}

std::vector<Pages::KindStats> Pages::stats() const {
    std::lock_guard<std::mutex> g(lock_);
    return kinds_;
}

uint64_t Pages::not_tracked() const {
    std::lock_guard<std::mutex> g(lock_);
    return not_tracked_;
}

}   // namespace hl::stream
