// HeyListen native engine: the C surface (hl/c/hl_drop.h) over the drop policy (hl/mem/Drop.h).
#include "hl/c/hl_drop.h"

#include <atomic>

#include "hl/mem/Drop.h"

namespace {
// (made once and never destroyed: the drop thread and the substrate's hook use it until the process ends)
std::atomic<hl::mem::Drop*> g_drop{nullptr};
hl_drop_host g_host;

const uint8_t* source_of(void* ctx, const hl::mem::DropSource& s) {
    (void)ctx;
    return g_host.source(g_host.ctx, s.name.c_str(), s.dict_id, s.size, s.compressed);
}
}   // namespace

extern "C" int hl_drop_start(const hl_drop_config* c, const hl_drop_host* h) {
    if (g_drop.load()) return 1;
    if (!c || !h || !h->is_dropped || !h->drop || !h->restore || !h->source || !h->release || !h->view || !h->protect)
        return 0;
    g_host = *h;
    hl::mem::Drop::Config config;
    config.granule = c->granule;
    config.age_ns = c->age_ns;
    config.retry_ns = c->retry_ns;
    config.hold_ns = c->hold_ns;
    config.kernel_ns = c->kernel_ns;
    config.give_up = c->give_up;
    config.pass_bytes = c->pass_bytes;
    config.require_cold = c->require_cold != 0;
    config.check = c->check != 0;
    hl::mem::Drop::Host host;
    host.ctx = h->ctx;
    host.is_dropped = h->is_dropped;
    host.drop = h->drop;
    host.restore = h->restore;
    host.source = source_of;
    host.release = h->release;
    host.view = h->view;
    host.resident = h->resident;
    host.protect = h->protect;
    host.plain = h->plain;
    g_drop.store(new hl::mem::Drop(config, host));
    return 1;
}

extern "C" int hl_drop_ready(void) { return g_drop.load() != nullptr; }

extern "C" void hl_drop_decoded(uint64_t dest, uint64_t size, uint64_t compressed, uint32_t dict_id, const char* name,
                                int64_t now_ns) {
    auto* d = g_drop.load();
    if (!d) return;
    hl::mem::DropSource s;
    s.dest = dest, s.size = size, s.compressed = compressed, s.dict_id = dict_id;
    s.name = name ? name : "";
    d->decoded(std::move(s), now_ns);
}

extern "C" uint64_t hl_drop_pass(int64_t now_ns) {
    auto* d = g_drop.load();
    return d ? d->pass(now_ns) : 0;
}

extern "C" int hl_drop_touch(uint64_t addr, int path, int64_t now_ns) {
    auto* d = g_drop.load();
    if (!d || path < 0 || path >= int(hl::mem::TouchPath::kCount)) return 0;
    return d->touch(addr, hl::mem::TouchPath(path), now_ns);
}

extern "C" int hl_drop_wait_pending(uint64_t addr) {
    auto* d = g_drop.load();
    return d && d->wait_pending(addr);
}

extern "C" void hl_drop_kernel_use(uint64_t lo, uint64_t hi, int64_t now_ns) {
    if (auto* d = g_drop.load()) d->kernel_use(lo, hi, now_ns);
}
extern "C" void hl_drop_imported(uint64_t lo, uint64_t hi) {
    if (auto* d = g_drop.load()) d->mark_imported(lo, hi);
}
extern "C" void hl_drop_forget(uint64_t lo, uint64_t hi) {
    if (auto* d = g_drop.load()) d->forget(lo, hi);
}
extern "C" uint64_t hl_drop_reconcile(void) {
    auto* d = g_drop.load();
    return d ? d->reconcile() : 0;
}
extern "C" uint64_t hl_drop_dropped_now(void) {
    auto* d = g_drop.load();
    return d ? d->stats().dropped_now.load(std::memory_order_relaxed) : 0;
}
extern "C" int hl_drop_report(char* out, size_t cap, int64_t now_ns) {
    auto* d = g_drop.load();
    return d ? d->report(out, cap, now_ns) : 0;
}
