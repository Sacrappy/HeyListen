// HeyListen native engine: the cold-page policy (hl/mem/ColdPages.h).
#include "hl/mem/ColdPages.h"

#include <cstdio>
#include <cstdlib>

namespace hl::mem {

namespace {
uint32_t load(uint32_t* w) { return std::atomic_ref<uint32_t>(*w).load(std::memory_order_relaxed); }
void store(uint32_t* w, uint32_t v) { std::atomic_ref<uint32_t>(*w).store(v, std::memory_order_relaxed); }
}   // namespace

ColdPages::ColdPages(const Config& config) : config_(config) {
    if (config_.unit_pages == 0 || config_.unit_pages > kOutMask || config_.units == 0) return;
    if (config_.give_up > kLevelMask) config_.give_up = kLevelMask;
    // (calloc: fresh zero pages from the system, so a 32 MB table over a 512 GB space costs RAM only where units are seen)
    state_ = static_cast<uint32_t*>(std::calloc(config_.units, sizeof(uint32_t)));
    imported_ = static_cast<uint64_t*>(std::calloc((config_.units + 63) / 64, sizeof(uint64_t)));
}

ColdPages::~ColdPages() {
    std::free(state_);
    std::free(imported_);
}

void ColdPages::begin_pass() {
    if (pass_ < kHoldMax) pass_++;
    budget_ = config_.budget_pages;
    stats_.passes.fetch_add(1, std::memory_order_relaxed);
}

uint32_t ColdPages::hold_after(uint32_t level) const {
    const uint64_t wait = (uint64_t)config_.backoff_passes << (level > 1 ? level - 1 : 0);
    const uint64_t until = pass_ + wait;
    return until > kHoldMax ? kHoldMax : (uint32_t)until;
}

bool ColdPages::look(uint64_t unit, uint32_t resident) {
    if (!ok() || unit >= config_.units) return false;
    uint32_t* w = &state_[unit];
    const uint32_t s = load(w);
    const uint32_t out = out_of(s);
    uint32_t level = level_of(s);
    if (out) {
        if (!resident) return false;   // (still out)
        // (came back: someone needed these pages. Left in for a while, longer each time, for good after give_up times)
        stats_.back_pages.fetch_add(resident < out ? resident : out, std::memory_order_relaxed);
        stats_.back_units.fetch_add(1, std::memory_order_relaxed);
        stats_.out_pages.fetch_sub(out, std::memory_order_relaxed);
        if (level < kLevelMask) level++;
        if (level == config_.give_up) stats_.given_up_units.fetch_add(1, std::memory_order_relaxed);
        store(w, make(0, level, hold_after(level)));
        return false;
    }
    if (!resident || imported(unit) || level >= config_.give_up || pass_ < hold_of(s) || budget_ < (int64_t)resident)
        return false;
    return true;
}

void ColdPages::taken_out(uint64_t unit, uint32_t left, uint32_t stayed) {
    if (!ok() || unit >= config_.units) return;
    uint32_t* w = &state_[unit];
    const uint32_t s = load(w);
    budget_ -= (int64_t)left + stayed;
    if (stayed) stats_.stayed_pages.fetch_add(stayed, std::memory_order_relaxed);
    if (!left) {   // (nothing left: not tried again before a hold has passed)
        store(w, make(0, level_of(s), hold_after(1)));
        return;
    }
    if (left > kOutMask) left = kOutMask;
    store(w, make(out_of(s) + left > kOutMask ? kOutMask : out_of(s) + left, level_of(s), hold_of(s)));
    stats_.taken_pages.fetch_add(left, std::memory_order_relaxed);
    stats_.taken_units.fetch_add(1, std::memory_order_relaxed);
    stats_.out_pages.fetch_add(left, std::memory_order_relaxed);
}

void ColdPages::forget(uint64_t lo, uint64_t hi) {
    if (!ok()) return;
    if (hi > config_.units) hi = config_.units;
    for (uint64_t u = lo; u < hi; u++) {
        const uint32_t s = std::atomic_ref<uint32_t>(state_[u]).exchange(0, std::memory_order_relaxed);
        if (out_of(s)) stats_.out_pages.fetch_sub(out_of(s), std::memory_order_relaxed);
    }
}

void ColdPages::mark_imported(uint64_t lo, uint64_t hi) {
    if (!ok()) return;
    if (hi > config_.units) hi = config_.units;
    for (uint64_t u = lo; u < hi; u++) {
        const uint64_t bit = 1ull << (u & 63);
        if (!(std::atomic_ref<uint64_t>(imported_[u >> 6]).fetch_or(bit, std::memory_order_relaxed) & bit))
            stats_.imported_units.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ColdPages::imported(uint64_t unit) const {
    if (!ok() || unit >= config_.units) return false;
    return std::atomic_ref<uint64_t>(imported_[unit >> 6]).load(std::memory_order_relaxed) >> (unit & 63) & 1;
}

int ColdPages::report(char* out, size_t cap) {
    if (!cap) return 0;
    const double mb = config_.page_bytes / 1048576.0;
    const uint64_t taken = stats_.taken_pages.load(), back_pages = stats_.back_pages.load(), back_units = stats_.back_units.load();
    const int64_t now_out = stats_.out_pages.load();
    const int n = std::snprintf(
        out, cap,
        "%.0f MB out of the working set now; taken out %.0f MB in all (%.0f MB since the last line); came back %.1f MB in "
        "%llu units in all (%.1f MB in %llu units since the last line); %.1f MB stayed in when taken out; %llu units left "
        "in for good, %llu imported; %llu passes",
        now_out * mb, (double)taken * mb, (double)(taken - last_taken_) * mb, (double)back_pages * mb,
        (unsigned long long)back_units, (double)(back_pages - last_back_pages_) * mb,
        (unsigned long long)(back_units - last_back_units_), (double)stats_.stayed_pages.load() * mb,
        (unsigned long long)stats_.given_up_units.load(), (unsigned long long)stats_.imported_units.load(),
        (unsigned long long)stats_.passes.load());
    last_taken_ = taken, last_back_pages_ = back_pages, last_back_units_ = back_units;
    return n < 0 ? 0 : n;
}

}   // namespace hl::mem
