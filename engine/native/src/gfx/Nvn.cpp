// HeyListen native engine: the NVN procedures the graphics front end calls (hl/gfx/Nvn.h).
#include "hl/gfx/Nvn.h"

#include <cstdio>

namespace hl::gfx {

namespace {
struct ProcInfo {
    const char* name;
    ProcArgs args;
};
// (the argument counts are NVN's signatures: the object first, then its values; the OS layer's procedures read these
// registers and no others)
constexpr ProcInfo kProcs[kProcCount] = {
    {"nvnBlendStateSetDefaults", {1, 0}},
    {"nvnBlendStateSetBlendTarget", {2, 0}},
    {"nvnBlendStateSetBlendFunc", {5, 0}},
    {"nvnBlendStateSetBlendEquation", {3, 0}},
    {"nvnColorStateSetDefaults", {1, 0}},
    {"nvnColorStateSetBlendEnable", {3, 0}},
    {"nvnColorStateSetLogicOp", {2, 0}},
    {"nvnChannelMaskStateSetChannelMask", {6, 0}},
    {"nvnDepthStencilStateSetDepthTestEnable", {2, 0}},
    {"nvnDepthStencilStateSetDepthWriteEnable", {2, 0}},
    {"nvnDepthStencilStateSetDepthFunc", {2, 0}},
    {"nvnDepthStencilStateSetStencilTestEnable", {2, 0}},
    {"nvnDepthStencilStateSetStencilFunc", {3, 0}},
    {"nvnDepthStencilStateSetStencilOp", {5, 0}},
    {"nvnPolygonStateSetCullFace", {2, 0}},
    {"nvnPolygonStateSetFrontFace", {2, 0}},
    {"nvnPolygonStateSetPolygonMode", {2, 0}},
    {"nvnPolygonStateSetPolygonOffsetEnables", {2, 0}},
    {"nvnCommandBufferBindBlendState", {2, 0}},
    {"nvnCommandBufferBindColorState", {2, 0}},
    {"nvnCommandBufferSetBlendColor", {2, 0}},
    {"nvnCommandBufferBindChannelMaskState", {2, 0}},
    {"nvnCommandBufferBindDepthStencilState", {2, 0}},
    {"nvnCommandBufferSetStencilValueMask", {3, 0}},
    {"nvnCommandBufferSetStencilMask", {3, 0}},
    {"nvnCommandBufferSetStencilRef", {3, 0}},
    {"nvnCommandBufferBindPolygonState", {2, 0}},
    {"nvnCommandBufferSetPolygonOffsetClamp", {1, 3}},
    {"nvnCommandBufferSetDepthClamp", {2, 0}},
};
}   // namespace

const char* proc_name(uint32_t proc) { return proc < kProcCount ? kProcs[proc].name : nullptr; }

ProcArgs proc_args(uint32_t proc) { return proc < kProcCount ? kProcs[proc].args : ProcArgs{6, 4}; }

bool same_call(const Command& a, const Command& b) {
    if (a.op != b.op) return false;
    const ProcArgs n = proc_args(a.op);
    for (uint32_t i = 0; i < n.ints; i++)
        if (a.x[i] != b.x[i]) return false;
    for (uint32_t i = 0; i < n.floats; i++)
        if (a.f[i] != b.f[i]) return false;
    return true;
}

std::string describe(const Command& c) {
    char b[256];
    const ProcArgs n = proc_args(c.op);
    int k = proc_name(c.op) ? std::snprintf(b, sizeof b, "%s(", proc_name(c.op))
                            : std::snprintf(b, sizeof b, "proc#%u(", c.op);
    for (uint32_t i = 0; i < n.ints && k > 0 && (size_t)k < sizeof b; i++)
        k += std::snprintf(b + k, sizeof b - k, "%s%#llx", i ? ", " : "", (unsigned long long)c.x[i]);
    for (uint32_t i = 0; i < n.floats && k > 0 && (size_t)k < sizeof b; i++)
        k += std::snprintf(b + k, sizeof b - k, ", f%#x", c.f[i]);
    if (k > 0 && (size_t)k < sizeof b) std::snprintf(b + k, sizeof b - k, ")");
    return b;
}

}   // namespace hl::gfx
