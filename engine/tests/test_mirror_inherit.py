"""Compile and run the retired-mirror inheritance rule (vk_memory.cpp mirror_make, TICK-036; no game/GPU required)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """mirror_make still takes only the pages the CPU did not write since the retired mirror's sync"""
    src = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for needle in ('#include "mirror_inherit.h"', "mirror_inherit::runs(", 'getenv("RX_VK_INHERIT_ALL")',
                   "page_gen(page) > r.synced_gen", "region_gen[region] <= r.synced_gen",
                   "if (r.made_epoch == mirror_epoch && r.made_seg > texture_segment()) {", "p->made_epoch = mirror_epoch, p->made_seg = texture_segment();",
                   "vkCmdCopyBuffer(upload_command_buffer(), r.mirror, p->mirror, (uint32_t)parts.size(), parts.data());"):
        if needle not in src:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="mirror-inherit-") as tmp:
        exe = Path(tmp) / "mirror-inherit"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_mirror_inherit.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
