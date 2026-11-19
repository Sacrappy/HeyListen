// SPDX-License-Identifier: GPL-3.0-or-later
// Descriptor arrays of image buffers, storage images and texture buffers through the real texture pass and SPIR-V back end:
// synthetic compute programs, no game or GPU (nx2/test_image_arrays.py builds, runs and validates them).
//
// A handle read from a constant buffer at a dynamic offset (base + element * 8) is a descriptor array: the texture pass
// sizes it by the words left after the base (DynamicDescriptorCount) and indexes it with the element. The bases here
// leave room for 2 or 3 descriptors. The same programs with the handle at a constant offset are single descriptors.
// Image buffers and storage images are read and written, or added to atomically; texture buffers and textures fetched.
//
//     image_arrays_ir <output folder>
// writes <case>.spv for each case and prints one line a case:
//     <case> <words> texbuf:<counts> imgbuf:<counts> tex:<counts> img:<counts>   (Shader::Info's descriptors, in order)
//     <case> refused: <what the translation threw>
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"

using namespace Shader;

struct TestEnvironment final : Environment {
    TestEnvironment() { stage = Stage::Compute; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 1; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
};

enum class Kind { TextureBuffer, ImageBuffer, Texture, Image, ImageBufferAtomic, ImageAtomic };

struct Array {
    Kind kind;
    u32 count;   // (1: a handle at a constant offset, a single descriptor)
};

struct Case {
    std::string name;
    std::vector<Array> arrays;
    bool nonuniform;      // (the device indexes every kind of descriptor array nonuniformly: NonUniform decorations)
    bool fetch_bounds;    // (Shader::Backend::SPIRV::fetch_bounds and fetch_bounds_buffers: texel fetches bounded by the size)
};

// The texture pass's limit with no device limits (HostTranslateInfo::DEFAULT_DESCRIPTOR_LIMIT) reads 16 bytes a
// descriptor: a base 8 * count bytes before that end leaves room for count descriptors of 8 bytes
constexpr u32 kCbufEnd = 16 * HostTranslateInfo::DEFAULT_DESCRIPTOR_LIMIT;

IR::U32 Handle(IR::IREmitter& ir, u32 cbuf, u32 count, const IR::U32& element) {
    if (count == 1) {
        return ir.GetCbuf(ir.Imm32(cbuf), ir.Imm32(0x40));
    }
    const IR::U32 offset{ir.IAdd(ir.Imm32(kCbufEnd - 8 * count), IR::U32{ir.ShiftLeftLogical(element, ir.Imm32(3))})};
    return ir.GetCbuf(ir.Imm32(cbuf), offset);
}

IR::Value AsU32x4(IR::IREmitter& ir, const IR::Value& color) {
    std::array<IR::U32, 4> parts;
    for (size_t i = 0; i < parts.size(); ++i) {
        parts[i] = ir.BitCast<IR::U32, IR::F32>(IR::F32{ir.CompositeExtract(color, i)});
    }
    return ir.CompositeConstruct(parts[0], parts[1], parts[2], parts[3]);
}

std::vector<u32> Emit(const Case& c, Info& info_out) {
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {64, 1, 1};
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    IR::AbstractSyntaxNode ret{};
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.push_back(ret);

    IR::IREmitter ir{block};
    const IR::U32 lane{ir.LocalInvocationIdX()};   // (each invocation its own element: nonuniform)
    const IR::U32 next{ir.IAdd(lane, ir.Imm32(1))};
    IR::TextureInstInfo out{};   // (where sampled reads are stored: a single image buffer bound at c0[0x100])
    out.type.Assign(TextureType::Buffer);
    out.image_format.Assign(ImageFormat::R32_UINT);
    u32 cbuf{2};
    for (const Array& array : c.arrays) {
        const IR::U32 handle{Handle(ir, cbuf++, array.count, lane)};
        IR::TextureInstInfo info{};
        info.image_format.Assign(ImageFormat::R32_UINT);
        switch (array.kind) {
        case Kind::ImageBuffer: {
            info.type.Assign(TextureType::Buffer);
            const IR::Value texel{ir.ImageRead(handle, lane, info)};
            ir.ImageWrite(handle, next, texel, info);
            break;
        }
        case Kind::Image: {
            info.type.Assign(TextureType::Color2D);
            const IR::Value texel{ir.ImageRead(handle, ir.CompositeConstruct(lane, ir.Imm32(0)), info)};
            ir.ImageWrite(handle, ir.CompositeConstruct(next, ir.Imm32(1)), texel, info);
            break;
        }
        case Kind::TextureBuffer: {
            info.type.Assign(TextureType::Buffer);
            const IR::Value texel{ir.ImageFetch(handle, lane, {}, {}, {}, info)};
            ir.ImageWrite(ir.Imm32(0x100), lane, AsU32x4(ir, texel), out);
            break;
        }
        case Kind::Texture: {
            info.type.Assign(TextureType::Color2D);
            const IR::Value texel{
                ir.ImageFetch(handle, ir.CompositeConstruct(lane, ir.Imm32(0)), {}, ir.Imm32(0), {}, info)};
            ir.ImageWrite(ir.Imm32(0x100), next, AsU32x4(ir, texel), out);
            break;
        }
        case Kind::ImageBufferAtomic: {
            info.type.Assign(TextureType::Buffer);
            const IR::U32 old{ir.ImageAtomicIAdd(handle, lane, ir.Imm32(1), info)};
            ir.ImageWrite(ir.Imm32(0x100), lane, ir.CompositeConstruct(old, old, old, old), out);
            break;
        }
        case Kind::ImageAtomic: {
            info.type.Assign(TextureType::Color2D);
            const IR::U32 old{ir.ImageAtomicIAdd(handle, ir.CompositeConstruct(lane, ir.Imm32(0)), ir.Imm32(1), info)};
            ir.ImageWrite(ir.Imm32(0x100), lane, ir.CompositeConstruct(old, old, old, old), out);
            break;
        }
        }
    }
    ir.Epilogue();

    TestEnvironment env;
    HostTranslateInfo host_info{};
    host_info.support_snorm_render_buffer = true;
    host_info.ApplyDescriptorLimitPolicy();   // (as TranslateProgram: no device limits, the defaults)
    Optimization::TexturePass(env, program, host_info);
    Optimization::DeadCodeEliminationPass(program);
    Optimization::CollectShaderInfoPass(env, program);
    info_out = program.info;

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_sampled_image_array_nonuniform_indexing = c.nonuniform;
    profile.support_storage_image_array_nonuniform_indexing = c.nonuniform;
    profile.support_uniform_texel_buffer_array_nonuniform_indexing = c.nonuniform;
    profile.support_storage_texel_buffer_array_nonuniform_indexing = c.nonuniform;
    // (upstream sets Backend::SPIRV::fetch_bounds and fetch_bounds_buffers here from c.fetch_bounds: the bounded fetches are not in this tree)
    return Backend::SPIRV::EmitSPIRV(profile, program);
}

template <typename Descriptors>
std::string Counts(const Descriptors& descriptors) {
    std::string text;
    for (const auto& desc : descriptors) {
        text += (text.empty() ? "" : ",") + std::to_string(desc.count);
    }
    return text;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: image_arrays_ir <output folder>\n");
        return 2;
    }
    const std::filesystem::path folder{argv[1]};
    std::filesystem::create_directories(folder);
    const std::array<std::pair<const char*, std::vector<Array>>, 12> programs{{
        {"imgbuf-1", {{Kind::ImageBuffer, 1}}},
        {"img-1", {{Kind::Image, 1}}},
        {"texbuf-1", {{Kind::TextureBuffer, 1}}},
        {"imgbuf-atomic-1", {{Kind::ImageBufferAtomic, 1}}},
        {"img-atomic-1", {{Kind::ImageAtomic, 1}}},
        {"mixed-1", {{Kind::TextureBuffer, 1}, {Kind::ImageBuffer, 1}, {Kind::Texture, 1}, {Kind::Image, 1}}},
        {"imgbuf-2", {{Kind::ImageBuffer, 2}}},
        {"img-3", {{Kind::Image, 3}}},
        {"texbuf-2", {{Kind::TextureBuffer, 2}}},
        {"imgbuf-atomic-2", {{Kind::ImageBufferAtomic, 2}}},
        {"img-atomic-3", {{Kind::ImageAtomic, 3}}},
        {"mixed", {{Kind::TextureBuffer, 2}, {Kind::ImageBuffer, 2}, {Kind::Texture, 2}, {Kind::Image, 3}}},
    }};
    std::vector<Case> cases;
    for (const auto& [name, arrays] : programs) {
        cases.push_back({name, arrays, false, false});
        cases.push_back({std::string{name} + "-nonuniform", arrays, true, false});
        if (std::string_view{name}.starts_with("texbuf") || std::string_view{name}.starts_with("mixed")) {
            cases.push_back({std::string{name} + "-bounds", arrays, false, true});
        }
    }
    for (const Case& c : cases) {
        Info info;
        std::vector<u32> words;
        try {
            words = Emit(c, info);
        } catch (const std::exception& e) {
            std::printf("%s refused: %s\n", c.name.c_str(), e.what());
            continue;
        }
        std::ofstream file(folder / (c.name + ".spv"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
        if (!file) {
            std::fprintf(stderr, "%s: not written\n", c.name.c_str());
            return 3;
        }
        std::printf("%s %zu texbuf:%s imgbuf:%s tex:%s img:%s\n", c.name.c_str(), words.size(),
                    Counts(info.texture_buffer_descriptors).c_str(), Counts(info.image_buffer_descriptors).c_str(),
                    Counts(info.texture_descriptors).c_str(), Counts(info.image_descriptors).c_str());
    }
    return 0;
}
