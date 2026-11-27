# Copyright (C) 2026 Sacrappy
"""The game is told the memory it uses on every PC, and a freed region's pages leave that count (host/hos_mem.c, DEGRADE1).

With the used memory report off from 32 GB of RAM (the rule of 15 Nov), the game read ~9.6 GB free whatever it held and its
mapped pages grew from 360 MB to 7168 MB in 15 minutes on a 32 GB PC; once the system's commit ran short the Market fell from
80-87 fps to 10 (issues 30, 121). The default is now on for every PC (HOS_MEMINFO_OLD=1 off, HOS_MEMINFO_RAM_RULE=1 the old
rule). And a region the game frees without freeing its pages first takes those pages out of the count, as Longshot's
da1c3f524 did (HOS_REGION_FREE_KEEPS_COUNT=1: kept).

meminfo_old_by_ram, meminfo_old_default, hos_used_memory, pages_count_locked, pages_report_locked and
region_count_out_locked are cut into a fixture with hos_log as a spy.

    python -I -B tests/test_meminfo_report.py [--cc gcc]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("marker not found once: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#define HOS_GUEST_SPACE (1ULL << 39)
#define LOCK()
#define UNLOCK()
static uint64_t hos_heap_size = 6894ULL << 20, hos_total_memory = 9809ULL << 20;
static int logs;
static void hos_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("log: "), vprintf(fmt, a), printf("\n");
    va_end(a);
    logs++;
}
static int env_on(const char *name) {
    const char *e = getenv(name);
    return e && e[0] && e[0] != '0';
}
static uint64_t mapped_bytes, region_unmapped_bytes, mapped_peak, mapped_logged;
@@DEFINES@@
@@CODE@@
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(int argc, char **argv) {
    (void)argc;
    if (!strcmp(argv[1], "rule")) {
        CHECK(meminfo_old_by_ram(31861ULL << 20) == 1);   /* (a "32 GB" PC: off by the old rule) */
        CHECK(meminfo_old_by_ram(16000ULL << 20) == 0);
        printf("rule ok\n");
        return 0;
    }
    if (!strcmp(argv[1], "default")) {   /* (no switch: on, whatever the RAM) */
        CHECK(meminfo_old_default() == 0);
        printf("default ok\n");
        return 0;
    }
    if (!strcmp(argv[1], "old")) {   /* (HOS_MEMINFO_OLD=1 in the environment) */
        CHECK(meminfo_old_default() == 1);
        printf("old ok\n");
        return 0;
    }
    if (!strcmp(argv[1], "count")) {
        const uint64_t base = 0x1100000000ULL, mb = 1ULL << 20;
        mapped_bytes += (uint64_t)pages_count_locked(base, 64 * mb, 1);
        mapped_bytes += (uint64_t)pages_count_locked(base + 32 * mb, 64 * mb, 1);   /* (overlap counts once) */
        CHECK(mapped_bytes == 96 * mb);
        CHECK(hos_used_memory(32 * mb) == 32 * mb + hos_heap_size + 96 * mb);
        mapped_bytes -= (uint64_t)pages_count_locked(base, 16 * mb, 0);   /* (FreeMemoryPages of a part) */
        CHECK(mapped_bytes == 80 * mb);
        /* (the region freed with its pages still mapped: counted out, once) */
        CHECK(region_count_out_locked(base, 512 * mb) == 80 * mb);
        CHECK(mapped_bytes == 0 && region_unmapped_bytes == 80 * mb);
        CHECK(region_count_out_locked(base, 512 * mb) == 0);
        /* (never past the console's total) */
        mapped_bytes += (uint64_t)pages_count_locked(base, 4096 * mb, 1);
        CHECK(hos_used_memory(32 * mb) == hos_total_memory);
        printf("count ok\n");
        return 0;
    }
    return 2;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    src = (ROOT / "host" / "hos_mem.c").read_text(encoding="utf-8")
    free = block(src, "void hos_region_free(uint64_t addr) {")
    if "if (!region_free_keeps_count()) region_count_out_locked(regions[i].addr, regions[i].size);" not in free:
        raise SystemExit("FAIL: hos_region_free does not count a freed region's pages out")
    defines = "\n".join(line for line in src.splitlines() if line.startswith("#define PAGES_LEAF_"))
    decl = next(line for line in src.splitlines() if line.startswith("static uint64_t *mapped_bits["))
    code = "\n".join((decl,
                      block(src, "static int64_t pages_count_locked(uint64_t a, uint64_t n, int map) {"),
                      block(src, "static void pages_report_locked(void) {"),
                      block(src, "static uint64_t region_count_out_locked(uint64_t addr, uint64_t size) {"),
                      block(src, "static int meminfo_old_by_ram(uint64_t ram) {"),
                      block(src, "static int meminfo_old_default(void) {"),
                      block(src, "uint64_t hos_used_memory(uint64_t base) {")))
    with tempfile.TemporaryDirectory(prefix="meminfo-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        c.write_text(FIXTURE.replace("@@DEFINES@@", defines).replace("@@CODE@@", code), encoding="utf-8")
        r = subprocess.run([args.cc, "-std=gnu11", "-O1", str(c), "-o", str(exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        import os
        for arm, env in (("rule", {}), ("default", {}), ("old", {"HOS_MEMINFO_OLD": "1"}), ("count", {})):
            e = {k: v for k, v in os.environ.items() if not k.startswith("HOS_MEMINFO") and k != "HOS_REGION_FREE_KEEPS_COUNT"}
            e.update(env)
            r = subprocess.run([str(exe), arm], capture_output=True, text=True, timeout=30, env=e)
            print(r.stdout, end="")
            if r.returncode or (arm + " ok") not in r.stdout:
                raise SystemExit("FAIL: " + arm)
    print("PASS: the used memory report is on for every PC, HOS_MEMINFO_OLD=1 turns it off, a freed region's pages leave the count")


if __name__ == "__main__":
    main()
