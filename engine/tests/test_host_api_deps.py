"""Game-free: the host objects' dependency blob (nx2/build.py host_deps) takes gpu/vk/vk_api.h on Windows as it does on Linux.
host/rx_backend.c fills the back end's host table with MKWVK_API_VERSION: a cached host object from before a change of it
still passed the old version and the built game fell back to the WebGPU renderer ("rx: the Vulkan back end did not start")
while the DLL, built from the same tree, was at the new one. vk_api.h is one of the shader stamp's files: a change of it is a
change of every host object that reads the API.

python -I -B tests/test_host_api_deps.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    lines = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8").splitlines()

    def only(needle):
        found = [i for i, l in enumerate(lines) if needle in l]
        if len(found) != 1:
            raise SystemExit("FAIL: nx2/build.py has %d host_deps lines for %s" % (len(found), needle))
        return found[0]

    api = only('host_deps += open(os.path.join(GPU, "vk", "vk_api.h")')
    dlss = only('host_deps += open(os.path.join(GPU, "vk", "dlss_math.h")')
    indent = lambda s: len(s) - len(s.lstrip())
    if indent(lines[api]) != indent(lines[dlss]):
        raise SystemExit("FAIL: host_deps reads vk_api.h inside a platform branch; dlss_math.h is read outside one")
    print("ok: the host objects depend on gpu/vk/vk_api.h on every platform")


if __name__ == "__main__":
    sys.exit(main())
