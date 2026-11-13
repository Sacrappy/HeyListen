// HeyListen native engine: the graphics front end's recording pass, its bridge and its check, run with no game
// (tests/test_native_engine.py).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hl/bridge/GfxBridge.h"
#include "hl/gfx/Nvn.h"
#include "hl/gfx/PassCheck.h"
#include "hl/gfx/Prepare.h"
#include "hl/gfx/RecordPass.h"
#include "hl/gfx/RenderState.h"
#include "hl/gfx/StateCheck.h"
#include "hl/gfx/SubmeshDraw.h"

using namespace hl;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {

// A guest memory of 4 KB at guest address 0x1000, and a record of the guest calls asked for.
struct FakeRuntime final : bridge::GuestRuntime {
    uint8_t memory[4096] = {};
    struct Call { bridge::GuestThread as; bridge::GuestAddr fn; uint64_t a0, a1; };
    std::vector<Call> calls;
    void* ptr(bridge::GuestAddr at) const override { return const_cast<uint8_t*>(memory) + (at - 0x1000); }
    bridge::GuestThread current() const override { return (void*)0x77; }
    uint64_t call(bridge::GuestThread as, bridge::GuestAddr fn, uint64_t a0, uint64_t a1) override {
        calls.push_back({as, fn, a0, a1});
        return 0;
    }
    uint64_t call_with(bridge::GuestThread as, bridge::GuestAddr fn, const uint64_t* args, unsigned nargs, const void*, unsigned) override {
        calls.push_back({as, fn, nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0});
        return 0;
    }
    void log(const char*) override {}
};

void the_pass_runs_the_phase_its_mode_names() {
    FakeRuntime rt;
    bridge::GfxBridge b(rt);
    gfx::RecordPass pass(b);
    const uint64_t args = 0x1100, listener = 0x5555;
    const uint32_t modes[] = {0, 1, 2, 0};
    for (uint32_t m : modes) {
        std::memcpy(rt.memory + 0x100 + 28, &m, 4);
        CHECK(pass.run(listener, args) == 1);
    }
    CHECK(rt.calls.size() == 3);   // (mode 2 records nothing)
    CHECK(rt.calls[0].fn == bridge::kScenePass && rt.calls[1].fn == bridge::kSecondPass && rt.calls[2].fn == bridge::kScenePass);
    for (const auto& c : rt.calls) CHECK(c.a0 == listener && c.a1 == args && c.as == (void*)0x77);
    CHECK(pass.stats().passes == 4 && pass.stats().scene == 2 && pass.stats().second == 1);
}

gfx::PassRecording recording(std::initializer_list<uint32_t> ops, uint64_t arg) {
    gfx::PassRecording r;
    for (uint32_t op : ops) {
        gfx::Command c;
        c.op = op;
        c.x[1] = arg;
        r.emit(c);
    }
    return r;
}

void the_shape_is_the_sequence_of_procedures() {
    CHECK(recording({1, 2, 3}, 0).shape() == recording({1, 2, 3}, 99).shape());   // (arguments do not count)
    CHECK(recording({1, 2, 3}, 0).shape() != recording({1, 3, 2}, 0).shape());
    CHECK(recording({1, 2}, 0).shape() != recording({1, 2, 2}, 0).shape());
    CHECK(recording({}, 0).shape() != recording({0}, 0).shape());
}

void the_check_compares_the_two_paths() {
    gfx::PassCheck check;
    unsigned native = 0;
    for (int i = 0; i < 1000; i++) native += check.next_path() == gfx::PassPath::Native;
    CHECK(native > 400 && native < 600);

    check.add(gfx::PassPath::Translated, 0, recording({1, 2, 3}, 0));
    check.add(gfx::PassPath::Translated, 0, recording({1, 2, 3, 3}, 0));
    check.add(gfx::PassPath::Native, 0, recording({1, 2, 3}, 5));
    check.add(gfx::PassPath::Native, 0, recording({1, 2, 3}, 6));
    gfx::PassCheck::Report r = check.report();
    CHECK(r.path[0].passes == 2 && r.path[1].passes == 2 && r.path[0].calls == 7 && r.path[1].calls == 6);
    CHECK(r.path[0].most_calls == 4 && r.path[0].shapes == 2 && r.path[1].shapes == 1);
    CHECK(r.path[1].unmatched == 0 && r.path[0].unmatched == 1 && r.path[0].unmatched_shapes == 1);
    // (procedures 1 and 2 once a pass on both paths; 3 1.5 times a pass translated, once native)
    CHECK(r.ops_compared == 3 && r.most_op == 3 && r.most_op_difference > 0.33 && r.most_op_difference < 0.34);

    // (the same stream in another mode is another shape; a native stream the translated path never made is unmatched)
    check.add(gfx::PassPath::Native, 1, recording({1, 2, 3}, 0));
    check.add(gfx::PassPath::Native, 0, recording({1, 2}, 0));
    r = check.report();
    CHECK(r.path[1].unmatched == 2 && r.path[1].unmatched_shapes == 2);
    CHECK(r.by_mode[0] == 5 && r.by_mode[1] == 1 && r.by_mode[2] == 0);
}

// ---- the render-state function ----

// Guest memory anywhere, 4 KB pages made on first touch.
struct SparseRuntime final : bridge::GuestRuntime {
    mutable std::map<uint64_t, std::vector<uint8_t>> pages;
    std::vector<std::pair<uint64_t, uint64_t>> writes;
    void* ptr(bridge::GuestAddr at) const override {
        std::vector<uint8_t>& p = pages[at >> 12];
        if (p.empty()) p.resize(4096 + 512);   // (an access may run past the page's end: one block at most)
        return p.data() + (at & 4095);
    }
    bridge::GuestThread current() const override { return nullptr; }
    uint64_t call(bridge::GuestThread, bridge::GuestAddr, uint64_t, uint64_t) override { return 0; }
    uint64_t call_with(bridge::GuestThread, bridge::GuestAddr, const uint64_t*, unsigned, const void*, unsigned) override { return 0; }
    void log(const char*) override {}
    void written(bridge::GuestAddr at, uint64_t n) override { writes.push_back({at, n}); }
    template <class T> void put(uint64_t at, T v) { std::memcpy(ptr(at), &v, sizeof v); }
    template <class T> T get(uint64_t at) const { T v; std::memcpy(&v, ptr(at), sizeof v); return v; }
};

struct Recorder final : gfx::CommandSink {
    std::vector<gfx::Command> calls;
    void emit(const gfx::Command& c) override { calls.push_back(c); }
};

struct NoOffsets final : gfx::PolygonOffsets {
    mutable int asked = 0;
    void at(int32_t slot, uint32_t& factor, uint32_t& units) const override {
        asked++;
        factor = 0x100 + (uint32_t)slot, units = 0x200 + (uint32_t)slot;
    }
};

void render_state_derives_depth_blend_and_slot() {
    NoOffsets offsets;
    gfx::StateInputs in;
    in.channel_masks = 0xffffffff;
    in.mode_bits = 0xf0;
    in.blend_target_mask = 0x5;
    in.offset_slot = 1;
    // (opaque: depth test, depth writes, less or equal, slot 1 already applied)
    gfx::StateOutputs out = gfx::RenderState::decide(in, offsets);
    CHECK(out.depth_test == 1 && out.depth_write == 1 && out.depth_func == 4 && out.depth_clamp == 0);
    CHECK(out.mode_bits == 0xf2 && out.blend_targets == 0 && !out.slot_changed && offsets.asked == 0);
    CHECK(out.done == 0xffffffff && out.channel_masks == 0xffffffff);
    // (blend kind 2, reversed depth: no depth writes, greater or equal, blending on the mask's targets, slot 2)
    in.key = 4;
    in.flags = 0x40;
    out = gfx::RenderState::decide(in, offsets);
    CHECK(out.depth_write == 0 && out.depth_func == 7 && out.blend_targets == 5 && out.mode_bits == 0xf3);
    CHECK(out.slot_changed && out.offset_slot == 2 && out.offset_clamp[0] == 0x102 && out.offset_clamp[1] == 0x202);
    CHECK(out.offset_clamp[2] == 0 && offsets.asked == 1);
    // (key bit 7: equal; no channel masks: greater; depth test off: always; flag 4 forces writes, flag 8 stops them)
    in.key = 0x80;
    in.flags = 0;
    CHECK(gfx::RenderState::decide(in, offsets).depth_func == 3);
    in.channel_masks = 0;
    in.flags = 0x40;
    CHECK(gfx::RenderState::decide(in, offsets).depth_func == 5);
    in.flags = 0x20 | 0x4;
    out = gfx::RenderState::decide(in, offsets);
    CHECK(out.depth_test == 0 && out.depth_func == 8 && out.depth_write == 1);
    in.flags = 0x4 | 0x8;
    CHECK(gfx::RenderState::decide(in, offsets).depth_write == 0);
    // (depth mode 5: writes on, the function by channel masks; 4: not equal, no writes)
    in.flags = 0;
    in.key = 6;
    in.depth_mode = 5;
    in.channel_masks = 1;
    out = gfx::RenderState::decide(in, offsets);
    CHECK(out.depth_func == 4 && out.depth_write == 1);
    in.depth_mode = 4;
    out = gfx::RenderState::decide(in, offsets);
    CHECK(out.depth_func == 6 && out.depth_write == 0);
    // (depth mode 8: the slot from the block, 32-bit: -5 + 4 is slot 0xffffffff, stored as 0xff)
    in.depth_mode = 8;
    in.offset_slot_base = -5;
    in.offset_slot = 0xff;
    out = gfx::RenderState::decide(in, offsets);
    CHECK(out.slot_changed && out.offset_slot == 0xff && out.offset_clamp[0] == 0xff);
}

void render_state_records_the_calls_in_order() {
    NoOffsets offsets;
    gfx::StateInputs in;
    in.key = 2;
    in.blend_target_mask = 0x81;
    in.channel_masks = 0x000000f1;
    in.stencil_test = 1;
    in.stencil_ref = 0x22;
    in.stencil_value_mask = 0x33;
    in.polygon_offset = 1;
    for (int t = 0; t < 8; t++)
        for (int i = 0; i < 6; i++) in.blend[t][i] = (uint8_t)(t * 16 + i);
    const gfx::StateOutputs out = gfx::RenderState::decide(in, offsets);
    gfx::StateHandles at{0xcb, 0xb1, 0x10, 0x20, 0x30, 0x40, 0x50};
    Recorder r;
    gfx::RenderState::emit(in, out, at, r);
    CHECK(r.calls.size() == 2 + 2 * 5 + 3 + 9 + 7 + 3 + 7 && r.calls.size() <= gfx::RenderState::kMostCommands);
    using gfx::Proc;
    CHECK(r.calls[0].op == (uint32_t)Proc::BlendStateSetDefaults && r.calls[0].x[0] == 0x10);
    CHECK(r.calls[2].op == (uint32_t)Proc::ColorStateSetBlendEnable && r.calls[2].x[1] == 0 && r.calls[2].x[2] == 1);
    // (target 7's factors in the order the procedure takes them: source and destination RGB, then alpha)
    const gfx::Command& f7 = r.calls[2 + 5 + 2];
    CHECK(f7.op == (uint32_t)Proc::BlendStateSetBlendFunc && f7.x[1] == 0x70 && f7.x[2] == 0x72 && f7.x[3] == 0x71 &&
          f7.x[4] == 0x73);
    CHECK(r.calls[2 + 5 + 4].op == (uint32_t)Proc::CommandBufferBindBlendState && r.calls[2 + 5 + 4].x[0] == 0xcb);
    const size_t m = 2 + 10 + 3;
    CHECK(r.calls[m - 1].op == (uint32_t)Proc::CommandBufferSetBlendColor && r.calls[m - 1].x[1] == 0xb1);
    CHECK(r.calls[m].op == (uint32_t)Proc::ChannelMaskStateSetChannelMask && r.calls[m].x[2] == 1 && r.calls[m].x[3] == 0);
    CHECK(r.calls[m + 1].x[1] == 1 && r.calls[m + 1].x[2] == 1 && r.calls[m + 1].x[5] == 1);
    CHECK(r.calls[m + 2].x[2] == 0);
    const size_t s = m + 9 + 7;
    CHECK(r.calls[s].op == (uint32_t)Proc::CommandBufferSetStencilValueMask && r.calls[s].x[2] == 0x33);
    CHECK(r.calls[s + 2].op == (uint32_t)Proc::CommandBufferSetStencilRef && r.calls[s + 2].x[2] == 0x22);
    CHECK(r.calls[s + 6].op == (uint32_t)Proc::PolygonStateSetPolygonOffsetEnables && r.calls[s + 6].x[1] == 7);
    const gfx::Command& clamp = r.calls[r.calls.size() - 2];
    CHECK(clamp.op == (uint32_t)Proc::CommandBufferSetPolygonOffsetClamp && clamp.f[0] == out.offset_clamp[0]);
    // (flag bit 17: nothing recorded)
    in.flags = 1ull << 17;
    Recorder none;
    gfx::RenderState::emit(in, gfx::RenderState::decide(in, offsets), at, none);
    CHECK(none.calls.empty());
}

void the_bridge_reads_and_writes_the_block_in_place() {
    SparseRuntime rt;
    const uint64_t block = 0x7000080, material = 0x20000, callback = 0x30000, renderer = 0x40000, sp = 0x7000000;
    rt.put<uint64_t>(bridge::kEmptyFunctionSlot, 0x835c3bb0);
    rt.put<uint64_t>(block + 24, material);
    rt.put<uint64_t>(block + 40, renderer);
    rt.put<uint64_t>(renderer + 216, 0xc0ffee);
    rt.put<uint64_t>(material + 1384, callback);
    rt.put<uint64_t>(callback + 16, 0x835c3bb0);
    rt.put<uint32_t>(block + 112, 0xffffffff);
    rt.put<uint8_t>(block + 88, 1);
    rt.put<uint64_t>(block + 160, 0x1111);
    rt.put<uint64_t>(block + 168, 0x2222);
    rt.put<uint32_t>(material + 1224 + 16, 0xaa);   // (slot 2: units, factor)
    rt.put<uint32_t>(material + 1228 + 16, 0xbb);
    bridge::StateBridge sb(rt);
    gfx::StateInputs in;
    gfx::StateHandles at;
    CHECK(sb.read(block, 0x104, sp, in, at));
    CHECK(in.key == 4 && in.channel_masks == 0xffffffff && in.offset_slot == 1 && in.blend_color[1] == 0x2222);
    CHECK(at.command_buffer == 0xc0ffee && at.blend_color == block + 248 && at.blend == sp - 176 + 8);
    CHECK(at.polygon == sp - 176 + 60 && at.color == sp - 176 + 72 && at.channel_mask == sp - 176 + 76);
    const gfx::StateOutputs out = gfx::RenderState::decide(in, sb.offsets(block));
    CHECK(out.slot_changed && out.offset_clamp[0] == 0xbb && out.offset_clamp[1] == 0xaa);
    // (what the bytes should be, and what writing in place leaves: the same)
    uint8_t expected[bridge::StateBridge::kBlockSize];
    sb.copy(block, expected);
    bridge::StateBridge::expect(expected, out);
    sb.write(block, out);
    uint8_t actual[bridge::StateBridge::kBlockSize];
    sb.copy(block, actual);
    CHECK(std::memcmp(expected, actual, sizeof actual) == 0);
    CHECK(rt.get<uint8_t>(block + 90) == 4 && rt.get<uint64_t>(block + 256) == 0x2222 && rt.get<uint8_t>(block + 88) == 2);
    CHECK(rt.get<uint32_t>(block + 152) == 0xffffffff && rt.get<uint32_t>(block + 328) == 0xbb);
    CHECK(rt.writes.size() == 1 && rt.writes[0].first == block + 88 && rt.writes[0].second == 351 - 88);
    // (a state callback set: not answered)
    rt.put<uint64_t>(callback + 16, 0x1234);
    CHECK(!sb.read(block, 0x104, sp, in, at));
    // (the procedures' globals: one a procedure)
    for (uint32_t p = 0; p < gfx::kProcCount; p++) CHECK(bridge::proc_global(p) >= 0x84d67000 && bridge::proc_global(p) < 0x84d68000);
    CHECK(bridge::proc_global(gfx::kProcCount) == 0);
}

void the_nvn_sink_passes_each_procedures_arguments() {
    struct Seen { uint32_t proc, nx, nf; uint64_t x0; uint32_t f2; };
    static std::vector<Seen> seen;
    gfx::NvnEntryPoints nvn;
    nvn.call = [](void*, uint32_t proc, const uint64_t* x, uint32_t nx, const uint32_t* f, uint32_t nf) -> uint64_t {
        seen.push_back({proc, nx, nf, x[0], nf > 2 ? f[2] : 0});
        return 0x5000 + proc;
    };
    gfx::NvnSink sink(nvn);
    gfx::Command c = gfx::command(gfx::Proc::CommandBufferSetPolygonOffsetClamp, 0x99);
    c.f[2] = 0x3f800000;
    sink.emit(c);
    sink.emit(gfx::command(gfx::Proc::ChannelMaskStateSetChannelMask, 0x77, 1, 1, 1, 1, 1));
    CHECK(seen.size() == 2 && seen[0].nx == 1 && seen[0].nf == 3 && seen[0].x0 == 0x99 && seen[0].f2 == 0x3f800000);
    CHECK(seen[1].proc == (uint32_t)gfx::Proc::ChannelMaskStateSetChannelMask && seen[1].nx == 6 && seen[1].nf == 0);
    // (a query is answered by asking, and not sent again by the sink; the indexed draw passes all eight registers)
    const gfx::Command q = gfx::command(gfx::Proc::BufferGetAddress, 0x42);
    CHECK(nvn.ask(q) == 0x5000 + (uint32_t)gfx::Proc::BufferGetAddress && seen.size() == 3 && seen[2].nx == 1);
    sink.emit(q);
    CHECK(seen.size() == 3);
    sink.emit(gfx::command(gfx::Proc::CommandBufferDrawElementsInstanced, 1, 2, 3, 4, 5, 6, 7, 8));
    CHECK(seen.size() == 4 && seen[3].nx == 8);
    for (uint32_t p = 0; p < gfx::kProcCount; p++) CHECK(gfx::proc_name(p) && std::strncmp(gfx::proc_name(p), "nvn", 3) == 0);
    CHECK(!gfx::proc_name(gfx::kProcCount));
}

void the_state_check_compares_calls_and_bytes() {
    static std::vector<std::string> lines;
    gfx::StateCheck check("render state", [](void*, const char* line) { lines.push_back(line); }, nullptr, 1);
    std::vector<gfx::Command> a = {gfx::command(gfx::Proc::ColorStateSetLogicOp, 0x10, 3),
                                   gfx::command(gfx::Proc::CommandBufferBindColorState, 0xcb, 0x10)};
    std::vector<gfx::Command> b = a;
    b[0].x[4] = 0xdead;   // (not an argument of the procedure: not compared)
    uint8_t x[8] = {1, 2, 3}, y[8] = {1, 2, 3};
    CHECK(check.compare(0x100, 4, a, b, x, y, sizeof x));
    b[1].x[1] = 0x11;
    CHECK(!check.compare(0x100, 4, a, b, x, y, sizeof x));
    y[5] = 9;
    b = a;
    CHECK(!check.compare(0x100, 4, a, b, x, y, sizeof x));
    b.pop_back();
    CHECK(!check.compare(0x100, 4, a, b, x, x, sizeof x));
    check.deferred();
    const gfx::StateCheck::Report r = check.report();
    CHECK(r.calls == 4 && r.commands == 7 && r.mismatched_calls == 3 && r.command_mismatches == 2);
    CHECK(r.block_mismatches == 1 && r.deferred == 1);
    // (only the first mismatch in full, with its differing call)
    CHECK(lines.size() == 2 && lines[1].find("nvnCommandBufferBindColorState(0xcb, 0x10)") != std::string::npos);
    CHECK(lines[0].rfind("render state: mismatch", 0) == 0);
    // (a path that changed is a mismatch of the calls even when the lists agree)
    CHECK(!check.compare(0x100, 4, a, a, x, x, sizeof x, true));
    CHECK(check.report().command_mismatches == 3);
}

// ---- the sub-mesh submission ----

void the_prologue_sets_the_geometry_mode_when_it_changes() {
    gfx::PrologueInputs in;
    in.sub_mesh = 2;
    in.sub_meshes = 2;
    CHECK(!gfx::SubmeshDraw::prologue(in).in_range);
    in.sub_mesh = -1;   // (a signed comparison, as the game's)
    CHECK(gfx::SubmeshDraw::prologue(in).in_range);
    in.sub_mesh = 1;
    in.geometry_mode = 1;
    in.applied_mode = 1;
    in.mode_bits = 0x20;
    gfx::PrologueOutputs out = gfx::SubmeshDraw::prologue(in);
    CHECK(out.in_range && !out.mode_changed && out.mode_bits == 0x20);
    Recorder r;
    gfx::SubmeshDraw::emit_prologue(out, 0xcb, r);
    CHECK(r.calls.empty());
    in.applied_mode = 0;
    out = gfx::SubmeshDraw::prologue(in);
    CHECK(out.mode_changed && out.applied_mode == 1 && out.mode_bits == 0x4020);
    gfx::SubmeshDraw::emit_prologue(out, 0xcb, r);
    CHECK(r.calls.size() == 1 && r.calls[0].op == (uint32_t)gfx::Proc::CommandBufferSetGeometryProcessingMode);
    CHECK(r.calls[0].x[0] == 0xcb && r.calls[0].x[1] == 1);
    in.geometry_mode = 2;
    in.mode_bits = 0x4021;
    out = gfx::SubmeshDraw::prologue(in);
    CHECK(out.mode_changed && out.applied_mode == 2 && out.mode_bits == 0x21);
}

void the_submission_picks_its_path_and_counts() {
    gfx::SubmitInputs in;
    in.prepared = 2;   // (bit 0 clear: nothing at all)
    CHECK(gfx::SubmeshDraw::decide(in, nullptr).how == gfx::Submit::None && !gfx::SubmeshDraw::decide(in, nullptr).counted);
    in.prepared = 1;
    in.recorded = 1;
    in.mode_bits = 0x20;
    in.instances = 1;
    in.triangles = 0xfffffff0;
    in.sub_mesh_triangles = 0x20;   // (32-bit counter)
    gfx::SubmitOutputs out = gfx::SubmeshDraw::decide(in, nullptr);
    CHECK(out.how == gfx::Submit::CallCommands && out.marked && out.counted && out.triangles == 0x10);
    gfx::SubmitHandles at{0xcb, 0x9050};
    Recorder r;
    gfx::SubmeshDraw::emit(out, nullptr, 0, at, r);
    CHECK(r.calls.size() == 1 && r.calls[0].op == (uint32_t)gfx::Proc::CommandBufferCallCommands);
    CHECK(r.calls[0].x[0] == 0xcb && r.calls[0].x[1] == 1 && r.calls[0].x[2] == 0x9050);
    // (more than one instance, no list, or lists not allowed: by the mesh kind)
    in.instances = 3;
    CHECK(gfx::SubmeshDraw::path(in) == gfx::Submit::Elements);
    in.instances = 0;
    in.mode_bits = 0;
    gfx::IndexedInputs ix;
    ix.first_offset = 0x100;
    ix.last_offset = 0x40;   // (a negative span shifts arithmetically, as the game's)
    ix.last_count = 100;
    ix.index_shift = 1;
    ix.mode = 4, ix.type = 2, ix.base_vertex = 7, ix.buffer = 0xb0;
    out = gfx::SubmeshDraw::decide(in, &ix);
    CHECK(out.how == gfx::Submit::Elements && !out.marked && out.instances == 1 && out.count == 100u - 0x60u);
    r.calls.clear();
    gfx::SubmeshDraw::emit(out, &ix, 0x10000, at, r);
    CHECK(r.calls.size() == 2 && r.calls[0].op == (uint32_t)gfx::Proc::BufferGetAddress && r.calls[0].x[0] == 0xb0);
    const gfx::Command& d = r.calls[1];
    CHECK(d.op == (uint32_t)gfx::Proc::CommandBufferDrawElementsInstanced && d.x[0] == 0xcb && d.x[1] == 4 && d.x[2] == 2);
    CHECK(d.x[3] == 100u - 0x60u && d.x[4] == 0x10100 && d.x[5] == 7 && d.x[6] == 0 && d.x[7] == 1);
    in.mesh_kind = 2;
    in.mesh_tasks = 65;
    out = gfx::SubmeshDraw::decide(in, nullptr);
    CHECK(out.how == gfx::Submit::MeshTasks && out.tasks == 3);
    in.mesh_kind = 1;
    CHECK(gfx::SubmeshDraw::decide(in, nullptr).tasks == 65);
    r.calls.clear();
    gfx::SubmeshDraw::emit(gfx::SubmeshDraw::decide(in, nullptr), nullptr, 0, at, r);
    CHECK(r.calls.size() == 1 && r.calls[0].op == (uint32_t)gfx::Proc::CommandBufferDrawMeshTasks && r.calls[0].x[1] == 0 &&
          r.calls[0].x[2] == 65);
    in.mesh_kind = 3;   // (draws nothing, still counted)
    out = gfx::SubmeshDraw::decide(in, nullptr);
    CHECK(out.how == gfx::Submit::None && out.counted);
}

void the_draw_bridge_reads_and_writes_in_place() {
    SparseRuntime rt;
    const uint64_t drawable = 0x100000, block = 0x200000, context = 0x300000, cb = 0x310000, model = 0x400000,
                   mesh = 0x500000, descs = 0x600000, records = 0x700000, ranges = 0x800000, holder = 0x900000;
    rt.put<uint64_t>(bridge::kInlineCallSwitchSlot, 0xa00000);
    rt.put<uint32_t>(0xa00000, 0);
    rt.put<uint16_t>(drawable + 0x1e, 3 << 4);
    rt.put<uint32_t>(drawable + 0x54, 0x80);
    rt.put<uint16_t>(drawable + 0x5c, 2);
    rt.put<uint64_t>(drawable + 0x68, model);
    rt.put<uint64_t>(block + 0x28, context);
    rt.put<uint8_t>(block + 0x3b, 0);
    rt.put<uint32_t>(block + 0x50, 10);
    rt.put<uint32_t>(block + 0x5c, 0x20);
    rt.put<uint64_t>(context + 0xd8, cb);
    rt.put<uint64_t>(model + 0x8, mesh);
    rt.put<uint64_t>(model + 0x638, records);
    rt.put<uint8_t>(model + 0x69e, 1);
    rt.put<uint8_t>(model + 0x69f, 0);
    rt.put<uint64_t>(mesh + 0x18, descs);
    const uint64_t desc = descs + 2 * 0x38, record = records + 2 * 0x270;
    rt.put<uint64_t>(desc, ranges);
    rt.put<uint64_t>(desc + 0x10, holder);
    rt.put<uint64_t>(holder + 8, 0xbbbb);
    rt.put<uint32_t>(desc + 0x24, 3);
    rt.put<uint32_t>(desc + 0x28, 1);
    rt.put<uint32_t>(desc + 0x30, 9);
    rt.put<uint16_t>(desc + 0x34, 2);
    rt.put<uint32_t>(ranges, 0x20);
    rt.put<uint32_t>(ranges + 8, 0x60);
    rt.put<uint32_t>(ranges + 12, 5);
    rt.put<uint32_t>(bridge::kPrimitiveTable + 12, 0x44);
    rt.put<uint32_t>(bridge::kIndexTypeTable + 4, 0x55);
    rt.put<uint8_t>(record + 0x58, 1);
    rt.put<uint32_t>(record + 0x268, 7);
    bridge::DrawBridge db(rt);
    // (the route: by the flags, the mask and the switch)
    CHECK(db.route(drawable, 0) == bridge::SubmitRoute::Direct);
    CHECK(db.route(drawable, 0x80) == bridge::SubmitRoute::Direct);   // (bit 7 alone is not a path of its own)
    rt.put<uint32_t>(drawable + 0x54, 0x81);
    CHECK(db.route(drawable, 0x01) == bridge::SubmitRoute::Clusters && db.route(drawable, 0x80) == bridge::SubmitRoute::Direct);
    rt.put<uint32_t>(drawable + 0x54, 0x28);
    CHECK(db.route(drawable, 8) == bridge::SubmitRoute::Query && db.route(drawable, 0) == bridge::SubmitRoute::Callback);
    rt.put<uint32_t>(drawable + 0x54, 0);
    rt.put<uint32_t>(0xa00000, 1);
    CHECK(db.route(drawable, 0) == bridge::SubmitRoute::Inline);
    rt.put<uint32_t>(0xa00000, 0);

    gfx::PrologueInputs pin;
    uint64_t ctx = 0;
    db.read_prologue(drawable, block, 2, pin, ctx);
    CHECK(ctx == context && pin.sub_meshes == 3 && pin.geometry_mode == 1 && pin.applied_mode == 0 && pin.mode_bits == 0x20);
    const gfx::PrologueOutputs pout = gfx::SubmeshDraw::prologue(pin);
    uint8_t expected[bridge::DrawBridge::kSnapshotSize], actual[bridge::DrawBridge::kSnapshotSize];
    db.snapshot(block, cb, expected);
    bridge::DrawBridge::expect_prologue(expected, pout);
    db.write_prologue(block, pout);
    db.snapshot(block, cb, actual);
    CHECK(std::memcmp(expected, actual, sizeof actual) == 0);
    CHECK(rt.get<uint8_t>(block + 0x3b) == 1 && rt.get<uint32_t>(block + 0x5c) == 0x4020 && db.command_buffer(ctx) == cb);

    // (two instances: the indexed draw)
    gfx::SubmitInputs in;
    gfx::SubmitHandles at;
    db.read_submit(drawable, block, 2, 1, ctx, in, at);
    CHECK(in.instances == 2 && in.recorded == 1 && in.triangles == 10 && in.sub_mesh_triangles == 7);
    CHECK(at.command_buffer == cb && at.command_list == record + 0x50);
    CHECK(gfx::SubmeshDraw::path(in) == gfx::Submit::Elements);
    gfx::IndexedInputs ix;
    db.read_indexed(drawable, 2, ix);
    CHECK(ix.first_offset == 0x20 && ix.last_offset == 0x60 && ix.last_count == 5 && ix.index_shift == 1);
    CHECK(ix.mode == 0x44 && ix.type == 0x55 && ix.base_vertex == 9 && ix.buffer == 0xbbbb);
    gfx::SubmitOutputs out = gfx::SubmeshDraw::decide(in, &ix);
    CHECK(out.count == 5 + 0x20 && out.instances == 2);
    // (one instance: the pre-recorded list, the command buffer marked)
    rt.put<uint16_t>(drawable + 0x5c, 1);
    db.read_submit(drawable, block, 2, 1, ctx, in, at);
    out = gfx::SubmeshDraw::decide(in, nullptr);
    CHECK(out.how == gfx::Submit::CallCommands);
    db.snapshot(block, cb, expected);
    bridge::DrawBridge::expect_submit(expected, out);
    rt.writes.clear();
    db.write_submit(block, at, out);
    db.snapshot(block, cb, actual);
    CHECK(std::memcmp(expected, actual, sizeof actual) == 0);
    CHECK(rt.get<uint8_t>(cb + 0x10) == 1 && rt.get<uint32_t>(block + 0x50) == 17 && rt.writes.size() == 2);
    // (not prepared: nothing read past the result)
    db.read_submit(drawable, block, 2, 0, ctx, in, at);
    CHECK(in.prepared == 0 && at.command_buffer == 0);
}

// ---- the shared preparation ----

gfx::PrepareInputs cached_inputs() {
    gfx::PrepareInputs in;
    in.material = true;
    in.state_key = in.applied_state_key = 4;
    in.mode_bits = 0x8;
    in.program = in.applied_program = 0x1234;
    in.list_flags = 3;
    return in;
}

void the_preparation_decides_its_route_in_order() {
    gfx::PrepareInputs in = cached_inputs();
    gfx::PrepareOutputs out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::Cached && out.result == 1 && out.marked && out.call_list && !out.mask_written);
    in.list_flags = 1;   // (present, not called: nothing recorded)
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::Cached && !out.marked && !out.call_list);

    in = cached_inputs();
    in.material = false;
    in.state_key = 9;   // (the material is tested first)
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::NoMaterial && out.result == 0 && gfx::Prepare::native(out.route));

    in = cached_inputs();
    in.applied_state_key = 5;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::StateChanged);
    in = cached_inputs();
    in.material_state = 7;   // (compared only with mode bit 0)
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);
    in.mode_bits |= 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::StateChanged);
    in.applied_material_state = 7;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);

    in = cached_inputs();
    in.applied_program = 0x5678;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramChanged && !gfx::Prepare::native(out.route) && !out.call_list);

    // (pending bits: less the drawable's; all applied already: written back, else left)
    in = cached_inputs();
    in.pending = 0x0f;
    in.drawable_mask = 0x03;
    in.applied_mask = 0xfc;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::Cached && out.mask_written && out.applied_mask == 0x0c);
    in.applied_mask = 0x04;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Masked);

    in = cached_inputs();
    in.view_blocks = true;
    in.drawable_block = 2;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::DrawableBlock);
    in.block_binding = 0xffffffffu;   // (no binding: nothing to bind)
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);
    in.block_binding = 0;
    in.applied_drawable_block = 2;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);

    in = cached_inputs();
    in.mode_bits = 0;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Streams);
    in = cached_inputs();
    in.list_flags = 2;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Streams);
    in = cached_inputs();
    in.inline_switch = true;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Inline);
    in.list_flags = 1;   // (the switch matters only for a call)
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);

    in = cached_inputs();
    in.cluster = 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);
    in.drawable_kinds = 4;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Clusters);
    in.drawable_kinds = 0;
    in.cluster_count = 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Clusters);

    Recorder r;
    gfx::PrepareHandles at;
    at.command_buffer = 0xcb, at.command_list = 0x710;
    gfx::Prepare::emit(gfx::Prepare::decide(cached_inputs()), at, r);
    CHECK(r.calls.size() == 1 && r.calls[0].op == (uint32_t)gfx::Proc::CommandBufferCallCommands);
    CHECK(r.calls[0].x[0] == 0xcb && r.calls[0].x[1] == 1 && r.calls[0].x[2] == 0x710);
    CHECK(std::string(gfx::route_name(gfx::PrepareRoute::Cached)) == "cached");
}

