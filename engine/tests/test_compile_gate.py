"""The driver's compiles (gpu/vk/compile_gate.h): the header's weights and rule under threads, and the wiring that puts the
gate on the start-up pipeline build and the prediction in vk_draw.cpp (RX_VK_COMPILE_AT_ONCE, the memory it reads, the
programs left out for their size, the turn taken around every build, the freed memory handed back). Ported from Longshot
c392df3a (Tomiss), widened here to the start-up graphics pipelines (issue 88).
python -I -B tests/test_compile_gate.py [--cxx clang++]"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_wiring():
    dr = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    for needle in (
        '#include "compile_gate.h"',
        "RX_VK_COMPILE_AT_ONCE",
        "compile_gate::weight_words(",
        "compile_gate::fits_alone(",
        "compile_gate::trims(",
        "memory_available_mb()",
        "compile_gate::cgroup_room_mb(",
        # (the turn is taken around the driver's compiles, not held across the queues' bookkeeping)
        "const CompileTurn turn(",
        # (the start-up build: the turn around each of its three kinds of item)
        "void precompile_run(void (*progress)(size_t done, size_t total), bool wait) {",
        # (a stopped preparation ends a wait without a turn)
        "prepare_stopped()",
    ):
        if needle not in dr:
            raise SystemExit("vk_draw.cpp no longer has: " + needle)
    start = dr.index("void precompile_run(void (*progress)(size_t done, size_t total), bool wait) {")
    body = dr[start:dr.index("void predict_run(", start)]
    for needle in (
        "const CompileTurn turn(compile_weight(p));",   # (a compute pipeline: by its size, or pinned as upstream has it)
        "const CompileTurn turn(compile_weight(*it.program));",   # (a graphics pipeline: by the program's SPIR-V)
        "predictlimits::too_big(",              # (the two programs the driver never finishes are left to their draw)
        "compile_gate_begin();",
    ):
        if needle not in body:
            raise SystemExit("precompile_run no longer has: " + needle)
    if "the compiles: as many at once as" not in dr:   # (compile_gate_begin's own line, once a phase)
        raise SystemExit("the gate no longer says what it holds")
    # (the prediction's own builds take a turn too: they are the same driver compiles, before the game)
    pred = dr[dr.index("void predict_run("):dr.index("void compute_optimize(", dr.index("void predict_run("))]
    if "const CompileTurn turn(" not in pred:
        raise SystemExit("predict_run's builds no longer take a turn at the gate")
    # (nothing of it on Windows by itself: a Windows run must behave as it did, and does when the switch is unset)
    if "dev.props.vendorID == 0x8086" not in dr:
        raise SystemExit("the gate no longer names Mesa's Intel driver as the one it is on for")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory(prefix="compile-gate-") as temp:
        exe = Path(temp) / "compile_gate_test.exe"
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/compile_gate_test.cpp"),
                        "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, timeout=120, capture_output=True, text=True).stdout
        assert "compile_gate: ok" in out, out
    print("PASS:", out.strip())


if __name__ == "__main__":
    main()
