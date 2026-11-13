// HeyListen native engine: the render state written as the back end's records (hl/c/hl_gfx_rec.h; direct-emission.md,
// bricks A and B).
//
// With no switch the render-state function's commands go to the NVN procedures as before. HL_GFX_REC=1 writes them as
// records into the command buffer's own segment through the OS layer's surface (no NVN call). HL_GFX_REC_CHECK=1 keeps
// the NVN path and compares, at every call, the records its procedures appended with the records RecordSink writes into
// a scratch segment for the same commands, byte for byte, and the accounting the game reads back. With both switches
// the records are written for real, then the NVN procedures append theirs after them, the two are compared and the NVN
// ones dropped (the segment and the accounting put back).
#include "gfx_rec.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "hl/c/hl_gfx_rec.h"
#include "hl/gfx/Records.h"

namespace {

const hl_gfx_rec_host* g_host;
std::once_flag g_mode_once;
int g_mode;

struct Counters {
    std::atomic<uint64_t> written{0}, written_records{0}, fell_back{0};
    std::atomic<uint64_t> calls{0}, records{0}, bytes{0}, mismatched{0}, accounting{0}, stray{0};
};
Counters g_n;
std::mutex g_log_lock;
uint32_t g_logged;
std::atomic<int64_t> g_reported{0};

int mode() {
    std::call_once(g_mode_once, [] { g_mode = g_host && g_host->mode ? g_host->mode(g_host->user) : 0; });
    return g_mode;
}

void log(const char* line) {
    if (g_host->log) g_host->log(g_host->user, line);
}

// the command buffer's own segment
struct LiveSegment final : hl::gfx::RecordTarget {
    void* cmd;
    explicit LiveSegment(void* c) : cmd(c) {}
    void* append(uint32_t op, uint32_t payload, uint32_t hw) override {
        return g_host->append(g_host->user, cmd, op, payload, hw);
    }
};

struct HostGuest final : hl::gfx::GuestReader {
    const void* read(uint64_t guest, uint32_t n) override { return g_host->guest(g_host->user, guest, n); }
};

thread_local hl::gfx::ScratchSegment t_scratch;

// (one record in a line: its offset, op, size and up to 48 payload bytes)
void describe(char* line, size_t cap, const char* side, const uint8_t* p, size_t at, size_t n) {
    if (at + sizeof(hl_rec_hdr) > n) {
        std::snprintf(line, cap, "  %s: none (%zu bytes)", side, n);
        return;
    }
    hl_rec_hdr h;
    std::memcpy(&h, p + at, sizeof h);
    int k = std::snprintf(line, cap, "  %s +%zu: op %u size %u:", side, at, h.op, h.size);
    for (size_t i = at + 8; i < at + h.size && i < n && i < at + 8 + 48 && k > 0 && (size_t)k < cap; i++)
        k += std::snprintf(line + k, cap - k, " %02x", p[i]);
}

// Compares one call's two record runs (native, nvn) and the accounting each left. Returns true when equal.
bool compare(uint64_t cb, const uint8_t* native, size_t n_native, const hl_rec_mark& native_acc, const uint8_t* nvn,
             size_t n_nvn, const hl_rec_mark& nvn_acc, uint32_t stray) {
    uint64_t records = 0;
    size_t first = (size_t)-1;
    for (size_t at = 0; at + sizeof(hl_rec_hdr) <= n_nvn;) {
        hl_rec_hdr h;
        std::memcpy(&h, nvn + at, sizeof h);
        if (h.size < sizeof h || at + h.size > n_nvn) break;
        records++;
        if (first == (size_t)-1 && (at + h.size > n_native || std::memcmp(native + at, nvn + at, h.size))) first = at;
        at += h.size;
    }
    if (first == (size_t)-1 && n_native != n_nvn) first = n_native < n_nvn ? n_native : n_nvn;
    const bool acc = native_acc.cmd_used != nvn_acc.cmd_used || native_acc.hw_used != nvn_acc.hw_used ||
                     native_acc.last_rec != nvn_acc.last_rec;
    const uint64_t call = g_n.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_n.records.fetch_add(records, std::memory_order_relaxed);
    g_n.bytes.fetch_add(n_nvn, std::memory_order_relaxed);
    if (stray) g_n.stray.fetch_add(stray, std::memory_order_relaxed);
    if (first == (size_t)-1 && !acc && !stray) return true;
    g_n.mismatched.fetch_add(1, std::memory_order_relaxed);
    if (acc) g_n.accounting.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(g_log_lock);
        if (g_logged >= 8) return false;
        g_logged++;
    }
    char line[640];
    std::snprintf(line, sizeof line,
                  "hl gfx rec: check: mismatch at call %llu (command buffer %#llx): native %zu bytes, NVN %zu bytes "
                  "(%llu records); first difference at +%lld; commands not written %u; accounting cmd %llu/%llu, hw "
                  "%llu/%llu, last %llu/%llu (native/NVN)",
                  (unsigned long long)call, (unsigned long long)cb, n_native, n_nvn, (unsigned long long)records,
                  first == (size_t)-1 ? -1LL : (long long)first, stray, (unsigned long long)native_acc.cmd_used,
                  (unsigned long long)nvn_acc.cmd_used, (unsigned long long)native_acc.hw_used,
                  (unsigned long long)nvn_acc.hw_used, (unsigned long long)native_acc.last_rec,
                  (unsigned long long)nvn_acc.last_rec);
    log(line);
    if (first != (size_t)-1) {
        describe(line, sizeof line, "native", native, first, n_native);
        log(line);
        describe(line, sizeof line, "NVN", nvn, first, n_nvn);
        log(line);
    }
    return false;
}

