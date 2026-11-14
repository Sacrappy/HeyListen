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
    for (uint32_t p = 0; p < gfx::kProcCount; p++) CHECK(bridge::proc_global(p) >= 0x84d66f00 && bridge::proc_global(p) < 0x84d68600);   // (the loader's globals)
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
    in.state_key = in.applied_state_key = in.state.applied_key = 4;
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
    CHECK(gfx::Prepare::decide(in, false).route == gfx::PrepareRoute::StateChanged);
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::StateApplied && out.state_step && out.state.applied_key == 4 && out.call_list);
    in = cached_inputs();
    in.material_state = 7;   // (compared only with mode bit 0)
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);
    in.mode_bits |= 1;
    in.material_state_in.id = 7;
    CHECK(gfx::Prepare::decide(in, false).route == gfx::PrepareRoute::StateChanged);
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::StateApplied && out.state.applied_material == 7 && !out.state_recorded);
    in.applied_material_state = 7;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Cached);

    // (another program with the interface applied: bound alone, the block told)
    in = cached_inputs();
    in.applied_program = 0x5678;
    in.context = 0x9000;
    in.program_object = 0xabc, in.program_stages = 0x11;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBound && gfx::Prepare::native(out.route) && out.call_list);
    CHECK(out.program_bound && out.program == 0x1234 && out.binder == 0x9188 && out.mode_written && out.mode_bits == 0x8);
    CHECK(!out.id_written[0] && !out.id_written[1] && !out.id_written[2] && !out.mask_cleared && !out.binding_written);
    // (the defaults: bit 12 and 13 follow the program's bits 7 and 8; a part skipped when both say defaults)
    in.program_flags = 0x180;
    in.mode_bits = 0x8 | 0x1000;
    in.interface_id[0] = 5;   // (not read: skipped)
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBound && out.mode_bits == (0x8 | 0x1000 | 0x2000) && !out.id_written[0]);
    // (a new first id with the same words: recorded and the applied bits cleared; the third word differs: no bind,
    // the per-drawable block's word and entry rewritten)
    in = cached_inputs();
    in.applied_program = 0x5678;
    in.interface_id[0] = 5;
    in.words[2] = 0x33;
    in.pending = 1, in.applied_mask = 1;   // (cleared by the program step: the pending bit is then left)
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::Masked);
    in.pending = 0;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBound && out.id_written[0] && out.ids[0] == 5 && out.mask_cleared);
    CHECK(out.binding_written && out.block_binding == 0x33);
    in.view_blocks = true;   // (the entry is now 0xff and the word not -1: the block is bound by the translation)
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::DrawableBlock);
    in.view_blocks = false;
    in.words[1] = 2;   // (a bind)
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramChanged && !gfx::Prepare::native(out.route) && !out.program_bound);
    // (the second part, then the textures: a new id with slots binds them, unless bit 15)
    in = cached_inputs();
    in.applied_program = 0x5678;
    in.interface_id[1] = 9;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBound && out.id_written[1] && out.mask_cleared);
    in.words[4] = 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    in = cached_inputs();
    in.applied_program = 0x5678;
    in.interface_id[2] = 4;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBound && out.id_written[2] && !out.mask_cleared);
    in.texture_mask = 0x40000000;   // (above the 30 slots: nothing to bind)
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramBound);
    in.texture_mask = 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    in.mode_bits |= 0x8000;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramBound);

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

