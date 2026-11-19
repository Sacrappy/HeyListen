"""The commit limit (RAM plus the page file on Windows; HeyListen issues 55 and 62, Hookshot 2972, Longshot 91a1ae8b; on Linux,
since Longshot f1b17d59, MemAvailable plus SwapFree from /proc/meminfo). Every VRAM block is charged to the process's commit, as
the guest's memory is: a run held private commit at the VRAM blocks plus ~21 GB the whole way, and a 32 GB PC reached its commit
limit in Hyrule Field (the renderer's own C++ allocation refused: exception 0x20474343).

Checked here: the textures' budget has a commit rule. Below RX_VK_COMMIT_RESERVE_MB of headroom (GlobalMemoryStatusEx's
ullAvailPageFile, which the POSIX shim fills with MemAvailable plus SwapFree on Linux) the shortfall comes off the budget, as the
driver's limit's excess does, and the eviction gives blocks back; "vk: commit limit near:" says so, at most every 600 frames.
python -I -B tests/test_commit_limit.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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


def main():
    tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
    head = block(tex, "static bool commit_headroom(uint64_t* free_bytes)")
    # (one caller on both: Windows' RAM plus the page file, Linux' MemAvailable plus SwapFree, the same field of the same call)
    if "ullAvailPageFile" not in head or "GlobalMemoryStatusEx(&m)" not in head:
        raise SystemExit("FAIL: commit_headroom does not read GlobalMemoryStatusEx's ullAvailPageFile")
    if "#if defined(_WIN32)" in head or "(void)free_bytes;" in head:
        raise SystemExit("FAIL: commit_headroom is still Windows only: Linux never looked (Longshot f1b17d59)")
    reserve = block(tex, "static uint64_t commit_reserve()")
    if "RX_VK_COMMIT_RESERVE_MB" not in reserve or "2560ull << 20" not in reserve:
        raise SystemExit("FAIL: commit_reserve is not RX_VK_COMMIT_RESERVE_MB with a 2560 MB default")
    rule = re.search(r"if \(reserve && commit_headroom\(&commit_free\) && commit_free < reserve\) \{(.*?)\n        \}", tex, re.S)
    if not rule:
        raise SystemExit("FAIL: the textures' budget has no commit rule")
    body = rule.group(1)
    for need in ("const uint64_t over = reserve - commit_free;",
                 "budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);",
                 "vk: commit limit near:"):
        if need not in body:
            raise SystemExit("FAIL: the commit rule lacks: " + need)
    # (the rule sits among the budget's limits, before the budget is used to evict)
    if tex.find("commit_headroom(&commit_free)") > tex.find("static int failures_seen = 0;"):
        raise SystemExit("FAIL: the commit rule comes after the budget's last limit")
    print("PASS: the textures give commit back near Windows' commit limit")
    # (MEM2, #55: the first-touch commit of guest memory refused at the limit waits and stops with the notice, it no longer
    # gives the fault on to the game's own store)
    glue = (ROOT / "host" / "mkw_glue.c").read_text(encoding="utf-8")
    retry = block(glue, "static int commit_retry(")
    if "ERROR_COMMITMENT_LIMIT" not in retry or "Sleep(50)" not in retry or "i < 100" not in retry:
        raise SystemExit("FAIL: commit_retry is not 100 tries of 50 ms at ERROR_COMMITMENT_LIMIT")
    if "if (held) hos_gp_hold(0);" not in retry:
        raise SystemExit("FAIL: commit_retry sleeps with the drops' hold taken")
    touch = block(glue, "static LONG CALLBACK commit_on_touch(")
    for need in ("commit_retry(GP(guest & ~0xFFFFULL), 0x10000, 1)", "hos_out_of_memory(1);", "hos_fatal(NULL, \"out of memory"):
        if need not in touch:
            raise SystemExit("FAIL: the first-touch handler lacks: " + need)
    commit = block(glue, "void hos_commit(uint64_t addr, uint64_t size)")
    if "tries >= 100" not in commit or "ERROR_COMMITMENT_LIMIT" not in commit:
        raise SystemExit("FAIL: hos_commit does not try again at the commit limit")
    print("PASS: a guest commit refused at the commit limit waits, then the out-of-memory stop")


if __name__ == "__main__":
    main()
