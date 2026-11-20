// SPDX-License-Identifier: GPL-3.0-or-later
// (longshot) The game's HDR10 output encoder as RenoDX's zelda-oot-oz has it with a custom tone mapper (output_0x92D6F81D,
// github.com/clshortfuse/renodx, MIT): HDR set to Native and its Tone mapper row on PsychoV35 (vk_shader.cpp). Found in the IR;
// nothing of the game's code is kept.
//
// What the encoder does, per channel of target 0: the finished picture (a texture's red, green and blue) through a 3x3 matrix
// of constant buffer words, which also holds the paper white in nits; times 1e-4 (PQ's 10000 nits); then PQ (ST 2084). The
// pass finds each channel's matrix row and the picture's channels, and writes instead, as the mod:
//  - Global Color Space Vanilla: the game's matrix with its white taken out (the row's product over the first row's sum);
//    BT.709: the picture's BT.709 to BT.2020; DCI-P3: the picture read as Display P3 (D65 white) to BT.2020;
//  - at most 0, times the UI's white (the mod's UI Brightness: the scene is in its units, scene_tonemap_pass.h), then PQ.
// The colour space and the UI's white are read from the renderer's bank at each draw (psychov35.h kSettingColorSpace,
// kSettingOutputScale).

#pragma once

#include <array>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/ir_opt/scene_tonemap_pass.h"

namespace Shader::Optimization {

namespace output_encode_detail {

using scene_tonemap_detail::Cbuf;
using scene_tonemap_detail::IsImm;

inline constexpr float kPqM1 = 0.1593017578125f, kPqM2 = 78.84375f, kPqC1 = 0.8359375f, kPqC2 = 18.8515625f,
                       kPqC3 = 18.6875f;
inline constexpr float kPqScale = 9.99999974737875163555145263671875e-05f;   // (1e-4: nits over PQ's 10000)

// RenoDX's (shaders/color/rgb.hlsl) BT709_TO_BT2020_MAT and XYZ_TO_BT2020_MAT * DISPLAYP3_TO_XYZ_MAT, rows by output channel
inline constexpr float kBt709ToBt2020[3][3] = {{0.627403915f, 0.329283029f, 0.0433130674f},
                                               {0.069097288f, 0.919540346f, 0.0113623189f},
                                               {0.0163914394f, 0.0880133137f, 0.895595253f}};
inline constexpr float kP3ToBt2020[3][3] = {{0.753833055f, 0.198597386f, 0.0475695953f},
                                            {0.0457438454f, 0.941777229f, 0.0124789253f},
                                            {-0.00121034053f, 0.0176017173f, 0.983608663f}};

/// One term of a matrix row's product: a value times a constant buffer word
struct Term {
    IR::Value input, coefficient;
};
/// A row's product as its terms: sums, products and fused multiply-adds of values with constant buffer words
inline bool Terms(const IR::Value& v, std::vector<Term>& out, int depth = 0) {
    if (v.IsImmediate() || depth > 8 || out.size() > 3) return false;
    const IR::Inst* inst{v.InstRecursive()};
    u32 bank, offset;
    switch (inst->GetOpcode()) {
    case IR::Opcode::FPMul32:
        for (size_t side : {size_t{1}, size_t{0}})
            if (Cbuf(inst->Arg(side), bank, offset)) return out.push_back({inst->Arg(1 - side), inst->Arg(side)}), true;
        return false;
    case IR::Opcode::FPFma32:
        if (Cbuf(inst->Arg(1), bank, offset)) out.push_back({inst->Arg(0), inst->Arg(1)});
        else if (Cbuf(inst->Arg(0), bank, offset)) out.push_back({inst->Arg(1), inst->Arg(0)});
        else return false;
        return Terms(inst->Arg(2), out, depth + 1);
    case IR::Opcode::FPAdd32:
        return Terms(inst->Arg(0), out, depth + 1) && Terms(inst->Arg(1), out, depth + 1);
    default:
        return false;
    }
}
/// A texture's channel: the sample it is taken from and which channel
inline bool Channel(const IR::Value& v, const IR::Inst*& sample, u32& index) {
    if (v.IsImmediate()) return false;
    const IR::Inst* inst{v.InstRecursive()};
    if (inst->GetOpcode() != IR::Opcode::CompositeExtractF32x4 || inst->Arg(0).IsImmediate() || !inst->Arg(1).IsImmediate())
        return false;
    sample = inst->Arg(0).InstRecursive(), index = inst->Arg(1).U32();
    return index < 3;
}

} // namespace output_encode_detail

/// The mod's output encoder for target 0's colour of the game's HDR10 encoder; nullptr when made, else why this program is
/// not one
inline const char* OutputEncodePass(IR::Program& program) {
    using namespace output_encode_detail;
    using scene_tonemap_detail::IrOps;
    if (program.stage != Stage::Fragment) return "not a fragment program";
    std::array<IR::Inst*, 3> writes{};
    std::array<IR::Block*, 3> write_blocks{};
    for (IR::Block* block : program.blocks)
        for (IR::Inst& inst : block->Instructions())
            if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(0).IsImmediate() && inst.Arg(0).U32() == 0 &&
                inst.Arg(1).IsImmediate() && inst.Arg(1).U32() < 3) {
                IR::Inst*& w{writes[inst.Arg(1).U32()]};
                if (w) return "target 0 written more than once";
                w = &inst;
                write_blocks[inst.Arg(1).U32()] = block;
            }
    for (IR::Inst* w : writes)
        if (!w || w->Arg(2).IsImmediate()) return "target 0's colour not written";
    IR::Block* const block{write_blocks[0]};
    if (write_blocks[1] != block || write_blocks[2] != block) return "target 0's colour written apart";