void the_preparation_state_step_derives_and_records() {
    gfx::MaterialState none;
    // (the key: offset enables, cull face and polygon mode from the bits, mode bit 9 forcing the two low bits)
    gfx::PrepareState s;
    s.applied_key = 1;
    s.offset_enables = 0x10;
    s.cull_face = 3, s.polygon_mode[0] = s.polygon_mode[1] = 2;
    CHECK(!gfx::Prepare::apply_state(s, 1, 0, 0, 0, 0, none));   // (same key: nothing)
    CHECK(gfx::Prepare::apply_state(s, 0x14, 0, 0, 0, 0, none));   // (bit 2: offsets on; bits 4-5 = 1: mode 1)
    CHECK(s.applied_key == 0x14 && s.offset_enables == 0x11 && s.cull_face == 3 && s.polygon_mode[0] == 1 && s.polygon_mode[1] == 1);
    CHECK(gfx::Prepare::apply_state(s, 0x08, 0x05, 0, 0, 0, none));   // (block bits 0x05 | key 0x08 = 0x0d: cull 1, mode 2)
    CHECK(s.cull_face == 1 && s.polygon_mode[0] == 2 && (s.offset_enables & 1) == 1);
    CHECK(gfx::Prepare::apply_state(s, 0x20, 0, 0x200, 0, 0, none));   // (mode bit 9: key | 3 = 0x23: cull 0, mode 0)
    CHECK(s.cull_face == 0 && s.polygon_mode[0] == 0 && (s.offset_enables & 1) == 0);
    // (bit 0 equal but bits 1, 2 not: rewritten, bit 0 only)
    s = gfx::PrepareState{};
    s.offset_enables = 0x06, s.cull_face = 3, s.polygon_mode[0] = s.polygon_mode[1] = 2, s.applied_key = 9;
    CHECK(gfx::Prepare::apply_state(s, 0, 0, 0, 0, 0, none) && s.offset_enables == 0x06);

    // (the material state: kind 5, blend on, the depth function reversed by flags bit 6)
    gfx::MaterialState m;
    m.id = 3, m.kind = 0x15, m.blend = 1;
    const float c[4] = {0.0f, 1.0f, 0.5f, 0.25f};
    std::memcpy(m.color, c, sizeof c);
    m.targets[2][0] = 9, m.targets[2][5] = 7;
    m.depth_test = 1, m.depth_write = 1, m.depth_func = 4;
    s = gfx::PrepareState{};
    const float neg[4] = {-0.0f, 1.0f, 0.5f, 0.25f};   // (-0 equals +0: no copy)
    std::memcpy(s.blend_color, neg, sizeof neg);
    CHECK(gfx::Prepare::apply_state(s, 0, 0, 3, 0x40, 0x0f, m));
    CHECK(s.applied_material == 3 && s.blend_color[0] == 0x80000000u && s.blend[2][0] == 9 && s.blend[2][5] == 7);
    CHECK(s.blend_targets == 0x0f && s.depth_test == 1 && s.depth_write == 1 && s.depth_func == 7);
    // (the same again: nothing; then only the depth function differs: recorded all the same)
    s.applied_material = 0;
    CHECK(!gfx::Prepare::apply_state(s, 0, 0, 3, 0x40, 0x0f, m));
    s.applied_material = 0, s.depth_func = 4;
    CHECK(gfx::Prepare::apply_state(s, 0, 0, 3, 0x40, 0x0f, m) && s.depth_func == 7);
    // (no mode bit 1: no depth; flags bit 16: no blend targets copied)
    s = gfx::PrepareState{};
    CHECK(gfx::Prepare::apply_state(s, 0, 0, 1, 0x10000, 0x01, m) && s.blend[2][0] == 0 && s.depth_func == 0);
    // (kind 2: depth test on, writes off, less-or-equal; blend off: no targets)
    m.kind = 2, m.blend = 0;
    s = gfx::PrepareState{};
    s.blend_targets = 5;
    CHECK(gfx::Prepare::apply_state(s, 0, 0, 3, 0, 0x0f, m));
    CHECK(s.blend_targets == 0 && s.depth_test == 1 && s.depth_write == 0 && s.depth_func == 4);
    // (another kind: only the id)
    m.kind = 1;
    s = gfx::PrepareState{};
    CHECK(!gfx::Prepare::apply_state(s, 0, 0, 3, 0, 0x0f, m) && s.applied_material == 3);

    // (the record: blend targets from the mask, the stencil values when the test is on, offsets all or none)
    s = gfx::PrepareState{};
    s.blend_targets = 0x05;
    s.blend[2][0] = 1, s.blend[2][1] = 2, s.blend[2][2] = 3, s.blend[2][3] = 4, s.blend[2][4] = 5, s.blend[2][5] = 6;
    s.channel_masks = 0x0000f00a;
    s.stencil_test = 1, s.stencil_ref = 0x22, s.stencil_value_mask = 0x33;
    s.offset_enables = 1, s.offset_clamp[2] = 0x3f800000;
    gfx::PrepareHandles h;
    h.state_buffer = 0xcb, h.blend = 0x10, h.color = 0x18, h.channel_mask = 0x14, h.depth_stencil = 0x20, h.polygon = 0x24;
    h.blend_color = 0x1f8;
    Recorder r;
    gfx::Prepare::emit_state(s, h, r);
    CHECK(r.calls.size() == 28 + 2 * 5 + 3);
    CHECK(r.calls[2].op == (uint32_t)gfx::Proc::ColorStateSetBlendEnable && r.calls[2].x[1] == 0);
    const gfx::Command& f = r.calls[2 + 5 + 2];   // (target 2's blend function: factors 0, 2, 1, 3)
    CHECK(f.op == (uint32_t)gfx::Proc::BlendStateSetBlendFunc && f.x[1] == 1 && f.x[2] == 3 && f.x[3] == 2 && f.x[4] == 4);
    const gfx::Command& mask0 = r.calls[2 + 10 + 3];   // (target 0: 0xa)
    CHECK(mask0.op == (uint32_t)gfx::Proc::ChannelMaskStateSetChannelMask && mask0.x[1] == 0 && mask0.x[2] == 0 && mask0.x[3] == 1 && mask0.x[5] == 1);
    const gfx::Command& clamp = r.calls[r.calls.size() - 2];
    CHECK(clamp.op == (uint32_t)gfx::Proc::CommandBufferSetPolygonOffsetClamp && clamp.f[2] == 0x3f800000);
    CHECK(r.calls[r.calls.size() - 4].op == (uint32_t)gfx::Proc::PolygonStateSetPolygonOffsetEnables && r.calls[r.calls.size() - 4].x[1] == 7);

    // (decide: bit 17 derives and writes but records nothing)
    gfx::PrepareInputs in = cached_inputs();
    in.applied_state_key = 5;
    in.state.applied_key = 5;
    in.flags = 0x20000;
    gfx::PrepareOutputs out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::StateApplied && out.state_step && !out.state_recorded);
    in.flags = 0;
    out = gfx::Prepare::decide(in);
    CHECK(out.state_recorded);
    Recorder all;
    gfx::Prepare::emit(out, h, all);
    CHECK(all.calls.size() == 28 + 1 && all.calls.back().op == (uint32_t)gfx::Proc::CommandBufferCallCommands);
}

