// HeyListen native engine: the call-by-call check (hl/gfx/StateCheck.h).
#include "hl/gfx/StateCheck.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "hl/gfx/Nvn.h"

namespace hl::gfx {

bool StateCheck::compare(uint64_t block, uint32_t key, const std::vector<Command>& native,
                         const std::vector<Command>& translated, const uint8_t* expected, const uint8_t* actual,
                         size_t size) {
    const uint64_t call = calls_.fetch_add(1, std::memory_order_relaxed) + 1;
    commands_.fetch_add(translated.size(), std::memory_order_relaxed);
    size_t first_call = native.size() == translated.size() ? (size_t)-1 : std::min(native.size(), translated.size());
    for (size_t i = 0; i < native.size() && i < translated.size(); i++)
        if (!same_call(native[i], translated[i])) {
            first_call = i;
            break;
        }
    size_t first_byte = (size_t)-1, bytes = 0;
    for (size_t i = 0; i < size; i++)
        if (expected[i] != actual[i]) {
            if (first_byte == (size_t)-1) first_byte = i;
            bytes++;
        }
    const bool calls_differ = first_call != (size_t)-1, block_differs = bytes != 0;
    if (!calls_differ && !block_differs) return true;
    mismatched_.fetch_add(1, std::memory_order_relaxed);
    if (calls_differ) command_mismatches_.fetch_add(1, std::memory_order_relaxed);
    if (block_differs) block_mismatches_.fetch_add(1, std::memory_order_relaxed);
    if (!log_) return false;
    {
        std::lock_guard<std::mutex> g(log_lock_);
        if (logged_ >= most_logged_) return false;
        logged_++;
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "mismatch at call %llu (block %#llx, key %#x): %zu NVN calls native, %zu translated; first different "
                  "call %lld; %zu block bytes differ, the first at +%lld",
                  (unsigned long long)call, (unsigned long long)block, key, native.size(), translated.size(),
                  calls_differ ? (long long)first_call : -1LL, bytes, block_differs ? (long long)first_byte : -1LL);
    log_(user_, line);
    if (calls_differ) {
        // (the differing call and the two after it, on both sides)
        for (size_t i = first_call; i < first_call + 3 && (i < native.size() || i < translated.size()); i++) {
            const std::string n = i < native.size() ? describe(native[i]) : "-";
            const std::string t = i < translated.size() ? describe(translated[i]) : "-";
            std::snprintf(line, sizeof line, "  call %zu: native %s, translated %s", i, n.c_str(), t.c_str());
            log_(user_, line);
        }
    }
    if (block_differs) {
        int k = std::snprintf(line, sizeof line, "  block bytes (offset native/translated):");
        size_t shown = 0;
        for (size_t i = first_byte; i < size && shown < 16 && k > 0 && (size_t)k < sizeof line; i++)
            if (expected[i] != actual[i]) {
                k += std::snprintf(line + k, sizeof line - k, " +%zu %02x/%02x", i, expected[i], actual[i]);
                shown++;
            }
        log_(user_, line);
    }
    return false;
}

StateCheck::Report StateCheck::report() const {
    Report r;
    r.calls = calls_.load(std::memory_order_relaxed);
    r.commands = commands_.load(std::memory_order_relaxed);
    r.mismatched_calls = mismatched_.load(std::memory_order_relaxed);
    r.command_mismatches = command_mismatches_.load(std::memory_order_relaxed);
    r.block_mismatches = block_mismatches_.load(std::memory_order_relaxed);
    r.deferred = deferred_.load(std::memory_order_relaxed);
    return r;
}

}   // namespace hl::gfx
