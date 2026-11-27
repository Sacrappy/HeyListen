# Copyright (C) 2026 Sacrappy
"""What a posted crash report holds to place a crash without reproducing it (OLD1, 27 Nov 2026), and how each part is cleaned.

The crash handler (host/mkw_glue.c) logs, after the build's line: a diagnosis in words, the time played (in all and since the
map changed) with the map, the RAM and commit charge with the process's private bytes and working set, and the game
allocator's last null. The helper (host/hos_crash_dialog.c build_report) sends every CRASH line, a register's value that looks
like text as <text>, and from the log before the crash: the last 3 slow frames and stalls, the last Vulkan error and the
switches hookshot_env.txt set (a value only when it is a number or on/off). The payload is kept under 48,000 bytes.
hos_crash_dialog.h's helpers are compiled and run here; the rest is checked in the sources (the helper's real runs with a
stand-in server are tests/test_crash_dialog.py's, on Linux).

python -I -B tests/test_crash_report_fields.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

MAIN = r"""
#include "hos_crash_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
    /* text: "hell" (4 printable in a row), "abc" alone (3 non-zero, all printable); not text: pointers, small numbers, 0 */
    CHECK(hos_crash_text_like(0x6c6c6568ull));
    CHECK(hos_crash_text_like(0x636261ull));
    CHECK(!hos_crash_text_like(0x7ff6a0001000ull) && !hos_crash_text_like(0x10) && !hos_crash_text_like(0));
    CHECK(!hos_crash_text_like(0x0000000180001234ull));
    char line[256] = "CRASH: guest regs: x0=2f686f6d652f61 x1=10 x2=7ff6a0001000 sp0=6c6c6568";
    hos_crash_mask_registers(line);
    CHECK(!strcmp(line, "CRASH: guest regs: x0=<text> x1=10 x2=7ff6a0001000 sp0=<text>"));
    char host[256] = "CRASH: host regs: rax=2e65766173 rbx=0 rip=7ff6a0001234";
    hos_crash_mask_registers(host);
    CHECK(!strcmp(host, "CRASH: host regs: rax=<text> rbx=0 rip=7ff6a0001234"));
    char other[128] = "CRASH: thread Main, sp 0x6c6c6568";   /* (only the register lines are masked) */
    hos_crash_mask_registers(other);
    CHECK(!strcmp(other, "CRASH: thread Main, sp 0x6c6c6568"));
    CHECK(hos_crash_extra_kind("[hos P] vk: slow frame at present 9: 80.0 ms") == HOS_CRASH_EXTRA_SLOW);
    CHECK(hos_crash_extra_kind("[hos s] stallwatch: gap 312.0 ms between presents") == HOS_CRASH_EXTRA_SLOW);
    CHECK(hos_crash_extra_kind("[hos P] vk: queue submit failed (VK_ERROR_DEVICE_LOST)") == HOS_CRASH_EXTRA_VULKAN);
    CHECK(hos_crash_extra_kind("[hos P] vk: a compute pipeline failed (-3) with the driver's optimizer") == HOS_CRASH_EXTRA_VULKAN);
    CHECK(hos_crash_extra_kind("[hos h] env: hookshot_env.txt sets RX_VK_FOG=0") == HOS_CRASH_EXTRA_SWITCH);
    CHECK(hos_crash_extra_kind("[hos M] map: Kokiri Forest (child)") == 0);
    char sw[128];
    CHECK(hos_crash_switch_line(sw, sizeof sw, "[hos h] env: hookshot_env.txt sets RX_VK_FOG=0") && !strcmp(sw, "RX_VK_FOG=0"));
    CHECK(hos_crash_switch_line(sw, sizeof sw, "[hos h] env: hookshot_env.txt sets HL_ROOM_TOKEN=abc over the environment's x") &&
          !strcmp(sw, "HL_ROOM_TOKEN=(value left out)"));
    CHECK(hos_crash_switch_line(sw, sizeof sw, "[hos h] env: hookshot_env.txt sets RX_VK_NAME=C:Users") &&
          !strcmp(sw, "RX_VK_NAME=(value left out)"));
    CHECK(!hos_crash_switch_line(sw, sizeof sw, "[hos h] env: hookshot_env.txt: X left out"));
    CHECK(HOS_CRASH_PAYLOAD_BUDGET == 48000 && HOS_CRASH_TAIL_KEEP == 100);
    puts("PASS: register values that look like text masked; slow frames, Vulkan errors and hookshot_env.txt switches told apart");
    return 0;
}
"""


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    glue = (ROOT / "host" / "mkw_glue.c").read_text(encoding="utf-8")
    dialog = (ROOT / "host" / "hos_crash_dialog.c").read_text(encoding="utf-8")
    header = (ROOT / "host" / "hos_crash_dialog.h").read_text(encoding="utf-8")
    # the crash handler's new lines, right after the build's line, all numbers or the game's own names
    for line in ('"CRASH: diagnosis: a guest %s through a null pointer', '"CRASH: play: %.1f s since play began',
                 '"CRASH: memory: RAM %llu MB, %llu MB free; commit %llu of %llu MB', '"CRASH: last failed allocation: %s"'):
        need(line in glue, "the crash handler logs " + line)
    report = glue[glue.index("static LONG WINAPI report_crash("):]
    need(report.index('CRASH: build %s, lifted code %s') < report.index("crash_context_lines(") < report.index("crash_backtrace("),
         "the context lines come right after the build's line")
    need("hos_crash_times(" in (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8"), "hos_main.c keeps the play and map times")
    need("hl_alloc_null_last(void)" in (ROOT / "host" / "hl_alloc_null.c").read_text(encoding="utf-8"), "the allocator's last null kept")
    # none of the new lines holds " at ", which the report's cleaning reads as a place
    for line in ("CRASH: diagnosis: a guest", "CRASH: play: ", "CRASH: memory: RAM", "CRASH: last failed allocation"):
        seg = glue[glue.index(line):glue.index('"', glue.index(line))]
        need(" at " not in seg, line + " has no ' at '")
    # the helper: masks the crash lines, sends slow / vulkan_error / switches through put_field (the cleaning), keeps the budget
    build = dialog[dialog.index("static int build_report("):]
    build = build[:build.index("\n}\n")]
    need("hos_crash_mask_registers(one);" in dialog, "the crash lines are masked")
    for key in ('\\"slow\\": [', '"vulkan_error", ex.vulkan', '\\"switches\\": [', "HOS_CRASH_PAYLOAD_BUDGET", "HOS_CRASH_TAIL_KEEP"):
        need(key in build, "build_report holds " + key)
    need("put(json, ex" not in build, "every extra goes through put_field, which cleans it")
    need("never saves, game files or memory." in header, "the public text says what is never sent")
    with tempfile.TemporaryDirectory(prefix="crash-fields-") as tmp:
        src = Path(tmp) / "t.c"
        src.write_text(MAIN, encoding="utf-8")
        exe = Path(tmp) / "t.exe"
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(ROOT / "host"),
                        "-o", str(exe), str(src)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