void the_prepare_bridge_reads_in_the_functions_order() {
    SparseRuntime rt;
    const uint64_t drawable = 0x100000, block = 0x200000, context = 0x300000, cb = 0x310000, materials = 0x400000,
                   material = 0x500000, lists = 0x600000, state = 0x700000, fallback = 0x800000;
    rt.put<uint64_t>(bridge::kInlineCallSwitchSlot, 0xa00000);
    rt.put<uint32_t>(0xa00000, 0);
    rt.put<uint64_t>(bridge::kDefaultMaterialSlot, fallback);
    // (the pass slot from the drawable: block pass 2 -> slot 3 -> material entry 2 of stride 0x50)
    rt.put<uint8_t>(block + 0x62, 0xff);
    rt.put<uint8_t>(block + 0x39, 2);
    rt.put<uint8_t>(drawable + 0x2c + 2, 3);
    rt.put<int8_t>(drawable + 0x3b + 3, 2);
    rt.put<uint8_t>(drawable + 0x5f, 0x50);
    rt.put<uint64_t>(drawable + 0x60, materials);
    const uint64_t entry = materials + 2 * 0x50;
    CHECK(bridge::PrepareBridge(rt).material(block, drawable) == entry);
    rt.put<uint8_t>(block + 0x62, 0xfe);   // (the block's slot, signed: -2)
    rt.put<int8_t>(drawable + 0x3b - 2, -1);
    CHECK(bridge::PrepareBridge(rt).material(block, drawable) == fallback);
    rt.put<uint8_t>(block + 0x62, 3);

    rt.put<uint64_t>(entry, 1);
    rt.put<uint64_t>(entry + 8, 0x1234);
    rt.put<uint8_t>(drawable + 0x58, 4);
    rt.put<uint8_t>(block + 0x59, 4);
    rt.put<uint32_t>(block + 0x5c, 0x9);
    rt.put<uint64_t>(drawable + 0x70, state);
    rt.put<uint32_t>(state + 0x154, 77);
    rt.put<uint32_t>(block + 0x98, 77);
    rt.put<uint64_t>(block + 0xd8, 0x1234);
    rt.put<uint16_t>(block + 0xb0, 0x0f);
    rt.put<uint16_t>(drawable + 0x14, 0x03);
    rt.put<uint16_t>(block + 0xb2, 0xfc);
    rt.put<uint64_t>(block + 0x18, material);
    rt.put<uint64_t>(material + 0x518, 0);
    rt.put<uint64_t>(drawable + 0x8, lists);
    rt.put<uint16_t>(lists + 0x14, 0x6);   // (list 2)
    rt.put<uint8_t>(entry + 0x10 + 2 * 16 + 8, 3);
    rt.put<uint64_t>(block + 0x28, context);
    rt.put<uint64_t>(context + 0xd8, cb);

    bridge::PrepareBridge pb(rt);
    gfx::PrepareInputs in;
    gfx::PrepareHandles at;
    pb.read(block, drawable, in, at);
    CHECK(in.material && in.mode_bits == 9 && in.material_state == 77 && in.applied_material_state == 77);
    CHECK(in.program == 0x1234 && in.pending == 0x0f && in.drawable_mask == 0x03 && in.applied_mask == 0xfc);
    CHECK(!in.view_blocks && in.list_flags == 3 && !in.inline_switch && in.cluster == 0);
    CHECK(at.command_buffer == cb && at.command_list == entry + 0x10 + 2 * 16);
    const gfx::PrepareOutputs out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::Cached);
    uint8_t expected[bridge::PrepareBridge::kSnapshotSize], actual[bridge::PrepareBridge::kSnapshotSize];
    pb.snapshot(block, at.command_buffer, expected);
    bridge::PrepareBridge::expect(expected, out);
    rt.writes.clear();
    pb.write(block, at, out);
    pb.snapshot(block, at.command_buffer, actual);
    CHECK(std::memcmp(expected, actual, sizeof actual) == 0);
    CHECK(rt.get<uint16_t>(block + 0xb2) == 0x0c && rt.get<uint8_t>(cb + 0x10) == 1 && rt.writes.size() == 2);

    // (the reads stop where the call leaves the path: another program, and nothing past it is read)
    rt.put<uint64_t>(block + 0xd8, 0x9999);
    pb.read(block, drawable, in, at);
    CHECK(in.applied_program == 0x9999 && in.pending == 0 && in.list_flags == 0 && at.command_buffer == 0);
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    rt.put<uint64_t>(block + 0xd8, 0x1234);
    // (the per-drawable blocks' table present and the entry changed with a binding: left)
    rt.put<uint64_t>(material + 0x518, 0xdead);
    rt.put<uint8_t>(drawable + 0x5a, 5);
    rt.put<uint32_t>(block + 0x80, 0);
    pb.read(block, drawable, in, at);
    CHECK(in.view_blocks && in.drawable_block == 5 && gfx::Prepare::decide(in).route == gfx::PrepareRoute::DrawableBlock);
    rt.put<uint32_t>(block + 0x80, 0xffffffffu);
    // (the inline switch: left before the command buffer is written)
    rt.put<uint32_t>(0xa00000, 1);
    pb.read(block, drawable, in, at);
    CHECK(in.inline_switch && gfx::Prepare::decide(in).route == gfx::PrepareRoute::Inline);
    rt.put<uint32_t>(0xa00000, 0);
    // (an empty material entry: nothing else read)
    rt.put<uint64_t>(entry, 0);
    pb.read(block, drawable, in, at);
    CHECK(!in.material && in.state_key == 0 && gfx::Prepare::decide(in).route == gfx::PrepareRoute::NoMaterial);
}

}   // namespace

int main() {
    the_pass_runs_the_phase_its_mode_names();
    the_shape_is_the_sequence_of_procedures();
    the_check_compares_the_two_paths();
    render_state_derives_depth_blend_and_slot();
    render_state_records_the_calls_in_order();
    the_bridge_reads_and_writes_the_block_in_place();
    the_nvn_sink_passes_each_procedures_arguments();
    the_state_check_compares_calls_and_bytes();
    the_prologue_sets_the_geometry_mode_when_it_changes();
    the_submission_picks_its_path_and_counts();
    the_draw_bridge_reads_and_writes_in_place();
    the_preparation_decides_its_route_in_order();
    the_prepare_bridge_reads_in_the_functions_order();
    std::printf("gfx: ok\n");
    return 0;
}
