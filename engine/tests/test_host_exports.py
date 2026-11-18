"""The Linux exe exports every function the Vulkan back end looks up in it by name (nx2/build.py HOST_EXPORTS).

The back end (hookshot_vk.so on Linux) finds some of the host's functions by name rather than through MkwVkHost:
GetProcAddress(GetModuleHandleA(nullptr), "hos_...") (the POSIX shim makes that dlsym(dlopen(NULL), ...)) or
dlsym(RTLD_DEFAULT, "hos_..."). On Windows they are DLL exports; an ELF executable's functions are in its dynamic symbol
table only when the link exports them, and the Linux link exported hos_posix_* and hos_jt_mark_ext alone: hos_gpu_live_pools
(RX_VK_POOL_FINALIZE, RX_VK_CAP_ONLY_GPU), hos_gpu_write_live (the staged lazy release), hos_gran_take, hos_lag_* and the
rest were never found there, and what they enable did nothing on Linux, silently.

This reads every "hos_..." name in gpu/ (each one is such a lookup: checked against the call it sits in) and checks that
HOST_EXPORTS covers it, so the next lookup cannot be added without its export. Then, on Linux, it links an exe as build.py
does (-ffunction-sections, --gc-sections, its --export-dynamic-symbol list) defining every name, and checks that each is in
the exe's dynamic symbol table (dlsym(RTLD_DEFAULT)); without the exports none is (the check that the test sees the bug).
"""
import argparse
import ast
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOOKUP = re.compile(r'GetProcAddress\(\s*(?:GetModuleHandle[AW]\(\s*nullptr\s*\)|exe)\s*,\s*"(hos_\w+)"|'
                    r'dlsym\(\s*RTLD_DEFAULT\s*,\s*"(hos_\w+)"|host_fn<[^>]*>\(\s*"(hos_\w+)"')


def host_exports():
    tree = ast.parse((ROOT / "nx2/build.py").read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "HOST_EXPORTS" for t in node.targets):
            return tuple(ast.literal_eval(node.value))
    raise SystemExit("nx2/build.py has no HOST_EXPORTS")


def looked_up():
    names, stray = {}, []
    for path in sorted((ROOT / "gpu").rglob("*")):
        if path.suffix not in (".cpp", ".h", ".c") or path.name.endswith("_spv.h"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        found = {next(g for g in m.groups() if g) for m in LOOKUP.finditer(text)}
        for name in found:
            names.setdefault(name, path.relative_to(ROOT).as_posix())
        # (a "hos_..." name in gpu/ outside such a call: a lookup this test does not recognise, so it would miss its export)
        for m in re.finditer(r'"(hos_\w+)"', text):
            if m.group(1) not in found:
                stray.append("%s: %s" % (path.relative_to(ROOT).as_posix(), m.group(1)))
    return names, stray


EXE = r'''
#include <dlfcn.h>
#include <stdio.h>
DEFINITIONS
static const char* const names[] = {NAMES};
int main(void) {
    int missing = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!dlsym(RTLD_DEFAULT, names[i])) { printf("not exported: %s\n", names[i]); missing++; }
    printf("%d of %u found\n", (int)(sizeof names / sizeof names[0]) - missing, (unsigned)(sizeof names / sizeof names[0]));
    return missing ? 1 : 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--cc", default="clang")
    a = p.parse_args()
    exports = host_exports()
    names, stray = looked_up()
    if stray:
        raise SystemExit("hos_ names in gpu/ outside a lookup this test knows:\n  " + "\n  ".join(stray))
    for want in ("hos_gpu_live_pools", "hos_gpu_write_live", "hos_jt_mark_ext", "hos_posix_guest_memfd"):
        if want not in names:
            raise SystemExit(f"the scan of gpu/ did not find the lookup of {want}: the patterns are out of date")
    missing = sorted(n for n in names if not any(fnmatch.fnmatchcase(n, e) for e in exports))
    if missing:
        raise SystemExit("looked up by the back end but not in nx2/build.py HOST_EXPORTS:\n  " +
                         "\n  ".join("%s (%s)" % (n, names[n]) for n in missing))
    build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
    link = build[build.index("exe = os.path.join(out, EXE)   # (+ hookshot.map"):]
    link = link[:link.index("capture_output=True")]
    if '"-Wl,--export-dynamic-symbol=" + s for s in HOST_EXPORTS' not in link:
        raise SystemExit("the Linux exe's link does not export HOST_EXPORTS")
    print("%d names looked up in the exe, all in HOST_EXPORTS" % len(names))
    if os.name == "nt":
        print("skipped the link: Linux only (on Windows they are DLL exports)")
        return
    cc = shutil.which(a.cc) or shutil.which("cc")
    if not cc:
        print("skipped the link: no C compiler")
        return
    lld = ["-fuse-ld=lld"] if shutil.which("ld.lld") else []
    flags = ["-Wl,--export-dynamic-symbol=" + e for e in exports]
    defs = "\n".join('__attribute__((visibility("default"))) int %s(void) { return %d; }' % (n, i + 1)
                     for i, n in enumerate(sorted(names)))
    src = EXE.replace("DEFINITIONS", defs).replace("NAMES", ", ".join('"%s"' % n for n in sorted(names)))
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = os.path.join(tmp, "exe.c"), os.path.join(tmp, "exe")
        Path(c).write_text(src, encoding="utf-8")

        def link_and_run(extra):
            r = subprocess.run([cc, "-O2", "-ffunction-sections", c, "-o", exe, "-Wl,--gc-sections"] + lld + extra + ["-ldl"],
                               capture_output=True, text=True)
            if r.returncode:
                raise SystemExit("exe link failed:\n" + r.stdout + r.stderr)
            return subprocess.run([exe], capture_output=True, text=True)
        r = link_and_run(flags)
        if r.returncode:
            raise SystemExit("with build.py's exports:\n" + r.stdout + r.stderr)
        print(r.stdout.strip().splitlines()[-1])
        r = link_and_run([])
        if r.returncode == 0:
            raise SystemExit("without the exports every name was still found: the test does not see what it checks")
    print("ok")


if __name__ == "__main__":
    main()
