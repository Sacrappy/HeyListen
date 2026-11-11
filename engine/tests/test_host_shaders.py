"""The checked-in host shader headers (gpu/vk/host_shaders/*_comp_spv.h) still match the .comp sources beside them.

The headers are checked in so a clone builds without glslang, and scripts/host_shaders.py makes them from the sources.
This recompiles each one and compares the SPIR-V, so a source edited without remaking its header (or the other way
round) is caught here.

python -I -B tests/test_host_shaders.py [--glslang glslangValidator]
"""
import argparse
import subprocess
import sys
from pathlib import Path

RUN_ALL_ARGS = ["--glslang", "{glslang}"]
# (glslang: a Linux runner package. The headers it checks are SPIR-V, the same on every platform)
RUN_ALL_SKIP = {"windows": "needs glslang: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--glslang")
    a = p.parse_args()
    cmd = [sys.executable, "-I", "-B", str(ROOT / "scripts/host_shaders.py"), "--check"]
    if a.glslang:
        cmd += ["--glslang", a.glslang]
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
