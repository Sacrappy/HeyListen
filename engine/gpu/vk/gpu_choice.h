/* Which GPU the renderer runs on (vk_main.cpp's pick_device): the Insert menu's GPU device row, RX_VK_DEVICE (which wins over
   the row there), and the software renderers (llvmpipe, lavapipe, SwiftShader: Vulkan device type CPU) that are never chosen
   while a real GPU is there. RX_VK_ALLOW_CPU=1 lets one be chosen (a developer's run). Plain C++, no Vulkan header: the
   tests compile it alone (tests/test_gpu_choice.cpp). */
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace mkwvk::gpu_choice {

/// The row's choice as a position in the Vulkan 1.3 devices (0-based), or -1 for Auto. menu: opts.gpu_device (0 Auto,
/// n the n-th usable device); usable: how many there are now. A choice past them (a GPU taken out since) is Auto.
inline int menu_device(int menu, int usable) { return menu > 0 && menu <= usable ? menu - 1 : -1; }

/// VkPhysicalDeviceType's values (vk_main.cpp static_asserts them against the header)
enum { TYPE_OTHER = 0, TYPE_INTEGRATED = 1, TYPE_DISCRETE = 2, TYPE_VIRTUAL = 3, TYPE_CPU = 4 };

/// A device as the choice sees it: whether it has Vulkan 1.3, its VkPhysicalDeviceType, and its driverVersion
struct Candidate {
    bool vulkan13;
    int type;
    uint32_t driver;
};

/// A software renderer: Mesa's llvmpipe and lavapipe, SwiftShader. It can start the game and then crash in it
inline bool is_software(int type) { return type == TYPE_CPU; }

/// RX_VK_ALLOW_CPU: set to anything but empty, 0, false, off or no
inline bool allow_cpu(const char* value) {
    if (!value) return false;
    std::string s(value);
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return !(s.empty() || s == "0" || s == "false" || s == "off" || s == "no");
}

/// Whether the choice may take it (and the Insert menu's row lists it): Vulkan 1.3, and not a software renderer unless allowed
inline bool usable(const Candidate& c, bool allowed_cpu) { return c.vulkan13 && (allowed_cpu || !is_software(c.type)); }

/// Auto's order: a discrete GPU, an integrated one, any other, a software renderer last (only reached with RX_VK_ALLOW_CPU)
inline int rank(int type) {
    return type == TYPE_DISCRETE ? 3 : type == TYPE_INTEGRATED ? 2 : is_software(type) ? 0 : 1;
}

struct Pick {
    int device = -1;                  // the device chosen (its position in the list given), -1 none
    bool target_unavailable = false;  // a device was asked for (target) and is not usable: Auto took over
    bool software_only = false;       // nothing usable, every device there a software renderer, none allowed: the GPU's driver is missing
};

/// n devices; target: the position asked for (RX_VK_DEVICE, or the row's choice) or -1 for Auto. A device asked for that is
/// usable is taken. Otherwise Auto: the best rank, the newer driver among equals (AMD laptops: the same GPU listed by two
/// drivers). Never a software renderer unless allowed_cpu, whatever was asked for and whatever else is there.
inline Pick choose(const Candidate* c, int n, int target, bool allowed_cpu) {
    Pick p;
    if (target >= 0) {
        if (target < n && usable(c[target], allowed_cpu)) {
            p.device = target;
            return p;
        }
        p.target_unavailable = true;
    }
    for (int i = 0; i < n; i++) {
        if (!usable(c[i], allowed_cpu)) continue;
        if (p.device < 0 || rank(c[i].type) > rank(c[p.device].type) ||
            (rank(c[i].type) == rank(c[p.device].type) && c[i].driver > c[p.device].driver))
            p.device = i;
    }
    if (p.device < 0 && n > 0 && !allowed_cpu) {
        p.software_only = true;
        for (int i = 0; i < n; i++) p.software_only = p.software_only && is_software(c[i].type);
    }
    return p;
}

/// The software renderer's short name for the message: "llvmpipe (LLVM 21.1.8, 256 bits)" is llvmpipe
inline std::string software_name(const char* device_name) {
    std::string s(device_name ? device_name : "");
    std::string low = s;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    for (const char* known : {"llvmpipe", "lavapipe", "swiftshader"}) {
        const size_t at = low.find(known);
        if (at != std::string::npos) return s.substr(at, std::strlen(known));
    }
    return s.empty() ? "software" : s;
}

/// The start-up message when only a software renderer is there (the host shows it in a window and ends the game)
inline std::string software_only_text(const char* device_name, bool windows) {
    std::string t = "HeyListen needs your graphics card's Vulkan driver. Only a software renderer (" + software_name(device_name) +
                    ") was found, which can't run the game.\n\n";
    t += windows ? "Install or update your graphics driver from AMD, NVIDIA or Intel, then start HeyListen again."
                 : "Install your distribution's Mesa Vulkan driver (for example mesa-vulkan-drivers, vulkan-radeon or "
                   "vulkan-intel), or NVIDIA's driver, then start HeyListen again. If you run it in a container or a "
                   "sandbox, make sure the GPU is visible there.";
    return t;
}

} // namespace mkwvk::gpu_choice
