"""RX_VK_READBACK_LATE (gpu/vk/readback_late.h): the rules that name the CPU reads of GPU results read without their wait.

The header is compiled with a small program that parses rule lists and asks which ranges are late:
- no rule, "0" and an empty list name nothing; "newest" names the ranges whose writer is the newest submission handed out;
- a hex size names every range of exactly that size; "-<hex size>" never, whatever the other rules say.
And, by the text, that invalidate() asks the rules only for a range that is pending, after the write-back's value is known, and
that the wait is skipped for a late range only (an unnamed range goes through wait_recorded / wait_value as before).

python -I -B tests/test_readback_late.py --cxx <compiler>
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CPP = r'''
#include "readback_late.h"
#include <cstdio>
static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    using namespace readback_late;
    CHECK(!parse(nullptr).any());
    CHECK(!parse("").any());
    CHECK(!parse("0").any());
    const Rules n = parse("newest");
    CHECK(n.any() && wanted(n, 0x40, true) && !wanted(n, 0x40, false));
    const Rules s = parse("3980,2000");
    CHECK(wanted(s, 0x3980, false) && wanted(s, 0x2000, true) && !wanted(s, 0x40, true) && !wanted(s, 0x1040, false));
    const Rules m = parse("newest,-1040,-c0");
    CHECK(wanted(m, 0x40, true) && !wanted(m, 0x1040, true) && !wanted(m, 0xc0, true) && !wanted(m, 0x40, false));
    const Rules b = parse("2000,-2000");
    CHECK(!wanted(b, 0x2000, false) && !wanted(b, 0x2000, true));
    CHECK(!parse("-1040").any());
    const Rules meter = parse("1040,c0");   // RX_VK_READBACK_STRICT's default with RADV
    CHECK(!meter.newest && wanted(meter, 0x1040, false) && wanted(meter, 0xc0, false) && !wanted(meter, 0x1b580, false)
          && !wanted(meter, 0x2000, true));
    if (!fails) std::printf("PASS the rules\n");
    return fails;
}
'''


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
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        cpp, exe = Path(tmp) / "t.cpp", Path(tmp) / "t.exe"
        cpp.write_text(CPP)
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"), str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    rp = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    inv = function(rp, "void invalidate(uint64_t gpu, uint64_t size) {")
    assert 'getenv("RX_VK_READBACK_LATE")' in function(rp, "bool readback_late_wanted(uint64_t size, bool newest) {"), \
        "the switch is not read from RX_VK_READBACK_LATE"
    i_val = inv.index("memory_writeback_value(host->gpu_guest(gpu), size, nullptr);")
    i_late = inv.index("readback_late_wanted(size, v >= f0)")
    i_wait = inv.index("if (pend && !late) {")
    assert i_val < i_late < i_wait, "the rules are asked before the write-back's value is known, or after the wait"
    assert "const bool pend = v > completed();" in inv and "pend && readback_late_wanted" in inv, "a range that is not pending is asked"
    # RX_VK_READBACK_STRICT: 0 / 1 / the driver's default as before, any other value a list of the sizes that wait (the light
    # meter's: 1040,c0); the strict wait is asked by size, inside the pending range's wait
    strict = function(rp, "bool readback_strict_wanted(uint64_t size) {")
    assert 'getenv("RX_VK_READBACK_STRICT")' in strict and 'strcmp(e, "0") && strcmp(e, "1")' in strict, \
        "0 and 1 are not kept apart from a list of sizes"
    assert "readback_late::parse(e)" in strict and "r.sizes.newest = false" in strict, "the sizes are not parsed as sizes only"
    assert "VK_DRIVER_ID_MESA_RADV" in strict, "the default is no longer RADV's"
    # unset on RADV: the light meter's sizes, not all (the 0x1b580 ranges' wait cost ~15% for no picture); unset elsewhere: off
    i_default = strict.index("if (!e && radv) {")
    assert strict.index('r.sizes = readback_late::parse("1040,c0");', i_default) < strict.index("} else if (e && strcmp", i_default), \
        "RADV's default is not the light meter's sizes"
    assert "r.all = e && atoi(e) != 0;" in strict, "only RX_VK_READBACK_STRICT=1 makes every range wait"
    i_strict = inv.index("(readback_strict_wanted(size) || readback_strict_slow(size)) && v > completed()")
    assert i_wait < i_strict, "the strict wait is asked outside the pending range's wait"
    # while the presents run slow (a loaded machine) the light meter's sizes wait for their write-back on any driver: without it
    # the CPU reads the meter's stale or reset sums and the exposure pulses, a white flash of ~16 frames (stream W1)
    slow = function(rp, "bool readback_strict_slow(uint64_t size) {")
    assert 'getenv("RX_VK_STRICT_SLOW_MS")' in slow and "22.0" in slow, "the slow limit is not RX_VK_STRICT_SLOW_MS, 22 ms"
    assert 'readback_late::parse("1040,c0")' in slow and "readback_late::wanted(meter, size, false)" in slow,         "the slow wait is not for the light meter's sizes only"
    assert "present_ms_ema.load" in slow and "limit > 0" in slow, "the slow wait does not follow the presents' interval, or cannot be off"
    assert "present_ms_ema.store(" in rp[rp.index("presents_now = presents;"):][:900], "the presents' interval is not smoothed at the present"
    print("PASS the wiring")


if __name__ == "__main__":
    main()
