// SPDX-License-Identifier: GPL-3.0-or-later
// The output encoder pass (ir_opt/output_encode_pass.h) on synthetic HDR10 encoder programs: the game's shape (a texture's
// colour through a 3x3 matrix of constant buffer words holding the paper white, times 1e-4, then PQ, to target 0), its rows
// in any order. The pass's IR is interpreted here and compared with RenoDX's output encoder (zelda-oot-oz output_0x92D6F81D
// with a custom tone mapper) worked out in double; the shapes it must not take are left as they are.
// tests/test_output_encode_pass.py builds and runs it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/ir_opt/output_encode_pass.h"

using namespace Shader;
namespace PV = Shader::Optimization::PsychoV35;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

constexpr u32 kBank = 3, kMatrix = 72 * 4;   // (the matrix's rows at words 72, 76 and 80: three words each, a pad)

// A game-like matrix: rows by output channel, the paper white (250 nits here) in each row's sum
constexpr double kWhite = 250.0;
constexpr double kGame[3][3] = {{0.60, 0.33, 0.07}, {0.08, 0.90, 0.02}, {0.02, 0.10, 0.88}};

enum class Shape { Game, RowsOutOfOrder, NoPq, OtherScale, TwoTextures };

struct Built {
    ObjectPool<IR::Inst> insts{512};
    ObjectPool<IR::Block> blocks{4};
    IR::Program program;
    std::array<IR::Inst*, 3> writes{};
};

void build(Built& b, Shape shape) {
    IR::Block* block = b.blocks.Create(b.insts);
    b.program.blocks.push_back(block);
    b.program.post_order_blocks.push_back(block);
    b.program.stage = Stage::Fragment;
    IR::IREmitter ir{*block};
    const auto attr = [&](int i) { return IR::F32{ir.GetAttribute(IR::Attribute(int(IR::Attribute::Generic0X) + i))}; };
    // (the texture's colour: a vector, its channels taken from it)
    const IR::Value texel = ir.CompositeConstruct(attr(0), attr(1), attr(2), attr(3));
    const IR::Value other = ir.CompositeConstruct(attr(1), attr(0), attr(2), attr(3));
    const auto channel = [&](int c, int row) {
        return IR::F32{ir.CompositeExtract(shape == Shape::TwoTextures && row == 2 && c == 1 ? other : texel, c)};
    };
    const auto coef = [&](int row, int c) { return ir.GetFloatCbuf(ir.Imm32(kBank), ir.Imm32(kMatrix + (row * 4 + c) * 4)); };
    const float scale = shape == Shape::OtherScale ? 1e-3f : 9.99999974737875163555145263671875e-05f;
    std::array<IR::F32, 3> light;
    // (green's products first, as the game's: then each row's red and blue on it)
    const std::array<int, 3> order = shape == Shape::RowsOutOfOrder ? std::array<int, 3>{2, 0, 1} : std::array<int, 3>{1, 0, 2};
    std::array<IR::F32, 3> green;
    for (const int row : order) green[row] = ir.FPMul(channel(1, row), coef(row, 1));
    for (const int row : order)
        light[row] = ir.FPMul(ir.FPFma(channel(2, row), coef(row, 2), ir.FPFma(channel(0, row), coef(row, 0), green[row])),
                              ir.Imm32(scale));
    const float m1 = shape == Shape::NoPq ? 1.0f / 2.4f : 0.1593017578125f;
    for (const int c : order) {
        const IR::F32 y = ir.FPExp2(ir.FPMul(ir.FPLog2(ir.FPAbs(light[c])), ir.Imm32(m1)));
        const IR::F32 r = ir.FPMul(ir.FPFma(y, ir.Imm32(18.8515625f), ir.Imm32(0.8359375f)),
                                   IR::F32{ir.FPRecip(ir.FPFma(y, ir.Imm32(18.6875f), ir.Imm32(1.0f)))});
        ir.SetFragColor(0, c, ir.FPExp2(ir.FPMul(ir.FPLog2(ir.FPAbs(r)), ir.Imm32(78.84375f))));
    }
    ir.SetFragColor(0, 3, attr(3));
    for (IR::Inst& inst : block->Instructions())
        if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(1).U32() < 3) b.writes[inst.Arg(1).U32()] = &inst;
}

