"""(TICK-061, C2) RX_VK_COLLECT_FAST: the write-mark scan with unaligned block loads takes the same pages as collect_in.

host/rx_backend.c's collect_in (the old walk: blocks only from aligned page numbers) and collect_in_fast are cut out of the
source and compiled with stubs (the mark array, the guest size); both run over copies of the same random marks and random
ranges (unaligned starts and ends, empty, single pages, past the guest size, overlapping), and must return the same pages in
the same order and leave the same marks (each taken page cleared, every other mark left).

python -I -B tests/test_collect_fast.py --cc <compiler>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    source = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    take = function(source, "static inline void collect_take(")
    fast = function(source, "static int64_t collect_in_fast(")
    old = function(source, "static int64_t collect_in(const uint64_t (*ranges)[2]")
    if "if (collect_fast_on()) return collect_in_fast(ranges, nranges, pages);" not in old:
        raise SystemExit("collect_in does not hand over to collect_in_fast under the switch")
    old = old.replace("if (collect_fast_on()) return collect_in_fast(ranges, nranges, pages);", "")
    fixture = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define A64_DIRTY_PAGES (1u << 16)
static uint8_t a64_dirty[A64_DIRTY_PAGES];
static uint64_t guest_size;
static uint64_t hos_guest_size(void) { return guest_size; }
static int collect_fast_on(void) { return 0; }
''' + take + "\n" + fast + "\n" + old + r'''
static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
int main(void) {
    static uint8_t marks[A64_DIRTY_PAGES], a[A64_DIRTY_PAGES];
    for (int round = 0; round < 400; round++) {
        guest_size = (uint64_t)A64_DIRTY_PAGES * 4096 - (next() % 3) * 4096 * (next() % 64);
        const int density = (int)(next() % 5);
        for (uint32_t i = 0; i < A64_DIRTY_PAGES; i++) {
            const uint64_t r = next();
            marks[i] = density == 0 ? 0 : (r % (density == 1 ? 4096 : density == 2 ? 97 : density == 3 ? 7 : 2)) == 0 ? (uint8_t)(1 + r % 3) : 0;
        }
        if (round % 7 == 0)   /* (runs of marks: whole blocks set) */
            for (int k = 0; k < 20; k++) { const uint32_t at = next() % A64_DIRTY_PAGES, n = next() % 600; for (uint32_t i = at; i < at + n && i < A64_DIRTY_PAGES; i++) marks[i] = 1; }
        uint64_t ranges[64][2];
        const int nr = (int)(next() % 64);
        for (int r = 0; r < nr; r++) {
            const uint64_t lo = next() % ((uint64_t)A64_DIRTY_PAGES * 4096 + 65536);
            const uint64_t len = next() % 5 == 0 ? next() % 8192 : next() % (64ull << 20);
            ranges[r][0] = lo, ranges[r][1] = lo + len;
        }
        memcpy(a64_dirty, marks, sizeof marks);
        uint32_t *p_old = NULL, *p_new = NULL;
        const int64_t n_old = collect_in(ranges, nr, &p_old);
        memcpy(a, a64_dirty, sizeof a);
        memcpy(a64_dirty, marks, sizeof marks);
        const int64_t n_new = collect_in_fast(ranges, nr, &p_new);
        if (n_old != n_new || memcmp(p_old, p_new, (size_t)n_old * sizeof *p_old) || memcmp(a, a64_dirty, sizeof a)) {
            fprintf(stderr, "FAIL: round %d: %lld pages against %lld (or other marks left)\n", round, (long long)n_old, (long long)n_new);
            return 1;
        }
        free(p_old), free(p_new);
    }
    puts("PASS: RX_VK_COLLECT_FAST takes the same pages in the same order and leaves the same marks");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="collect-fast-") as tmp:
        c = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "t.exe")
        Path(c).write_text(fixture, encoding="utf-8")
        subprocess.run([args.cc, "-x", "c", "-O2", "-o", exe, c], check=True)   # (C, also with a C++ driver)
        r = subprocess.run([exe], capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip())
        raise SystemExit(r.returncode)


if __name__ == "__main__":
    main()
