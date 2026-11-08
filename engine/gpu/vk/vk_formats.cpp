// NVN texture formats (gpu/render/nvn_api.h) as Vulkan formats, after the WebGPU renderer's nvn_pixel_format
// (gpu/render/rx_nvn.cpp): the same NVN -> suyu pixel format table, each suyu format as its Vulkan one. ASTC, which
// desktop GPUs do not sample, is decoded to RGBA8 (astc: the block size).
#include "vk_internal.h"

namespace mkwvk {


// ASTC (NVN 0x79..0x94): desktop GPUs sample it (textureCompressionASTC_LDR), so the blocks are uploaded as they are
// instead of being decoded. Blocks are 16 bytes; the block is 4x4 .. 12x12.
namespace {
constexpr int kAstcBw[14] = {4, 5, 5, 6, 6, 8, 8, 8, 10, 10, 10, 10, 12, 12};
constexpr int kAstcBh[14] = {4, 4, 5, 5, 6, 5, 6, 8, 5, 6, 8, 10, 10, 12};
constexpr VkFormat kAstcVk[14] = {
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
    VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
constexpr VkFormat kAstcVkSrgb[14] = {
    VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_ASTC_5x4_SRGB_BLOCK, VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_6x5_SRGB_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK, VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_8x6_SRGB_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK, VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x6_SRGB_BLOCK, VK_FORMAT_ASTC_10x8_SRGB_BLOCK, VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
    VK_FORMAT_ASTC_12x10_SRGB_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
} // namespace

namespace {
/// Whether this GPU samples ASTC at all (the desktop NVIDIA drivers do not: those blocks are decoded on upload)
bool astc_native() {
    static signed char known = -1;
    if (known < 0) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &p);
        known = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1 : 0;
        logf("vk: ASTC %s", known ? "sampled natively" : "not sampled by this GPU: blocks are decoded on upload");
    }
    return known == 1;
}
} // namespace

VkFormatInfo vk_format(int nvn_format) {
    if (nvn_format >= 0x79 && nvn_format <= 0x94) {
        const int i = (nvn_format - 0x79) % 14;
        const bool srgb = nvn_format >= 0x87;
        if (astc_native()) return {srgb ? kAstcVkSrgb[i] : kAstcVk[i], false, kAstcBw[i], kAstcBh[i]};
        return {srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, true, kAstcBw[i], kAstcBh[i]};
    }
    switch (nvn_format) {
    case NVN_FORMAT_R8: return {VK_FORMAT_R8_UNORM, false, 1, 1};
    case NVN_FORMAT_R8SN: return {VK_FORMAT_R8_SNORM, false, 1, 1};
    case NVN_FORMAT_R8UI: return {VK_FORMAT_R8_UINT, false, 1, 1};
    case NVN_FORMAT_R8I: return {VK_FORMAT_R8_SINT, false, 1, 1};
    case NVN_FORMAT_R16F: return {VK_FORMAT_R16_SFLOAT, false, 1, 1};
    case NVN_FORMAT_R16: return {VK_FORMAT_R16_UNORM, false, 1, 1};
    case NVN_FORMAT_R16SN: return {VK_FORMAT_R16_SNORM, false, 1, 1};
    case NVN_FORMAT_R16UI: return {VK_FORMAT_R16_UINT, false, 1, 1};
    case NVN_FORMAT_R16I: return {VK_FORMAT_R16_SINT, false, 1, 1};
    case NVN_FORMAT_R32F: return {VK_FORMAT_R32_SFLOAT, false, 1, 1};
    case NVN_FORMAT_R32UI: return {VK_FORMAT_R32_UINT, false, 1, 1};
    case NVN_FORMAT_R32I: return {VK_FORMAT_R32_SINT, false, 1, 1};
    case NVN_FORMAT_RG8: return {VK_FORMAT_R8G8_UNORM, false, 1, 1};
    case NVN_FORMAT_RG8SN: return {VK_FORMAT_R8G8_SNORM, false, 1, 1};
    case NVN_FORMAT_RG8UI: return {VK_FORMAT_R8G8_UINT, false, 1, 1};
    case NVN_FORMAT_RG8I: return {VK_FORMAT_R8G8_SINT, false, 1, 1};
    case NVN_FORMAT_RG16F: return {VK_FORMAT_R16G16_SFLOAT, false, 1, 1};
    case NVN_FORMAT_RG16: return {VK_FORMAT_R16G16_UNORM, false, 1, 1};
    case NVN_FORMAT_RG16SN: return {VK_FORMAT_R16G16_SNORM, false, 1, 1};
    case NVN_FORMAT_RG16UI: return {VK_FORMAT_R16G16_UINT, false, 1, 1};
    case NVN_FORMAT_RG16I: return {VK_FORMAT_R16G16_SINT, false, 1, 1};
    case NVN_FORMAT_RG32F: return {VK_FORMAT_R32G32_SFLOAT, false, 1, 1};
    case NVN_FORMAT_RG32UI: return {VK_FORMAT_R32G32_UINT, false, 1, 1};
    case NVN_FORMAT_RG32I: return {VK_FORMAT_R32G32_SINT, false, 1, 1};
    case NVN_FORMAT_RGB32F: return {VK_FORMAT_R32G32B32_SFLOAT, false, 1, 1};
    case NVN_FORMAT_RGBX8: return {VK_FORMAT_R8G8B8A8_UNORM, false, 1, 1};
    case NVN_FORMAT_RGBX8SN: return {VK_FORMAT_R8G8B8A8_SNORM, false, 1, 1};
    case NVN_FORMAT_RGBX8UI: return {VK_FORMAT_R8G8B8A8_UINT, false, 1, 1};
    case NVN_FORMAT_RGBX8I: return {VK_FORMAT_R8G8B8A8_SINT, false, 1, 1};
    case NVN_FORMAT_RGBA16F: return {VK_FORMAT_R16G16B16A16_SFLOAT, false, 1, 1};
    case NVN_FORMAT_RGBX16F: return {VK_FORMAT_R16G16B16A16_SFLOAT, false, 1, 1};
    case NVN_FORMAT_RGBX16: return {VK_FORMAT_R16G16B16A16_UNORM, false, 1, 1};
    case NVN_FORMAT_RGBX16SN: return {VK_FORMAT_R16G16B16A16_SNORM, false, 1, 1};
    case NVN_FORMAT_RGBX16UI: return {VK_FORMAT_R16G16B16A16_UINT, false, 1, 1};
    case NVN_FORMAT_RGBX16I: return {VK_FORMAT_R16G16B16A16_SINT, false, 1, 1};
    case NVN_FORMAT_RGBX32F: return {VK_FORMAT_R32G32B32A32_SFLOAT, false, 1, 1};
    case NVN_FORMAT_RGBX32UI: return {VK_FORMAT_R32G32B32A32_UINT, false, 1, 1};
    case NVN_FORMAT_RGBX32I: return {VK_FORMAT_R32G32B32A32_SINT, false, 1, 1};
    case NVN_FORMAT_RGB8: return {VK_FORMAT_R8G8B8_UNORM, false, 1, 1};
    case NVN_FORMAT_RGB8SN: return {VK_FORMAT_R8G8B8_SNORM, false, 1, 1};
    case NVN_FORMAT_RGB8UI: return {VK_FORMAT_R8G8B8_UINT, false, 1, 1};
    case NVN_FORMAT_RGB8I: return {VK_FORMAT_R8G8B8_SINT, false, 1, 1};
    case NVN_FORMAT_RGBA8: return {VK_FORMAT_R8G8B8A8_UNORM, false, 1, 1};
    case NVN_FORMAT_RGBA8SN: return {VK_FORMAT_R8G8B8A8_SNORM, false, 1, 1};
    case NVN_FORMAT_RGBA8UI: return {VK_FORMAT_R8G8B8A8_UINT, false, 1, 1};
    case NVN_FORMAT_RGBA8I: return {VK_FORMAT_R8G8B8A8_SINT, false, 1, 1};
    case NVN_FORMAT_RGBX8_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, false, 1, 1};
    case NVN_FORMAT_RGBA8_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, false, 1, 1};
    case NVN_FORMAT_RGB5: return {VK_FORMAT_B5G5R5A1_UNORM_PACK16, false, 1, 1};
    case NVN_FORMAT_RGBA16: return {VK_FORMAT_R16G16B16A16_UNORM, false, 1, 1};
    case NVN_FORMAT_RGBA16SN: return {VK_FORMAT_R16G16B16A16_SNORM, false, 1, 1};
    case NVN_FORMAT_RGBA16UI: return {VK_FORMAT_R16G16B16A16_UINT, false, 1, 1};
    case NVN_FORMAT_RGBA16I: return {VK_FORMAT_R16G16B16A16_SINT, false, 1, 1};
    case NVN_FORMAT_RGBA32F: return {VK_FORMAT_R32G32B32A32_SFLOAT, false, 1, 1};
    case NVN_FORMAT_RGBA32UI: return {VK_FORMAT_R32G32B32A32_UINT, false, 1, 1};
    case NVN_FORMAT_RGBA32I: return {VK_FORMAT_R32G32B32A32_SINT, false, 1, 1};
    case NVN_FORMAT_STENCIL8: return {VK_FORMAT_S8_UINT, false, 1, 1};
    case NVN_FORMAT_DEPTH16: return {VK_FORMAT_D16_UNORM, false, 1, 1};
    case NVN_FORMAT_DEPTH24: return {VK_FORMAT_X8_D24_UNORM_PACK32, false, 1, 1};
    case NVN_FORMAT_DEPTH32F: return {VK_FORMAT_D32_SFLOAT, false, 1, 1};
    case NVN_FORMAT_DEPTH24_STENCIL8: return {VK_FORMAT_D24_UNORM_S8_UINT, false, 1, 1};
    case NVN_FORMAT_DEPTH32F_STENCIL8: return {VK_FORMAT_D32_SFLOAT_S8_UINT, false, 1, 1};
    case NVN_FORMAT_RGBA4: return {VK_FORMAT_A4B4G4R4_UNORM_PACK16, false, 1, 1};
    case NVN_FORMAT_RGB5A1: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, false, 1, 1};
    case NVN_FORMAT_RGB565: return {VK_FORMAT_B5G6R5_UNORM_PACK16, false, 1, 1};
    case NVN_FORMAT_BGR565: return {VK_FORMAT_R5G6B5_UNORM_PACK16, false, 1, 1};
    case NVN_FORMAT_RGB10A2: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, 1, 1};
    case NVN_FORMAT_RGB10A2UI: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, false, 1, 1};
    case NVN_FORMAT_R11G11B10F: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, false, 1, 1};
    case NVN_FORMAT_RGB9E5F: return {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, false, 1, 1};
    case NVN_FORMAT_RGBA_DXT1: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGBA_DXT1_SRGB: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGBA_DXT3: return {VK_FORMAT_BC2_UNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGBA_DXT3_SRGB: return {VK_FORMAT_BC2_SRGB_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGBA_DXT5: return {VK_FORMAT_BC3_UNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGBA_DXT5_SRGB: return {VK_FORMAT_BC3_SRGB_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGTC1_UNORM: return {VK_FORMAT_BC4_UNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGTC1_SNORM: return {VK_FORMAT_BC4_SNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGTC2_UNORM: return {VK_FORMAT_BC5_UNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_RGTC2_SNORM: return {VK_FORMAT_BC5_SNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_BPTC_UNORM: return {VK_FORMAT_BC7_UNORM_BLOCK, false, 1, 1};
    case NVN_FORMAT_BPTC_UNORM_SRGB: return {VK_FORMAT_BC7_SRGB_BLOCK, false, 1, 1};
    case NVN_FORMAT_BPTC_SFLOAT: return {VK_FORMAT_BC6H_SFLOAT_BLOCK, false, 1, 1};
    case NVN_FORMAT_BPTC_UFLOAT: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, false, 1, 1};
    case NVN_FORMAT_BGRX8: return {VK_FORMAT_B8G8R8A8_UNORM, false, 1, 1};
    case NVN_FORMAT_BGRX8_SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, false, 1, 1};
    default: return {VK_FORMAT_UNDEFINED, false, 1, 1};
    }
}

bool is_depth_format(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_S8_UINT;
}
bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT;
}

// Bytes per block and block size of an NVN format: the texture code's table, plus ASTC's fixed 16-byte blocks
int nvn_block_size(int format, int* bw, int* bh) {
    int w = 1, h = 1;
    int bpb = host->fmt_bpb(format, &w, &h);
    const VkFormatInfo fi = vk_format(format);
    if (format >= 0x79 && format <= 0x94) {   // (the source blocks, whether or not the image holds them)
        const int i = (format - 0x79) % 14;
        bpb = 16, w = kAstcBw[i], h = kAstcBh[i];
    }
    if (bw) *bw = w;
    if (bh) *bh = h;
    return bpb;
}

} // namespace mkwvk
