"""Compile and run the lazy write-back's pool rule (gpu/vk/wb_pool_eager.h; no game/GPU) and check that vk_memory.cpp applies it
where a mirror sync's dirty page reaches the lazy write-back (issue 50, cut-grass debris)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    src = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for needle in ('#include "wb_pool_eager.h"',
                   'getenv("RX_VK_WB_POOL_EAGER")',
                   'getenv("RX_VK_WB_SMALL_EAGER_KB")',
                   "wb_pool_eager::may_defer_size(np->hi - lo, wb_small_eager_bytes())",
                   "if (wb_pool_eager::cpu_write_makes_eager(np->eager, wb_pool_eager_on())) nvn_pools_eager(page << kPageShift, (page + 1) << kPageShift, true);",
                   "if (wb_pool_eager::cpu_write_makes_eager(np->eager, wb_pool_eager_on())) nvn_pools_eager(g, g + 4096, true);"):
        if needle not in src:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="wb-pool-eager-") as tmp:
        exe = Path(tmp) / "wb-pool-eager.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_wb_pool_eager.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
