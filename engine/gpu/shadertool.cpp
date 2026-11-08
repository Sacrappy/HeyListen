// shadertool: one of the game's NVN2 programs (host/nvn2.c HOS_SHADER_DUMP: <code address>.code, the program header
// then Ampere instructions) through Eglantine's shader recompiler: capture to the end, LowerToMaxwell, the Maxwell
// frontend, SPIR-V. The path mkw_gpu.dll will take for every program; this checks it offline.
//   shadertool <file.code> [out.spv]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

namespace {

constexpr u32 HeaderSize = sizeof(Shader::ProgramHeader);   // 0x50

// The environment the frontend reads the lowered program from (Eglantine's GenericEnvironment, without a GPU)
class FileEnv final : public Shader::Environment {
public:
    FileEnv(const std::vector<u8>& bytes, bool graphics) {
        using Shader::Ampere::Instruction;
        const u32 native_header = graphics ? 128 : 0;   // (Turing and later: a 128-byte program header)
        if (graphics) {
            std::memcpy(&sph, bytes.data(), sizeof sph);
            const u32 type = sph.common0.shader_type;   // Maxwell ShaderType: 1 vertex .. 5 pixel
            stage = type == 1 ? Shader::Stage::VertexB : type == 2 ? Shader::Stage::TessellationControl
                  : type == 3 ? Shader::Stage::TessellationEval : type == 4 ? Shader::Stage::Geometry
                  : Shader::Stage::Fragment;
        } else {
            stage = Shader::Stage::Compute;
        }
        std::vector<Instruction> native;
        Shader::Ampere::ProgramCaptureState capture;
        for (u32 i = 0; native_header + (i + 1) * sizeof(Instruction) <= bytes.size(); i++) {
            auto& insn = native.emplace_back();
            std::memcpy(&insn, bytes.data() + native_header + i * sizeof(Instruction), sizeof insn);
            const auto st = capture.Observe(insn, i);
            if (st == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget)
                throw Shader::RuntimeError("branch outside capture bounds at +{:#x}", i * 16);
            if (st == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
        }
        captured = native.size();
        auto lowered = Shader::Ampere::LowerToMaxwell(native);
        if (!lowered)
            throw Shader::RuntimeError("lowering failed at +{:#x}: {}", lowered.error_offset, lowered.error);
        if (lowered.required_texture_buffer != ~0U) texture_bound = lowered.required_texture_buffer;
        code.resize(sizeof(sph) / sizeof(u64));
        std::memcpy(code.data(), &sph, sizeof sph);
        for (const u64 instruction : lowered.instructions) {
            if ((code.size() * sizeof(u64)) % 32 == 0) code.push_back(0);   // the Maxwell scheduling word
            code.push_back(instruction);
        }
        code.push_back(0);
        Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
        start_address = 0;
    }

    u64 ReadInstruction(u32 address) override {
        if (address / 8 >= code.size()) throw Shader::RuntimeError("instruction address {:#x} out of range", address);
        return code[address / 8];
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(code, address)) return *t;
        throw Shader::RuntimeError("invalid native indirect branch address {:#x}", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        std::printf("  env: c%u[%#x] read\n", index, offset);
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat::A8B8G8R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return texture_bound; }
    u32 LocalMemorySize() const override { return 0x10000; }
    u32 SharedMemorySize() const override { return 0xC000; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

    std::vector<u64> code;
    size_t captured{};
    u32 texture_bound = 0;
};

}   // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: shadertool <file.code> [out.spv]\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
    try {
        // a graphics program starts with its header (shader type 1-5 in common0); compute programs do not
        Shader::ProgramHeader probe{};
        std::memcpy(&probe, bytes.data(), sizeof probe);
        const u32 type = probe.common0.shader_type;
        bool graphics = type >= 1 && type <= 5;
        // the control section beside it (<code address>.ctl) has the NVN stage: 5 is compute (a compute program's
        // first word can look like a header)
        std::string ctl_path = argv[1];
        if (ctl_path.size() > 5 && ctl_path.substr(ctl_path.size() - 5) == ".code") {
            ctl_path = ctl_path.substr(0, ctl_path.size() - 5) + ".ctl";
            std::ifstream cin_(ctl_path, std::ios::binary);
            std::vector<u8> ctl((std::istreambuf_iterator<char>(cin_)), {});
            if (ctl.size() >= 0x58) {
                u16 st;
                std::memcpy(&st, ctl.data() + 0x56, 2);
                graphics = st != 5;
            }
        }
        FileEnv env(bytes, graphics);
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
        Shader::ObjectPool<Shader::IR::Inst> insts;
        Shader::ObjectPool<Shader::IR::Block> blocks;
        Shader::HostTranslateInfo host{};
        host.support_float64 = host.support_float16 = host.support_int64 = true;
        host.min_ssbo_alignment = 16;
        // (the lowered code starts after the header's 0x50 bytes, zeros for compute: the native address map assumes it)
        Shader::Maxwell::Flow::CFG cfg(env, flow_blocks, env.StartAddress() + HeaderSize, true);
        Shader::IR::Program program = Shader::Maxwell::TranslateProgram(insts, blocks, env, cfg, host);
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010600;
        profile.support_int8 = profile.support_int16 = profile.support_int64 = true;
        profile.support_descriptor_aliasing = profile.support_float_controls = true;
        profile.support_vertex_instance_id = true;
        profile.support_separate_denorm_behavior = profile.support_separate_rounding_mode = true;
        std::vector<u32> spirv;
        if (graphics) {
            Shader::RuntimeInfo runtime{};
            Shader::Backend::Bindings bindings{};
            spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        } else {
            spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
        }
        std::printf("%s: %s, %zu native instructions captured, %zu Maxwell words, SPIR-V %zu words\n", argv[1],
                    graphics ? "graphics" : "compute", env.captured, env.code.size(), spirv.size());
        // what the program reads from the constant banks (the driver's layout: NVN state the replay must answer)
        const Shader::Info& info = program.info;
        for (u32 i = 0; i < Shader::Info::MAX_CBUFS; i++)
            if (info.constant_buffer_mask & (1u << i))
                std::printf("  c%u: %#x bytes used\n", i, info.constant_buffer_used_sizes[i]);
        for (const auto& d : info.storage_buffers_descriptors)
            std::printf("  storage buffer at c%u[%#x] x%u%s\n", d.cbuf_index, d.cbuf_offset, d.count,
                        d.is_written ? " written" : "");
        for (const auto& d : info.texture_descriptors)
            std::printf("  texture type %d handle c%u[%#x]%s x%u\n", (int)d.type, d.cbuf_index, d.cbuf_offset,
                        d.has_secondary ? " (+secondary)" : "", d.count);
        for (const auto& d : info.texture_buffer_descriptors)
            std::printf("  texture buffer handle c%u[%#x]\n", d.cbuf_index, d.cbuf_offset);
        for (const auto& d : info.image_descriptors) std::printf("  image handle c%u[%#x]\n", d.cbuf_index, d.cbuf_offset);
        std::printf("  texture bound buffer c%u\n", env.texture_bound);
        if (argc > 2) {
            std::ofstream out(argv[2], std::ios::binary);
            out.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4);
        }
    } catch (const std::exception& e) {
        std::printf("%s: FAILED: %s\n", argv[1], e.what());
        return 1;
    }
    return 0;
}
