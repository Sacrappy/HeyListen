"""A thread's priority lowered right after CreateThread is applied on Linux (host/posix/win32_posix.c).

hos_thread_pin lowers OptimizeChangeBank (THREAD_PRIORITY_LOWEST) from the thread that creates it, usually before the new
thread has run and set its Linux thread id. SetThreadPriority skipped the nice value then, so on Linux the thread always
ran at normal priority and took the frame's cores (Kokiri village). Builds win32_posix.c as it is with a small program
that creates a waiting thread, lowers it at once, and has the thread read its own nice value; 200 rounds, all must be 10.

python -I -B tests/test_thread_priority_posix.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r"""
#include <windows.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>
static volatile int go;
static DWORD WINAPI body(void *p) { (void)p; while (!go) usleep(100); return (DWORD)getpriority(PRIO_PROCESS, 0); }
int main(void) {
    int bad = 0;
    for (int i = 0; i < 200; i++) {
        go = 0;
        HANDLE h = CreateThread(NULL, 0, body, NULL, 0, NULL);
        SetThreadPriority(h, THREAD_PRIORITY_LOWEST);
        go = 1;
        WaitForSingleObject(h, INFINITE);
        DWORD code = 0;
        GetExitCodeThread(h, &code);
        if ((int)code != 10) bad++;
        CloseHandle(h);
    }
    printf("%d of 200 threads not lowered\n", bad);
    return bad != 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the posix layer's checks run on Linux")
        return
    if os.getpriority(os.PRIO_PROCESS, 0) >= 10:
        print("SKIP: this process is already at nice 10 or lower priority; the lowering cannot be seen")
        return
    with tempfile.TemporaryDirectory(prefix="thread-prio-") as tmp:
        src, exe = Path(tmp) / "prio.c", Path(tmp) / "prio"
        src.write_text(PROGRAM, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-O1", "-I", str(ROOT / "host/posix"), "-I", str(ROOT / "host"), str(src),
                        str(ROOT / "host/posix/win32_posix.c"), "-o", str(exe), "-lpthread", "-ldl", "-lm"], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        print(r.stdout.strip())
        if r.returncode:
            raise SystemExit("FAIL: SetThreadPriority right after CreateThread did not lower the thread")
    print("ok")


if __name__ == "__main__":
    main()
