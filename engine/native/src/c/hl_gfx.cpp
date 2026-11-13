// HeyListen native engine: the C surface (hl/c/hl_gfx.h) over the graphics front end and its bridge.
#include "hl/c/hl_gfx.h"

#include <cstdio>
#include <cstring>

#include "hl/bridge/GfxBridge.h"
#include "hl/gfx/PassCheck.h"
#include "hl/gfx/RecordPass.h"

namespace {

struct HostRuntime final : hl::bridge::GuestRuntime {
    hl_guest_host host;
    void* ptr(hl::bridge::GuestAddr at) const override { return host.ptr(host.user, at); }
    hl::bridge::GuestThread current() const override { return host.current(host.user); }
    void call(hl::bridge::GuestThread as, hl::bridge::GuestAddr fn, uint64_t a0, uint64_t a1) override {
        host.call(host.user, as, fn, a0, a1);
    }
};

// (made once and never destroyed, as the job bridge's)
HostRuntime* g_runtime;
hl::bridge::GfxBridge* g_bridge;
hl::gfx::RecordPass* g_pass;
hl::gfx::PassCheck* g_check;
int g_mode;

thread_local hl::gfx::PassRecording t_recording;
thread_local uint32_t t_mode;

}   // namespace

extern "C" int hl_gfx_start(const hl_guest_host* host, int mode, int check) {
    if (g_runtime) return g_mode != HL_GFX_OFF;
    if (!host || !host->ptr || !host->current || !host->call) return 0;
    g_runtime = new HostRuntime;
    g_runtime->host = *host;
    g_bridge = new hl::bridge::GfxBridge(*g_runtime);
    g_pass = new hl::gfx::RecordPass(*g_bridge);
    if (check) g_check = new hl::gfx::PassCheck;
    g_mode = mode == HL_GFX_SHELL ? HL_GFX_SHELL : HL_GFX_OFF;
    return g_mode != HL_GFX_OFF;
}

extern "C" uint64_t hl_gfx_pass_address(void) { return hl::bridge::kRecordPass; }

extern "C" uint32_t hl_gfx_record_pass(uint64_t listener, uint64_t args) { return g_pass->run(listener, args); }

extern "C" int hl_gfx_check_next_native(void) {
    return g_mode != HL_GFX_OFF && g_check->next_path() == hl::gfx::PassPath::Native;
}

extern "C" void hl_gfx_check_begin(uint64_t args) {
    t_mode = g_bridge->mode(args);
    t_recording.clear();
}

extern "C" void hl_gfx_check_call(uint32_t op, const uint64_t* x, const uint32_t* f) {
    hl::gfx::Command c;
    c.op = op;
    std::memcpy(c.x, x, sizeof c.x);
    std::memcpy(c.f, f, sizeof c.f);
    t_recording.emit(c);
}

extern "C" void hl_gfx_check_end(int native) {
    g_check->add(native ? hl::gfx::PassPath::Native : hl::gfx::PassPath::Translated, t_mode, t_recording);
}

extern "C" int hl_gfx_report(char* out, size_t cap) {
    if (!g_pass || !cap) return 0;
    const hl::gfx::RecordPass::Stats& s = g_pass->stats();
    int n = std::snprintf(out, cap, "mode %d; native passes %llu (scene %llu, second %llu)", g_mode,
                          (unsigned long long)s.passes.load(), (unsigned long long)s.scene.load(),
                          (unsigned long long)s.second.load());
    if (g_check && n > 0 && (size_t)n < cap) {
        const hl::gfx::PassCheck::Report r = g_check->report();
        const char* names[2] = {"translated", "native"};
        n += std::snprintf(out + n, cap - n, "; check: passes by mode %llu/%llu/%llu", (unsigned long long)r.by_mode[0],
                           (unsigned long long)r.by_mode[1], (unsigned long long)r.by_mode[2]);
        for (int p = 0; p < 2 && n > 0 && (size_t)n < cap; p++) {
            const hl::gfx::PassCheck::PathStats& t = r.path[p];
            n += std::snprintf(out + n, cap - n,
                               "; %s: passes %llu, calls %llu (most %llu), shapes %llu, unmatched passes %llu (shapes %llu)",
                               names[p], (unsigned long long)t.passes, (unsigned long long)t.calls,
                               (unsigned long long)t.most_calls, (unsigned long long)t.shapes,
                               (unsigned long long)t.unmatched, (unsigned long long)t.unmatched_shapes);
        }
    }
    return n < 0 ? 0 : ((size_t)n < cap ? n : (int)cap - 1);
}
