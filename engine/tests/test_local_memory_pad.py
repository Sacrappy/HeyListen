"""The local memory array of a program is declared larger when RX_VK_LOCAL_PAD says so (default off).

The size rule (gpu/vk/eden/shader_recompiler/backend/spirv/local_memory_pad.h) is compiled and run on its own against a table: off by
default, padded only for arrays of RX_VK_LOCAL_PAD_FROM words or more and shorter than the target, never made smaller, and the
switches' text. The source is checked to use it where the array is declared, and the names to sit under a prefix the program
cache's stamp covers.

python -I -B tests/test_local_memory_pad.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPIRV = ROOT / "gpu/vk/eden/shader_recompiler/backend/spirv"
RECOMPILER = ROOT / "gpu/vk/eden"


def check_source():
    ctx = (SPIRV / "spirv_emit_context.cpp").read_text(encoding="utf-8")
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    for needle in ('getenv("RX_VK_LOCAL_PAD")', 'getenv("RX_VK_LOCAL_PAD_FROM")', "PaddedLocalWords(used_elements, pad_to, pad_from)",
                   "TypeArray(element_type, Const(num_elements))"):
        if needle not in ctx:
            raise SystemExit("spirv_emit_context.cpp no longer has: " + needle)
    if '"RX_VK_LOCAL"' not in shader:
        raise SystemExit("the program cache's stamp no longer covers the RX_VK_LOCAL names")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="local-memory-pad-") as tmp:
        exe = Path(tmp) / "pad"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(RECOMPILER),
                        "-I", str(RECOMPILER / "shader_recompiler" / ".."), str(ROOT / "tests/test_local_memory_pad.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
