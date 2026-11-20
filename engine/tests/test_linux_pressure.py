"""Linux memory pressure (gpu/vk/linux_pressure.h): the header over the Deck's and the desktop's measured numbers, and the
wiring in vk_texture.cpp and vk_memory.cpp (the reserves read from the environment, the sample in texture_frame, the budget
hold, the imports given back under it). Ported from Longshot f1b17d59 (dukenheimer).
python -I -B tests/test_linux_pressure.py [--cxx clang++]"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_wiring():
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in (
        '#include "linux_pressure.h"',
        "RX_VK_AVAIL_RESERVE_MB",
        "linux_pressure::PressureHold hold;",
        "linux_pressure::read_meminfo(&cached_avail, &cached_swap, nullptr, nullptr, &cached_has_swap)",
        "linux_pressure::shortfall(cached_avail, cached_swap, avail_res, commit_res, cached_has_swap)",
        "frame_no % linux_pressure::kSampleEveryPresents == 0",
        "linux_shortfall_bytes.store(over",
        "linux_pressure_shortfall",
        # (the commit reserve reaches Linux: it used to answer "Windows only" and never look)
        "commit_headroom(&commit_free)",
    ):
        if needle not in tex:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)
    # (the texture budget is cut on an integrated GPU alone: on a discrete card VRAM and RAM are different memories)
    start = tex.index("void texture_frame(uint64_t completed) {")
    body = tex[start:start + 200000]
    if "deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU" not in body:
        raise SystemExit("texture_frame's Linux pressure no longer tells an integrated GPU from a discrete one")
    if "budget = std::min(budget, held_cached);" not in body:
        raise SystemExit("texture_frame's Linux pressure no longer holds the texture budget")
    # (vk_memory.cpp: the imports are asked for under the same sample, once per episode at most its deepest shortfall)
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for needle in (
        '#include "linux_pressure.h"',
        "linux_pressure_shortfall(&over_seq)",
        "linux_pressure::episode_want(",
        "linux_pressure::import_want(",
        "cap_free_trim(",
    ):
        if needle not in mem:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)
    # (the header is not part of the shader translation: nothing here may change the translator's stamp)
    sh = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    if "linux_pressure" in sh:
        raise SystemExit("vk_shader.cpp (the translator's stamp) now reads the Linux pressure: every player's programs would be "
                         "translated again for a memory valve")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory(prefix="linux-pressure-") as temp:
        exe = Path(temp) / "linux_pressure_test.exe"
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/linux_pressure_test.cpp"),
                        "-o", str(exe)], check=True)
        out = subprocess.run([str(exe), temp], check=True, timeout=60, capture_output=True, text=True).stdout
        assert " 0 failures" in out, out
    print("PASS:", out.strip())


if __name__ == "__main__":
    main()