/// Whether every instruction's operands are made before it in the block
bool defined_before_use(const IR::Block& block) {
    std::unordered_map<const IR::Inst*, size_t> at;
    size_t i = 0;
    for (const IR::Inst& inst : block.Instructions()) at[&inst] = i++;
    for (const IR::Inst& inst : block.Instructions())
        for (size_t a = 0; a < inst.NumArgs(); a++)
            if (!inst.Arg(a).IsImmediate()) {
                const auto it = at.find(inst.Arg(a).InstRecursive());
                if (it == at.end() || it->second >= at[&inst]) return false;
            }
    return true;
}

struct Eval {
    std::array<float, 4> texel;
    std::array<float, PV::kSettingCount> settings{};
    std::unordered_map<const IR::Inst*, double> memo;
    double operator()(const IR::Value& v) {
        if (v.IsImmediate()) {
            const IR::Value r = v.Resolve();
            if (r.Type() == IR::Type::U1) return r.U1() ? 1.0 : 0.0;
            if (r.Type() == IR::Type::F32) return r.F32();
            return r.U32();
        }
        const IR::Inst* n = v.InstRecursive();
        if (const auto it = memo.find(n); it != memo.end()) return it->second;
        auto a = [&](size_t i) { return float((*this)(n->Arg(i))); };
        float r;
        switch (n->GetOpcode()) {
        case IR::Opcode::GetAttribute: r = texel[int(n->Arg(0).Attribute()) - int(IR::Attribute::Generic0X)]; break;
        case IR::Opcode::CompositeExtractF32x4: r = float((*this)(n->Arg(0).InstRecursive()->Arg(n->Arg(1).U32()))); break;
        case IR::Opcode::GetCbufF32: {
            const u32 bank = n->Arg(0).U32(), at = n->Arg(1).U32();
            if (bank == PV::kSettingsBank) {
                r = at % 4 == 0 && at / 4 < PV::kSettingCount ? settings[at / 4] : throw std::runtime_error("settings bank");
                break;
            }
            const u32 word = at / 4 - 72, row = word / 4, c = word % 4;
            r = bank == kBank && at % 4 == 0 && at / 4 >= 72 && row < 3 && c < 3 ? float(kGame[row][c] * kWhite)
                                                                               : throw std::runtime_error("cbuf");
            break;
        }
        case IR::Opcode::FPAdd32: r = a(0) + a(1); break;
        case IR::Opcode::FPMul32: r = a(0) * a(1); break;
        case IR::Opcode::FPFma32: r = std::fma(a(0), a(1), a(2)); break;
        case IR::Opcode::FPNeg32: r = -a(0); break;
        case IR::Opcode::FPAbs32: r = std::fabs(a(0)); break;
        case IR::Opcode::FPMax32: r = std::fmax(a(0), a(1)); break;
        case IR::Opcode::FPRecip32: r = 1.0f / a(0); break;
        case IR::Opcode::FPLog2: r = std::log2(a(0)); break;
        case IR::Opcode::FPExp2: r = std::exp2(a(0)); break;
        case IR::Opcode::SelectF32: r = a(0) != 0.0f ? a(1) : a(2); break;
        case IR::Opcode::FPOrdEqual32: r = a(0) == a(1); break;
        default: throw std::runtime_error("an opcode the fixture does not evaluate");
        }
        return memo[n] = r;
    }
};

