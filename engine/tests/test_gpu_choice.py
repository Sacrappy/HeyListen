"""Which GPU the renderer runs on (gpu/vk/gpu_choice.h and its use in vk_main.cpp's pick_device): the Insert menu's GPU
device row, and a software renderer (llvmpipe: Vulkan device type CPU) never taken beside a GPU, nor alone without
RX_VK_ALLOW_CPU=1, where the game stops with a message instead. Stand-in device lists, no game or GPU required.

python -I -B tests/test_gpu_choice.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def preset_exempt(menu, value):
    """the menu's row for opts.<value> leaves the quality preset alone (Row::preset_exempt, which set_row checks)"""
    start = menu.index("&opts.%s," % value, menu.index("Row rows[] = {"))
    row = menu[start:menu.index("},", start)]
    return row.rstrip().endswith(", true") and "if (!r.preset_exempt)" in menu


def block(text, head):
    """text from head to its closing brace"""
    at = text.index(head)
    depth, i = 0, text.index("{", at)
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[at:j + 1]
    raise SystemExit("unbalanced: " + head)


def check_software_renderers(vk, main_src, pick):
    """A software renderer (llvmpipe, lavapipe, SwiftShader) is never picked beside a GPU, and alone it stops the game with a
    message and exit code 3 (the code of no usable Vulkan: hos_main.c's --prepare-shaders): the sites that do it"""
    for need, why in (('getenv("RX_VK_ALLOW_CPU")', "the developers' override is read"),
                      ("gpu_choice::choose(", "the device comes from the tested choice"),
                      ("gpu_choice::usable(", "the menu's row lists only what may be taken"),
                      ("software_only_name = ", "the software-only case is remembered for mkwvk_init"),
                      ("only a software renderer was found", "the log says it")):
        if need not in pick:
            raise SystemExit("pick_device: %s is missing (%s)" % (need, why))
    for old in ("VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ?", "best_score"):
        if old in pick:
            raise SystemExit("pick_device ranks devices on its own again (%s): gpu_choice::choose does, tested" % old)
    init = block(main_src, "const MkwVkRenderer* mkwvk_init(const MkwVkHost* h)")
    if "!pick_device()" not in init or "h->no_usable_gpu(" not in init or "software_only_text(" not in init:
        raise SystemExit("mkwvk_init does not give the host the software-only message when pick_device fails on it")
    if "!prepare_only()" not in init[init.index("!pick_device()"):init.index("h->no_usable_gpu(")]:
        raise SystemExit("mkwvk_init shows a window in a --prepare-shaders run (the builder's: it exits 3 as before)")
    if "return nullptr" not in init[init.index("h->no_usable_gpu("):init.index("if (!create_device())")]:
        raise SystemExit("mkwvk_init goes on after pick_device failed")
    api = (vk / "vk_api.h").read_text(encoding="utf-8")
    version = re.search(r"#define MKWVK_API_VERSION (\d+)", api)
    if not version or int(version.group(1)) < 16 or "void (*no_usable_gpu)(const char *text);" not in api:
        raise SystemExit("vk_api.h: no_usable_gpu is not in the host table at version 16 (the interface is versioned)")
    rx = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
    stop = block(rx, "static void host_no_usable_gpu(const char *text)")
    for need, why in (("MessageBoxA(", "the window (SDL's box on Linux: host/posix MessageBoxA)"),
                      ('getenv("HOS_HEADLESS")', "no window without a display"),
                      ("hos_crash_clean_end();", "a clean end, not a crash window"),
                      ("ExitProcess(3);", "exit code 3, as no usable Vulkan")):
        if need not in stop:
            raise SystemExit("host_no_usable_gpu: %s is missing (%s)" % (need, why))
    if not re.search(r"host_out_of_memory,\s*host_no_usable_gpu,\s*(?:\w+,\s*)*\};", rx):
        raise SystemExit("vk_host does not fill no_usable_gpu after out_of_memory")
    hos = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    if "no usable Vulkan here" not in hos or "return 3;" not in hos[hos.index("no usable Vulkan here"):][:200]:
        raise SystemExit("hos_main.c: --prepare-shaders without Vulkan no longer exits 3 (the builder reads it)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    main_src = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    pick = main_src[main_src.index("bool pick_device() {"):]
    pick = pick[:pick.index("\n}\n")]
    for need, why in (("menu_init()", "the saved choice is read before it is used"),
                      ("menu_set_gpu_count((int)valid_gpus.size())", "the row offers only the usable GPUs"),
                      ("p.apiVersion >= VK_API_VERSION_1_3", "only Vulkan 1.3 GPUs are listed"),
                      ('getenv("RX_VK_DEVICE")', "RX_VK_DEVICE wins over the row"),
                      ("gpu_choice::menu_device(opts.gpu_device, (int)valid_gpus.size())", "the row's choice is taken")):
        if need not in pick:
            raise SystemExit("pick_device: %s is missing (%s)" % (need, why))
    if pick.index("menu_init()") > pick.index("gpu_choice::menu_device"):
        raise SystemExit("pick_device reads the GPU row before menu_init")
    if pick.index('getenv("RX_VK_DEVICE")') > pick.index("gpu_choice::menu_device"):
        raise SystemExit("pick_device takes the GPU row before RX_VK_DEVICE")
    check_software_renderers(vk, main_src, pick)
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    if not preset_exempt(menu, "gpu_device"):
        raise SystemExit("vk_menu.cpp: changing the GPU row resets the quality preset (and its VRAM budget) to Custom")
    with tempfile.TemporaryDirectory(prefix="gpu-choice-") as tmp:
        exe = Path(tmp) / "gpu-choice"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_gpu_choice.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
