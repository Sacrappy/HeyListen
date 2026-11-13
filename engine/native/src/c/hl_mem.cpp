// HeyListen native engine: the C surface (hl/c/hl_mem.h) over the memory module's cold-page policies.
#include "hl/c/hl_mem.h"

#include <atomic>

#include "hl/mem/ColdPages.h"

namespace {
// (one a kind, made once and never destroyed: the pass thread runs until the process ends)
std::atomic<hl::mem::ColdPages*> g_cold[HL_MEM_COLD_KINDS];

hl::mem::ColdPages* of(int kind) {
    return kind >= 0 && kind < HL_MEM_COLD_KINDS ? g_cold[kind].load() : nullptr;
}
}   // namespace

extern "C" int hl_mem_cold_start(int kind, const hl_mem_cold_config* c) {
    if (kind < 0 || kind >= HL_MEM_COLD_KINDS) return 0;
    if (g_cold[kind].load()) return 1;
    if (!c) return 0;
    hl::mem::ColdPages::Config config;
    config.units = c->units;
    config.unit_pages = c->unit_pages;
    config.page_bytes = c->page_bytes;
    config.backoff_passes = c->backoff_passes;
    config.give_up = c->give_up;
    config.budget_pages = c->budget_pages;
    config.settle_passes = c->settle_passes;
    auto* cold = new hl::mem::ColdPages(config);
    if (!cold->ok()) {
        delete cold;
        return 0;
    }
    g_cold[kind].store(cold);
    return 1;
}

extern "C" void hl_mem_cold_pass(int kind) {
    if (auto* c = of(kind)) c->begin_pass();
}
extern "C" int hl_mem_cold_look(int kind, uint64_t unit, uint32_t resident) {
    auto* c = of(kind);
    return c && c->look(unit, resident);
}
extern "C" void hl_mem_cold_taken(int kind, uint64_t unit, uint32_t left, uint32_t stayed) {
    if (auto* c = of(kind)) c->taken_out(unit, left, stayed);
}
extern "C" uint64_t hl_mem_cold_back_pages(int kind) {
    auto* c = of(kind);
    return c ? c->stats().back_pages.load(std::memory_order_relaxed) : 0;
}
extern "C" void hl_mem_cold_forget(uint64_t lo, uint64_t hi) {
    for (int k = 0; k < HL_MEM_COLD_KINDS; k++)
        if (auto* c = of(k)) c->forget(lo, hi);
}
extern "C" void hl_mem_cold_imported(uint64_t lo, uint64_t hi) {
    for (int k = 0; k < HL_MEM_COLD_KINDS; k++)
        if (auto* c = of(k)) c->mark_imported(lo, hi);
}
extern "C" int hl_mem_cold_report(int kind, char* out, size_t cap) {
    auto* c = of(kind);
    return c ? c->report(out, cap) : 0;
}
