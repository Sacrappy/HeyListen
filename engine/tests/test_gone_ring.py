"""Compile and run the destroy generation's ring (gpu/vk/gone_ring.h, TICK-052; no game/GPU required)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The textures' destroy generation and the recording threads' caches still use the ring, and the switches are still read"""
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for src, needle in ((tex, '#include "gone_ring.h"'), (tex, "destroy_gen_bump(&tex);"), (tex, "destroy_gen_bump(nullptr);"),
                        (tex, 'getenv("RX_VK_GONE_RING")'), (draw, "texture_kept_since(it->second.tex, it->second.destroy_gen, dgen)"),
                        (draw, "texture_kept_since(sl->tex, sl->destroy_gen, dgen_now)"), (mem, 'getenv("RX_VK_TL_POOLS_WIDE")'),
                        (mem, "TlPool& tl = tl_pool_entry(guest);")):
        if needle not in src:
            raise SystemExit("the source no longer has: " + needle)
    if "destroy_gen++" in tex:
        raise SystemExit("vk_texture.cpp bumps the destroy generation without the ring")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="gone-ring-") as tmp:
        exe = Path(tmp) / "gone-ring"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                        str(ROOT / "tests/test_gone_ring.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