    // each channel: back from its write through PQ (its five constants on the way) to the light in nits times 1e-4
    std::array<IR::F32, 3> nits;
    std::array<std::array<IR::Value, 3>, 3> coefficient;   // [channel][picture's channel]
    std::array<IR::Value, 3> picture;
    const IR::Inst* sample{};
    for (int c = 0; c < 3; c++) {
        std::vector<const IR::Inst*> stack{writes[c]->Arg(2).InstRecursive()};
        std::unordered_set<const IR::Inst*> seen;
        const IR::Inst* scaled{};
        unsigned constants = 0;
        while (!stack.empty() && seen.size() < 256) {
            const IR::Inst* inst{stack.back()};
            stack.pop_back();
            if (!seen.insert(inst).second || inst->GetOpcode() == IR::Opcode::Phi) continue;
            if (inst->GetOpcode() == IR::Opcode::FPMul32 && (IsImm(inst->Arg(0), kPqScale) || IsImm(inst->Arg(1), kPqScale))) {
                if (scaled && scaled != inst) return "a channel's light scaled to PQ twice";
                scaled = inst;
                continue;   // (the matrix below it)
            }
            for (size_t i = 0; i < inst->NumArgs(); i++) {
                const IR::Value arg{inst->Arg(i)};
                if (!arg.IsImmediate()) {
                    stack.push_back(arg.InstRecursive());
                    continue;
                }
                const float pq[5] = {kPqM1, kPqM2, kPqC1, kPqC2, kPqC3};
                for (unsigned k = 0; k < 5; k++)
                    if (IsImm(arg, pq[k])) constants |= 1u << k;
            }
        }
        if (!scaled) return "a channel's light not scaled to PQ's 10000 nits";
        if (constants != 0x1f) return "a channel not encoded with PQ's constants";
        nits[c] = IR::F32{scaled->Arg(IsImm(scaled->Arg(1), kPqScale) ? 0 : 1)};
        std::vector<Term> terms;
        if (!Terms(nits[c], terms) || terms.size() != 3) return "a channel's light not a matrix row of the picture";
        u32 found = 0;
        for (const Term& t : terms) {
            const IR::Inst* from;
            u32 index;
            if (!Channel(t.input, from, index)) return "a matrix row's term not a channel of a texture";
            if (sample && from != sample) return "the matrix rows read different textures";
            if (found & (1u << index)) return "a matrix row reads a channel twice";
            sample = from, found |= 1u << index;
            coefficient[c][index] = t.coefficient;
            picture[index] = t.input;
        }
    }

    // (before the last of the three writes, as the scene pass)
    IR::Inst* last{writes[0]};
    for (IR::Inst& inst : block->Instructions())
        if (&inst == writes[0] || &inst == writes[1] || &inst == writes[2]) last = &inst;
    auto& list{block->Instructions()};
    IR::IREmitter ir{*block, list.iterator_to(*last)};
    IrOps o{ir};
    const auto setting = [&](unsigned at) {
        return ir.GetFloatCbuf(ir.Imm32(PsychoV35::kSettingsBank), ir.Imm32(at * 4u));
    };
    scene_tonemap_detail::UseSettingsBank(program.info);
    const IR::F32 space{setting(PsychoV35::kSettingColorSpace)}, scale{setting(PsychoV35::kSettingOutputScale)};
    const IR::U1 bt709{o.Eq(space, o.Imm(1.0f))}, p3{o.Eq(space, o.Imm(2.0f))};
    // (Vanilla: the game's matrix over its white, the first row's sum)
    const IR::F32 white{o.Add(o.Add(IR::F32{coefficient[0][0]}, IR::F32{coefficient[0][1]}), IR::F32{coefficient[0][2]})};
    const IR::F32 inverse_white{IR::F32{ir.FPRecip(white)}};
    for (int c = 0; c < 3; c++) {
        const auto row = [&](const float (&m)[3][3]) {
            return o.Fma(IR::F32{picture[2]}, o.Imm(m[c][2]),
                         o.Fma(IR::F32{picture[1]}, o.Imm(m[c][1]), o.Mul(IR::F32{picture[0]}, o.Imm(m[c][0]))));
        };
        const IR::F32 vanilla{o.Mul(nits[c], inverse_white)};
        const IR::F32 linear{o.Select(bt709, row(kBt709ToBt2020), o.Select(p3, row(kP3ToBt2020), vanilla))};
        // PQ of the light at most 0, in the UI's white (RenoDX's pq::Encode)
        const IR::F32 y{o.Mul(o.Max(linear, o.Imm(0.0f)), scale)};
        const IR::F32 y_m1{o.Exp2(o.Mul(o.Log2(y), o.Imm(kPqM1)))};
        const IR::F32 ratio{o.Div(o.Fma(y_m1, o.Imm(kPqC2), o.Imm(kPqC1)), o.Fma(y_m1, o.Imm(kPqC3), o.Imm(1.0f)))};
        const IR::F32 encoded{o.Exp2(o.Mul(o.Log2(ratio), o.Imm(kPqM2)))};
        if (writes[c] != last) {
            list.erase(list.iterator_to(*writes[c]));
            list.insert(list.iterator_to(*last), *writes[c]);
        }
        writes[c]->SetArg(2, encoded);
    }
    return nullptr;
}

} // namespace Shader::Optimization
