// (hookshot, stream SH1) A selection whose body is empty is a Select. Pure: tests/empty_if_select_test.cpp.
//
// The translator makes an `if (p) { ... }` of every predicated instruction and leaves the construct in place when the body ends up
// holding nothing (the instruction folded to a constant, or went dead): a header block `OpSelectionMerge M; OpBranchConditional c T M`,
// a body block `T: OpBranch M` and the phis of M choosing between the header's value and the body's. OoT's two largest programs
// (mesh stages of ~475k and ~987k SPIR-V words, whose driver compile runs for minutes at tens of GB) hold 9044 of these in the
// smaller one (44% of its blocks) and 5196 in the other, with their phis. Here the construct becomes what it computes: the header
// branches to M and each phi of M is `OpSelect c value-from-T value-from-header`: the same value, no branch, no body block.
//
// Exact: the body holds no instruction, so no side effect, no subgroup operation and no memory access is moved; every phi of M
// reads an id that was defined before the header's branch (the body defines none). The rewrite is taken only when the ids it
// touches have no other reference: M is the target of exactly three edges (the merge declaration, the header's branch, the body's
// branch; later blocks may name it as a phi parent, which stays true), T is named exactly twice plus once for each phi of M, and every phi of M has exactly the two parents (header, T).
//
// RX_VK_FOLD_EMPTY_IF=<words> turns it on for the modules of that size or more (default off; a name under RX_VK_FOLD, which the
// program cache's stamp covers: vk_shader.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Shader::Backend::SPIRV {

/// The module size, in SPIR-V words, from which the rewrite is made: RX_VK_FOLD_EMPTY_IF=<n>. Unset, empty, 0 or not a number: never
/// (kEmptyIfDefault); 1: every module; n: a module of n words or more (the two programs are 475k and 987k).
inline constexpr std::uint32_t kEmptyIfDefault = 0;
inline std::uint32_t EmptyIfMinWords(const char* text) {
    if (!text || !*text) {
        return kEmptyIfDefault;
    }
    char* end{};
    const long long value{std::strtoll(text, &end, 10)};
    return (end == text || *end != '\0' || value <= 0 || value > 0xFFFFFFFFll) ? 0 : static_cast<std::uint32_t>(value);
}

