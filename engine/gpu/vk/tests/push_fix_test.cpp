// Game-free regression for vk_push_fix.h (issue 640): the mesh shader fallback's task programs named the push constant block's
// task payload address by its byte offset (64) where SPIR-V takes the member index (3), and NVIDIA's compiler faulted on the
// module. A small hand-made module: a push block of five members at offsets 0, 16, 24, 64, 72, and access chains into it.
//   clang++ -std=c++20 -O1 -Wall -Wextra gpu/vk/tests/push_fix_test.cpp -o push_fix_test.exe && push_fix_test.exe
#include <cstdio>
#include <vector>

#include "../vk_push_fix.h"

namespace {
int failures = 0;

void expect(const char* name, bool ok) {
    if (ok) return;
    std::fprintf(stderr, "%s: failed\n", name);
    failures++;
}

constexpr uint32_t op(uint32_t code, uint32_t words) { return words << 16 | code; }

// %1 main, %2 void, %3 fn, %4 u32, %5 block, %6 ptr block, %7 ptr u32, %8 const <first>, %9 const 2, %10 the push variable,
// %11 label, %12 and %13 the access chains, %14 a load; <extra> one more constant (%15) when given
std::vector<uint32_t> module(uint32_t first, bool push = true, int extra = -1) {
    const uint32_t bound = extra >= 0 ? 16 : 15;
    std::vector<uint32_t> w = {0x07230203u, 0x00010600u, 0, bound, 0,
                               op(17, 2), 1,                                                      // OpCapability Shader
                               op(14, 3), 0, 1,                                                   // OpMemoryModel Logical GLSL450
                               op(15, 6), 5, 1, 0x6e69616du, 0, 10};                              // OpEntryPoint GLCompute main
    const uint32_t offsets[5] = {0, 16, 24, 64, 72};
    for (uint32_t m = 0; m < 5; m++) w.insert(w.end(), {op(72, 5), 5, m, 35, offsets[m]});   // OpMemberDecorate Offset
    w.insert(w.end(), {op(71, 3), 5, 2,                                                       // OpDecorate Block
                       op(19, 2), 2, op(33, 3), 3, 2, op(21, 4), 4, 32, 0,                    // void, fn, u32
                       op(30, 7), 5, 4, 4, 4, 4, 4,                                           // the block
                       op(32, 4), 6, push ? 9u : 7u, 5, op(32, 4), 7, push ? 9u : 7u, 4,      // pointers
                       op(43, 4), 4, 8, first, op(43, 4), 4, 9, 2});                          // constants
    if (extra >= 0) w.insert(w.end(), {op(43, 4), 4, 15, (uint32_t)extra});
    w.insert(w.end(), {op(59, 4), 6, 10, push ? 9u : 7u,                                      // the variable
                       op(54, 5), 2, 1, 0, 3, op(248, 2), 11,                                 // OpFunction, OpLabel
                       op(65, 5), 7, 12, 10, 8, op(65, 5), 7, 13, 10, 9, op(61, 4), 4, 14, 12,
                       op(253, 1), op(56, 1)});
    return w;
}

/// The index operand of the access chain whose result is id, in w (0: none)
uint32_t chain_index(const std::vector<uint32_t>& w, uint32_t id) {
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
        if (!(w[i] >> 16)) break;
        if ((w[i] & 0xFFFF) == 65 && w[i + 2] == id) return w[i + 4];
    }
    return 0;
}
/// The value of u32 constant id in w (~0: none)
uint32_t constant(const std::vector<uint32_t>& w, uint32_t id) {
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
        if (!(w[i] >> 16)) break;
        if ((w[i] & 0xFFFF) == 43 && w[i + 2] == id) return w[i + 3];
    }
    return ~0u;
}
/// Whether every constant comes before the first function, as SPIR-V wants
bool constants_first(const std::vector<uint32_t>& w) {
    bool in_function = false;
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
        if (!(w[i] >> 16)) return false;
        if ((w[i] & 0xFFFF) == 54) in_function = true;
        if ((w[i] & 0xFFFF) == 43 && in_function) return false;
    }
    return true;
}
}  // namespace

int main() {
    std::vector<uint32_t> out;
    {   // the payload's address by its offset: the member's index, from a constant added for it
        const std::vector<uint32_t> in = module(64);
        expect("offset 64: one access chain fixed", mkwvk::push_offset_fix(in.data(), in.size(), out) == 1);
        expect("offset 64: a constant added", out.size() == in.size() + 4 && out[3] == in[3] + 1);
        expect("offset 64: member 3", constant(out, chain_index(out, 12)) == 3);
        expect("offset 64: the valid index left alone", chain_index(out, 13) == 9);
        expect("offset 64: constants before the function", constants_first(out));
    }
    {   // a module with a constant of that value already: that one, nothing added
        const std::vector<uint32_t> in = module(64, true, 3);
        out.clear();
        expect("existing constant: fixed", mkwvk::push_offset_fix(in.data(), in.size(), out) == 1);
        expect("existing constant: nothing added", out.size() == in.size() && out[3] == in[3]);
        expect("existing constant: it is used", chain_index(out, 12) == 15);
    }
    {   // the records' address at 72: member 4
        const std::vector<uint32_t> in = module(72);
        out.clear();
        expect("offset 72: fixed", mkwvk::push_offset_fix(in.data(), in.size(), out) == 1);
        expect("offset 72: member 4", constant(out, chain_index(out, 12)) == 4);
    }
    {   // valid indices, an index past the members that is no member's offset, and a block that is not a push constant one
        for (const auto& in : {module(4), module(40), module(64, false)}) {
            out.assign(3, 7);
            expect("left alone", mkwvk::push_offset_fix(in.data(), in.size(), out) == 0 && out.size() == 3);
        }
    }
    {   // a module cut short, and one that is not SPIR-V
        std::vector<uint32_t> in = module(64);
        in.resize(in.size() - 3);
        expect("cut short", mkwvk::push_offset_fix(in.data(), in.size(), out) == 0);
        in[0] = 0;
        expect("not SPIR-V", mkwvk::push_offset_fix(in.data(), in.size(), out) == 0);
        expect("empty", mkwvk::push_offset_fix(nullptr, 0, out) == 0);
    }
    std::printf("push_fix_test: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
