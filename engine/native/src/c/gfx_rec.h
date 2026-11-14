// HeyListen native engine: where the render-state function's commands go (hl_gfx_rec.cpp; hl/c/hl_gfx_rec.h).
#pragma once

#include <functional>

#include "hl/gfx/Nvn.h"
#include "hl/gfx/RenderState.h"

namespace hl::c {

/// The render-state function's commands for one call, through the sink the OS layer's switches choose: the NVN
/// procedures (NvnSink, as before), the records written directly (HL_GFX_REC=1), or both compared (HL_GFX_REC_CHECK=1).
void gfx_rec_emit(const gfx::StateInputs& in, const gfx::StateOutputs& out, const gfx::StateHandles& at,
                  const gfx::NvnEntryPoints& nvn);
/// Any producer's commands for one command buffer through the same choice (emit makes them into the sink it is given,
/// once or, checking, twice); command_buffer 0: the NVN procedures. Only commands RecordSink covers may be emitted.
void gfx_rec_emit_commands(uint64_t command_buffer, const gfx::NvnEntryPoints& nvn,
                           const std::function<void(gfx::CommandSink&)>& emit);

}   // namespace hl::c
