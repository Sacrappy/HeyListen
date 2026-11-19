"""The shaders' preparation, faster and honest about how long it takes (gpu/vk/prepare_work.h, vk_draw.cpp prepare_run, the builders).

A player's builder said "Prepare the shaders" for 112 minutes at 97%, under "Usually about 45 minutes". The step was half the CPUs
working (80% of the time is inside the driver's compiler, which is the CPU's), its bar went by programs (a compute program costs
what a hundred graphics ones do: 7% for 8 of 9 minutes) and its note never changed. This checks: the worker count and the bar's
costs (tests/prepare_speed_test.cpp compiles prepare_work.h), the switches that turn each change off, the memory gate the driver
builds wait at, the progress by cost reaching the builder, the time left the builders show (shader_time.py and ShaderStep.cs), and
that the compute variants and the list of dispatched programs only narrow what is built before the game, never what is kept.
python -I -B tests/test_prepare_speed.py [--cxx c++]
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def check_sources():
    problems = []
    draw = read("gpu", "vk", "vk_draw.cpp")
    run = block(draw, "bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {")
    if '#include "prepare_work.h"' not in draw:
        problems.append("vk_draw.cpp does not include prepare_work.h")
    if "prepare_worker_count(background, cpus)" not in run:
        problems.append("prepare_run does not take its worker count from prepare_worker_count")
    if "std::clamp(cpus / 2, 1u, 32u)" in run:
        problems.append("prepare_run still has the half-the-CPUs worker count in line")
    counts = block(draw, "unsigned prepare_worker_count(bool background, unsigned cpus) {")
    if 'getenv("RX_VK_PREPARE_WORKERS")' not in counts or "prepare_work::workers(" not in counts:
        problems.append("prepare_worker_count is not prepare_work::workers with RX_VK_PREPARE_WORKERS")
    # the memory gate: the driver's builds ask it, so more workers cannot run the PC out of commit
    gate = block(draw, "struct CommitGate {")
    for want in ("RX_VK_PREPARE_COMMIT_MB", "commit_left_mb()", "prepare_stopped()", "inside()"):
        if want not in gate:
            problems.append("CommitGate lacks " + want)
    if run.count("const CommitGate commit;") != 2:
        problems.append("both of prepare_run's driver builds (compute pipelines, stage libraries) must wait at the CommitGate")
    # compute programs: variants and the list play dispatched
    for want in ("RX_VK_WARM_VARIANTS", "RX_VK_WARM_COMPUTES"):
        if want not in draw:
            problems.append("the switch " + want + " is gone from vk_draw.cpp")
    warm = run[run.index("const auto warm_compute = [&](Program& p) {"):run.index("const auto worker = [&] {")]
    if "warm_used_only(" not in run or "used_computes.count(p.key)" not in warm:
        problems.append("warm_compute does not leave a program play never dispatched to its first dispatch when there is a list")
    if "warm_base_only()" not in warm:
        problems.append("warm_compute does not know RX_VK_WARM_VARIANTS")
    if "program_cache_save" in warm or "seed_compute(" in warm:
        problems.append("warm_compute must only build pipelines: what is translated stays in the program cache whatever is warmed")
    pre = block(draw, "void precompile_run(void (*progress)(size_t done, size_t total), bool wait) {")
    if "warm_base_only()" not in pre or "variants_seen" not in pre:
        problems.append("precompile_run does not leave the variants RX_VK_WARM_VARIANTS=base asks to play")
    # the bar by cost reaches the builder
    if "prepare_work::cost(" not in run or "prepare_cost_fraction.store" not in run:
        problems.append("prepare_run does not publish its progress by cost")
    main = read("gpu", "vk", "vk_main.cpp")
    if "prepare_progress_by_cost()" not in main:
        problems.append("the --prepare-shaders progress lines do not use the progress by cost")
    if "prepare_progress_by_cost" not in read("gpu", "vk", "vk_internal.h"):
        problems.append("vk_internal.h does not declare prepare_progress_by_cost")
    # the profile that says where the time went
    for want in ("PrepKind k_graphics", "k_fallback", "k_warm_compute", "shader_profile(prof)", "prepare profile:"):
        if want not in draw:
            problems.append("the preparation's profile lost " + want)
    shader = read("gpu", "vk", "vk_shader.cpp")
    for want in ("st_lockwait_ns", "st_write_ns", "void shader_profile(double out[6])"):
        if want not in shader:
            problems.append("vk_shader.cpp lost " + want)
    # the builders
    win = read("builder", "py", "hookshot_builder", "window.py")
    shaders_branch = win.index("shader_time.shaders_note(")
    if win.index('"Usually about 45 minutes"') < shaders_branch:
        problems.append('window.py shows "Usually about 45 minutes" during the shaders step')
    if "self.shaders_frac = f" not in win:
        problems.append("window.py does not take the shaders' @@progress for its time left")
    cs = read("builder", "HookshotBuilder", "MainWindow.xaml.cs")
    if "ShaderStep.Note(" not in cs or "shadersFraction = f" not in cs:
        problems.append("MainWindow.xaml.cs does not show the shaders' time left")
    if "public static string Note(" not in read("builder", "HookshotBuilder", "ShaderStep.cs"):
        problems.append("ShaderStep.cs lost Note")
    return problems


def check_note():
    spec = importlib.util.spec_from_file_location("shader_time", ROOT / "builder" / "py" / "hookshot_builder" / "shader_time.py")
    st = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(st)
    problems = []
    early = st.shaders_note(30, None)
    if "Preparing the shaders 0:30" not in early or " left" in early:
        problems.append("the note before the first estimate promises a time left: " + early)
    if " left" in st.shaders_note(60, 0.5) or " left" in st.shaders_note(400, 0.01):
        problems.append("the note gives a time left before two minutes and 3%")
    # a step at 25% after 10 minutes has 30 minutes to go
    if "About 30 minutes left" not in st.shaders_note(600, 0.25):
        problems.append("25 percent after 10 minutes: %r" % st.shaders_note(600, 0.25))
    # the reported case: 112 minutes at 97%: about 3 minutes, not a promise of 45
    late = st.shaders_note(112 * 60 + 39, 0.97)
    if "About 3 minutes left" not in late or "45" in late:
        problems.append("112:39 at 97 percent: %r" % late)
    if "About 1 h 40 min left" not in st.shaders_note(3600, 0.375):
        problems.append("an hour at 37.5 percent: %r" % st.shaders_note(3600, 0.375))
    if "Less than a minute left" not in st.shaders_note(900, 0.999) or "About a minute left" not in st.shaders_note(3000, 0.98):
        problems.append("the end of the step is not said as a minute")
    st.shaders_note(100, 1.0)   # (a fraction of 1.0: no division by zero)
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    problems = check_sources() + check_note()
    with tempfile.TemporaryDirectory(prefix="prepare-speed-") as folder:
        exe = Path(folder) / "t"
        r = subprocess.run([cxx, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "prepare_speed_test.cpp"),
                            "-o", str(exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit("FAIL: prepare_speed_test.cpp does not compile\n" + r.stdout + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(r.stdout, end="")
        if r.returncode:
            problems.append("prepare_speed_test failed")
    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    print("PASS: the preparation's workers, bar and time left")


if __name__ == "__main__":
    main()