void the_preparation_binds_changed_uniform_words() {
    gfx::PrepareInputs in = cached_inputs();
    in.applied_program = 0x5678;
    in.known = {0xa0, 0xa1, 0xb0, 0xb1};
    in.interface_id[0] = 5;   // (a new first part: its words compared)
    in.words[0] = 0xff01ff03;   // (tag 0xff: stage 0 slot 3 (byte 0), stage 1 slot 1 (byte 2), stage 2 none (byte 1))
    gfx::BufferObject& b = in.buffers[0];
    b.align_method = 0xa0, b.bind_method = 0xb0;
    b.size = 0x130, b.slot = 3, b.stride = 0x20, b.base = 0x10000;
    gfx::PrepareOutputs out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBuffers && out.word_written[0] && out.words[0] == 0xff01ff03);
    CHECK(out.bind_count == 2 && out.bind_marked && out.mask_cleared);
    // (address: base + slot * (stride >> 4) * size rounded up to 0x100)
    CHECK(out.binds[0].stage == 0 && out.binds[0].slot == 3 && out.binds[0].address == 0x10000 + 3 * 2 * 0x200);
    CHECK(out.binds[1].stage == 1 && out.binds[1].slot == 1 && out.binds[1].size == 0x130);
    // (tag 0xfe: stages 1, 6, 7 from bytes 2, 0, 1; a negative slot as the register holds it; the object's alignment)
    in.words[0] = 0xfe80ff02;
    b.align_method = 0xa1, b.alignment = 0x40;
    out = gfx::Prepare::decide(in);
    CHECK(out.bind_count == 2 && out.binds[0].stage == 1 && out.binds[0].slot == 0xffffff80u && out.binds[1].stage == 6);
    CHECK(out.binds[1].slot == 2 && out.binds[0].address == 0x10000 + 3 * 2 * 0x140);
    Recorder r;
    gfx::PrepareHandles h;
    h.bind_buffer = 0xcb;
    gfx::Prepare::emit(out, h, r);
    CHECK(r.calls.size() == 4 && r.calls[1].op == (uint32_t)gfx::Proc::CommandBufferBindUniformBuffer);
    CHECK(r.calls[1].x[0] == 0xcb && r.calls[1].x[1] == 1 && r.calls[2].x[1] == 6 && r.calls[1].x[4] == 0x130);
    // (the word -1: recorded, nothing bound; an unknown method, another tag, a storage object, the switch: left)
    in.words[0] = 0xffffffffu;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBound && out.word_written[0] && out.bind_count == 0);
    in.words[0] = 0xff01ff03;
    b.align_method = 0x99;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    b.align_method = 0xa0;
    in.words[0] = 0xfd01ff03;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    in.words[0] = 0xff01ff03;
    b.bind_method = 0xb1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    b.bind_method = 0xb0;
    in.inline_switch = true, in.list_flags = 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    in.inline_switch = false;
    // (words 3 and 4: the second part's)
    in = cached_inputs();
    in.applied_program = 0x5678;
    in.known = {0xa0, 0xa1, 0xb0, 0xb1};
    in.interface_id[1] = 9;
    in.words[4] = 0xffffff00;
    in.buffers[2].align_method = 0xa0, in.buffers[2].bind_method = 0xb0;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBuffers && out.word_written[4] && out.bind_count == 1 && out.binds[0].stage == 0);
}

