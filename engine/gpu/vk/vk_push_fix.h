// (hookshot, issue 640) The mesh shader fallback's task programs read the task payloads' address from the push constants with the
// member's byte offset (64) as its index, where SPIR-V takes the member's index (3): an access chain past the push block's last
// member. Such a module is invalid, and NVIDIA's compiler faults on it. push_offset_fix gives a module whose access chains into a
// push constant block by an index past its members, that is the Offset of one of them, take that member's index instead (a u32
// constant added before the first function when the module has none of that value). vk_main.cpp applies it as modules are made.
// Game-free test: tests/push_fix_test.cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mkwvk {

/// The number of access chains changed: 0, and out left alone, when the module has none to change
inline size_t push_offset_fix(const uint32_t* w, size_t n, std::vector<uint32_t>& out) {
    if (!w || n < 5 || w[0] != 0x07230203u) return 0;
    std::unordered_map<uint32_t, uint32_t> members, pointee, push_vars;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> constants;   // id -> type, value
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> offsets;   // struct -> offset -> member
    uint32_t u32_type = 0;
    size_t first_function = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        const uint32_t* a = w + i;
        if (op == 54) {   // OpFunction: the declarations are over
            first_function = i;
            break;
        }
        if (op == 21 && len == 4 && a[2] == 32 && a[3] == 0) u32_type = a[1];             // OpTypeInt 32 unsigned
        else if (op == 43 && len == 4) constants[a[2]] = {a[1], a[3]};                     // OpConstant
        else if (op == 30) members[a[1]] = len - 2;                                        // OpTypeStruct
        else if (op == 72 && len == 5 && a[3] == 35) offsets[a[1]][a[4]] = a[2];           // OpMemberDecorate Offset
        else if (op == 32 && len == 4) pointee[a[1]] = a[3];                               // OpTypePointer
        else if (op == 59 && len >= 4 && a[3] == 9) push_vars[a[2]] = a[1];                // OpVariable PushConstant
        i += len;
    }
    if (push_vars.empty() || !u32_type || !first_function) return 0;
    std::vector<std::pair<size_t, uint32_t>> fixes;   // (the word of the index, the member it is to be)
    for (size_t i = first_function; i < n;) {
        const uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        if ((op == 65 || op == 66) && len >= 5) {   // OpAccessChain / OpInBoundsAccessChain: type, result, base, first index
            const auto var = push_vars.find(w[i + 3]);
            const auto c = constants.find(w[i + 4]);
            if (var != push_vars.end() && c != constants.end() && c->second.first == u32_type) {
                const auto block = pointee.find(var->second);
                const auto m = block != pointee.end() ? members.find(block->second) : members.end();
                const auto at = block != pointee.end() ? offsets.find(block->second) : offsets.end();
                if (m != members.end() && at != offsets.end() && c->second.second >= m->second) {
                    const auto member = at->second.find(c->second.second);
                    if (member != at->second.end()) fixes.push_back({i + 4, member->second});
                }
            }
        }
        i += len;
    }
    if (fixes.empty()) return 0;
    std::unordered_map<uint32_t, uint32_t> u32_constants;   // value -> id
    for (const auto& [id, c] : constants)
        if (c.first == u32_type) u32_constants.emplace(c.second, id);
    out.assign(w, w + n);
    std::vector<uint32_t> added;
    for (const auto& [at, member] : fixes) {
        auto c = u32_constants.find(member);
        if (c == u32_constants.end()) {
            const uint32_t id = out[3]++;   // (the module's id bound)
            added.insert(added.end(), {(4u << 16) | 43u, u32_type, id, member});
            c = u32_constants.emplace(member, id).first;
        }
        out[at] = c->second;
    }
    out.insert(out.begin() + (std::ptrdiff_t)first_function, added.begin(), added.end());
    return fixes.size();
}

} // namespace mkwvk
