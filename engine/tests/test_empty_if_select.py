"""A selection with an empty body becomes the selects of its phis (RX_VK_FOLD_EMPTY_IF, default off).

The rewrite (gpu/vk/eden/shader_recompiler/backend/spirv/empty_if_select.h) is compiled and run on its own against hand-made
modules: either side as the body, both phi operand orders, and the cases it must leave alone (a body with an instruction, another
edge into the merge, a phi with a third parent, a module it does not know). The source is checked to call it after the module is
made and patched, behind the switch, whose name sits under a prefix the program cache's stamp covers.

python -I -B tests/test_empty_if_select.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPIRV = ROOT / "gpu/vk/eden/shader_recompiler/backend/spirv"
RECOMPILER = ROOT / "gpu/vk/eden"


def check_source():
    emit = (SPIRV / "emit_spirv.cpp").read_text(encoding="utf-8")
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    for needle in ('getenv("RX_VK_FOLD_EMPTY_IF")', "SelectEmptyIfs(code)", "PatchMeshInstructions(code, ctx);"):
        if needle not in emit:
            raise SystemExit("emit_spirv.cpp no longer has: " + needle)
    if emit.index("PatchMeshInstructions(code, ctx);") > emit.index("SelectEmptyIfs(code)"):
        raise SystemExit("the empty selections are rewritten before the mesh markers are patched")
    if '"RX_VK_FOLD"' not in shader:
        raise SystemExit("the program cache's stamp no longer covers the RX_VK_FOLD names")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="empty-if-select-") as tmp:
        exe = Path(tmp) / "empty_if"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(RECOMPILER),
                        "-I", str(RECOMPILER / "shader_recompiler" / ".."), str(ROOT / "tests/test_empty_if_select.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