void the_preparation_binds_new_textures() {
    gfx::PrepareInputs in = cached_inputs();
    in.applied_program = 0x5678;
    in.interface_id[2] = 4;
    in.texture_mask = 0x40000005;   // (slots 0 and 2; bit 30 is past the slots)
    in.textures[0].word = 0xff02ff01, in.textures[0].texture = 7, in.textures[0].sampler = 3;   // (tag 0xff: stage 0 slot 1, stage 1 slot 2)
    in.textures[2].word = 0xfeffff05, in.textures[2].texture = 9, in.textures[2].sampler = -2;  // (tag 0xfe: stage 6 slot 5)
    gfx::PrepareOutputs out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBuffers && out.texture_count == 2 && out.id_written[2]);
    CHECK(out.textures[0].texture == 7 && out.textures[0].sampler == 3 && out.textures[0].stages == 2);
    CHECK(out.textures[0].stage[0] == 0 && out.textures[0].slot[0] == 1 && out.textures[0].stage[1] == 1 && out.textures[0].slot[1] == 2);
    CHECK(out.textures[1].sampler == 0xfffffffeu && out.textures[1].stages == 1 && out.textures[1].stage[0] == 6);
    Recorder r;
    gfx::PrepareHandles h;
    h.bind_buffer = 0xcb, h.device = 0xde, h.handles[0] = 0x111, h.handles[1] = 0x222;
    gfx::Prepare::emit(out, h, r);
    CHECK(r.calls.size() == 1 + 3 + 2 + 1);   // (program, handle and two binds, handle and one bind, the list)
    CHECK(r.calls[1].op == (uint32_t)gfx::Proc::DeviceGetTextureHandle && r.calls[1].x[0] == 0xde && r.calls[1].x[1] == 7);
    CHECK(r.calls[3].op == (uint32_t)gfx::Proc::CommandBufferBindTexture && r.calls[3].x[2] == 2 && r.calls[3].x[3] == 0x111);
    CHECK(r.calls[5].x[1] == 6 && r.calls[5].x[3] == 0x222);
    // (a word -1, a texture id -1: nothing; a texture not ready, mode bit 7, another tag: left)
    in.textures[2].word = 0xffffffffu;
    in.textures[0].texture = 0xffffffffu;
    CHECK(gfx::Prepare::decide(in).texture_count == 0);
    in.textures[0].texture = 7;
    in.textures[0].ready = 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    in.textures[0].ready = 0;
    // (mode bit 7: the handle table's entries bound deferred, the command buffer marked; the texture objects not looked at)
    in.mode_bits |= 0x80;
    in.textures[0].entry = 0x9008;
    out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBuffers && out.texture_count == 1 && out.textures[0].deferred && out.bind_marked);
    Recorder rd;
    gfx::Prepare::emit(out, h, rd);
    CHECK(rd.calls.size() == 1 + 2 + 1 && rd.calls[1].op == (uint32_t)gfx::Proc::CommandBufferBindTexturesDeferred);
    CHECK(rd.calls[1].x[1] == 0 && rd.calls[1].x[2] == 1 && rd.calls[1].x[3] == 1 && rd.calls[1].x[4] == 0x9008);
    in.inline_switch = true, in.list_flags = 1;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
    in.inline_switch = false, in.list_flags = 3;
    in.mode_bits &= ~0x80u;
    in.textures[0].word = 0xfd02ff01;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::ProgramChanged);
}

