# Copyright (C) 2026 Sacrappy

"""RX_VK_BC_DISK_SYNC (JOBS1, 27 Nov 2026): a large texture whose BC blocks the disk cache has, but has not read ahead into its pool, is
read from the disk at once (gpu/vk/vk_bc_disk.h read_now) instead of transcoded on the GPU. On the laptop (Vega 8) the terrain's
1365 x 1365 x 90 arrays, transcoded 3-7 times as play began, froze the game 8-20 s or lost the device (5 of 10 starts).

The rule (gpu/vk/bc_disk_sync.h) is compiled with a small program: on unless =0, 16 MB and up (RX_VK_BC_DISK_SYNC_MB). And, by the
text, that the read is asked only after the pool missed and before the old path (probe, transcode), that a failed read or a key the
disk does not have falls back to it, and that the read checks the record's hash and its pack.

python -I -B tests/test_bc_disk_sync.py --cxx <compiler>
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

CPP = r'''
#include "bc_disk_sync.h"
#include <cstdio>
static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    using namespace bc_disk_sync;
    const uint64_t MB = 1ull << 20;
    CHECK(min_bytes(nullptr) == 16 * MB && min_bytes("") == 16 * MB && min_bytes("0") == 16 * MB && min_bytes("-4") == 16 * MB);
    CHECK(min_bytes("64") == 64 * MB && min_bytes("1") == MB);
    CHECK(wanted(nullptr, 16 * MB, 16 * MB) && wanted("", 107 * MB, 16 * MB) && wanted("1", 16 * MB, 16 * MB));   // (on by default)
    CHECK(!wanted(nullptr, 16 * MB - 1, 16 * MB));   // (a smaller texture: the old path)
    CHECK(!wanted("0", 214 * MB, 16 * MB));          // (=0: off)
    if (!fails) std::printf("PASS the rule\n");
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
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    bcd = (ROOT / "gpu/vk/vk_bc_disk.h").read_text(encoding="utf-8").replace("\r\n", "\n")
    sw = function(tex, "bool bcd_sync_wanted(uint64_t size) {")
    assert 'getenv("RX_VK_BC_DISK_SYNC")' in sw and 'getenv("RX_VK_BC_DISK_SYNC_MB")' in sw and "bc_disk_sync::wanted(" in sw, \
        "the switches or the rule are not used"
    i_take = tex.index("hit = bcd_take(disk_key, size);")
    i_sync = tex.index("if (!hit.buf && bcd_sync_wanted(size)) hit = bcd_read_now(disk_key, size);")
    i_probe = tex.index("bcdisk::probe(disk_key, (uint32_t)size);", i_take)
    assert i_take < i_sync < i_probe, "the read is not asked only after the pool missed and before the old path"
    rd = function(tex, "BcCacheEntry bcd_read_now(uint64_t key, uint64_t size) {")
    assert "destroy_buffer(e.buf, e.alloc), ok = false" in rd and "return {};" in rd, "a failed read does not fall back to the transcode"
    rn = function(bcd, "inline bool read_now(uint64_t key, uint32_t size, uint8_t* out) {")
    assert "if (!p || p->size != size) return false;" in rn, "a key the disk does not have is not refused"
    assert "texcache::hash_blocks(out, l.size) != l.hash" in rn and "s.ix.seq[l.gen] == seq" in rn, "the read is not checked"
    assert "std::ifstream f(" in rn and "s.rd[" not in rn, "the read uses the disk thread's file handles"
    print("PASS the wiring")


if __name__ == "__main__":
    main()