/// RenoDX's: the colour space's BT.2020 light, at most 0, times the UI's white, PQ
std::array<double, 3> reference(const std::array<float, 4>& t, int space, double ui) {
    static constexpr double k709[3][3] = {{0.627403915, 0.329283029, 0.0433130674},
                                          {0.069097288, 0.919540346, 0.0113623189},
                                          {0.0163914394, 0.0880133137, 0.895595253}};
    static constexpr double kP3[3][3] = {{0.753833055, 0.198597386, 0.0475695953},
                                         {0.0457438454, 0.941777229, 0.0124789253},
                                         {-0.00121034053, 0.0176017173, 0.983608663}};
    std::array<double, 3> out;
    const double white = (kGame[0][0] + kGame[0][1] + kGame[0][2]) * kWhite;
    for (int c = 0; c < 3; c++) {
        const auto& m = space == 1 ? k709[c] : space == 2 ? kP3[c] : kGame[c];
        double linear = m[0] * t[0] + m[1] * t[1] + m[2] * t[2];
        if (space == 0) linear *= kWhite / white;
        const double y = std::pow(std::max(linear, 0.0) * ui / 10000.0, 0.1593017578125);
        out[c] = std::pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
    }
    return out;
}

int main() {
    const std::array<float, 4> colours[] = {{1.0f, 1.0f, 1.0f, 1.0f}, {0.18f, 0.18f, 0.18f, 1.0f}, {4.0f, 2.0f, 0.5f, 1.0f},
                                            {0.0f, 0.0f, 0.0f, 1.0f}, {1.2f, -0.05f, 0.3f, 0.5f}, {0.02f, 0.6f, 9.0f, 1.0f}};
    for (const Shape shape : {Shape::Game, Shape::RowsOutOfOrder}) {
        Built b;
        build(b, shape);
        const char* why = Optimization::OutputEncodePass(b.program);
        if (why) std::fprintf(stderr, "the game's shape refused: %s\n", why);
        CHECK(why == nullptr);
        CHECK(defined_before_use(*b.program.blocks[0]));
        const Info& info = b.program.info;
        CHECK((info.constant_buffer_mask & (1u << PV::kSettingsBank)) != 0);
        CHECK(info.constant_buffer_used_sizes[PV::kSettingsBank] >= PV::kSettingCount * 4);
        for (const int space : {0, 1, 2})
            for (const float ui : {80.0f, 203.0f, 480.0f})
                for (const auto& t : colours) {
                    Eval e{t, {}};
                    e.settings[PV::kSettingColorSpace] = float(space);
                    e.settings[PV::kSettingOutputScale] = ui / 10000.0f;
                    const auto want = reference(t, space, ui);
                    for (int c = 0; c < 3; c++) {
                        const double got = e(b.writes[c]->Arg(2));
                        if (!(std::fabs(got - want[c]) <= 5e-5)) {   // (PQ codes, 0 to 1: a 10-bit step is 1e-3)
                            std::fprintf(stderr, "channel %d of (%g %g %g), space %d, UI %g: %.8g, RenoDX %.8g\n", c, t[0], t[1],
                                         t[2], space, ui, got, want[c]);
                            return 1;
                        }
                    }
                }
    }
    std::printf("the game's shape: RenoDX's encoder, Vanilla (the game's matrix over its white), BT.709 and DCI-P3, in the UI's "
                "white, either order of rows\n");
    for (const Shape shape : {Shape::NoPq, Shape::OtherScale, Shape::TwoTextures}) {
        Built b;
        build(b, shape);
        std::array<IR::Value, 3> before;
        for (int c = 0; c < 3; c++) before[c] = b.writes[c]->Arg(2);
        size_t count = 0;
        for (const IR::Inst& inst : b.program.blocks[0]->Instructions()) (void)inst, count++;
        const char* why = Optimization::OutputEncodePass(b.program);
        CHECK(why != nullptr);
        size_t after = 0;
        for (const IR::Inst& inst : b.program.blocks[0]->Instructions()) (void)inst, after++;
        CHECK(after == count);
        for (int c = 0; c < 3; c++) CHECK(b.writes[c]->Arg(2) == before[c]);
        std::printf("not the game's encoder: left as it is (%s)\n", why);
    }
    std::printf("PASS\n");
    return 0;
}
