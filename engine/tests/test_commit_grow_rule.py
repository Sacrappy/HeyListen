# Copyright (C) 2026 Sacrappy
"""Compile and run the commit grow rule (gpu/vk/commit_grow_rule.h; no game/GPU): only below the reserve, the size asked
((reserve - left) + reserve, 64 KB up), one try every 10 s, none after 6 in a row that did not raise the limit, the count
again from none after a rise; and by the text, that vk_texture.cpp asks the rule at the commit rule, hands the try to a detached
helper thread (never the render thread), commits and releases the block at once, logs one line, and has the switch
RX_VK_COMMIT_GROW; Windows only."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ['#include "commit_grow_rule.h"',
                   'getenv("RX_VK_COMMIT_GROW")',
                   "commit_grow_rule::should_try(",
                   "commit_grow_rule::ask_bytes(",
                   "commit_grow_rule::started(",
                   "commit_grow_rule::finished(",
                   "std::thread(commit_grow_try, ask).detach();",
                   "commit_grow_busy.exchange(true)",
                   "VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS)",
                   "VirtualFree(p, 0, MEM_RELEASE)",
                   "ullTotalPageFile",
                   "(the page file grew)",
                   "Windows gave no more commit (the limit stays"]:
        if needle not in src:
            raise SystemExit("gpu/vk/vk_texture.cpp no longer has: " + needle)
    i = src.index("static void commit_grow_try(uint64_t size) {")
    body = src[i:src.index("\n}\n", i)]
    if not body.index("commit_limit_now()") < body.index("VirtualAlloc") < body.index("VirtualFree") < body.rindex("commit_limit_now()"):
        raise SystemExit("commit_grow_try must read the limit, commit, release at once, then read the limit again")
    if "#ifdef _WIN32" not in src[i - 900:i]:
        raise SystemExit("commit_grow_try must be Windows only")
    j = src.index("commit_grow_rule::should_try(")
    if src.index("commit limit near: %llu MB of commit left") < j:
        raise SystemExit("the try must sit at the commit rule, before its log line")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="commit-grow-rule-") as tmp:
        exe = Path(tmp) / "commit-grow-rule"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_commit_grow_rule.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
