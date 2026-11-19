"""The prediction's limits (gpu/vk/predict_limits.h): the header against the sizes measured on the PC 3050, and the wiring in
vk_draw.cpp's predict_run (what is left out for its size, where the commit is looked at, that the builds in hand are waited for).
python -I -B tests/test_predict_limits.py [--cxx clang++]"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_wiring():
    dr = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    start = dr.index("void predict_run(void (*progress)(size_t done, size_t total), bool background, bool noted) {")
    body = dr[start:dr.index("void compute_optimize(Program& q) {", start)]
    for needle in (
        "predictlimits::low_commit(commit_left_mb(), reserve_mb)",
        "predictlimits::too_big(words, max_kwords)",
        "RX_VK_PREDICT_MAX_KWORDS",
        "RX_VK_PREDICT_RESERVE_MB",
        # (the stop reaches the main loop, and the builds in hand are waited for with the memory still looked at)
        "!out_of_time.load() && !out_of_memory.load()",
        "the builds in hand are let finish",
        # (the wait for them is bounded, and what the workers use outlives the function)
        "RX_VK_PREDICT_WAIT_S", "shared->abandoned = true;", "t.detach()", "predict_slow_add(", "predict_slow_has(", "RX_VK_TEST_SLOW_PIPELINE",
        "std::make_shared<Shared>()", "if (S.abandoned.load()) return;",
    ):
        if needle not in body:
            raise SystemExit("predict_run no longer has: " + needle)
    # (the size a program is judged by comes from the program cache's record)
    sh = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    if "st.spirv_words = (uint32_t)spirv.size();" not in sh:
        raise SystemExit("program_cache_read no longer sets the stage's SPIR-V size")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory(prefix="predict-limits-") as temp:
        exe = Path(temp) / "predict_limits_test.exe"
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/predict_limits_test.cpp"), "-o", str(exe)],
                       check=True)
        out = subprocess.run([str(exe)], check=True, timeout=60, capture_output=True, text=True).stdout
        assert " 0 failures" in out, out
    print("PASS:", out.strip())


if __name__ == "__main__":
    main()
