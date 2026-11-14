"""Compile and run the PCIe link read from sysfs (gpu/vk/pcie_link.h, TICK-048 on Linux; no game/GPU required)."""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """vk_memory.cpp still asks sysfs when NVML gives nothing, and the device still records VK_EXT_pci_bus_info"""
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    for src, needle in ((mem, '#include "pcie_link.h"'), (mem, "pcie_link::sysfs_link(\"/sys\""),
                        (mem, "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT"),
                        (main, "VK_EXT_PCI_BUS_INFO_EXTENSION_NAME")):
        if needle not in src:
            raise SystemExit("the source no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    if sys.platform == "win32":
        print("pcie_link: sysfs is Linux only (skipped)")
        return
    with tempfile.TemporaryDirectory(prefix="pcie-link-") as tmp:
        exe = Path(tmp) / "pcie-link"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_pcie_link.cpp"), "-o", str(exe)], check=True)
        tree = Path(tmp) / "sys"
        tree.mkdir()
        subprocess.run([str(exe), str(tree)], check=True)


if __name__ == "__main__":
    main()
