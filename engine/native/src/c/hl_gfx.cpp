// HeyListen native engine: the C surface (hl/c/hl_gfx.h) over the graphics front end and its bridge.
#include "hl/c/hl_gfx.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "host_runtime.h"

#include "hl/bridge/GfxBridge.h"
#include "hl/gfx/Nvn.h"
#include "hl/gfx/PassCheck.h"
#include "hl/gfx/RecordPass.h"
#include "hl/gfx/RenderState.h"
#include "hl/gfx/StateCheck.h"

namespace {

// (the shared adapter over the OS layer's callbacks, plus what only the graphics front end is handed)
struct HostRuntime final : hl::c::HostRuntime {
    hl_gfx_host gfx{};
    void written(hl::bridge::GuestAddr at, uint64_t n) override {
        if (gfx.written) gfx.written(gfx.user, at, n);
    }
};

// (made once and never destroyed, as the job bridge's)
std::once_flag g_runtime_once;
HostRuntime* g_runtime;
hl::bridge::GfxBridge* g_bridge;
hl::gfx::RecordPass* g_pass;
hl::gfx::PassCheck* g_check;
int g_mode = HL_GFX_OFF;
bool g_pass_started;

thread_local hl::gfx::PassRecording t_recording;
thread_local uint32_t t_mode;

// the render-state function
hl::bridge::StateBridge* g_state;
hl::gfx::NvnEntryPoints g_nvn;
hl::gfx::StateCheck* g_state_check;
std::atomic<uint64_t> g_state_answered{0}, g_state_left{0};

struct StateCall {
    uint64_t block = 0;
    uint32_t key = 0;
    hl::gfx::PassRecording native, translated;
    uint8_t expected[hl::bridge::StateBridge::kBlockSize];
};
thread_local StateCall t_state;

bool runtime(const hl_guest_host* host) {
    if (!hl::c::complete(host)) return false;
    std::call_once(g_runtime_once, [host] {
        HostRuntime* r = new HostRuntime;
        r->host = *host;
        g_runtime = r;
    });
    return true;
}

void state_log(void* user, const char* line) {
    const hl_gfx_host* gfx = static_cast<const hl_gfx_host*>(user);
    if (gfx->log) gfx->log(gfx->user, line);
}

}   // namespace

