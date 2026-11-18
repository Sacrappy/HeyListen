"""HOS_JOBTRACE's renderer marks on Linux: jt_mark (gpu/vk/vk_replay.cpp) finds the exe's hos_jt_mark_ext (host/hos_jobtrace.c).

The Windows renderer looks the mark up with GetProcAddress on the exe (__declspec(dllexport)); on Linux an executable's
functions are not in its dynamic symbol table unless the link exports them, so nx2/build.py's exe link names it in
--export-dynamic-symbol and jt_mark looks it up with dlsym(RTLD_DEFAULT). This builds the same shape: an exe compiled with
-ffunction-sections and linked with --gc-sections and every --export-dynamic-symbol of build.py's link line, with the mark
declared as hos_jobtrace.c declares it, and a shared library with jt_mark cut out of vk_replay.cpp, which the exe dlopens;
a mark made in the library must reach the exe. It also fails when the export is left out of the link (the check that the
test sees the bug). Linux only (on Windows the mark is a DLL export).
"""
import argparse
import ast
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, head):
    start = source.index(head)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


EXE = r'''
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#define HOOKSHOT_DECLSPEC_dllexport __attribute__((visibility("default")))
#define __declspec(x) HOOKSHOT_DECLSPEC_##x
static int got_kind;
static uint64_t got_v;
static void HOS_JT_MARK(int kind, uint64_t v) { got_kind = kind, got_v = v; }
#define HOS_JT(type, a, b) HOS_JT_MARK(a, b)
MARK_DEFINITION
int main(int argc, char** argv) {
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    void (*mark)(int, uint64_t) = (void (*)(int, uint64_t))dlsym(lib, "test_mark");
    if (!mark) return 4;
    mark(17, 0x1234);
    (void)argc;
    if (got_kind != 17 || got_v != 0x1234) { fprintf(stderr, "the mark did not reach the exe\n"); return 1; }
    puts("mark reached the exe");
    return 0;
}
'''

LIB = r'''
#include <cstdint>
#ifndef _WIN32
#include <dlfcn.h>
#endif
namespace mkwvk {
JT_MARK
}
extern "C" __attribute__((visibility("default"))) void test_mark(int kind, uint64_t v) { mkwvk::jt_mark(kind, v); }
'''


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    if os.name == "nt":
        print("skipped: Linux only (on Windows hos_jt_mark_ext is a DLL export)")
        return
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    jt = function(replay, "void jt_mark(int kind, uint64_t v) {")
    assert 'dlsym(RTLD_DEFAULT, "hos_jt_mark_ext")' in jt, "jt_mark looks the mark up in the process on Linux"
    head = replay[:replay.index("namespace mkwvk {")]
    assert re.search(r"#ifndef _WIN32\s*\n#include <dlfcn.h>", head), "vk_replay.cpp includes dlfcn.h on Linux"
    jobtrace = (ROOT / "host/hos_jobtrace.c").read_text(encoding="utf-8")
    mark = re.search(r"^__declspec\(dllexport\) void hos_jt_mark_ext\(int kind, uint64_t v\) \{[^\n]*\}$", jobtrace, re.M)
    assert mark, "hos_jobtrace.c's hos_jt_mark_ext as the test copies it"
    build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
    link = build[build.index("exe = os.path.join(out, EXE)"):]
    link = link[:link.index("capture_output=True")]
    assert '"-Wl,--export-dynamic-symbol=" + s for s in HOST_EXPORTS' in link, "the exe's link exports HOST_EXPORTS"
    tree = ast.parse(build)
    names = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign) and
                 any(isinstance(t, ast.Name) and t.id == "HOST_EXPORTS" for t in n.targets))
    exports = ["-Wl,--export-dynamic-symbol=" + s for s in names]
    assert "-Wl,--export-dynamic-symbol=hos_jt_mark_ext" in exports, "the exe's link exports hos_jt_mark_ext"
    cc = shutil.which(a.cxx.replace("clang++", "clang").replace("g++", "gcc")) or a.cxx
    lld = ["-fuse-ld=lld"] if shutil.which("ld.lld") else []
    with tempfile.TemporaryDirectory() as tmp:
        exe_c, lib_cpp = os.path.join(tmp, "exe.c"), os.path.join(tmp, "lib.cpp")
        lib = os.path.join(tmp, "libjt.so")
        Path(exe_c).write_text(EXE.replace("MARK_DEFINITION", mark.group(0)), encoding="utf-8")
        Path(lib_cpp).write_text(LIB.replace("JT_MARK", jt), encoding="utf-8")
        r = subprocess.run([a.cxx, "-std=c++20", "-shared", "-fPIC", "-O2", lib_cpp, "-o", lib], capture_output=True, text=True)
        if r.returncode:
            sys.exit("library compile failed:\n" + r.stdout + r.stderr)

        def link_and_run(flags):
            exe = os.path.join(tmp, "exe")
            r = subprocess.run([cc, "-O2", "-ffunction-sections", exe_c, "-o", exe, "-Wl,--gc-sections"] + lld + flags + ["-ldl"],
                               capture_output=True, text=True)
            if r.returncode:
                sys.exit("exe link failed:\n" + r.stdout + r.stderr)
            return subprocess.run([exe, lib], capture_output=True, text=True)
        r = link_and_run(exports)
        if r.returncode:
            sys.exit("with build.py's exports: " + r.stdout + r.stderr)
        print(r.stdout.strip())
        r = link_and_run([e for e in exports if not e.endswith("=hos_jt_mark_ext")])
        if r.returncode == 0:
            sys.exit("without the export the mark still arrived: the test does not see what it checks")
    print("ok")


if __name__ == "__main__":
    main()
