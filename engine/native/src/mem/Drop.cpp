// HeyListen native engine: GPU data's system-memory copy given back for good (hl/mem/Drop.h).
#include "hl/mem/Drop.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "hl/stream/Pages.h"

namespace hl::mem {

Drop::Drop(const Config& config, const Host& host) : config_(config), host_(host) {
    if (!config_.granule || (config_.granule & (config_.granule - 1))) config_.granule = 65536;
}

void Drop::decoded(DropSource src, int64_t now) {
    if (!src.size || src.name.empty()) return;
    const uint64_t g = config_.granule;
    const uint64_t first = (src.dest + g - 1) & ~(g - 1), end = (src.dest + src.size) & ~(g - 1);
    std::lock_guard<std::mutex> t(table_lock_);
    // (older candidates this decode overlaps are replaced: their memory now holds this file. Their granules still
    // dropped keep their source and stay where by_granule_ finds them)
    auto it = by_dest_.upper_bound(src.dest);
    if (it != by_dest_.begin()) {
        auto prev = std::prev(it);
        if (prev->second->src.dest + prev->second->src.size > src.dest) it = prev;
    }
    while (it != by_dest_.end() && it->first < src.dest + src.size) {
        if (it->second->src.dest + it->second->src.size > src.dest) {
            it->second->candidate = false;
            stats_.replaced.fetch_add(1, std::memory_order_relaxed);
            it = by_dest_.erase(it);
        } else {
            ++it;
        }
    }
    if (end <= first) return;   // (no whole granule: nothing to drop)
    if (by_dest_.size() >= config_.max_ranges) evict_locked();
    auto* r = new Range;
    r->src = std::move(src);
    r->id = ++next_id_;
    r->next_ns = now + config_.age_ns;
    r->first = first;
    r->state.assign((end - first) / g, kCandidate);
    if (config_.check) r->hash.assign(r->state.size(), 0);
    by_dest_[r->src.dest] = r;
    all_[r->id] = r;
    stats_.ranges.fetch_add(1, std::memory_order_relaxed);
}

void Drop::evict_locked() {
    // (the oldest candidate without a dropped granule leaves the candidates: by id, the order of the decodes)
    Range* oldest = nullptr;
    for (auto& [dest, r] : by_dest_)
        if (!r->dropped && (!oldest || r->id < oldest->id)) oldest = r;
    if (!oldest) return;
    oldest->candidate = false;
    by_dest_.erase(oldest->src.dest);
}

bool Drop::imported(uint64_t granule) const {
    std::lock_guard<std::mutex> m(marks_lock_);
    auto it = std::upper_bound(imported_.begin(), imported_.end(), std::make_pair(granule, UINT64_MAX));
    if (it == imported_.begin()) return false;
    --it;
    return granule < it->second;
}

bool Drop::kernel_busy(uint64_t granule, int64_t now) const {
    std::lock_guard<std::mutex> m(marks_lock_);
    auto it = kernel_until_.find(granule);
    return it != kernel_until_.end() && now < it->second;
}

void Drop::mark_imported(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return;
    const uint64_t g = config_.granule;
    lo &= ~(g - 1), hi = (hi + g - 1) & ~(g - 1);
    {
        // (a decision under way over this range ends first: the import then sees the granule kept or dropped, and
        // the substrate restores a dropped one before the driver takes it)
        std::shared_lock<std::shared_mutex> p(pending_lock_);
    }
    std::lock_guard<std::mutex> m(marks_lock_);
    imported_.emplace_back(lo, hi);
    std::sort(imported_.begin(), imported_.end());
    size_t n = 0;
    for (size_t i = 0; i < imported_.size(); i++) {
        if (n && imported_[i].first <= imported_[n - 1].second)
            imported_[n - 1].second = std::max(imported_[n - 1].second, imported_[i].second);
        else
            imported_[n++] = imported_[i];
    }
    imported_.resize(n);
}

void Drop::kernel_use(uint64_t lo, uint64_t hi, int64_t now) {
    if (hi <= lo) return;
    const uint64_t g = config_.granule;
    std::shared_lock<std::shared_mutex> p(pending_lock_);   // (waits for a decision under way)
    std::lock_guard<std::mutex> m(marks_lock_);
    if (kernel_until_.size() > 65536)   // (old marks pruned now and then)
        for (auto it = kernel_until_.begin(); it != kernel_until_.end();) it = it->second <= now ? kernel_until_.erase(it) : std::next(it);
    for (uint64_t a = lo & ~(g - 1); a < hi; a += g) kernel_until_[a] = now + config_.kernel_ns;
}

bool Drop::wait_pending(uint64_t addr) {
    const uint64_t lo = pending_lo_.load(std::memory_order_acquire), hi = pending_hi_.load(std::memory_order_acquire);
    if (!(addr >= lo && addr < hi)) return false;
    stats_.waited.fetch_add(1, std::memory_order_relaxed);
    std::shared_lock<std::shared_mutex> p(pending_lock_);
    return true;
}

// one range's granules: the checks of the header in order, then the drop of those that pass (range lock held)
bool Drop::decide(Range& r, int64_t now, uint64_t& looked, uint64_t& dropped) {
    const uint64_t g = config_.granule;
    std::vector<uint32_t> cold;   // granule indexes that passed every check but the comparison
    bool waiting = false;          // some granule may pass later (in use now)
    for (uint32_t i = 0; i < r.state.size(); i++) {
        if (r.state[i] != kCandidate) continue;
        const uint64_t a = r.first + uint64_t(i) * g;
        if (imported(a)) { r.state[i] = kNever, refuse(Refuse::Imported); continue; }
        if (kernel_busy(a, now)) { refuse(Refuse::KernelUse), waiting = true; continue; }
        if (host_.plain && !host_.plain(host_.ctx, a)) { refuse(Refuse::NotPlain), waiting = true; continue; }
        if (config_.require_cold && host_.resident && host_.resident(host_.ctx, a)) {
            refuse(Refuse::Resident), waiting = true;
            continue;
        }
        cold.push_back(i);
    }
    r.next_ns = now + config_.retry_ns;
    if (cold.empty()) return waiting || r.dropped;   // (a range with granules dropped may drop them again after a restore)
    const uint8_t* src = host_.source(host_.ctx, r.src);
    if (!src) {
        stats_.source_failed.fetch_add(1, std::memory_order_relaxed);
        refuse(Refuse::NoSource, cold.size());
        for (uint32_t i : cold) r.state[i] = kNever;
        return false;
    }
    stats_.sources.fetch_add(1, std::memory_order_relaxed);
    const uint64_t lo = r.first + uint64_t(cold.front()) * g, hi = r.first + uint64_t(cold.back() + 1) * g;
    {
        // (the decision: writers of these granules fault on the read-only pages and wait in wait_pending; the kernel
        // and the driver wait in kernel_use / mark_imported, and their marks are looked at again here)
        std::unique_lock<std::shared_mutex> p(pending_lock_);
        pending_lo_.store(lo, std::memory_order_release);
        pending_hi_.store(hi, std::memory_order_release);
        for (uint32_t i : cold) {
            const uint64_t a = r.first + uint64_t(i) * g;
            looked += g;
            if (imported(a)) { r.state[i] = kNever, refuse(Refuse::Imported); continue; }
            if (kernel_busy(a, now)) { refuse(Refuse::KernelUse), waiting = true; continue; }
            if (host_.protect(host_.ctx, a, g, 1)) { refuse(Refuse::NotPlain), waiting = true; continue; }
            const uint8_t* now_bytes = host_.view(host_.ctx, a);
            const uint8_t* want = src + (a - r.src.dest);
            if (!now_bytes || std::memcmp(now_bytes, want, g) != 0) {
                // (written since the decode: it stays as it is, for good)
                host_.protect(host_.ctx, a, g, 0);
                r.state[i] = kNever, refuse(Refuse::Changed);
                continue;
            }
            const uint64_t h = config_.check ? hl::stream::page_hash(want, g) : 0;
            if (const int why = host_.drop(host_.ctx, a, g)) {
                host_.protect(host_.ctx, a, g, 0);
                // (-2: the substrate knows the back end imported it, which this module may not have been told yet)
                if (why == -2) r.state[i] = kNever, refuse(Refuse::Imported);
                else refuse(Refuse::Substrate), waiting = true;
                continue;
            }
            if (config_.check) r.hash[i] = h;
            r.state[i] = kDropped;
            r.dropped++;
            dropped++;
            {
                std::lock_guard<std::mutex> t(table_lock_);
                by_granule_[a] = &r;
            }
            stats_.dropped_granules.fetch_add(1, std::memory_order_relaxed);
            stats_.dropped_now.fetch_add(1, std::memory_order_relaxed);
        }
        pending_lo_.store(0, std::memory_order_release);
        pending_hi_.store(0, std::memory_order_release);
    }
    host_.release(host_.ctx, src);
    if (waiting) return true;
    // (granules dropped now become candidates again after a restore: the range stays while it has any)
    for (uint8_t s : r.state)
        if (s != kNever) return true;
    return false;
}

uint64_t Drop::pass(int64_t now) {
    stats_.passes.fetch_add(1, std::memory_order_relaxed);
    // the ranges due, from where the last pass stopped
    std::vector<std::pair<uint64_t, uint64_t>> due;   // (id, dest)
    std::vector<Range*> dead;
    {
        std::lock_guard<std::mutex> t(table_lock_);
        uint64_t bytes = 0;
        auto it = by_dest_.lower_bound(cursor_);
        for (size_t seen = 0; seen < by_dest_.size() && bytes < config_.pass_bytes; seen++, ++it) {
            if (it == by_dest_.end()) it = by_dest_.begin();
            Range* r = it->second;
            if (now < r->next_ns) continue;
            due.emplace_back(r->id, r->src.dest);
            bytes += r->state.size() * config_.granule;
            cursor_ = it->first + 1;
        }
        for (auto& [id, r] : all_)
            if (!r->candidate && !r->dropped) dead.push_back(r);
    }
    // ranges with nothing left to do are deleted (under their lock: a touch may hold one it found earlier)
    for (Range* r : dead) {
        std::lock_guard<std::mutex> l(range_lock(r->id));
        std::lock_guard<std::mutex> t(table_lock_);
        if (r->candidate || r->dropped) continue;
        if (by_dest_.count(r->src.dest) && by_dest_[r->src.dest] == r) by_dest_.erase(r->src.dest);
        all_.erase(r->id);
        delete r;
    }
    uint64_t looked = 0, dropped = 0;
    for (auto& [id, dest] : due) {
        if (looked >= config_.pass_bytes) break;
        std::lock_guard<std::mutex> l(range_lock(id));
        Range* r;
        {
            std::lock_guard<std::mutex> t(table_lock_);
            auto it = all_.find(id);
            if (it == all_.end() || !it->second->candidate) continue;
            r = it->second;
            if (r->restores >= config_.give_up) {
                r->candidate = false;
                by_dest_.erase(r->src.dest);
                continue;
            }
        }
        if (!decide(*r, now, looked, dropped)) {
            std::lock_guard<std::mutex> t(table_lock_);
            r->candidate = false;
            auto it = by_dest_.find(r->src.dest);
            if (it != by_dest_.end() && it->second == r) by_dest_.erase(it);
        }
    }
    return dropped;
}

namespace {
struct FillCtx {
    const uint8_t* src;
    uint64_t src_dest;
};
int fill_from_source(void* ctx, uint8_t* buf, uint64_t addr, uint64_t size) {
    auto* f = static_cast<FillCtx*>(ctx);
    std::memcpy(buf, f->src + (addr - f->src_dest), size);
    return 0;
}
}   // namespace

bool Drop::touch(uint64_t addr, TouchPath path, int64_t now) {
    const uint64_t g = config_.granule;
    const uint64_t granule = addr & ~(g - 1);
    uint64_t id;
    {
        std::lock_guard<std::mutex> t(table_lock_);
        auto it = by_granule_.find(granule);
        if (it == by_granule_.end()) return false;
        id = it->second->id;
    }
    std::lock_guard<std::mutex> l(range_lock(id));
    Range* r;
    {
        std::lock_guard<std::mutex> t(table_lock_);
        auto it = by_granule_.find(granule);
        if (it == by_granule_.end() || it->second->id != id) {
            // (restored by another thread meanwhile, or the granule went to another range: ours if it is not dropped)
            return host_.is_dropped(host_.ctx, granule) == 0;
        }
        r = it->second;
    }
    if (!host_.is_dropped(host_.ctx, granule)) return true;
    const uint8_t* src = host_.source(host_.ctx, r->src);
    if (!src) {
        stats_.restore_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    stats_.sources.fetch_add(1, std::memory_order_relaxed);
    // every dropped granule of the range, in runs of neighbours: one restore a run
    FillCtx f{src, r->src.dest};
    bool ok = true;
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->state.size();) {
        if (r->state[i] != kDropped) { i++; continue; }
        uint32_t e = i;
        while (e < r->state.size() && r->state[e] == kDropped) e++;
        const uint64_t a = r->first + uint64_t(i) * g, size = uint64_t(e - i) * g;
        if (host_.restore(host_.ctx, a, size, fill_from_source, &f)) {
            stats_.restore_failed.fetch_add(1, std::memory_order_relaxed);
            ok = false;
            i = e;
            continue;
        }
        for (uint32_t k = i; k < e; k++) {
            if (config_.check) {
                const uint8_t* got = host_.view(host_.ctx, r->first + uint64_t(k) * g);
                stats_.checked.fetch_add(1, std::memory_order_relaxed);
                if (!got || hl::stream::page_hash(got, g) != r->hash[k]) stats_.mismatches.fetch_add(1, std::memory_order_relaxed);
            }
            r->state[k] = kCandidate;
            r->dropped--;
            n++;
            std::lock_guard<std::mutex> t(table_lock_);
            by_granule_.erase(r->first + uint64_t(k) * g);
        }
        i = e;
    }
    host_.release(host_.ctx, src);
    stats_.dropped_now.fetch_sub(n, std::memory_order_relaxed);
    stats_.restored_granules.fetch_add(n, std::memory_order_relaxed);
    stats_.restores.fetch_add(1, std::memory_order_relaxed);
    stats_.by_path[int(path)].fetch_add(1, std::memory_order_relaxed);
    // (needed again: held back, longer each time, for good after give_up restores)
    r->restores++;
    {
        std::lock_guard<std::mutex> t(table_lock_);
        if (r->restores >= config_.give_up) {
            refuse(Refuse::GivenUp);
            r->candidate = false;
            auto it = by_dest_.find(r->src.dest);
            if (it != by_dest_.end() && it->second == r) by_dest_.erase(it);
        } else if (r->candidate) {
            r->next_ns = now + (config_.hold_ns << (r->restores - 1));
        }
    }
    return ok;
}

void Drop::forget(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return;
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> t(table_lock_);
        for (auto it = by_granule_.lower_bound(lo & ~(config_.granule - 1)); it != by_granule_.end() && it->first < hi; ++it)
            ids.push_back(it->second->id);
        auto it = by_dest_.upper_bound(lo);
        if (it != by_dest_.begin()) {
            auto prev = std::prev(it);
            if (prev->second->src.dest + prev->second->src.size > lo) it = prev;
        }
        while (it != by_dest_.end() && it->first < hi) {
            it->second->candidate = false;
            it = by_dest_.erase(it);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (uint64_t id : ids) {
        std::lock_guard<std::mutex> l(range_lock(id));
        std::lock_guard<std::mutex> t(table_lock_);
        auto f = all_.find(id);
        if (f == all_.end()) continue;
        Range* r = f->second;
        for (uint32_t k = 0; k < r->state.size(); k++) {
            const uint64_t a = r->first + uint64_t(k) * config_.granule;
            if (r->state[k] != kDropped || a < lo || a >= hi) continue;
            r->state[k] = kNever;
            r->dropped--;
            by_granule_.erase(a);
            stats_.dropped_now.fetch_sub(1, std::memory_order_relaxed);
            stats_.forgotten.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

uint64_t Drop::reconcile() {
    std::vector<uint64_t> gone;
    {
        std::lock_guard<std::mutex> t(table_lock_);
        for (auto& [a, r] : by_granule_)
            if (!host_.is_dropped(host_.ctx, a)) gone.push_back(a);
    }
    uint64_t n = 0;
    for (uint64_t a : gone) {
        uint64_t id;
        {
            std::lock_guard<std::mutex> t(table_lock_);
            auto it = by_granule_.find(a);
            if (it == by_granule_.end()) continue;
            id = it->second->id;
        }
        std::lock_guard<std::mutex> l(range_lock(id));
        if (host_.is_dropped(host_.ctx, a)) continue;   // (dropped again meanwhile: cannot be, the drop thread is us)
        std::lock_guard<std::mutex> t(table_lock_);
        auto it = by_granule_.find(a);
        if (it == by_granule_.end() || it->second->id != id) continue;
        Range* r = it->second;
        r->state[(a - r->first) / config_.granule] = kNever;
        r->dropped--;
        by_granule_.erase(it);
        stats_.dropped_now.fetch_sub(1, std::memory_order_relaxed);
        stats_.forgotten.fetch_add(1, std::memory_order_relaxed);
        n++;
    }
    return n;
}

int Drop::report(char* out, size_t cap, int64_t now) {
    if (!cap) return 0;
    const double mb = config_.granule / 1048576.0;
    const uint64_t dropped = stats_.dropped_granules.load(), restores = stats_.restores.load();
    const double secs = last_report_ns_ && now > last_report_ns_ ? (now - last_report_ns_) / 1e9 : 0;
    static const char* const kRefuse[] = {"imported", "kernel", "in use", "changed", "no source", "not plain",
                                          "substrate", "given up"};
    char refused[256];
    int k = 0;
    for (int i = 0; i < int(Refuse::kCount); i++)
        k += std::snprintf(refused + k, sizeof refused - k, "%s%s %llu", i ? ", " : "", kRefuse[i],
                           (unsigned long long)stats_.refused[i].load());
    const int n = std::snprintf(
        out, cap,
        "%.1f MB dropped now; dropped %.1f MB in all (%.1f MB since the last line); %llu restores of %.1f MB (%llu since "
        "the last line, %.1f a second): guest read %llu, guest write %llu, host read %llu, host write %llu, kernel %llu, import %llu; "
        "failed %llu; check: %llu granules hashed, %llu MISMATCHES; refused (granules): %s; ranges %llu (replaced "
        "%llu), sources decoded %llu (failed %llu), forgotten %llu, writers waited %llu",
        stats_.dropped_now.load() * mb, dropped * mb, (dropped - last_dropped_report_) * mb,
        (unsigned long long)restores, stats_.restored_granules.load() * mb,
        (unsigned long long)(restores - last_restores_report_), secs > 0 ? (restores - last_restores_report_) / secs : 0.0,
        (unsigned long long)stats_.by_path[0].load(), (unsigned long long)stats_.by_path[1].load(),
        (unsigned long long)stats_.by_path[2].load(), (unsigned long long)stats_.by_path[3].load(),
        (unsigned long long)stats_.by_path[4].load(), (unsigned long long)stats_.by_path[5].load(),
        (unsigned long long)stats_.restore_failed.load(),
        (unsigned long long)stats_.checked.load(), (unsigned long long)stats_.mismatches.load(), refused,
        (unsigned long long)stats_.ranges.load(), (unsigned long long)stats_.replaced.load(),
        (unsigned long long)stats_.sources.load(), (unsigned long long)stats_.source_failed.load(),
        (unsigned long long)stats_.forgotten.load(), (unsigned long long)stats_.waited.load());
    last_dropped_report_ = dropped, last_restores_report_ = restores, last_report_ns_ = now;
    return n < 0 ? 0 : n;
}

}   // namespace hl::mem
