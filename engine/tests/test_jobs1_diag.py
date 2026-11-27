# Copyright (C) 2026 Sacrappy

"""JOBS1 (27 Nov 2026) diagnostics, off by default: HL_STALL_WATCH_STACK (host/hos_jobtrace.c: each suspended thread's stack walked,
named after it is resumed), RX_VK_LATE_FIX_WATCH (gpu/vk/vk_replay.cpp: a long wait for a late fix's value logged), HL_JOBS_WATCH
(native/src/bridge/JobGroup.cpp: a long module job group dumped). No game here: the wiring in the source.

    python -I -B tests/test_jobs1_diag.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)
    print("PASS:", what)


def read(p):
    return (ROOT / p).read_text(encoding="utf-8").replace("\r\n", "\n")


def body(src, head):
    i = src.index(head)
    return src[i:src.index("\n}\n", i)]


def main():
    rep = read("gpu/vk/vk_replay.cpp")
    jt = read("host/hos_jobtrace.c")
    jg = read("native/src/bridge/JobGroup.cpp")
    loop = read("native/src/bridge/DispatchLoop.h")
    check('getenv("HL_STALL_WATCH_STACK")' in jt, "HL_STALL_WATCH_STACK is read")
    walk = body(jt, "static int sw_walk(")
    check("RtlVirtualUnwind" in walk and "#ifdef _WIN32" in walk and "#else" in walk, "the walk unwinds on Windows only, a plain fallback elsewhere")
    for bad in ("sw_where(", "snprintf", "hos_log", "malloc", "GetModuleHandle"):
        check(bad not in walk, f"nothing named or allocated while the thread is suspended ({bad})")
    sus = jt[jt.index("SuspendThread(hs[i])"):jt.index("ResumeThread(hs[i])")]
    check("sw_where_stack" not in sus and "sw_walk(" in sus, "the frames are named after the thread is resumed")
    check('getenv("RX_VK_LATE_FIX_WATCH")' in rep and "late_fix_watch_note(\"a sender's wait\"" in rep and
          "late_fix_watch_note(\"the fix thread\"" in rep, "RX_VK_LATE_FIX_WATCH: both waits for a late fix")
    note = body(rep, "void late_fix_watch_note(const char* who, uint64_t lv, int64_t waited_ns) {")
    check("std::try_to_lock" in note and "vkWaitSemaphores" not in note, "the watch takes no lock it could wait on, and waits for nothing")
    check('std::getenv("HL_JOBS_WATCH")' in jg and "watch_enter(g_at);" in jg and "watch_leave(g_at);" in jg, "HL_JOBS_WATCH: a group's entry and end")
    check("requires { q.running(job); }" in loop, "the dispatch loop tells a policy that wants it which job a context runs")
    print("PASS")


if __name__ == "__main__":
    main()
