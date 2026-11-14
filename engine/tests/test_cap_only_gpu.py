"""RX_VK_CAP_ONLY_GPU: a pool made between two refreshes of the pool list is a pool, and out of the left-out ranges, from
the moment it is made (gpu/vk/vk_memory.cpp: cap_pool_born, called from memory_pool_flags at nvnMemoryPoolInitialize), and
the clip's list comes from the host's live pools (host/nvdrv.c hos_gpu_live_pools), which a pool made over an older one's
memory does not lose.

python -I -B tests/test_cap_only_gpu.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    merged = block(source, "std::vector<std::pair<uint64_t, uint64_t>> ranges_merged(")
    clip = block(source, "std::vector<std::pair<uint64_t, uint64_t>> ranges_clip(")
    hit = block(source, "bool ranges_hit(")
    born = block(source, "void cap_pool_born(")
    # the wiring: the pool made tells the clip before the call returns; the refresh reads the host's live pools
    if "cap_pool_born_now(gpu, size);" not in block(source, "void memory_pool_flags("):
        raise SystemExit("memory_pool_flags does not tell the clip of the pool made")
    now = block(source, "void cap_pool_born_now(")
    for want in ("cap_pool_born(all_pool_ranges, cap_excl", "cap_ranges_ver++", "born_stamp.push_back"):
        if want not in now:
            raise SystemExit(f"cap_pool_born_now does not use {want!r}")
    if "hos_gpu_live_pools" not in block(source, "void refresh_pool_ranges_locked("):
        raise SystemExit("the pool list's refresh does not read the host's live pools")
    union = block(source, "std::vector<std::pair<uint64_t, uint64_t>> capture_union_locked(")
    if "nvn_pool_record_ranges(gpu);" not in union or "ranges_clip(imp, gpu, true)" not in union:
        raise SystemExit("the clip does not keep every pool the back end has a record of")
    if "o->magic == NVN_MAGIC('P', 'O', 'O', 'L')" not in block((ROOT / "host" / "nvn_native.c").read_text(encoding="utf-8"),
                                                                "NV(nvnMemoryPoolFinalize)"):
        raise SystemExit("a pool finalized twice untracks its range twice")
    if "cap_born_take(born);" not in block(source, "void memory_collect_written("):
        raise SystemExit("the replay's collection does not stamp the pools made")
    host = (ROOT / "host" / "nvdrv.c").read_text(encoding="utf-8")
    if "tracked[i].lo" not in block(host, "__declspec(dllexport) int hos_gpu_live_pools("):
        raise SystemExit("hos_gpu_live_pools does not list the counted pools")
    fixture = f'''#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{merged}
{clip}
{hit}
{born}
int main() {{
    // an import over [0x10000, 0x90000) with one pool [0x20000, 0x30000): the rest of it left out
    std::vector<std::pair<uint64_t, uint64_t>> pools = {{{{0x20000, 0x30000}}}};
    std::vector<std::pair<uint64_t, uint64_t>> excl = ranges_clip({{{{0x10000, 0x90000}}}}, pools, false);
    CHECK(excl.size() == 2 && excl[0].first == 0x10000 && excl[0].second == 0x20000);
    CHECK(ranges_hit(excl, 0x52000, 0x53ab0));   // (heap memory between pools: left out)
    // a pool made there between two refreshes: a pool, and no binding of it reaches the left-out ranges, at once
    cap_pool_born(pools, excl, 0x50000, 0x54001);
    CHECK(!ranges_hit(excl, 0x52000, 0x53ab0));
    CHECK(!ranges_hit(excl, 0x50000, 0x55000));   // (to its page end)
    CHECK(ranges_hit(excl, 0x4f000, 0x50000) && ranges_hit(excl, 0x55000, 0x56000));   // (its neighbours still out)
    CHECK(pools.size() == 2 && pools[1].first == 0x50000 && pools[1].second == 0x55000);
    // made over the first pool's memory again (re-created at the same address): still a pool
    cap_pool_born(pools, excl, 0x20000, 0x30000);
    CHECK(pools.size() == 2 && !ranges_hit(excl, 0x20000, 0x30000));
    std::printf("PASS: a pool made between two refreshes is never left out\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="cap-only-gpu-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
