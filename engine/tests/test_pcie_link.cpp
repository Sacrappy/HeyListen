#include "../gpu/vk/pcie_link.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace fs = std::filesystem;

static void node(const fs::path& p, const char* speed, const char* width, const char* wmax) {
    fs::create_directories(p);
    if (speed) std::ofstream(p / "max_link_speed") << speed << "\n";
    if (width) std::ofstream(p / "current_link_width") << width << "\n";
    if (wmax) std::ofstream(p / "max_link_width") << wmax << "\n";
}
static void link_dev(const fs::path& sys, const fs::path& target) {
    fs::create_directories(sys / "bus/pci/devices");
    fs::create_symlink(target, sys / "bus/pci/devices" / target.filename());
}
static bool near(double a, double b) { return std::fabs(a - b) < 0.01; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const fs::path root = argv[1];
    using mkwvk::pcie_link::sysfs_link;
    using mkwvk::pcie_link::lane_gbps;

    CHECK("Gen4 lane", near(lane_gbps(16.0), 1.969));
    CHECK("Gen1 lane", near(lane_gbps(2.5), 0.25));
    CHECK("unknown speed", lane_gbps(0) == 0 && lane_gbps(12) == 0);

    {   // an RX 9070 XT: card and its internal switch say Gen5 x16, the root port Gen4 x16 (a real PC's tree)
        const fs::path sys = root / "amd";
        const fs::path rp = sys / "devices/pci0000:00/0000:00:03.1";
        const fs::path up = rp / "0000:09:00.0", down = up / "0000:0a:00.0", gpu = down / "0000:0b:00.0";
        node(rp, "16.0 GT/s PCIe", "16", "16");
        node(up, "32.0 GT/s PCIe", "16", "16");
        node(down, "32.0 GT/s PCIe", "16", "16");
        node(gpu, "32.0 GT/s PCIe", "16", "16");
        link_dev(sys, gpu);
        const auto l = sysfs_link(sys.string(), 0, 0x0b, 0, 0);
        CHECK("amd: the root port's link", near(l.gbps, 1.969 * 16) && l.gen == 4 && l.width == 16);
        CHECK("amd: limited by the root port", l.limited_by == "0000:00:03.1" && l.hops == 4);
    }
    {   // an x16 card in an x4 slot, Gen3: the slow link, chunk diff on
        const fs::path sys = root / "x4";
        const fs::path rp = sys / "devices/pci0000:00/0000:00:01.2", gpu = rp / "0000:03:00.0";
        node(rp, "8.0 GT/s PCIe", "4", "4");
        node(gpu, "16.0 GT/s PCIe", "4", "16");
        link_dev(sys, gpu);
        const auto l = sysfs_link(sys.string(), 0, 3, 0, 0);
        CHECK("x4: Gen3 x4", near(l.gbps, 0.985 * 4) && l.gen == 3 && l.width == 4 && l.gbps <= 8.0);
    }
    {   // a width wider than the maximum is capped; "Unknown" speed nodes and a missing width are skipped
        const fs::path sys = root / "odd";
        const fs::path rp = sys / "devices/pci0000:00/0000:00:01.0", gpu = rp / "0000:01:00.0";
        node(rp, "Unknown", "0", nullptr);
        node(gpu, "16.0 GT/s PCIe", "16", "8");
        link_dev(sys, gpu);
        const auto l = sysfs_link(sys.string(), 0, 1, 0, 0);
        CHECK("odd: width capped at the maximum", near(l.gbps, 1.969 * 8) && l.width == 8 && l.hops == 1);
    }
    {   // an integrated GPU: no PCIe link files at all
        const fs::path sys = root / "igpu";
        const fs::path gpu = sys / "devices/pci0000:00/0000:00:02.0";
        node(gpu, nullptr, nullptr, nullptr);
        link_dev(sys, gpu);
        CHECK("igpu: none", sysfs_link(sys.string(), 0, 0, 2, 0).gbps < 0);
    }
    CHECK("no such device", sysfs_link((root / "amd").string(), 0, 0x42, 0, 0).gbps < 0);
    std::puts("pcie_link: ok");
    return 0;
}
