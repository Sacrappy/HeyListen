"""Syntax check of the Vulkan backend's sources (gpu/vk): Eden's shader recompiler, sirit and the backend.

Usage:  python -I nx2/vkcheck.py [substring ...]   (only the sources whose path contains one of them)
"""
import concurrent.futures
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit
import toolchain

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VK = os.path.join(ROOT, "gpu", "vk")
CPM = os.path.join(kit.WORK, "eg", ".cache", "cpm")


def pkg(name):
    d = os.path.join(CPM, name)
    return os.path.join(d, sorted(os.listdir(d))[-1])


def sources():
    out = []
    for dp, _, fn in os.walk(os.path.join(VK, "eden")):
        out += [os.path.join(dp, f) for f in fn if f.endswith(".cpp")]
    for dp, _, fn in os.walk(os.path.join(VK, "sirit", "src")):
        out += [os.path.join(dp, f) for f in fn if f.endswith(".cpp")]
    out += [os.path.join(VK, f) for f in os.listdir(VK) if f.endswith(".cpp")]
    return sorted(out)


def flags():
    return toolchain.native_target() + [
        "-std=c++20", "-O2", "-g0", "-DNDEBUG", "-Wno-everything", "-DVK_NO_PROTOTYPES",
        "-I" + os.path.join(VK, "compat"), "-I" + os.path.join(ROOT, "gpu", "compat"),
        "-I" + os.path.join(VK, "eden"), "-I" + os.path.join(VK, "sirit", "include"),
        "-I" + os.path.join(VK, "sirit", "src"),
        "-I" + os.path.join(pkg("spirv-headers"), "include"),
        "-I" + os.path.join(pkg("spirv-headers"), "include", "spirv", "unified1"),
        "-I" + os.path.join(pkg("vulkanheaders"), "include"),
        "-I" + os.path.join(pkg("vulkanmemoryallocator"), "include"),
        "-I" + os.path.join(ROOT, "gpu", "render")]


def main():
    cxx = toolchain.need(toolchain.find("clang++"))
    srcs = [s for s in sources() if len(sys.argv) < 2 or any(a in s for a in sys.argv[1:])]
    fl = flags()

    def one(s):
        r = subprocess.run([cxx] + fl + ["-fsyntax-only", s], capture_output=True, text=True)
        return s, r.returncode, r.stderr

    bad = 0
    with concurrent.futures.ThreadPoolExecutor(4) as ex:
        for s, rc, err in ex.map(one, srcs):
            if rc:
                bad += 1
                print("== %s" % os.path.relpath(s, ROOT))
                print("\n".join(l for l in err.splitlines() if "error" in l)[:1500])
    print("%d of %d sources failed" % (bad, len(srcs)))


if __name__ == "__main__":
    main()
