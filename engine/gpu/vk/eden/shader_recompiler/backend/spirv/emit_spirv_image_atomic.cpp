// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <tuple>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/ir/modifiers.h"

namespace Shader::Backend::SPIRV {
namespace {
void DecorateNonUniform(EmitContext& ctx, Id object) {   // (as emit_spirv_image.cpp's)
    if (ctx.non_uniform_ids.insert(object.value).second) {
        ctx.Decorate(object, spv::Decoration::NonUniform);
    }
}

// (hookshot) The image an atomic works on: the image itself, or for an array (count above 1, DefineImages and
// DefineImageBuffers) the element the texture pass's index picks, as Image() in emit_spirv_image.cpp takes it for loads and
// stores; nonuniform (true) where the profile indexes that kind of array so. A single image still takes no index
Id Image(EmitContext& ctx, IR::TextureInstInfo info, const IR::Value& index, bool& non_uniform) {
    const bool buffer{info.type == TextureType::Buffer};
    const auto [id, pointer_type, count]{[&] {
        if (buffer) {
            const ImageBufferDefinition& def{ctx.image_buffers.at(info.descriptor_index)};
            return std::tuple{def.id, def.pointer_type, def.count};
        }
        const ImageDefinition& def{ctx.images.at(info.descriptor_index)};
        return std::tuple{def.id, def.pointer_type, def.count};
    }()};
    non_uniform = false;
    if (count <= 1) {
        if (!index.IsImmediate() || index.U32() != 0) {
            throw NotImplementedException("Image indexing");
        }
        return id;
    }
    const Id idx{index.IsImmediate() ? ctx.Const(index.U32()) : ctx.Def(index)};
    const Id element{ctx.OpAccessChain(pointer_type, id, idx)};
    non_uniform = !index.IsImmediate() && (buffer ? ctx.profile.support_storage_texel_buffer_array_nonuniform_indexing
                                                  : ctx.profile.support_storage_image_array_nonuniform_indexing);
    if (non_uniform) {
        DecorateNonUniform(ctx, idx);
        DecorateNonUniform(ctx, element);
        (buffer ? ctx.uses_nonuniform_storage_texel_buffer : ctx.uses_nonuniform_storage_image) = true;
    }
    return element;
}

std::pair<Id, Id> AtomicArgs(EmitContext& ctx) {
    const Id scope{ctx.Const(static_cast<u32>(spv::Scope::Device))};
    const Id semantics{ctx.u32_zero_value};
    return {scope, semantics};
}

Id ImageAtomicU32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords, Id value,
                  Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    bool non_uniform{};
    const Id image{Image(ctx, info, index, non_uniform)};
    const Id pointer{ctx.OpImageTexelPointer(ctx.image_u32, image, coords, ctx.Const(0U))};
    if (non_uniform) {
        DecorateNonUniform(ctx, pointer);
    }
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], pointer, scope, semantics, value);
}
} // Anonymous namespace

Id EmitImageAtomicIAdd32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                         Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitImageAtomicSMin32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                         Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitImageAtomicUMin32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                         Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitImageAtomicSMax32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                         Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitImageAtomicUMax32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                         Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitImageAtomicInc32(EmitContext&, IR::Inst*, const IR::Value&, Id, Id) {
    // TODO: This is not yet implemented
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitImageAtomicDec32(EmitContext&, IR::Inst*, const IR::Value&, Id, Id) {
    // TODO: This is not yet implemented
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitImageAtomicAnd32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                        Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitImageAtomicOr32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                       Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicOr);
}

Id EmitImageAtomicXor32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                        Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicXor);
}

Id EmitImageAtomicExchange32(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                             Id value) {
    return ImageAtomicU32(ctx, inst, index, coords, value, &Sirit::Module::OpAtomicExchange);
}

Id EmitBindlessImageAtomicIAdd32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicSMin32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicUMin32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicSMax32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicUMax32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicInc32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicDec32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicAnd32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicOr32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicXor32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBindlessImageAtomicExchange32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicIAdd32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicSMin32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicUMin32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicSMax32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicUMax32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicInc32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicDec32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicAnd32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicOr32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicXor32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBoundImageAtomicExchange32(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

} // namespace Shader::Backend::SPIRV
