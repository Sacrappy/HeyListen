"""Storage buffers are bound and translated at 16 bytes at least (gpu/vk/vk_internal.h storage_binding_align).

The translator reads 8- and 16-byte storage accesses through vector views (offset >> 3, >> 4), exact only while a binding
starts on a 16-byte boundary; AMD (RADV, AMDVLK) and Mesa's ANV report a 4-byte minStorageBufferOffsetAlignment, and a
binding started there could make those reads return the wrong words. Builds storage_binding_align as vk_internal.h has
it over a stand-in device and checks: 4 -> 16, 16 -> 16, 64 -> 64, and RX_VK_SSBO_DEVICE_ALIGN=1 the device's own
(4 -> 4). Then that the translator's profile and host info, the descriptor writer and the program cache's stamp all
take it, so the shaders' offsets and the bindings agree, and a cache translated at 4 is not used at 16.

    python -I -B tests/test_ssbo_binding_align.py [--cxx g++]

Ported from Hookshot 6e11217e1 (#2442), the storage alignment part; the check is HeyListen's.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"

PROGRAM = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct Limits { uint64_t minStorageBufferOffsetAlignment; };
struct Props { Limits limits; };
struct Device { Props props; } dev;
%(fn)s
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        dev.props.limits.minStorageBufferOffsetAlignment = strtoull(argv[i], nullptr, 10);
        const uint64_t got = storage_binding_align(), want = strtoull(argv[i + 1], nullptr, 10);
        if (got != want) {
            printf("FAIL: device alignment %%s: bound at %%llu, want %%llu\n", argv[i], (unsigned long long)got,
                   (unsigned long long)want);
            bad++;
        }
    }
    return bad != 0;
}
'''


def section(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    internal = (VK / "vk_internal.h").read_text(encoding="utf-8")
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    fn = section(internal, "inline uint64_t storage_binding_align()", "\n}\n") + "\n}\n"

    profile = section(shader, "Shader::Profile make_profile(bool wide)", "\n}\n")
    host = section(shader, "Shader::HostTranslateInfo host_info()", "\n}\n")
    stamp = section(shader, "uint64_t disk_stamp()", "\n}\n")
    for name, body, want in (("make_profile", profile, "p.min_ssbo_alignment = storage_binding_align();"),
                             ("host_info", host, "h.min_ssbo_alignment = storage_binding_align();"),
                             ("disk_stamp", stamp, "storage_binding_align()")):
        if want not in body:
            raise SystemExit("FAIL: %s does not take storage_binding_align (%s)" % (name, want))
    if "const uint64_t salign = std::max<uint64_t>(storage_binding_align(), 4);" not in draw:
        raise SystemExit("FAIL: the descriptor writer (vk_draw.cpp) does not bind at storage_binding_align")
    if "minStorageBufferOffsetAlignment" in profile + host:
        raise SystemExit("FAIL: the translator still reads the device's own storage alignment")

    with tempfile.TemporaryDirectory(prefix="ssbo-align-") as tmp:
        src, exe = Path(tmp) / "t.cpp", Path(tmp) / "t"
        src.write_text(PROGRAM % {"fn": fn}, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Werror", str(src), "-o", str(exe)], check=True)
        env = {k: v for k, v in os.environ.items() if k != "RX_VK_SSBO_DEVICE_ALIGN"}
        for extra, cases in (({}, ["4", "16", "16", "16", "64", "64", "1", "16"]),
                             ({"RX_VK_SSBO_DEVICE_ALIGN": "1"}, ["4", "4", "16", "16", "64", "64"]),
                             ({"RX_VK_SSBO_DEVICE_ALIGN": "0"}, ["4", "16"])):
            r = subprocess.run([str(exe), *cases], capture_output=True, text=True, env=dict(env, **extra), timeout=30)
            if r.returncode:
                raise SystemExit("FAIL (%s):\n%s%s" % (extra or "default", r.stdout, r.stderr))
    print("PASS: storage buffers bound and translated at 16 bytes at least (RX_VK_SSBO_DEVICE_ALIGN=1: the device's own)")


if __name__ == "__main__":
    sys.exit(main())