void the_preparation_binds_the_drawable_block_after_the_textures() {
    gfx::PrepareInputs in = cached_inputs();
    in.known = {0xa0, 0xa1, 0xb0, 0xb1};
    in.view_blocks = true, in.drawable_block = 3, in.applied_drawable_block = 1, in.block_binding = 0xffff02ff;
    in.drawable_buffer.align_method = 0xa0, in.drawable_buffer.bind_method = 0xb0, in.drawable_buffer.base = 0x5000;
    CHECK(gfx::Prepare::decide(in).route == gfx::PrepareRoute::DrawableBlock);   // (never made ready: guest code)
    in.drawable_ready = 1;
    in.applied_program = 0x5678;
    in.interface_id[2] = 4, in.texture_mask = 1;
    in.textures[0].word = 0xffffff00, in.textures[0].texture = 7;
    gfx::PrepareOutputs out = gfx::Prepare::decide(in);
    CHECK(out.route == gfx::PrepareRoute::ProgramBuffers && out.drawable_bound && out.drawable_block == 3 && out.bind_marked);
    CHECK(out.tail_binds == 0 && out.bind_count == 1 && out.binds[0].stage == 2 && out.binds[0].slot == 2);   // (byte 1: stage 2)
    Recorder r;
    gfx::PrepareHandles h;
    gfx::Prepare::emit(out, h, r);
    // (program, texture handle and bind, then the drawable block, then the list)
    CHECK(r.calls.size() == 5 && r.calls[3].op == (uint32_t)gfx::Proc::CommandBufferBindUniformBuffer && r.calls[3].x[3] == 0x5000);
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

    // (another program: its parts read, the block's copies; the textures' part not read under mode bit 15)
    const uint64_t program = 0x1234, part0 = 0xb00000, part1 = 0xb10000, part2 = 0xb20000;
    rt.put<uint64_t>(block + 0xd8, 0x9999);
    rt.put<uint64_t>(block + 0xd0, 0x7777);
    rt.put<uint16_t>(block + 0xb0, 0);   // (no pending bits: the new id clears the applied ones)
    rt.put<uint64_t>(program, part0);
    rt.put<uint64_t>(program + 8, part1);
    rt.put<uint64_t>(program + 0x10, part2);
    rt.put<uint64_t>(program + 0x20, 0xabc);
    rt.put<uint32_t>(program + 0x28, 0x1f);
    rt.put<uint16_t>(program + 0x30, 0x100);   // (the second part's words are the defaults)
    rt.put<uint32_t>(part0, 21);
    rt.put<uint32_t>(part0 + 4, 0x10), rt.put<uint32_t>(part0 + 8, 0x11), rt.put<uint32_t>(part0 + 12, 0x12);
    rt.put<uint32_t>(part1, 22);
    rt.put<uint32_t>(bridge::guest::kDefaultWordsB, 0x20), rt.put<uint32_t>(bridge::guest::kDefaultWordsB + 4, 0x21);
    rt.put<uint32_t>(part2, 23);
    rt.put<uint32_t>(part2 + 4, 0);
    rt.put<uint32_t>(block + 0x78, 0x10), rt.put<uint32_t>(block + 0x7c, 0x11), rt.put<uint32_t>(block + 0x80, 0x12);
    rt.put<uint32_t>(block + 0x84, 0x20), rt.put<uint32_t>(block + 0x88, 0x21);
    rt.put<uint32_t>(block + 0x90, 1), rt.put<uint32_t>(block + 0x94, 2), rt.put<uint32_t>(block + 0x8c, 3);
    pb.read(block, drawable, in, at);
    CHECK(in.applied_program == 0x9999 && in.context == context && in.program_object == 0xabc && in.program_stages == 0x1f);
    CHECK(in.interface_id[0] == 21 && in.interface_id[1] == 22 && in.interface_id[2] == 23 && in.texture_mask == 0);
    CHECK(in.words[2] == 0x12 && in.words[3] == 0x20 && in.applied_words[4] == 0x21 && in.applied_interface_id[2] == 3);
    CHECK(in.pending == 0 && in.applied_mask == 0x0c && in.list_flags == 3);
    gfx::PrepareOutputs pout = gfx::Prepare::decide(in);
    CHECK(pout.route == gfx::PrepareRoute::ProgramBound && pout.mask_cleared && !pout.mask_written);
    pb.snapshot(block, at.command_buffer, expected);
    bridge::PrepareBridge::expect(expected, pout);
    pb.write(block, at, pout);
    pb.snapshot(block, at.command_buffer, actual);
    CHECK(std::memcmp(expected, actual, sizeof actual) == 0);
    CHECK(rt.get<uint64_t>(block + 0xd8) == program && rt.get<uint64_t>(block + 0xd0) == 0);
    CHECK(rt.get<uint32_t>(block + 0x90) == 21 && rt.get<uint32_t>(block + 0x94) == 22 && rt.get<uint32_t>(block + 0x8c) == 23);
    CHECK(rt.get<uint32_t>(block + 0x5c) == (0x9 | 0x2000) && rt.get<uint16_t>(block + 0xb2) == 0);
    Recorder pr;
    gfx::Prepare::emit(pout, at, pr);
    CHECK(pr.calls.size() == 2 && pr.calls[0].op == (uint32_t)gfx::Proc::CommandBufferBindProgram);
    CHECK(pr.calls[0].x[0] == context + 0x188 && pr.calls[0].x[1] == 0xabc && pr.calls[0].x[2] == 0x1f);
    // (with bit 15 the texture part is not read)
    rt.put<uint64_t>(block + 0xd8, 0x9999);
    rt.put<uint32_t>(block + 0x5c, 0x8009);
    pb.read(block, drawable, in, at);
    CHECK(in.interface_id[2] == 0 && in.texture_mask == 0);
    rt.put<uint32_t>(block + 0x5c, 0x9);
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
    the_preparation_state_step_derives_and_records();
    the_preparation_binds_changed_uniform_words();
    the_preparation_binds_new_textures();
    the_preparation_binds_the_drawable_block_after_the_textures();
    the_prepare_bridge_reads_in_the_functions_order();
    std::printf("gfx: ok\n");
    return 0;
}
