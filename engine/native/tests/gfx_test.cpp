// HeyListen native engine: the graphics front end's recording pass, its bridge and its check, run with no game
// (tests/test_native_engine.py).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hl/bridge/GfxBridge.h"
#include "hl/gfx/PassCheck.h"
#include "hl/gfx/RecordPass.h"

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
    void call(bridge::GuestThread as, bridge::GuestAddr fn, uint64_t a0, uint64_t a1) override { calls.push_back({as, fn, a0, a1}); }
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

    // (the same stream in another mode is another shape; a native stream the translated path never made is unmatched)
    check.add(gfx::PassPath::Native, 1, recording({1, 2, 3}, 0));
    check.add(gfx::PassPath::Native, 0, recording({1, 2}, 0));
    r = check.report();
    CHECK(r.path[1].unmatched == 2 && r.path[1].unmatched_shapes == 2);
    CHECK(r.by_mode[0] == 5 && r.by_mode[1] == 1 && r.by_mode[2] == 0);
}

}   // namespace

int main() {
    the_pass_runs_the_phase_its_mode_names();
    the_shape_is_the_sequence_of_procedures();
    the_check_compares_the_two_paths();
    std::printf("gfx: ok\n");
    return 0;
}
