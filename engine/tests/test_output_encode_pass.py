"""The output encoder pass (gpu/vk/eden/shader_recompiler/ir_opt/output_encode_pass.h) on synthetic IR: the game's HDR10
encoder shape (a texture's colour through a matrix of constant buffer words, times 1e-4, PQ) takes RenoDX's zelda-oot-oz
encoder, its Global Color Space and UI Brightness read from the renderer's bank; the shapes it must not take are left as they
are (tests/output_encode_ir.cpp). And the wiring: the encoder is found by PQ's constants in its code, its program keyed and
translated with PsychoV35's, and its rows are PsychoV35's.

python -I -B tests/test_output_encode_pass.py [--cxx clang++]
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import concurrent.futures
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
ENV_ON_H = str(VK / "compat" / "hookshot_env.h")


def build_and_run(cxx):
    sources = sorted((VK / "eden" / "shader_recompiler" / "frontend" / "ir").glob("*.cpp"))
    flags = ["-std=c++20", "-include", ENV_ON_H, "-O1", "-D_CRT_SECURE_NO_WARNINGS", "-DMKWVK_INCLUDES=2"]
    for header in ("algorithm", "cstdio", "cstdlib", "cstring", "functional", "string", "vector"):
        flags += ["-include", header]
    flags += ["-I" + str(p) for p in (VK / "compat", ROOT / "gpu" / "compat", VK / "eden", ROOT / "gpu" / "third_party" / "suyu")]
    with tempfile.TemporaryDirectory(prefix="output-encode-") as temp:
        folder = Path(temp)
        sources = [ROOT / "tests" / "output_encode_ir.cpp", *sources]
        objs = [folder / ("%03d.o" % i) for i in range(len(sources))]
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
            list(pool.map(lambda job: subprocess.run([cxx, *flags, "-c", str(job[0]), "-o", str(job[1])], check=True),
                          zip(sources, objs)))
        exe = folder / "fixture.exe"
        subprocess.run([cxx, *map(str, objs), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout, end="")
        print(run.stderr, end="")
        if run.returncode != 0 or "PASS" not in run.stdout:
            raise SystemExit("FAIL: the output encoder pass on synthetic IR")


def wiring():
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    fails = []
    # (found as the scene programs are, where a fragment stage's code is read (HeyListen: parse_code, the one place, for the game's
    # programs and the seed's); never a scene mapping program too)
    if shader.count("c.pq_fit = !c.scene_fit;") != 1:
        fails.append("the encoder not found by its constants where a program's code is read")
    if "(fs->scene_fit || fs->pq_fit) && scene_tonemap_on()" not in shader:
        fails.append("the encoder's program not keyed and translated with PsychoV35's")
    if "const bool encoder = fsc->pq_fit;" not in shader or \
            "encoder ? Shader::Optimization::OutputEncodePass(fs->program)" not in shader:
        fails.append("translate_into does not run the encoder pass for the encoder")
    guard = shader.find("Shader::Optimization::GuardFragmentOutputs(fs->program")
    if not (0 <= shader.find("OutputEncodePass(fs->program)") < guard):
        fails.append("the encoder pass after the NaN guard")
    for row in ("PsychoV35 UI brightness", "PsychoV35 color space"):
        if '"%s"' % row not in menu:
            fails.append("the row " + row)
    for slot in ("kSettingSceneScale", "kSettingColorSpace", "kSettingOutputScale"):
        if "out[pv::%s]" % slot not in menu:
            fails.append("psychov35_bank does not give " + slot)
    if "int pv35_ui_white = 203, pv35_color_space = 1;" not in (VK / "vk_menu.h").read_text(encoding="utf-8"):
        fails.append("the UI's white not 203 nits, or the colour space not BT.709 (RenoDX's defaults)")
    if fails:
        raise SystemExit("FAIL: " + "; ".join(fails))
    print("wiring: the encoder's detection, key, translation, rows and bank agree")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    wiring()
    build_and_run(a.cxx)
    print("PASS: the output encoder pass")


if __name__ == "__main__":
    main()
