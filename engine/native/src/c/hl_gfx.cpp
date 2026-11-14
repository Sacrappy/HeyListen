// HeyListen native engine: the C surface (hl/c/hl_gfx.h) over the graphics front end and its bridge.
#include "hl/c/hl_gfx.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "gfx_rec.h"
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

namespace {
// (the OS layer's NVN entry points and callbacks: the same for every native producer, set by the first to start)
std::once_flag g_gfx_once;
bool gfx_host(const hl_guest_host* host, const hl_gfx_host* gfx) {
    if (!gfx || !gfx->nvn || !runtime(host)) return false;
    std::call_once(g_gfx_once, [gfx] {
        g_runtime->gfx = *gfx;
        g_nvn.user = gfx->user;
        g_nvn.call = gfx->nvn;
    });
    return true;
}
}   // namespace

extern "C" int hl_gfx_state_start(const hl_guest_host* host, const hl_gfx_host* gfx, int check) {
    if (g_state) return 1;
    if (!gfx_host(host, gfx)) return 0;
    if (check) g_state_check = new hl::gfx::StateCheck("render state", state_log, &g_runtime->gfx);
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
    hl::c::gfx_rec_emit(in, out, at, g_nvn);   // (the NVN procedures, or the records directly: hl_gfx_rec.cpp)
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

// ---- the sub-mesh submission ----

namespace {
using hl::bridge::DrawBridge;
using hl::bridge::SubmitRoute;
using hl::gfx::Submit;

DrawBridge* g_draw;
hl::gfx::StateCheck* g_draw_check_prologue;
hl::gfx::StateCheck* g_draw_check_submit;
// (native: answered by way; check: compared by way. Left to the translation: by route)
std::atomic<uint64_t> g_draw_by[4], g_draw_unprepared{0}, g_draw_out_of_range{0}, g_draw_left[5];

struct DrawCall {
    uint64_t drawable = 0, block = 0;
    uint32_t sub_mesh = 0, mask = 0;
    hl::bridge::GuestAddr context = 0, command_buffer = 0;
    bool in_range = false, reached = false, path_differs = false;
    hl::gfx::PassRecording native, translated;
    uint8_t expected[DrawBridge::kSnapshotSize];
};
thread_local DrawCall t_draw;

// The submission: decided on what the preparation left (the index buffer's address asked through the NVN entry points),
// written when `write`, its commands into `sink`; `expected`: the bytes it should leave, from those there now.
void submit(DrawCall& t, uint64_t prepared, hl::gfx::CommandSink& sink, bool write, uint8_t* expected) {
    hl::gfx::SubmitInputs in;
    hl::gfx::SubmitHandles at;
    g_draw->read_submit(t.drawable, t.block, t.sub_mesh, prepared, t.context, in, at);
    hl::gfx::IndexedInputs indexed;
    const bool elements = hl::gfx::SubmeshDraw::path(in) == Submit::Elements && (prepared & 1);
    if (elements) g_draw->read_indexed(t.drawable, t.sub_mesh, indexed);
    const hl::gfx::SubmitOutputs out = hl::gfx::SubmeshDraw::decide(in, elements ? &indexed : nullptr);
    uint64_t address = 0;
    if (elements) address = g_nvn.ask(hl::gfx::command(hl::gfx::Proc::BufferGetAddress, indexed.buffer));
    if (!(prepared & 1)) g_draw_unprepared.fetch_add(1, std::memory_order_relaxed);
    else g_draw_by[(int)out.how].fetch_add(1, std::memory_order_relaxed);
    t.command_buffer = at.command_buffer;
    if (expected) {
        g_draw->snapshot(t.block, t.command_buffer, expected);
        DrawBridge::expect_submit(expected, out);
    }
    if (write) g_draw->write_submit(t.block, at, out);
    hl::gfx::SubmeshDraw::emit(out, elements ? &indexed : nullptr, address, at, sink);
}
}   // namespace

extern "C" uint64_t hl_gfx_draw_address(void) { return hl::bridge::kSubmitSubmesh; }
extern "C" uint64_t hl_gfx_draw_prepare_address(void) { return hl::bridge::kPrepareSubmesh; }
extern "C" uint64_t hl_gfx_draw_prepare_return(void) { return hl::bridge::kPrepareReturn; }
extern "C" uint64_t hl_gfx_draw_frame(void) { return hl::bridge::kSubmitFrame; }

extern "C" int hl_gfx_draw_start(const hl_guest_host* host, const hl_gfx_host* gfx, int check) {
    if (g_draw) return 1;
    if (!gfx_host(host, gfx)) return 0;
    if (check) {
        g_draw_check_prologue = new hl::gfx::StateCheck("draw prologue", state_log, &g_runtime->gfx);
        g_draw_check_submit = new hl::gfx::StateCheck("draw submission", state_log, &g_runtime->gfx);
    }
    g_draw = new DrawBridge(*g_runtime);
    return 1;
}

extern "C" int hl_gfx_draw_begin(uint64_t drawable, uint64_t block, uint32_t sub_mesh, uint32_t mask) {
    const SubmitRoute route = g_draw->route(drawable, mask);
    if (route != SubmitRoute::Direct) {
        g_draw_left[(int)route].fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    DrawCall& t = t_draw;
    t.drawable = drawable, t.block = block, t.sub_mesh = sub_mesh, t.mask = mask;
    hl::gfx::PrologueInputs in;
    g_draw->read_prologue(drawable, block, sub_mesh, in, t.context);
    const hl::gfx::PrologueOutputs out = hl::gfx::SubmeshDraw::prologue(in);
    if (!out.in_range) {
        g_draw_out_of_range.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    // (the block first: the function writes it before the call)
    g_draw->write_prologue(block, out);
    hl::gfx::NvnSink sink(g_nvn);
    hl::gfx::SubmeshDraw::emit_prologue(out, out.mode_changed ? g_draw->command_buffer(t.context) : 0, sink);
    return 2;
}

extern "C" void hl_gfx_draw_end(uint64_t prepared) {
    hl::gfx::NvnSink sink(g_nvn);
    submit(t_draw, prepared, sink, true, nullptr);
}

extern "C" int hl_gfx_draw_check_begin(uint64_t drawable, uint64_t block, uint32_t sub_mesh, uint32_t mask) {
    const SubmitRoute route = g_draw->route(drawable, mask);
    if (route != SubmitRoute::Direct) {
        g_draw_left[(int)route].fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    DrawCall& t = t_draw;
    t.drawable = drawable, t.block = block, t.sub_mesh = sub_mesh, t.mask = mask;
    t.reached = t.path_differs = false;
    hl::gfx::PrologueInputs in;
    g_draw->read_prologue(drawable, block, sub_mesh, in, t.context);
    const hl::gfx::PrologueOutputs out = hl::gfx::SubmeshDraw::prologue(in);
    t.in_range = out.in_range;
    if (!out.in_range) g_draw_out_of_range.fetch_add(1, std::memory_order_relaxed);
    t.command_buffer = out.mode_changed ? g_draw->command_buffer(t.context) : 0;
    t.native.clear();
    t.translated.clear();
    hl::gfx::SubmeshDraw::emit_prologue(out, t.command_buffer, t.native);
    g_draw->snapshot(block, t.command_buffer, t.expected);
    DrawBridge::expect_prologue(t.expected, out);
    return 1;
}

namespace {
void compare(hl::gfx::StateCheck* check, DrawCall& t, bool path_differs) {
    uint8_t actual[DrawBridge::kSnapshotSize];
    g_draw->snapshot(t.block, t.command_buffer, actual);
    check->compare(t.block, t.sub_mesh, t.native.calls(), t.translated.calls(), t.expected, actual, sizeof actual,
                   path_differs);
}
}   // namespace

extern "C" void hl_gfx_draw_check_segment(void) {
    DrawCall& t = t_draw;
    // (the body reached its preparation: the prologue's segment is over, and the sub-mesh must have been in range)
    compare(g_draw_check_prologue, t, !t.in_range);
    t.native.clear();
    t.translated.clear();
}

extern "C" void hl_gfx_draw_check_prepared(uint64_t prepared) {
    DrawCall& t = t_draw;
    t.reached = true;
    // (the way decided at the entry must still hold on what the preparation left)
    t.path_differs = g_draw->route(t.drawable, t.mask) != SubmitRoute::Direct;
    submit(t, prepared, t.native, false, t.expected);
}

extern "C" void hl_gfx_draw_check_call(uint32_t proc, const uint64_t* x, const uint32_t* f) {
    hl::gfx::Command c;
    c.op = proc;
    std::memcpy(c.x, x, sizeof c.x);
    std::memcpy(c.f, f, sizeof c.f);
    t_draw.translated.emit(c);
}

extern "C" void hl_gfx_draw_check_end(void) {
    DrawCall& t = t_draw;
    if (t.reached) compare(g_draw_check_submit, t, t.path_differs);
    else compare(g_draw_check_prologue, t, t.in_range);   // (an in-range sub-mesh must reach the preparation)
}

extern "C" int hl_gfx_draw_report(char* out, size_t cap) {
    if (!g_draw || !cap) return 0;
    auto v = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    int n = std::snprintf(out, cap,
                          "draw: %s pre-recorded %llu, indexed %llu, mesh tasks %llu, nothing %llu, not prepared %llu, "
                          "out of range %llu; left to the translation: query %llu, callback %llu, clusters %llu, "
                          "inline %llu",
                          g_draw_check_submit ? "compared" : "native", v(g_draw_by[(int)Submit::CallCommands]),
                          v(g_draw_by[(int)Submit::Elements]), v(g_draw_by[(int)Submit::MeshTasks]),
                          v(g_draw_by[(int)Submit::None]), v(g_draw_unprepared), v(g_draw_out_of_range),
                          v(g_draw_left[(int)SubmitRoute::Query]), v(g_draw_left[(int)SubmitRoute::Callback]),
                          v(g_draw_left[(int)SubmitRoute::Clusters]), v(g_draw_left[(int)SubmitRoute::Inline]));
    const hl::gfx::StateCheck* checks[2] = {g_draw_check_prologue, g_draw_check_submit};
    const char* names[2] = {"prologue", "submission"};
    for (int i = 0; i < 2 && checks[i] && n > 0 && (size_t)n < cap; i++) {
        const hl::gfx::StateCheck::Report r = checks[i]->report();
        n += std::snprintf(out + n, cap - n,
                           "; check %s: calls compared %llu, NVN calls compared %llu, mismatched calls %llu (NVN calls "
                           "%llu, bytes %llu)",
                           names[i], (unsigned long long)r.calls, (unsigned long long)r.commands,
                           (unsigned long long)r.mismatched_calls, (unsigned long long)r.command_mismatches,
                           (unsigned long long)r.block_mismatches);
    }
    return n < 0 ? 0 : ((size_t)n < cap ? n : (int)cap - 1);
}

// ---- the shared preparation ----

namespace {
using hl::bridge::PrepareBridge;
using hl::gfx::PrepareRoute;

PrepareBridge* g_prep;
bool g_prep_state;   // (the state step native: HL_GFX_PREP_STATE)
hl::gfx::StateCheck* g_prep_check;
// (every call by route; with the check, the translated NVN calls of the calls of each route too)
std::atomic<uint64_t> g_prep_routes[hl::gfx::kPrepareRoutes], g_prep_nvn[hl::gfx::kPrepareRoutes];

struct PrepCall {
    uint64_t block = 0, drawable = 0;
    PrepareRoute route = PrepareRoute::NoMaterial;
    uint32_t result = 0;
    hl::gfx::PrepareHandles at;
    uint32_t groups[HL_GFX_PREP_GROUPS] = {};
    hl::gfx::PassRecording native, translated;
    uint8_t expected[PrepareBridge::kSnapshotSize];
};
thread_local PrepCall t_prep;

// The paths' histogram: a call's shape is its route, its result and how many NVN calls of each kind the translated body
// made (each count capped), packed in one word.
std::mutex g_shapes_lock;
std::vector<std::pair<uint64_t, uint64_t>> g_shapes;   // (shape, calls), unsorted; a few hundred shapes at most

// (the bits of each kind's count in a shape, by HL_GFX_PREP_GROUP_*: a count is capped at what they hold)
constexpr uint32_t kShapeBits[HL_GFX_PREP_GROUPS] = {6, 6, 2, 6, 6, 6, 3};

uint64_t shape_of(const PrepCall& t, uint64_t result) {
    uint64_t s = (uint64_t)t.route | ((result & 1) << 4);
    uint32_t at = 5;
    for (int g = 0; g < HL_GFX_PREP_GROUPS; g++) {
        const uint32_t cap = (1u << kShapeBits[g]) - 1;
        s |= (uint64_t)(t.groups[g] < cap ? t.groups[g] : cap) << at;
        at += kShapeBits[g];
    }
    return s;
}

void file_shape(uint64_t s) {
    std::lock_guard<std::mutex> lock(g_shapes_lock);
    for (auto& e : g_shapes)
        if (e.first == s) {
            e.second++;
            return;
        }
    g_shapes.push_back({s, 1});
}
}   // namespace

extern "C" uint64_t hl_gfx_prep_address(void) { return hl::bridge::kPrepareSubmesh; }

extern "C" int hl_gfx_prep_start(const hl_guest_host* host, const hl_gfx_host* gfx, int check, int state) {
    if (g_prep) return 1;
    g_prep_state = state != 0;
    if (!gfx_host(host, gfx)) return 0;
    if (check) g_prep_check = new hl::gfx::StateCheck("preparation", state_log, &g_runtime->gfx);
    g_prep = new PrepareBridge(*g_runtime);
    return 1;
}

extern "C" int hl_gfx_prep_apply(uint64_t block, uint64_t drawable, uint64_t sp, uint64_t* result) {
    hl::gfx::PrepareInputs in;
    hl::gfx::PrepareHandles at;
    g_prep->read(block, drawable, in, at);
    const hl::gfx::PrepareOutputs out = hl::gfx::Prepare::decide(in, g_prep_state);
    if (out.state_recorded || out.bind_count) g_prep->state_handles(block, sp, at);
    g_prep_routes[(int)out.route].fetch_add(1, std::memory_order_relaxed);
    if (!hl::gfx::Prepare::native(out.route)) return 0;
    // (the block and the mark first: the function writes them before its call)
    g_prep->write(block, at, out);
    hl::gfx::NvnSink sink(g_nvn);
    hl::gfx::Prepare::emit(out, at, sink);
    *result = out.result;
    return 1;
}

extern "C" void hl_gfx_prep_check_begin(uint64_t block, uint64_t drawable, uint64_t sp) {
    PrepCall& t = t_prep;
    t.block = block, t.drawable = drawable;
    hl::gfx::PrepareInputs in;
    g_prep->read(block, drawable, in, t.at);
    const hl::gfx::PrepareOutputs out = hl::gfx::Prepare::decide(in, g_prep_state);
    if (out.state_recorded || out.bind_count) g_prep->state_handles(block, sp, t.at);
    g_prep_routes[(int)out.route].fetch_add(1, std::memory_order_relaxed);
    t.route = out.route;
    t.result = out.result;
    for (uint32_t& g : t.groups) g = 0;
    t.native.clear();
    t.translated.clear();
    hl::gfx::Prepare::emit(out, t.at, t.native);
    g_prep->snapshot(block, t.at.command_buffer ? t.at.command_buffer : t.at.bind_buffer, t.expected);
    PrepareBridge::expect(t.expected, out);
}

extern "C" void hl_gfx_prep_check_call(uint32_t proc, uint32_t group, const uint64_t* x, const uint32_t* f) {
    PrepCall& t = t_prep;
    hl::gfx::Command c;
    c.op = proc;
    std::memcpy(c.x, x, sizeof c.x);
    std::memcpy(c.f, f, sizeof c.f);
    t.translated.emit(c);
    t.groups[group < HL_GFX_PREP_GROUPS ? group : (uint32_t)HL_GFX_PREP_GROUP_OTHER]++;   // (one type: GCC -Wextra)
}

extern "C" void hl_gfx_prep_check_end(uint64_t result) {
    PrepCall& t = t_prep;
    g_prep_nvn[(int)t.route].fetch_add(t.translated.calls().size(), std::memory_order_relaxed);
    file_shape(shape_of(t, result));
    if (!hl::gfx::Prepare::native(t.route)) return;
    uint8_t actual[PrepareBridge::kSnapshotSize];
    g_prep->snapshot(t.block, t.at.command_buffer ? t.at.command_buffer : t.at.bind_buffer, actual);
    // (the result is the function's w0: a different one is a mismatch whatever the calls and bytes)
    g_prep_check->compare(t.block, (uint32_t)t.drawable, t.native.calls(), t.translated.calls(), t.expected, actual,
                          sizeof actual, result != t.result);
}

extern "C" int hl_gfx_prep_report(char* out, size_t cap) {
    if (!g_prep || !cap) return 0;
    int n = std::snprintf(out, cap, "preparation: %s", g_prep_check ? "calls by route" : "native calls by route");
    for (uint32_t r = 0; r < hl::gfx::kPrepareRoutes && n > 0 && (size_t)n < cap; r++)
        n += std::snprintf(out + n, cap - n, "%s %s %llu", r ? "," : "", hl::gfx::route_name((PrepareRoute)r),
                           (unsigned long long)g_prep_routes[r].load(std::memory_order_relaxed));
    if (g_prep_check && n > 0 && (size_t)n < cap) {
        n += std::snprintf(out + n, cap - n, "; translated NVN calls by route");
        for (uint32_t r = 0; r < hl::gfx::kPrepareRoutes && n > 0 && (size_t)n < cap; r++)
            n += std::snprintf(out + n, cap - n, "%s %llu", r ? "," : "",
                               (unsigned long long)g_prep_nvn[r].load(std::memory_order_relaxed));
        const hl::gfx::StateCheck::Report r = g_prep_check->report();
        if (n > 0 && (size_t)n < cap)
            n += std::snprintf(out + n, cap - n,
                               "; check: calls compared %llu, NVN calls compared %llu, mismatched calls %llu (NVN calls "
                               "%llu, bytes %llu)",
                               (unsigned long long)r.calls, (unsigned long long)r.commands,
                               (unsigned long long)r.mismatched_calls, (unsigned long long)r.command_mismatches,
                               (unsigned long long)r.block_mismatches);
    }
    return n < 0 ? 0 : ((size_t)n < cap ? n : (int)cap - 1);
}

extern "C" int hl_gfx_prep_shapes(char* out, size_t cap) {
    if (!g_prep_check || !cap) return 0;
    std::vector<std::pair<uint64_t, uint64_t>> s;
    {
        std::lock_guard<std::mutex> lock(g_shapes_lock);
        s = g_shapes;
    }
    if (s.empty()) return 0;
    std::sort(s.begin(), s.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    // (route/result: state, program, uniform, storage, texture, call, other NVN calls, each count at most its cap)
    int n = std::snprintf(out, cap, "preparation shapes %zu (route/result: state program uniform storage texture call "
                                    "other = calls):", s.size());
    static constexpr int kOrder[HL_GFX_PREP_GROUPS] = {HL_GFX_PREP_GROUP_STATE,   HL_GFX_PREP_GROUP_PROGRAM,
                                                       HL_GFX_PREP_GROUP_UNIFORM, HL_GFX_PREP_GROUP_STORAGE,
                                                       HL_GFX_PREP_GROUP_TEXTURE, HL_GFX_PREP_GROUP_CALL,
                                                       HL_GFX_PREP_GROUP_OTHER};
    for (size_t i = 0; i < s.size() && i < 16 && n > 0 && (size_t)n < cap; i++) {
        const uint64_t k = s[i].first;
        uint32_t count[HL_GFX_PREP_GROUPS];
        uint32_t at = 5;
        for (int g = 0; g < HL_GFX_PREP_GROUPS; g++) {
            count[g] = (uint32_t)((k >> at) & ((1u << kShapeBits[g]) - 1));
            at += kShapeBits[g];
        }
        n += std::snprintf(out + n, cap - n, " %s/%u:", hl::gfx::route_name((PrepareRoute)(k & 15)),
                           (unsigned)((k >> 4) & 1));
        for (int g : kOrder)
            if (n > 0 && (size_t)n < cap) n += std::snprintf(out + n, cap - n, " %u", count[g]);
        if (n > 0 && (size_t)n < cap) n += std::snprintf(out + n, cap - n, " = %llu;", (unsigned long long)s[i].second);
    }
    return n < 0 ? 0 : ((size_t)n < cap ? n : (int)cap - 1);
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
