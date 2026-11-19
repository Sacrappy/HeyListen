"""Wave64 GPUs (issue 67): the translator emulates the guest's 32-lane warps on a wider subgroup, only where the GPU cannot run
a stage at 32 lanes, and the cache stamp carries that only then. Reads the sources (no GPU, no game)."""
import re
from pathlib import Path

vk = Path(__file__).resolve().parents[1] / "gpu/vk"
main = (vk / "vk_main.cpp").read_text(encoding="utf-8")
shader = (vk / "vk_shader.cpp").read_text(encoding="utf-8")
internal = (vk / "vk_internal.h").read_text(encoding="utf-8")
draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")

def body(src, head):
    i = src.index(head)
    j = src.index("\n}\n", i)
    return src[i:j]

# the device decides: wide when the stage cannot be pinned to 32 and the default is wider; RX_VK_WARP_WIDE forces it
assert "warp_wide_stages" in internal and "warp_lanes" in internal
assert "dev.warp_wide_stages = kStages & ~dev.sg32_stages;" in main, "per stage: the ones the GPU does not pin to 32 lanes (RDNA 3.5 vertex, issue 37)"
assert re.search(r"else if \(sg\.subgroupSize > 32\)", main), "only with a default wider than 32"
assert "RX_VK_WARP_WIDE" in main
assert "if (forced) dev.warp_wide_stages = kStages;" in main
# a forced run uses a width over 32, so the wide code is made even on a 32-lane GPU
assert re.search(r"forced \? 64u : 33u", main)

# the profile follows it, per stage, and the narrow one is what it was
mk = body(shader, "Shader::Profile make_profile(bool wide) {")
assert "p.warp_size_potentially_larger_than_guest = wide;" in mk
assert "p.max_subgroup_size = wide ? dev.warp_lanes : 32;" in mk
pf = body(shader, "const Shader::Profile& profile(VkShaderStageFlagBits stage) {")
assert "make_profile(false)" in pf and "make_profile(true)" in pf and "dev.warp_wide_stages & stage" in pf
assert "profile()" not in shader, "every translation names its stage"
emits = re.findall(r"EmitSPIRV\(profile\(([^)]*(?:\([^)]*\))?[^)]*)\)", shader)
assert len(emits) == 7, emits
assert "VK_SHADER_STAGE_COMPUTE_BIT" in emits[0] and "VK_SHADER_STAGE_VERTEX_BIT" in emits[1]
assert any("fallback ? VK_SHADER_STAGE_COMPUTE_BIT" in e for e in emits), "a fallback task / mesh program runs as compute"

# the stamp: added only when on, so a 32-lane GPU keeps its cache
st = body(shader, "uint64_t disk_stamp() {")
m = re.search(r"if \(dev\.warp_wide_stages\) \{(.*?)\n    \}\n", st, re.S)
assert m and "dev.warp_lanes" in m.group(1) and "dev.warp_wide_stages" in m.group(1), "the stamp carries the wide stages and their width, only then"
assert st.index("dev.warp_wide_stages") > st.index("handle_tables_on()"), "added after the existing inputs, not among them"

# a stage the GPU cannot hold to 32 lanes is never asked to (VUID-VkPipelineShaderStageRequiredSubgroupSizeCreateInfo-requiredSubgroupSize-02761)
assert "if (!(dev.sg32_stages & st.stage)) return;" in draw
assert "dev.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT" in (vk / "vk_coopmat.cpp").read_text(encoding="utf-8")
assert "dev.sg32_stages = pin ? sc.requiredSubgroupSizeStages : 0;" in main
print("PASS: wide-warp translation only where 32 lanes cannot be pinned; the stamp carries it only then")
