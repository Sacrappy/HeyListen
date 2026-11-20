#include "../gpu/vk/gpu_choice.h"

#include <cstdio>
#include <cstring>
#include <string>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

using namespace mkwvk::gpu_choice;

// stand-in device lists: {Vulkan 1.3, type, driverVersion}
static const Candidate LLVMPIPE = {true, TYPE_CPU, 1};
static const Candidate RADV_IGPU = {true, TYPE_INTEGRATED, 100};
static const Candidate RTX = {true, TYPE_DISCRETE, 50};

int main() {
    CHECK("Auto stays Auto", menu_device(0, 2) == -1);
    CHECK("the n-th usable GPU", menu_device(1, 2) == 0 && menu_device(2, 2) == 1);
    CHECK("a GPU taken out since is Auto", menu_device(2, 1) == -1);
    CHECK("a negative choice (a hand-edited ini) is Auto", menu_device(-1, 2) == -1);
    CHECK("no usable GPU: Auto", menu_device(1, 0) == -1);

    CHECK("RX_VK_ALLOW_CPU unset is off", !allow_cpu(nullptr));
    CHECK("RX_VK_ALLOW_CPU empty, 0, false, off, no are off", !allow_cpu("") && !allow_cpu("0") && !allow_cpu("false") &&
                                                                  !allow_cpu("OFF") && !allow_cpu("No"));
    CHECK("RX_VK_ALLOW_CPU=1 is on", allow_cpu("1") && allow_cpu("true") && allow_cpu("yes"));

    {   // the v0.4.2 PC: llvmpipe is the only device
        const Candidate d[] = {LLVMPIPE};
        const Pick p = choose(d, 1, -1, false);
        CHECK("CPU only: nothing is picked", p.device == -1);
        CHECK("CPU only: said as a software renderer alone", p.software_only && !p.target_unavailable);
        const Pick asked = choose(d, 1, 0, false);
        CHECK("CPU only, asked for by RX_VK_DEVICE: still nothing", asked.device == -1 && asked.software_only && asked.target_unavailable);
        const Pick dev = choose(d, 1, -1, true);
        CHECK("CPU only, RX_VK_ALLOW_CPU=1: it runs", dev.device == 0 && !dev.software_only);
        CHECK("the Insert menu does not list it", !usable(d[0], false) && usable(d[0], true));
    }
    {   // two software renderers and no GPU
        const Candidate d[] = {LLVMPIPE, {true, TYPE_CPU, 7}};
        const Pick p = choose(d, 2, -1, false);
        CHECK("two CPU devices: nothing, a software renderer alone", p.device == -1 && p.software_only);
        CHECK("two CPU devices, allowed: the newer driver", choose(d, 2, -1, true).device == 1);
    }
    {   // llvmpipe listed before a discrete GPU, and after
        const Candidate a[] = {LLVMPIPE, RTX};
        const Candidate b[] = {RTX, LLVMPIPE};
        CHECK("CPU + discrete: the discrete GPU", choose(a, 2, -1, false).device == 1 && choose(b, 2, -1, false).device == 0);
        CHECK("CPU + discrete, allowed: still the discrete GPU", choose(a, 2, -1, true).device == 1 && choose(b, 2, -1, true).device == 0);
        const Pick asked = choose(a, 2, 0, false);
        CHECK("CPU + discrete, the CPU asked for: never taken, Auto takes the GPU",
              asked.device == 1 && asked.target_unavailable && !asked.software_only);
        CHECK("CPU + discrete, the CPU asked for and allowed: taken", choose(a, 2, 0, true).device == 0);
        CHECK("CPU + discrete, the GPU asked for", choose(a, 2, 1, false).device == 1 && !choose(a, 2, 1, false).target_unavailable);
    }
    {   // an integrated GPU only, and with llvmpipe beside it (the 5600H with its RADV)
        const Candidate a[] = {RADV_IGPU};
        const Candidate b[] = {LLVMPIPE, RADV_IGPU};
        CHECK("integrated only: it", choose(a, 1, -1, false).device == 0 && !choose(a, 1, -1, false).software_only);
        CHECK("integrated + CPU: the integrated GPU", choose(b, 2, -1, false).device == 1);
    }
    {   // the ranks and the AMD laptop's two listings of one GPU are as before
        const Candidate d[] = {RADV_IGPU, RTX, {true, TYPE_DISCRETE, 90}, {true, TYPE_VIRTUAL, 999}};
        CHECK("discrete over integrated, the newer driver among discrete", choose(d, 4, -1, false).device == 2);
        const Candidate e[] = {{true, TYPE_VIRTUAL, 999}, RADV_IGPU};
        CHECK("integrated over a virtual GPU", choose(e, 2, -1, false).device == 1);
        const Candidate f[] = {{true, TYPE_VIRTUAL, 1}, LLVMPIPE};
        CHECK("a virtual GPU over a software renderer (allowed)", choose(f, 2, -1, true).device == 0);
        const Candidate g[] = {{true, TYPE_VIRTUAL, 1}, {true, TYPE_CPU, 999}};
        CHECK("a virtual GPU over a software renderer with the newer driver (allowed)", choose(g, 2, -1, true).device == 0);
    }
    {   // a GPU without Vulkan 1.3 is not a software-only machine, and is not taken
        const Candidate d[] = {{false, TYPE_DISCRETE, 1}, LLVMPIPE};
        const Pick p = choose(d, 2, -1, false);
        CHECK("an old GPU beside llvmpipe: nothing, but the driver is there", p.device == -1 && !p.software_only);
        const Candidate old_cpu[] = {{false, TYPE_CPU, 1}};
        CHECK("a software renderer with no Vulkan 1.3 alone is a software renderer alone", choose(old_cpu, 1, -1, false).software_only);
        CHECK("no devices at all: no message about a software renderer", !choose(nullptr, 0, -1, false).software_only);
        CHECK("an asked-for device past the list is Auto", choose(d, 2, 9, false).target_unavailable);
    }

    CHECK("llvmpipe's name", software_name("llvmpipe (LLVM 21.1.8, 256 bits)") == "llvmpipe");
    CHECK("lavapipe's name", software_name("Lavapipe (LLVM 15.0.7)") == "Lavapipe");
    CHECK("SwiftShader's name", software_name("SwiftShader Device (LLVM 10.0.0)") == "SwiftShader");
    CHECK("another name is kept", software_name("Some CPU") == "Some CPU");

    const std::string linux_text = software_only_text("llvmpipe (LLVM 21.1.8, 256 bits)", false);
    const std::string windows_text = software_only_text("llvmpipe (LLVM 21.1.8, 256 bits)", true);
    const char* head = "HeyListen needs your graphics card's Vulkan driver. Only a software renderer (llvmpipe) was found, "
                       "which can't run the game.";
    CHECK("the message names the problem", linux_text.rfind(head, 0) == 0 && windows_text.rfind(head, 0) == 0);
    CHECK("Linux's hint: Mesa's Vulkan driver, the packages, NVIDIA's",
          linux_text.find("mesa-vulkan-drivers") != std::string::npos && linux_text.find("vulkan-radeon") != std::string::npos &&
              linux_text.find("vulkan-intel") != std::string::npos && linux_text.find("NVIDIA") != std::string::npos &&
              linux_text.find("AMD, NVIDIA or Intel") == std::string::npos);
    CHECK("Windows' hint: the driver from AMD, NVIDIA or Intel",
          windows_text.find("AMD, NVIDIA or Intel") != std::string::npos && windows_text.find("Mesa") == std::string::npos);
    std::puts("gpu choice: all checks passed");
}