void report() {
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t was = g_reported.load(std::memory_order_relaxed);
    if (now - was < 10 || !g_reported.compare_exchange_strong(was, now)) return;
    char line[512];
    int k = std::snprintf(line, sizeof line, "hl gfx rec: calls written as records %llu (records %llu), left to NVN %llu",
                          (unsigned long long)g_n.written.load(), (unsigned long long)g_n.written_records.load(),
                          (unsigned long long)g_n.fell_back.load());
    if (k > 0 && (size_t)k < sizeof line)
        k += std::snprintf(line + k, sizeof line - k, ", commands not written %llu", (unsigned long long)g_n.stray.load());
    if ((g_mode & HL_REC_CHECK) && k > 0 && (size_t)k < sizeof line)
        std::snprintf(line + k, sizeof line - k,
                      "; check: calls compared %llu, records %llu, bytes %llu, mismatched calls %llu (accounting %llu)",
                      (unsigned long long)g_n.calls.load(), (unsigned long long)g_n.records.load(),
                      (unsigned long long)g_n.bytes.load(), (unsigned long long)g_n.mismatched.load(),
                      (unsigned long long)g_n.accounting.load());
    log(line);
}

}   // namespace

extern "C" void hl_gfx_rec_register(const hl_gfx_rec_host* host) { g_host = host; }

namespace hl::c {

void gfx_rec_emit(const gfx::StateInputs& in, const gfx::StateOutputs& out, const gfx::StateHandles& at,
                  const gfx::NvnEntryPoints& nvn) {
    gfx::NvnSink nvn_sink(nvn);
    const int m = mode();
    // (nothing recorded: the flags' bit 17)
    if (!m || !at.command_buffer || ((in.flags >> 17) & 1)) {
        gfx::RenderState::emit(in, out, at, nvn_sink);
        return;
    }
    void* cmd = g_host->open(g_host->user, at.command_buffer);
    if (!cmd) {
        g_n.fell_back.fetch_add(1, std::memory_order_relaxed);
        gfx::RenderState::emit(in, out, at, nvn_sink);
        report();
        return;
    }
    HostGuest guest;
    hl_rec_mark before, mid, after;
    g_host->mark(g_host->user, cmd, &before);
    if (m & HL_REC_WRITE) {
        LiveSegment live(cmd);
        gfx::RecordSink sink(live, guest, at.command_buffer);
        gfx::RenderState::emit(in, out, at, sink);
        g_n.written.fetch_add(1, std::memory_order_relaxed);
        g_n.written_records.fetch_add(sink.records(), std::memory_order_relaxed);
        if (m & HL_REC_CHECK) {
            g_host->mark(g_host->user, cmd, &mid);
            gfx::RenderState::emit(in, out, at, nvn_sink);
            g_host->mark(g_host->user, cmd, &after);
            // (the two runs' accounting as deltas from where each started)
            hl_rec_mark native_acc = mid, nvn_acc = after;
            nvn_acc.cmd_used = before.cmd_used + (after.cmd_used - mid.cmd_used);
            nvn_acc.hw_used = before.hw_used + (after.hw_used - mid.hw_used);
            const uint8_t* bytes = g_host->bytes(g_host->user, cmd, before.offset);
            compare(at.command_buffer, bytes, mid.offset - before.offset, native_acc, bytes + (mid.offset - before.offset),
                    after.offset - mid.offset, nvn_acc, sink.stray());
            g_host->rewind(g_host->user, cmd, &mid);
        } else if (sink.stray()) {
            g_n.stray.fetch_add(sink.stray(), std::memory_order_relaxed);
        }
    } else {
        gfx::ScratchSegment& scratch = t_scratch;
        scratch.reset(before);
        gfx::RecordSink sink(scratch, guest, at.command_buffer);
        gfx::RenderState::emit(in, out, at, sink);
        gfx::RenderState::emit(in, out, at, nvn_sink);
        g_host->mark(g_host->user, cmd, &after);
        compare(at.command_buffer, scratch.bytes().data(), scratch.bytes().size(), scratch.mark(),
                g_host->bytes(g_host->user, cmd, before.offset), after.offset - before.offset, after, sink.stray());
    }
    report();
}

}   // namespace hl::c
