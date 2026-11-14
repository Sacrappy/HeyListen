// (HeyListen TICK-048, Linux) The GPU's PCIe link speed from sysfs, for the mirror syncs' chunk diff default (vk_memory.cpp
// diff_mode). NVML gives it on Windows with an NVIDIA card; on Linux nvml.dll never loads, and AMD and Intel cards have no NVML,
// so the slow-link default was never taken there. The Vulkan device's PCI address (VK_EXT_pci_bus_info) names its sysfs node;
// the walk goes up through every bridge to the root port and keeps the narrowest link. AMD cards sit behind an internal switch
// whose virtual links report the card's own maximum (Gen5 x16 on an RX 9070 XT in a Gen4 slot), so the card's own node alone
// is not enough: the root port's link is the real one.
//
// As with NVML: the maximum speed is read (the link drops to Gen1 at idle), and the current width (the slot's, not the card's).
// Pure, no Vulkan: tests/test_pcie_link.cpp builds a fake sysfs tree.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace mkwvk::pcie_link {

// GB/s a lane a direction at a link speed in GT/s (2.5 .. 64: Gen1 .. Gen6); 0 when unknown
inline double lane_gbps(double gts) {
    static const struct { double gts, gbps; } t[] = {{2.5, 0.25}, {5, 0.5}, {8, 0.985}, {16, 1.969}, {32, 3.938}, {64, 7.563}};
    for (const auto& e : t)
        if (gts > e.gts - 0.1 && gts < e.gts + 0.1) return e.gbps;
    return 0;
}

inline int gen_of(double gts) {
    static const double t[] = {2.5, 5, 8, 16, 32, 64};
    for (int i = 0; i < 6; i++)
        if (gts > t[i] - 0.1 && gts < t[i] + 0.1) return i + 1;
    return 0;
}

struct Link {
    double gbps = -1;          // (the narrowest link on the way to the root; -1: none read)
    int gen = 0;               // (that link's)
    unsigned width = 0;
    std::string limited_by;    // (the PCI address whose link it is)
    int hops = 0;              // (the nodes read)
};

namespace detail {
inline std::string read_line(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::string s;
    if (f) std::getline(f, s);
    return s;
}
// "0000:0b:00.0": a PCI function's node (not "pci0000:00", the host bridge's, where the walk ends)
inline bool is_pci_address(const std::string& n) {
    unsigned d, b, s, f;
    char tail;
    return n.size() == 12 && std::sscanf(n.c_str(), "%4x:%2x:%2x.%1x%c", &d, &b, &s, &f, &tail) == 4;
}
}  // namespace detail

// sysroot: "/sys" (a test passes its fake tree)
inline Link sysfs_link(const std::string& sysroot, uint32_t domain, uint32_t bus, uint32_t device, uint32_t function) {
    namespace fs = std::filesystem;
    Link r;
    char name[32];
    std::snprintf(name, sizeof name, "%04x:%02x:%02x.%x", domain & 0xffff, bus & 0xff, device & 0x1f, function & 7);
    std::error_code ec;
    fs::path p = fs::canonical(fs::path(sysroot) / "bus/pci/devices" / name, ec);
    if (ec) return r;
    for (int guard = 0; guard < 16 && detail::is_pci_address(p.filename().string()); guard++, p = p.parent_path()) {
        const double gts = std::strtod(detail::read_line(p / "max_link_speed").c_str(), nullptr);   // ("16.0 GT/s PCIe")
        unsigned w = (unsigned)std::strtoul(detail::read_line(p / "current_link_width").c_str(), nullptr, 10);
        const unsigned wmax = (unsigned)std::strtoul(detail::read_line(p / "max_link_width").c_str(), nullptr, 10);
        if (!w || (wmax && wmax < w)) w = wmax;
        const double lane = lane_gbps(gts);
        if (!lane || !w || w > 32) continue;   // ("Unknown", a node without a PCIe capability: skipped)
        r.hops++;
        const double g = lane * w;
        if (r.gbps < 0 || g < r.gbps) {
            r.gbps = g;
            r.gen = gen_of(gts);
            r.width = w;
            r.limited_by = p.filename().string();
        }
    }
    return r;
}

}  // namespace mkwvk::pcie_link
