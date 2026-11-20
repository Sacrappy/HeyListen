"""A sparse BC image is made with a usage the driver said it can make sparse, keeping its raw views when it can.

texture_create asks vkGetPhysicalDeviceSparseImageFormatProperties whether a too-big 2D array can be sparse, and for a BC
format it asks with TRANSFER_SRC | TRANSFER_DST | SAMPLED only. The image was then created with x.usage, which for BC
formats may also hold STORAGE and COLOR_ATTACHMENT (the raw block views): a usage with the sparse flags the driver was
never asked about, so the image's limits for it were undefined (VUID-VkImageCreateInfo-imageCreateMaxMipLevels-02251).
Now the sparse image's flags and usage are asked with vkGetPhysicalDeviceImageFormatProperties first; a BC image the
driver refuses that way is asked again without COLOR_ATTACHMENT, then without STORAGE, as a compressed image refused is
made in the non-sparse path, and x.usage follows what is made so its raw views are only made when the image has them.

The block of create() between the sparse flags and vkCreateImage is compiled against stubbed Vulkan calls, with no game
and no GPU: the stubs say which usages the "driver" accepts with the sparse flags, and record what vkCreateImage gets.
The usage gate of make_view is compiled with it, so each case also checks which raw views the image then gets.

python -I -B tests/test_sparse_bc_usage.py [--cxx g++] [--source ROOT]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdint>
#include <cstdio>
#include <vector>
typedef uint32_t VkFlags;
typedef VkFlags VkImageUsageFlags;
typedef int VkResult;
typedef int VkFormat;
typedef int VkImageType;
typedef int VkImageTiling;
typedef uint64_t VkImage;
typedef void* VkDevice;
typedef void* VkPhysicalDevice;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_FORMAT_NOT_SUPPORTED = -11;
constexpr VkFlags VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x1, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2,
                  VK_IMAGE_USAGE_SAMPLED_BIT = 0x4, VK_IMAGE_USAGE_STORAGE_BIT = 0x8,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10;
constexpr VkFlags VK_IMAGE_CREATE_SPARSE_BINDING_BIT = 0x1, VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT = 0x2;
constexpr VkFormat VK_FORMAT_R8G8B8A8_UNORM = 37, VK_FORMAT_BC7_UNORM_BLOCK = 145;
struct VkExtent3D { uint32_t width, height, depth; };
struct VkImageCreateInfo {
    VkFlags flags = 0;
    VkImageType imageType = 1;
    VkFormat format = 0;
    VkExtent3D extent{};
    uint32_t mipLevels = 1, arrayLayers = 1;
    VkFlags samples = 1;
    VkImageTiling tiling = 0;
    VkImageUsageFlags usage = 0;
};
struct VkImageFormatProperties {
    VkExtent3D maxExtent;
    uint32_t maxMipLevels, maxArrayLayers;
    VkFlags sampleCounts;
    uint64_t maxResourceSize;
};
struct Dev { VkDevice device = nullptr; VkPhysicalDevice physical = nullptr; } dev;
struct Tex { VkFormat format = 0; VkImageUsageFlags usage = 0; VkImage image = 0; } x;
bool is_bc(VkFormat f) { return f == VK_FORMAT_BC7_UNORM_BLOCK; }

VkImageUsageFlags refused;            // (usage bits the driver refuses with the sparse flags)
uint32_t max_layers = 2048;
std::vector<VkImageUsageFlags> asked; // (the usages vkGetPhysicalDeviceImageFormatProperties was asked with)
int made;
VkImageUsageFlags made_usage;
bool made_asked;                      // (vkCreateImage got a usage the format query accepted with its flags)
std::vector<VkImageUsageFlags> accepted;

VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling,
                                                  VkImageUsageFlags usage, VkFlags flags, VkImageFormatProperties* p) {
    asked.push_back(usage);
    if ((usage & refused) || !(flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *p = {{16384, 16384, 1}, 15, max_layers, 1, 1ull << 40};
    accepted.push_back(usage);
    return VK_SUCCESS;
}
VkResult vkCreateImage(VkDevice, const VkImageCreateInfo* ci, const void*, VkImage* img) {
    ++made, made_usage = ci->usage;
    made_asked = false;
    for (VkImageUsageFlags u : accepted) made_asked |= u == ci->usage;
    *img = 1;
    return VK_SUCCESS;
}

bool make(VkFormat format, VkImageUsageFlags usage, uint32_t layers) {
    x = {};
    x.format = format, x.usage = usage;
    asked.clear(), accepted.clear(), made = 0, made_usage = 0, made_asked = false;
    VkImageCreateInfo ci{};
    ci.format = format;
    ci.extent = {1024, 1024, 1};
    ci.mipLevels = 11, ci.arrayLayers = layers;
    ci.usage = usage;
    ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
@BLOCK@
    return ok;
}

// (the usage gate of make_view, from vk_texture.cpp: a raw view asks for a usage the image must hold)
uint64_t view_made(VkImageUsageFlags usage) {
    const Tex* tex = &x;
    constexpr uint64_t VK_NULL_HANDLE = 0;
@GATE@    return 1;
}

int failures;
void check(bool ok, const char* label) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
int main() {
    const VkImageUsageFlags base = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags raw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    refused = 0;
    bool ok = make(VK_FORMAT_BC7_UNORM_BLOCK, base | raw, 64);
    check(ok && made == 1 && made_usage == (base | raw) && x.usage == (base | raw),
          "a sparse BC image the driver takes with its raw usage keeps STORAGE and COLOR_ATTACHMENT (and its raw views)");
    check(made_asked, "... and is made with the usage the format query accepted with the sparse flags");
    check(view_made(VK_IMAGE_USAGE_STORAGE_BIT) && view_made(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
          "... and make_view then makes its raw storage and attachment views");

    refused = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ok = make(VK_FORMAT_BC7_UNORM_BLOCK, base | raw, 64);
    check(ok && made == 1 && made_asked && made_usage == (base | VK_IMAGE_USAGE_STORAGE_BIT) &&
              x.usage == made_usage,
          "a sparse BC image refused with COLOR_ATTACHMENT loses only that bit, and x.usage follows it");
    check(view_made(VK_IMAGE_USAGE_STORAGE_BIT) && !view_made(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
          "... and make_view still makes its raw storage view, not the attachment one");

    refused = raw;
    ok = make(VK_FORMAT_BC7_UNORM_BLOCK, base | raw, 64);
    check(ok && made == 1 && made_asked && made_usage == base && x.usage == base,
          "a sparse BC image refused with both raw usages is made without them, and x.usage follows it");
    check(!view_made(VK_IMAGE_USAGE_STORAGE_BIT) && !view_made(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
          "... and make_view makes neither raw view");

    refused = VK_IMAGE_USAGE_STORAGE_BIT;
    ok = make(VK_FORMAT_R8G8B8A8_UNORM, base | raw, 64);
    check(!ok && made == 0 && x.usage == (base | raw),
          "a sparse uncompressed image refused keeps its usage and is not made (its residency was asked with it)");

    refused = 0, max_layers = 32;
    ok = make(VK_FORMAT_BC7_UNORM_BLOCK, base | raw, 64);
    check(!ok && made == 0, "a sparse image past the format's limits for its flags and usage is not made");
    max_layers = 2048;

    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    src = (args.source / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
    flag = "        ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;\n"
    try:
        start = src.index(flag, src.index("Texture* create(const Key& key")) + len(flag)
        make = src.index("vkCreateImage(dev.device, &ci, nullptr, &x.image) == VK_SUCCESS;\n", start)
        end = src.index("\n", make) + 1
        gate = src.index("    if (!usage || (usage & tex->usage) != usage) return VK_NULL_HANDLE;\n",
                         src.index("VkImageView make_view(Texture* tex"))
        gate_end = src.index("\n", gate) + 1
    except ValueError:
        raise SystemExit("the sparse image is no longer made there in create(): update this test's anchors") from None
    with tempfile.TemporaryDirectory(prefix="sparse-bc-", ignore_cleanup_errors=True) as tmp:
        cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test.exe"
        cpp.write_text(FIXTURE.replace("@BLOCK@", src[start:end]).replace("@GATE@", src[gate:gate_end]), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-variable", str(cpp), "-o", str(exe)],
                       check=True, timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    print(result.stdout + result.stderr, end="")
    if result.returncode != 0 or "0 failures" not in result.stdout.splitlines():
        raise SystemExit("a sparse BC image is made with a usage the driver was not asked about with the sparse flags")
    print("a sparse BC image is made with a usage the driver accepts sparse, keeping its raw views when it can")


if __name__ == "__main__":
    main()
