"""Check which guest pages a live or retiring GPU import still holds (memory_guest_imported: the host decommits the pages it says
nobody imports). Ported from Longshot 839a8cc4 (#468) and fa8ae6a8 (#469).

python -I -B tests/test_pending_guest_import.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    start = source.index("int memory_guest_imported(")
    function = source[start:source.index("\n}", start) + 2]
    fixture = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#include "gpu/vk/import_reach.h"

struct Pool { uint64_t lo, hi, memory; };
std::map<uint64_t, Pool> imported;
import_reach::Reach live_reach;
std::deque<decltype(imported)::node_type> retired_pools, evicted_pools;
std::atomic<int64_t> st_retired_walk_ns{0};
std::mutex pools_mu, bg_mu;
std::map<uint64_t, uint64_t> bg_inflight;
std::vector<std::pair<uint64_t, Pool>> bg_ready;
struct DeadImport { uint64_t value, bytes; uint64_t lo, hi; };
std::deque<DeadImport> dead_imports;
""" + function + r"""

int main() {
    auto check = [] {
        assert(!memory_guest_imported(0x1000, 0x2000));
        assert(!memory_guest_imported(0x5000, 0x6000));
        assert(!memory_guest_imported(0x6000, 0x7000));
        assert(memory_guest_imported(0x2000, 0x3000));
        assert(memory_guest_imported(0x4000, 0x6000));
        assert(memory_guest_imported(0x1000, 0x6000));
    };
    dead_imports.push_back({1, 0x3000, 0x2000, 0});
    check();
    dead_imports.clear();
    bg_inflight.emplace(0x2000, 0x5000);
    check();
    bg_inflight.clear();
    bg_ready.emplace_back(0x2000, Pool{0x2000, 0x5000, 1});
    check();
    bg_ready.clear();
    {   // (an import given back, its memory still the GPU's until it is freed: in evicted_pools)
        std::map<uint64_t, Pool> table;
        table.emplace(0x2000, Pool{0x2000, 0x5000, 1});
        evicted_pools.push_back(table.extract(0x2000));
        check();
        evicted_pools.clear();
    }
    {   // (a live one, and the end of its range: a range that ends where it starts, or starts where it ends, is not over it)
        imported.emplace(0x2000, Pool{0x2000, 0x5000, 1});
        live_reach.add(0x3000);
        check();
        imported.clear();
    }
    assert(!memory_guest_imported(0x3000, 0x3000));   // (an empty range)
    assert(!memory_guest_imported(0x2000, 0x5000));
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / "check.cpp"
        exe = Path(tmp) / "check.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-I" + str(ROOT), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], cwd=tmp, check=True)


if __name__ == "__main__":
    main()