extern "C" int hl_gfx_start(const hl_guest_host* host, int mode, int check) {
    if (g_pass_started) return g_mode != HL_GFX_OFF;
    if (!runtime(host)) return 0;
    g_bridge = new hl::bridge::GfxBridge(*g_runtime);
    g_pass = new hl::gfx::RecordPass(*g_bridge);
    if (check) g_check = new hl::gfx::PassCheck;
    g_mode = mode == HL_GFX_SHELL ? HL_GFX_SHELL : HL_GFX_OFF;
    g_pass_started = true;
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

extern "C" int hl_gfx_report(char* out, size_t cap, const char* const* op_names, size_t n_names) {
    if (!g_pass || !cap) return 0;
    const hl::gfx::RecordPass::Stats& s = g_pass->stats();
    int n = std::snprintf(out, cap, "mode %d; native passes %llu (scene %llu, second %llu)", g_mode,
                          (unsigned long long)s.passes.load(), (unsigned long long)s.scene.load(),
                          (unsigned long long)s.second.load());
    if (g_check && n > 0 && (size_t)n < cap) {
        const hl::gfx::PassCheck::Report r = g_check->report();
        const char* names[2] = {"translated", "native"};
        n += std::snprintf(out + n, cap - n,
                           "; check: passes by mode %llu/%llu/%llu, procedures compared %u, largest difference of calls a "
                           "pass %.1f%% (%s)",
                           (unsigned long long)r.by_mode[0], (unsigned long long)r.by_mode[1],
                           (unsigned long long)r.by_mode[2], r.ops_compared, 100.0 * r.most_op_difference,
                           r.most_op < n_names && op_names[r.most_op] ? op_names[r.most_op] : "?");
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

// ---- the render-state function ----

extern "C" uint32_t hl_gfx_nvn_count(void) { return hl::gfx::kProcCount; }
extern "C" const char* hl_gfx_nvn_name(uint32_t proc) { return hl::gfx::proc_name(proc); }
extern "C" uint64_t hl_gfx_nvn_global(uint32_t proc) { return hl::bridge::proc_global(proc); }
extern "C" uint64_t hl_gfx_state_address(void) { return hl::bridge::kApplyRenderState; }

extern "C" int hl_gfx_state_start(const hl_guest_host* host, const hl_gfx_host* gfx, int check) {
    if (g_state) return 1;
    if (!gfx || !gfx->nvn || !runtime(host)) return 0;
    g_runtime->gfx = *gfx;
    g_nvn.user = gfx->user;
    g_nvn.call = gfx->nvn;
    if (check) g_state_check = new hl::gfx::StateCheck(state_log, &g_runtime->gfx);
    g_state = new hl::bridge::StateBridge(*g_runtime);
    return 1;
}

extern "C" int hl_gfx_state_apply(uint64_t block, uint64_t key, uint64_t sp) {
    hl::gfx::StateInputs in;
    hl::gfx::StateHandles at;
    if (!g_state->read(block, key, sp, in, at)) {
        g_state_left.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const hl::gfx::StateOutputs out = hl::gfx::RenderState::decide(in, g_state->offsets(block));
    // (the block first: the blend color call reads it from there)
    g_state->write(block, out);
    hl::gfx::NvnSink sink(g_nvn);
    hl::gfx::RenderState::emit(in, out, at, sink);
    g_state_answered.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

extern "C" int hl_gfx_state_check_begin(uint64_t block, uint64_t key, uint64_t sp) {
    hl::gfx::StateInputs in;
    hl::gfx::StateHandles at;
    if (!g_state->read(block, key, sp, in, at)) {
        g_state_check->deferred();
        return 0;
    }
    StateCall& t = t_state;
    t.block = block;
    t.key = (uint32_t)key;
    const hl::gfx::StateOutputs out = hl::gfx::RenderState::decide(in, g_state->offsets(block));
    t.native.clear();
    hl::gfx::RenderState::emit(in, out, at, t.native);
    g_state->copy(block, t.expected);
    hl::bridge::StateBridge::expect(t.expected, out);
    t.translated.clear();
    return 1;
}

extern "C" void hl_gfx_state_check_call(uint32_t proc, const uint64_t* x, const uint32_t* f) {
    hl::gfx::Command c;
    c.op = proc;
    std::memcpy(c.x, x, sizeof c.x);
    std::memcpy(c.f, f, sizeof c.f);
    t_state.translated.emit(c);
}

extern "C" void hl_gfx_state_check_end(void) {
    StateCall& t = t_state;
    uint8_t actual[hl::bridge::StateBridge::kBlockSize];
    g_state->copy(t.block, actual);
    g_state_check->compare(t.block, t.key, t.native.calls(), t.translated.calls(), t.expected, actual, sizeof actual);
}

extern "C" int hl_gfx_state_report(char* out, size_t cap) {
    if (!g_state || !cap) return 0;
    int n = std::snprintf(out, cap, "render state: native calls %llu, left to the translation %llu",
                          (unsigned long long)g_state_answered.load(), (unsigned long long)g_state_left.load());
    if (g_state_check && n > 0 && (size_t)n < cap) {
        const hl::gfx::StateCheck::Report r = g_state_check->report();
        n += std::snprintf(out + n, cap - n,
                           "; check: calls compared %llu, NVN calls compared %llu, mismatched calls %llu (NVN calls "
                           "%llu, block bytes %llu), not compared (callback) %llu",
                           (unsigned long long)r.calls, (unsigned long long)r.commands,
                           (unsigned long long)r.mismatched_calls, (unsigned long long)r.command_mismatches,
                           (unsigned long long)r.block_mismatches, (unsigned long long)r.deferred);
    }
    return n < 0 ? 0 : ((size_t)n < cap ? n : (int)cap - 1);
}
