// HeyListen native engine: the C surface (hl/c/hl_mem.h) over the memory module's cold-page policy.
#include "hl/c/hl_mem.h"

#include <atomic>

#include "hl/mem/ColdPages.h"

namespace {
// (made once and never destroyed: the pass thread runs until the process ends)
std::atomic<hl::mem::ColdPages*> g_cold{nullptr};
}   // namespace

extern "C" int hl_mem_cold_start(const hl_mem_cold_config* c) {
    if (g_cold.load()) return 1;
    if (!c) return 0;
    hl::mem::ColdPages::Config config;
    config.units = c->units;
    config.unit_pages = c->unit_pages;
    config.page_bytes = c->page_bytes;
    config.backoff_passes = c->backoff_passes;
    config.give_up = c->give_up;
    config.budget_pages = c->budget_pages;
    auto* cold = new hl::mem::ColdPages(config);
    if (!cold->ok()) {
        delete cold;
        return 0;
    }
    g_cold.store(cold);
    return 1;
}

extern "C" void hl_mem_cold_pass(void) {
    if (auto* c = g_cold.load()) c->begin_pass();
}
extern "C" int hl_mem_cold_look(uint64_t unit, uint32_t resident) {
    auto* c = g_cold.load();
    return c && c->look(unit, resident);
}
extern "C" void hl_mem_cold_taken(uint64_t unit, uint32_t left, uint32_t stayed) {
    if (auto* c = g_cold.load()) c->taken_out(unit, left, stayed);
}
extern "C" void hl_mem_cold_forget(uint64_t lo, uint64_t hi) {
    if (auto* c = g_cold.load()) c->forget(lo, hi);
}
extern "C" void hl_mem_cold_imported(uint64_t lo, uint64_t hi) {
    if (auto* c = g_cold.load()) c->mark_imported(lo, hi);
}
extern "C" int hl_mem_cold_report(char* out, size_t cap) {
    auto* c = g_cold.load();
    return c ? c->report(out, cap) : 0;
}