/// Rewrites the empty selections of a module in place; returns how many it rewrote.
inline std::size_t SelectEmptyIfs(std::vector<std::uint32_t>& code) {
    constexpr std::uint32_t kOpLabel = 248, kOpBranch = 249, kOpBranchConditional = 250, kOpSelectionMerge = 247, kOpPhi = 245,
                            kOpSelect = 169, kOpLoopMerge = 246, kOpSwitch = 251;
    if (code.size() < 6) {
        return 0;
    }
    const std::uint32_t bound{code[3]};
    // every word after an instruction's first counted by value: ids (and any literal that looks like one, which only makes a
    // rewrite refuse itself)
    std::vector<std::uint32_t> occurrences(bound, 0);
    std::vector<std::uint32_t> branched_to(bound, 0);   // (references by a branch, a merge declaration or a switch: the edges into a block)
    struct Ins {
        std::size_t at;
        std::uint32_t op, size;
    };
    std::vector<Ins> ins;
    for (std::size_t i = 5; i < code.size();) {
        const std::uint32_t size{code[i] >> 16};
        if (size == 0 || i + size > code.size()) {
            return 0;   // not a module this knows: untouched
        }
        ins.push_back({i, code[i] & 0xFFFFu, size});
        const std::uint32_t op{code[i] & 0xFFFFu};
        const bool edges{op == kOpBranch || op == kOpBranchConditional || op == kOpSelectionMerge || op == kOpLoopMerge || op == kOpSwitch};
        for (std::uint32_t k = 1; k < size; ++k) {
            if (code[i + k] < bound) {
                ++occurrences[code[i + k]];
                if (edges) {
                    ++branched_to[code[i + k]];
                }
            }
        }
        i += size;
    }
    // a label followed at once by a branch: its target
    std::vector<std::uint32_t> empty_target(bound, 0);
    for (std::size_t n = 0; n + 1 < ins.size(); ++n) {
        if (ins[n].op == kOpLabel && ins[n + 1].op == kOpBranch && ins[n].size == 2 && ins[n + 1].size == 2) {
            empty_target[code[ins[n].at + 1]] = code[ins[n + 1].at + 1];
        }
    }
    std::vector<bool> drop(ins.size(), false);
    // per instruction: the replacement words (a branch, or the select of a phi); empty vector = keep
    std::vector<std::vector<std::uint32_t>> replace(ins.size());
    std::size_t rewritten{};
    std::vector<std::uint32_t> merged_into(bound, 0);   // (a block whose label went: the block its instructions went on in)
    std::uint32_t header{};   // (the current block's label)
    for (std::size_t n = 0; n < ins.size(); ++n) {
        if (ins[n].op == kOpLabel) {
            header = code[ins[n].at + 1];
        }
        if (ins[n].op != kOpSelectionMerge || n + 1 >= ins.size() || ins[n + 1].op != kOpBranchConditional ||
            ins[n + 1].size != 4 || ins[n].size != 3) {
            continue;
        }
        const std::uint32_t merge{code[ins[n].at + 1]};
        const std::uint32_t cond{code[ins[n + 1].at + 1]};
        const std::uint32_t on_true{code[ins[n + 1].at + 2]};
        const std::uint32_t on_false{code[ins[n + 1].at + 3]};
        const bool body_is_true{on_false == merge};
        const std::uint32_t body{body_is_true ? on_true : on_false};
        if ((on_true != merge && on_false != merge) || on_true == on_false || body >= bound || merge >= bound ||
            empty_target[body] != merge || branched_to[merge] != 3) {
            continue;
        }
        // the body's label and branch: the two instructions right after the header's terminator... find them
        std::size_t body_label{ins.size()};
        for (std::size_t m = n + 2; m + 1 < ins.size() && m < n + 4; ++m) {
            if (ins[m].op == kOpLabel && code[ins[m].at + 1] == body) {
                body_label = m;
                break;
            }
        }
        if (body_label == ins.size() || ins[body_label + 1].op != kOpBranch) {
            continue;
        }
        // the merge block's label follows the body's branch at once (the order the translator writes them in)
        const std::size_t merge_label{body_label + 2};
        if (merge_label >= ins.size() || ins[merge_label].op != kOpLabel || code[ins[merge_label].at + 1] != merge) {
            continue;
        }
        std::size_t phis{};
        bool ok{true};
        for (std::size_t m = merge_label + 1; m < ins.size() && ins[m].op == kOpPhi; ++m, ++phis) {
            const std::uint32_t* p{&code[ins[m].at]};
            ok = ok && ins[m].size == 7 && ((p[4] == header && p[6] == body) || (p[4] == body && p[6] == header));
        }
        if (!ok || occurrences[body] != 2 + phis) {
            continue;
        }
        // rewrite: drop the merge declaration and the branch, the body's two instructions and the merge's label (the header's block goes
        // on into M's: M had no other way in), the phis become selects, and a phi that named M as its parent names the header
        drop[n] = drop[n + 1] = true;
        drop[body_label] = drop[body_label + 1] = drop[merge_label] = true;
        merged_into[merge] = merged_into[header] != 0 ? merged_into[header] : header;
        for (std::size_t m = merge_label + 1; m < ins.size() && ins[m].op == kOpPhi; ++m) {
            const std::uint32_t* p{&code[ins[m].at]};
            const bool header_first{p[4] == header};
            const std::uint32_t from_header{header_first ? p[3] : p[5]};
            const std::uint32_t from_body{header_first ? p[5] : p[3]};
            replace[m] = {(6u << 16) | kOpSelect, p[1], p[2], cond, body_is_true ? from_body : from_header,
                          body_is_true ? from_header : from_body};
        }
        ++rewritten;
    }
    if (rewritten == 0) {
        return 0;
    }
    std::vector<std::uint32_t> out;
    out.reserve(code.size());
    out.insert(out.end(), code.begin(), code.begin() + 5);
    for (std::size_t n = 0; n < ins.size(); ++n) {
        if (drop[n]) {
            continue;
        }
        if (!replace[n].empty()) {
            out.insert(out.end(), replace[n].begin(), replace[n].end());
        } else if (ins[n].op == kOpPhi) {   // (parents are every second word from the fifth)
            const std::size_t at{out.size()};
            out.insert(out.end(), code.begin() + ins[n].at, code.begin() + ins[n].at + ins[n].size);
            for (std::uint32_t k = 4; k < ins[n].size; k += 2) {
                out[at + k] = merged_into[out[at + k] < bound ? out[at + k] : 0] != 0 ? merged_into[out[at + k]] : out[at + k];
            }
        } else {
            out.insert(out.end(), code.begin() + ins[n].at, code.begin() + ins[n].at + ins[n].size);
        }
    }
    code.swap(out);
    return rewritten;
}

} // namespace Shader::Backend::SPIRV
