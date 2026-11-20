"""Instance-rate vertex divisors over 1 are used only where the device has the feature, not only the extension.

create_device (gpu/vk/vk_main.cpp) set dev.vertex_divisor when VK_EXT_vertex_attribute_divisor was there and chained its
feature struct as the device reported it, but never read vertexAttributeInstanceRateDivisor back. On a device with the
extension and the feature off, the pipelines (gpu/vk/vk_draw.cpp) still chained divisors over 1, which breaks
VUID-VkVertexInputBindingDivisorDescription-vertexAttributeInstanceRateDivisor-02229 ("If the
vertexAttributeInstanceRateDivisor feature is not enabled, divisor must be 1"). The flag is now the feature, read after
vkGetPhysicalDeviceFeatures2 and before vkCreateDevice.

python -I -B tests/test_vertex_divisor_feature.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "gpu" / "vk" / "vk_main.cpp"
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"

CHECK = "dev.vertex_divisor = dev.vertex_divisor && vdiv.vertexAttributeInstanceRateDivisor;"


def main():
    text = MAIN.read_text(encoding="utf-8")
    at = text.find(CHECK)
    if at < 0:
        raise SystemExit("dev.vertex_divisor is set by the extension alone, not by vertexAttributeInstanceRateDivisor")
    query = text.find("vkGetPhysicalDeviceFeatures2(dev.physical, &f);")
    create = text.find("vkCreateDevice(dev.physical, &ci, nullptr, &dev.device)")
    if not 0 <= query < at < create:
        raise SystemExit("the vertexAttributeInstanceRateDivisor check is not between the feature query and vkCreateDevice")
    draw = DRAW.read_text(encoding="utf-8")
    if "if (dev.vertex_divisor) vdivs.push_back(" not in draw:
        raise SystemExit("the pipelines chain vertex divisors without dev.vertex_divisor")
    # VUID-VkVertexInputBindingDivisorDescriptionKHR-divisor-01870: divisor <= maxVertexAttribDivisor (only 2^16-1 required)
    if "std::min(s.vstream[b].divisor, dev.max_vertex_divisor)" not in draw:
        raise SystemExit("the game's divisor is chained without clamping it to maxVertexAttribDivisor (VUID 01870)")
    if "dev.max_vertex_divisor = vp.maxVertexAttribDivisor;" not in text:
        raise SystemExit("maxVertexAttribDivisor is not queried")
    print("vertex divisors over 1 are used only with vertexAttributeInstanceRateDivisor, up to maxVertexAttribDivisor: ok")


if __name__ == "__main__":
    main()
