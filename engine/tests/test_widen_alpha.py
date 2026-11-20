"""RGB textures widened to RGBA get an alpha of one in their own format, not one picked by component size alone.

Vulkan devices rarely sample three-component formats, so upload_inner (gpu/vk/vk_texture.cpp) widens RGB rows to RGBA
with widen_rows. The alpha it added was chosen by component size alone: 0xFF, half 1.0 or 1.0f. That read as -0.008 for
R8G8B8_SNORM, 0.23 for R16G16B16_UNORM, 0.47 for R16G16B16_SNORM, and 255, 15360 or 0x3F800000 for the
integer formats. widen_alpha gives each format its own one. This compiles widen_alpha and widen_rows out of the source and
checks the alpha bytes for every format widened() widens.

python -I -B tests/test_widen_alpha.py --cxx clang++ --headers <Vulkan-Headers include>
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

/* ACTUAL */

struct Case { VkFormat f; int from, to; uint64_t alpha; const char* name; };
int main() {
    const Case cases[] = {
        {VK_FORMAT_R8G8B8_UNORM, 3, 4, 0xFF, "R8G8B8_UNORM"},
        {VK_FORMAT_R8G8B8_SNORM, 3, 4, 0x7F, "R8G8B8_SNORM"},
        {VK_FORMAT_R8G8B8_UINT, 3, 4, 1, "R8G8B8_UINT"},
        {VK_FORMAT_R8G8B8_SINT, 3, 4, 1, "R8G8B8_SINT"},
        {VK_FORMAT_R16G16B16_SFLOAT, 6, 8, 0x3C00, "R16G16B16_SFLOAT"},
        {VK_FORMAT_R16G16B16_UNORM, 6, 8, 0xFFFF, "R16G16B16_UNORM"},
        {VK_FORMAT_R16G16B16_SNORM, 6, 8, 0x7FFF, "R16G16B16_SNORM"},
        {VK_FORMAT_R16G16B16_UINT, 6, 8, 1, "R16G16B16_UINT"},
        {VK_FORMAT_R16G16B16_SINT, 6, 8, 1, "R16G16B16_SINT"},
        {VK_FORMAT_R32G32B32_SFLOAT, 12, 16, 0x3F800000, "R32G32B32_SFLOAT"},
        {VK_FORMAT_R32G32B32_UINT, 12, 16, 1, "R32G32B32_UINT"},
        {VK_FORMAT_R32G32B32_SINT, 12, 16, 1, "R32G32B32_SINT"},
    };
    int bad = 0;
    for (const Case& c : cases) {
        uint8_t src[2 * 12], dst[2 * 16];
        for (int i = 0; i < 2 * c.from; i++) src[i] = (uint8_t)(0x40 + i);
        std::memset(dst, 0xEE, sizeof dst);
        widen_rows(dst, src, 2, c.from, c.to, widen_alpha(c.f));
        for (int t = 0; t < 2; t++) {
            uint64_t a = 0;
            std::memcpy(&a, dst + t * c.to + c.from, c.to - c.from);
            if (std::memcmp(dst + t * c.to, src + t * c.from, c.from) || a != c.alpha) {
                std::printf("%s: texel %d alpha %#llx, wanted %#llx\n", c.name, t, (unsigned long long)a,
                            (unsigned long long)c.alpha);
                bad++;
            }
        }
    }
    return bad ? 1 : 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    tex = (a.source / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    if "uint32_t widen_alpha(VkFormat f) {" not in tex:
        raise SystemExit("widen_rows picks the alpha by component size alone: no widen_alpha(VkFormat)")
    if "widen_alpha(src_vf)" not in block(tex, "bool upload_inner(Texture& tex, const Ranges* dirty) {"):
        raise SystemExit("upload_inner does not give widen_rows the source format's alpha")
    actual = block(tex, "uint32_t widen_alpha(VkFormat f) {") + "\n" + block(tex, "void widen_rows(")
    with tempfile.TemporaryDirectory(prefix="widen-alpha-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(FIXTURE.replace("/* ACTUAL */", actual), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(a.headers), str(cpp), "-o",
                        str(exe)], check=True)
        if subprocess.run([str(exe)], timeout=15).returncode:
            raise SystemExit("an RGB format widened to RGBA has the wrong alpha")
    print("RGB formats widened to RGBA get their own alpha one: ok")


if __name__ == "__main__":
    main()
