# SPDX-License-Identifier: GPL-3.0-or-later
"""How task and mesh programs run: the default by driver and OS, and the switch's precedence (gpu/vk/mesh_mode.h, vk_main.cpp).

Issue 51 (an RX 6800 on Linux with RADV loses the GPU in native task/mesh draws, upstream Hookshot issue 2245): on Linux with
an RDNA 2 AMD GPU (vendor 0x1002, RADV; Navi 21 to 24 and the RDNA 2 APUs by device ID) Automatic is the compute path; other
RDNA 3 and 4 keep the native path. Issue 81 (white foliage and glows with RADV's native path on RDNA 3.5): the Strix Point,
Krackan Point and Strix Halo APUs take the compute path too. RX_VK_MESH_SHADERS=native keeps the GPU's own mesh shaders
there, compute forces the compute path on any GPU, and RX_VK_FORCE_MESH_FALLBACK=1 is Compute over both (as before). The rule is
compiled and run alone; the source checks read vk_main.cpp. No game or GPU is needed.

    python -I -B tests/test_mesh_mode_switch.py
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

RULE = r'''#include "mesh_mode.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
using namespace mesh_rule;
static Choice run(const char* env, const char* force, bool linux_os, uint32_t vendor, uint32_t device = 0x73bf) {
    return effective(choose(env, force), linux_os, vendor, device);
}
int main() {
    const uint32_t amd = 0x1002, nvidia = 0x10DE, intel = 0x8086;
    // (the default by driver and OS: only RDNA 2 AMD on Linux, RADV, is Compute; Windows AMD, RDNA 3 and 4 on Linux, and every other
    // vendor, are Automatic. 0x73bf Navi 21, 0x73df Navi 22, 0x73ff Navi 23, 0x743f Navi 24, 0x163f Van Gogh, 0x1681 Rembrandt)
    for (uint32_t rdna2 : {0x73bfu, 0x73afu, 0x73dfu, 0x73ffu, 0x73efu, 0x743fu, 0x7422u, 0x163fu, 0x1681u, 0x164cu}) {
        CHECK(run(nullptr, nullptr, true, amd, rdna2).mode == 2);
        CHECK(strstr(run(nullptr, nullptr, true, amd, rdna2).why, "issue 2245"));
        CHECK(run(nullptr, nullptr, false, amd, rdna2).mode == 0);          // Windows: Automatic (the task + mesh rule is separate)
        CHECK(run(nullptr, nullptr, true, nvidia, rdna2).mode == 0);
        CHECK(run(nullptr, nullptr, true, intel, rdna2).mode == 0);
    }
    // (RDNA 3.5 APUs on Linux, RADV: their native mesh shaders draw foliage and glows white, issue 81. 0x150e Strix Point
    // (Radeon 880M, 890M), 0x1114 Krackan Point (840M, 860M), 0x1586 Strix Halo (8040S, 8050S, 8060S); Windows and other
    // vendors stay Automatic, and native overrides)
    for (uint32_t rdna35 : {0x150eu, 0x1114u, 0x1586u}) {
        CHECK(run(nullptr, nullptr, true, amd, rdna35).mode == 2);
        CHECK(strstr(run(nullptr, nullptr, true, amd, rdna35).why, "issue 81") != nullptr);
        CHECK(strstr(run(nullptr, nullptr, true, amd, rdna35).why, "issue 2245") == nullptr);
        CHECK(run(nullptr, nullptr, false, amd, rdna35).mode == 0);        // Windows: Automatic (the task + mesh rule is separate)
        CHECK(run(nullptr, nullptr, true, nvidia, rdna35).mode == 0);
        CHECK(run("native", nullptr, true, amd, rdna35).mode == 1);
        CHECK(run("compute", nullptr, true, amd, rdna35).mode == 2);
    }
    // (RDNA 3 and 4 on RADV run the native path: 0x744c Navi 31 (RX 7900 XT), 0x7480 Navi 33, 0x7550 and 0x7551 Navi 48 (RX 9070 XT),
    // 0x15bf Phoenix; RDNA 1: 0x731f, 0x7340)
    for (uint32_t other : {0x744cu, 0x7480u, 0x7550u, 0x7551u, 0x15bfu, 0x731fu, 0x7340u, 0u}) {
        CHECK(run(nullptr, nullptr, true, amd, other).mode == 0 && !strcmp(run(nullptr, nullptr, true, amd, other).why, "the default"));
    }
    CHECK(run(nullptr, nullptr, false, nvidia).mode == 0 && !strcmp(run(nullptr, nullptr, false, nvidia).why, "the default"));
    // (an empty value, and any word that is neither native nor compute, are Automatic: Compute on Linux RDNA 2)
    for (const char* other : {"", "1", "2", "0", "automatic", "nativ", "compute2", " native", "fallback"}) {
        CHECK(run(other, nullptr, true, amd).mode == 2);
        CHECK(run(other, nullptr, true, nvidia).mode == 0);
        CHECK(run(other, nullptr, false, amd).mode == 0);
        CHECK(run(other, nullptr, true, amd, 0x7550).mode == 0);
    }
    // (the switch: native keeps the GPU's own mesh shaders on Linux RDNA 2 too; compute is Compute on any GPU, RDNA 4 included)
    CHECK(run("native", nullptr, true, amd).mode == 1 && !strcmp(run("native", nullptr, true, amd).why, "RX_VK_MESH_SHADERS"));
    CHECK(run("Native", nullptr, true, amd).mode == 1 && run("NATIVE", nullptr, true, nvidia).mode == 1);
    CHECK(run("native", nullptr, false, amd).mode == 1);
    CHECK(run("compute", nullptr, true, nvidia).mode == 2 && run("COMPUTE", nullptr, false, amd).mode == 2);
    CHECK(run("compute", nullptr, true, amd, 0x7550).mode == 2);
    CHECK(!strcmp(run("compute", nullptr, true, amd).why, "RX_VK_MESH_SHADERS"));
    // (RX_VK_FORCE_MESH_FALLBACK, set and not 0, is Compute over the switch; 0 leaves the choice to the rest)
    CHECK(run("native", "1", true, amd).mode == 2 && !strcmp(run("native", "1", true, amd).why, "RX_VK_FORCE_MESH_FALLBACK"));
    CHECK(run("native", "1", false, nvidia).mode == 2 && run(nullptr, "1", false, nvidia).mode == 2);
    CHECK(run("native", "7", true, amd).mode == 2);
    CHECK(run("native", "0", true, amd).mode == 1 && run(nullptr, "0", true, amd).mode == 2 && run(nullptr, "0", true, nvidia).mode == 0);
    puts("mesh mode: Linux AMD RDNA 2 and RDNA 3.5 are Compute by default, RX_VK_MESH_SHADERS chooses, RX_VK_FORCE_MESH_FALLBACK is over it");
    return 0;
}
'''


def block(text, marker):
    start = text.index(marker)
    depth = 0
    for at in range(text.index("{", start), len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated: " + marker)


def check_source():
    vk = ROOT / "gpu" / "vk"
    main_cpp = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    device = block(main_cpp, "bool create_device() {")
    if 'mesh_rule::choose(getenv("RX_VK_MESH_SHADERS"), getenv("RX_VK_FORCE_MESH_FALLBACK"))' not in device or \
            "mesh_rule::effective(" not in device or "dev.props.vendorID" not in device or "dev.props.deviceID" not in device:
        raise SystemExit("FAIL: create_device does not take the mode from mesh_mode.h's rule, with the device's vendor")
    if "if (mesh_choice.mode == 2) dev.mesh_shader = false;" not in device:
        raise SystemExit("FAIL: the Compute choice does not leave the GPU's mesh shaders unused")
    if 'getenv("RX_VK_FORCE_MESH_FALLBACK")) dev.mesh_shader' in device:
        raise SystemExit("FAIL: RX_VK_FORCE_MESH_FALLBACK is read outside the rule")
    if 'logf("vk: mesh shaders: %s, from %s: %s"' not in device or "mesh_choice.why" not in device:
        raise SystemExit("FAIL: the log line does not say which path was chosen and why")
    if "#ifdef _WIN32" not in device or "constexpr bool linux_os = false;" not in device:
        raise SystemExit("FAIL: the OS is not told to the rule (Windows AMD must keep the mesh shaders)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="mesh-mode-") as tmp:
        src = Path(tmp) / "mesh_mode_test.cpp"
        src.write_text(RULE, encoding="utf-8")
        exe = Path(tmp) / "mesh-mode"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(src),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
    print("PASS: mesh mode default by driver and OS, switch precedence")


if __name__ == "__main__":
    main()
