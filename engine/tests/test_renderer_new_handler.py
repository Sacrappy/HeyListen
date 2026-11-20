"""The renderer's C++ allocations at Windows' commit limit (issues 55 and 62; Longshot 1280). hookshot_vk.dll's `operator new`
that malloc refused threw a std::bad_alloc that nothing caught: "exception 0x20474343 at KERNELBASE.dll", the game gone at once
with no wait and no out-of-memory notice, on PCs with 32 GB. gpu/vk/vk_main.cpp's new_refused (the C++ new_handler,
installed by mkwvk_init) now tries again every 50 ms for up to 5 s while the commit is nearly used, then asks the host for its
out-of-memory line and notice (MkwVkHost::out_of_memory, API version 15), after which the game stops.

Compiles the real new_refused cut out of vk_main.cpp with a fake host and commit (tests/renderer_new_handler_test.cpp), each
case in its own process:

- plenty / unknown: 4 GB or more of commit left, or not known: the std::bad_alloc as before, at once (a bad shader's size is
  not the limit, and its catch still holds it);
- limit: a real refusal (1 PB asked of operator new) with the commit nearly used: tries for 5 s, then the host's out_of_memory;
- recovers: refused twice, then it goes on, the host not asked;
- oldhost: a host with no out_of_memory: the exception after 5 s, as before;
- budget: the 5 s are a thread's own, and a refusal after a pause of over 250 ms starts them again.

And by text: mkwvk_init installs the handler, the host fills out_of_memory and stops through the notice, and the API version.

python -I -B tests/test_renderer_new_handler.py [--cc g++]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "g++"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ("plenty", "unknown", "recovers", "limit", "oldhost", "budget")


def block(text, head):
    at = text.find(head)
    if at < 0:
        raise SystemExit("FAIL: not found: " + head)
    depth, i = 0, text.index("{", at)
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[at:j + 1]
    raise SystemExit("FAIL: unbalanced: " + head)


def check_sites():
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    if not re.search(r"#define MKWVK_API_VERSION 1[5-9]\b", api) or "void (*out_of_memory)(const char *what);" not in api:
        raise SystemExit("FAIL: vk_api.h has no out_of_memory in its host table at version 15")
    main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    init = block(main, "const MkwVkRenderer* mkwvk_init(const MkwVkHost* h)")
    if "std::set_new_handler(new_refused)" not in init or init.find("host = h;") > init.find("std::set_new_handler"):
        raise SystemExit("FAIL: mkwvk_init does not install new_refused after it has the host")
    rx = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    stop = block(rx, "static void host_out_of_memory(const char *what)")
    if "hos_out_of_memory(1)" not in stop or "hos_fatal(" not in stop:
        raise SystemExit("FAIL: the host's out_of_memory does not show the notice (waited for) and stop")
    # (later fields follow it: version 16's no_usable_gpu)
    if not re.search(r"hos_perf_info,\s*host_out_of_memory,\s*(?:\w+,\s*)*\};", rx):
        raise SystemExit("FAIL: vk_host does not fill out_of_memory after perf_info")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="g++")
    args = parser.parse_args()
    check_sites()
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    piece = block(main_cpp, "static void new_refused()")
    cc = shutil.which(args.cc) or shutil.which("clang++")
    if not cc:
        raise SystemExit("test_renderer_new_handler: no C++ compiler %r (pass --cc)" % args.cc)
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="renderer-new-handler-") as temporary:
        folder = Path(temporary)
        (folder / "new_handler.inc").write_text(piece + "\n", encoding="utf-8")
        executable = folder / "fixture.exe"
        subprocess.run([cc, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(folder),
                        str(ROOT / "tests/renderer_new_handler_test.cpp"), "-o", str(executable), "-pthread"] + (["-static"] if os.name == "nt" else []),
                       check=True, creationflags=flags)
        for case in CASES:
            result = subprocess.run([str(executable), case], capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", timeout=60, creationflags=flags)
            print("%s: %s" % (case, result.stdout.replace("\n", "\n  ").rstrip()))
            if result.returncode:
                print(result.stderr, end="", file=sys.stderr)
                raise SystemExit("FAIL: case %s (exit %d)" % (case, result.returncode))
    print("PASS: a refused C++ allocation at the commit limit waits up to 5 s, then the host says out of memory")


if __name__ == "__main__":
    main()
