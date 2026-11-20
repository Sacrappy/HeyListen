"""Linux Vulkan startup uses only surface-supported swapchain settings (the V-Sync mode's images, clamped to the surface's
minimum and maximum).

Run with: python -I -B tests/test_vk_swapchain_compat.py
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gpu" / "vk" / "vk_main.cpp"


def function(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_swapchain(source):
    body = function(source, "bool create_swapchain(")
    required = (
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
        "if (result != VK_SUCCESS)",
        "present_mode::image_plan(pin, picked, framegen_on(), caps.minImageCount, caps.maxImageCount)",
        "ci.minImageCount = (uint32_t)plan.count;",
        "ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT",
        "caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT",
        "ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT",
        "caps.supportedCompositeAlpha & ci.compositeAlpha",
        "VK_PRESENT_MODE_FIFO_KHR",
        "vkCreateSwapchainKHR failed",
    )
    for item in required:
        if item not in body:
            raise SystemExit("create_swapchain no longer handles %s" % item)
    # (image_plan takes the surface's own minimum and maximum: the row cannot ask past either)
    if "dev.swap_min_images = caps.minImageCount;" not in body or "dev.swap_max_images = caps.maxImageCount;" not in body:
        raise SystemExit("create_swapchain no longer keeps the surface's image limits for the V-Sync mode row")


def check_linux_start(source):
    body = function(source, "int start(void* hwnd)")
    linux = body[body.index("#else"):body.index("#endif")]
    for item in (
        "w->create_surface",
        "queue-family present support failed",
        "w->drawable_size",
        "SDL returned zero drawable size",
        "swapchain creation failed for drawable",
    ):
        if item not in linux:
            raise SystemExit("Linux Vulkan startup no longer checks %s" % item)


def main():
    source = SOURCE.read_text(encoding="utf-8")
    check_swapchain(source)
    check_linux_start(source)
    print("PASS: Vulkan swapchain respects surface limits and Linux startup reports failures")


if __name__ == "__main__":
    main()
