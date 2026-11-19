// The rewrite of an empty selection into selects (backend/spirv/empty_if_select.h): hand-made modules of the few instructions it reads.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "shader_recompiler/backend/spirv/empty_if_select.h"

using namespace Shader::Backend::SPIRV;
using Words = std::vector<std::uint32_t>;

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

enum : std::uint32_t { Phi = 245, Select = 169, Label = 248, Branch = 249, BranchCond = 250, SelMerge = 247, Return = 253, Fn = 54, FnEnd = 56,
                       IAdd = 128, LoopMerge = 246 };

static void ins(Words& w, std::uint32_t op, std::initializer_list<std::uint32_t> operands) {
    w.push_back(((static_cast<std::uint32_t>(operands.size()) + 1) << 16) | op);
    w.insert(w.end(), operands);
}
static Words module_start() { return {0x07230203u, 0x00010600u, 0, 64, 0}; }

/// header 10 branches on 20 to the body 11 (true) or the merge 12; body empty or holding an add; merge 12 holds two phis in either order
static Words make(bool body_is_true, bool body_has_code, bool extra_edge, bool three_parents = false) {
    Words w = module_start();
    ins(w, Fn, {1, 2, 0, 3});
    ins(w, Label, {10});
    ins(w, SelMerge, {12, 0});
    if (body_is_true) ins(w, BranchCond, {20, 11, 12}); else ins(w, BranchCond, {20, 12, 11});
    ins(w, Label, {11});
    if (body_has_code) ins(w, IAdd, {4, 40, 21, 22});
    ins(w, Branch, {12});
    if (extra_edge) {   // (a block of its own that also branches to the merge)
        ins(w, Label, {13});
        ins(w, Branch, {12});
    }
    ins(w, Label, {12});
    ins(w, Phi, {4, 30, 21, 10, 22, 11});
    if (three_parents) ins(w, Phi, {4, 31, 23, 11, 24, 10, 25, 13});
    else ins(w, Phi, {4, 31, 23, 11, 24, 10});
    ins(w, Branch, {14});
    ins(w, Label, {14});
    ins(w, Phi, {4, 50, 30, 12});   // (a later block's phi naming the merge as its parent)
    ins(w, Return, {});
    ins(w, FnEnd, {});
    return w;
}

static std::size_t count_op(const Words& w, std::uint32_t op) {
    std::size_t n = 0;
    for (std::size_t i = 5; i < w.size(); i += w[i] >> 16) {
        n += (w[i] & 0xFFFFu) == op;
    }
    return n;
}
static bool has(const Words& w, std::initializer_list<std::uint32_t> ins_words) {
    const Words want(ins_words);
    for (std::size_t i = 5; i < w.size(); i += w[i] >> 16) {
        if (w.size() >= i + want.size() && std::equal(want.begin(), want.end(), w.begin() + i)) return true;
    }
    return false;
}

int main() {
    // (the body on the true side: the phi takes the body's value where the condition holds)
    {
        Words w = make(true, false, false);
        CHECK(SelectEmptyIfs(w) == 1);
        CHECK(count_op(w, SelMerge) == 0 && count_op(w, BranchCond) == 0 && count_op(w, Phi) == 1);
        CHECK(count_op(w, Label) == 2 && count_op(w, Select) == 2 && count_op(w, Branch) == 1);   // (the header goes on in the merge's block)
        CHECK(has(w, {(2u << 16) | Branch, 14}) && !has(w, {(2u << 16) | Branch, 12}));
        CHECK(has(w, {(5u << 16) | Phi, 4, 50, 30, 10}));   // (the later phi's parent is the header now)
        CHECK(has(w, {(6u << 16) | Select, 4, 30, 20, 22, 21}));   // (phi 30: 21 from the header, 22 from the body)
        CHECK(has(w, {(6u << 16) | Select, 4, 31, 20, 23, 24}));   // (phi 31 lists the body first)
        CHECK(w[3] == 64);
    }
    // (the body on the false side: the same values, the condition read the other way)
    {
        Words w = make(false, false, false);
        CHECK(SelectEmptyIfs(w) == 1);
        CHECK(has(w, {(6u << 16) | Select, 4, 30, 20, 21, 22}));
        CHECK(has(w, {(6u << 16) | Select, 4, 31, 20, 24, 23}));
    }
    // (a body with an instruction, another edge into the merge, a phi with a third parent: left as they are)
    {
        Words w = make(true, true, false);
        const Words before = w;
        CHECK(SelectEmptyIfs(w) == 0 && w == before);
        w = make(true, false, true);
        const Words before2 = w;
        CHECK(SelectEmptyIfs(w) == 0 && w == before2);
        w = make(true, false, false, true);
        const Words before3 = w;
        CHECK(SelectEmptyIfs(w) == 0 && w == before3);
    }
    // (not a module: untouched)
    {
        Words w{0x07230203u, 0, 0, 8, 0, 0};
        CHECK(SelectEmptyIfs(w) == 0 && w.size() == 6);
        Words small{1, 2};
        CHECK(SelectEmptyIfs(small) == 0);
    }
    // (the switch's text: a size in words, off when unset, empty, zero or not a number)
    CHECK(EmptyIfMinWords(nullptr) == kEmptyIfDefault && kEmptyIfDefault == 0);
    CHECK(EmptyIfMinWords("") == 0 && EmptyIfMinWords("0") == 0 && EmptyIfMinWords("-5") == 0 && EmptyIfMinWords("x1") == 0);
    CHECK(EmptyIfMinWords("1") == 1 && EmptyIfMinWords("300000") == 300000);
    std::printf("empty_if_select: %d failures\n", bad);
    return bad ? 1 : 0;
}
