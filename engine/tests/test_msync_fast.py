"""(TICK-056) RX_VK_MSYNC_FAST: the mirror sync's faster page walk finds the same pages and leaves the same write-back sets.

Two helpers of gpu/vk/vk_memory.cpp are cut out of the source and compiled with stubs:
- next_dirty_page (the next page written after a pool's sync, regions skipped whole) against a page-by-page walk with page_gen,
  over random generation tables (pages, whole-region stamps, missing chunks, ranges starting anywhere in a region);
- lazy_cpu_pages (a sync's dirty pages noted at once, with a bitmap of the pages already in cpu_pages) against lazy_cpu_page
  called page by page, over random game pools (eager or not, gaps between them) and calls interleaved with lazy_filter's rule
  (skipped_pages takes only pages cpu_pages does not hold): cpu_pages, skipped_pages and the counter end up the same.
And, by the text: mirror_sync calls them only with the switch, and the batch after the page loop.

python -I -B tests/test_msync_fast.py --cxx <compiler>
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

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
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    load = function(source, "inline uint32_t load_u32(")
    page_gen = function(source, "inline uint32_t page_gen(")
    next_dirty = function(source, "uint64_t next_dirty_page(")
    pool_at = function(source, "NvnPool* nvn_pool_at(")
    one = function(source, "void lazy_cpu_page(uint64_t page)")
    many = function(source, "void lazy_cpu_pages(")
    bits = re.search(r"std::vector<std::unique_ptr<uint64_t\[\]>> cpu_page_bits;", source)
    if not bits:
        raise SystemExit("FAIL: cpu_page_bits is not declared as expected")
    sync = function(source, "int64_t mirror_sync(Pool& p)")
    for want in ("const bool fast = msync_fast_on();", "page = next_dirty_page(page, page_end, p.synced_gen);",
                 "if (fast) lazy_pages.push_back((uint32_t)page);", "else lazy_cpu_page(page);",
                 "if (fast) lazy_cpu_pages(lazy_pages);"):
        if want not in sync:
            raise SystemExit("FAIL: mirror_sync no longer has: " + want)
    if sync.index("if (fast) lazy_cpu_pages(lazy_pages);") < sync.index("if (run_hi) runs.push_back"):
        raise SystemExit("FAIL: the write-back notes are batched before the page loop ends")
    if 'getenv("RX_VK_MSYNC_FAST")' not in function(source, "bool msync_fast_on()"):
        raise SystemExit("FAIL: msync_fast_on does not read RX_VK_MSYNC_FAST")

    fixture = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_set>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
static void logf(const char*, ...) {}
constexpr uint64_t kPageShift = 12, kChunkPages = 1u << 16, kRegionPages = 512;
constexpr uint64_t kPages = 1ull << 22;   // (the test's guest: 16 GB, 64 chunks)
std::vector<std::unique_ptr<uint32_t[]>> gen_chunks(kPages / kChunkPages);
std::vector<uint32_t> region_gen(kPages / kRegionPages);
std::vector<uint32_t> region_full(kPages / kRegionPages);
''' + load + "\n" + page_gen + "\n" + next_dirty + r'''
struct NvnPool { uint64_t hi = 0; uint32_t flags = 0; uint64_t first_wb = 0; bool eager = false, skipped = false; uint8_t why = 0; };
std::mutex nvn_pools_mu;
std::map<uint64_t, NvnPool> nvn_pools;
std::unordered_set<uint32_t> cpu_pages, skipped_pages;
int64_t st_lazy_cpu = 0;
''' + pool_at + "\n" + one + "\n" + bits.group(0) + "\n" + many + r'''
static void stamp(uint64_t p, uint32_t g) {
    auto& c = gen_chunks[p / kChunkPages];
    if (!c) { c.reset(new uint32_t[kChunkPages]); std::fill(c.get(), c.get() + kChunkPages, 0u); }
    c[p % kChunkPages] = g;
    region_gen[p / kRegionPages] = std::max(region_gen[p / kRegionPages], g);
}
int main() {
    std::mt19937_64 rng(56);
    // next_dirty_page against a page-by-page walk
    for (int round = 0; round < 40; round++) {
        for (auto& c : gen_chunks) c.reset();
        std::fill(region_gen.begin(), region_gen.end(), 0u);
        std::fill(region_full.begin(), region_full.end(), 0u);
        uint32_t g = 1;
        for (int i = 0; i < 3000; i++) {
            const uint64_t p = rng() % kPages;
            const int n = (int)(rng() % 40) + 1;
            ++g;
            for (int k = 0; k < n && p + k < kPages; k++) stamp(p + k, g);
        }
        for (int i = 0; i < 20; i++) {   // (whole regions: region_full, the chunk not always made)
            const uint64_t r = rng() % (kPages / kRegionPages);
            ++g;
            region_full[r] = g, region_gen[r] = g;
        }
        for (int q = 0; q < 200; q++) {
            const uint64_t lo = rng() % kPages, hi = std::min(kPages, lo + 1 + rng() % 40000);
            const uint32_t synced = (uint32_t)(rng() % (g + 1));
            std::vector<uint64_t> want, got;
            for (uint64_t p = lo; p < hi; p++) if (page_gen(p) > synced) want.push_back(p);
            for (uint64_t p = lo; p < hi;) {
                p = next_dirty_page(p, hi, synced);
                if (p >= hi) break;
                got.push_back(p++);
            }
            CHECK(got == want);
        }
    }
    // lazy_cpu_pages against lazy_cpu_page, many calls in a row with lazy_filter's inserts between them
    for (int round = 0; round < 30; round++) {
        nvn_pools.clear();
        uint64_t at = (rng() % 64) << kPageShift;
        for (int i = 0; i < 30; i++) {
            NvnPool np;
            const uint64_t lo = at + ((rng() % 8) << kPageShift);
            np.hi = lo + ((1 + rng() % 300) << kPageShift);
            np.eager = rng() % 4 == 0;
            nvn_pools[lo] = np;
            at = np.hi;
        }
        const uint64_t top = at >> kPageShift;
        std::unordered_set<uint32_t> cpu_a, skip_a, cpu_b, skip_b;
        int64_t n_a = 0, n_b = 0;
        cpu_page_bits.clear();
        for (int call = 0; call < 60; call++) {
            for (int k = 0; k < 50; k++) {   // (lazy_filter: pages cpu_pages does not hold)
                const uint32_t p = (uint32_t)(rng() % top);
                if (!cpu_a.count(p)) skip_a.insert(p);
                if (!cpu_b.count(p)) skip_b.insert(p);
            }
            std::vector<uint32_t> pages;   // (a sync's dirty pages: in address order)
            for (uint64_t p = rng() % top; p < top && pages.size() < 400; p += 1 + rng() % 6) pages.push_back((uint32_t)p);
            cpu_pages.swap(cpu_a), skipped_pages.swap(skip_a), st_lazy_cpu = n_a;
            for (uint32_t p : pages) lazy_cpu_page(p);
            cpu_pages.swap(cpu_a), skipped_pages.swap(skip_a), n_a = st_lazy_cpu;
            cpu_pages.swap(cpu_b), skipped_pages.swap(skip_b), st_lazy_cpu = n_b;
            lazy_cpu_pages(pages);
            cpu_pages.swap(cpu_b), skipped_pages.swap(skip_b), n_b = st_lazy_cpu;
            CHECK(cpu_a == cpu_b);
            CHECK(skip_a == skip_b);
            CHECK(n_a == n_b);
        }
    }
    std::printf("PASS next_dirty_page finds the page walk's pages; lazy_cpu_pages leaves lazy_cpu_page's sets\n");
}
'''
    work = ROOT / "work"
    work.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="msync-fast-", dir=work))
    cpp, exe = out / "fixture.cpp", out / "fixture.exe"
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wno-unused-function", "-Werror", str(cpp), "-o", str(exe)],
                   check=True)
    env = dict(os.environ)
    compiler = Path(args.cxx)
    if compiler.is_file():
        env["PATH"] = str(compiler.resolve().parent) + os.pathsep + env.get("PATH", "")
    subprocess.run([str(exe)], check=True, env=env)
    print("Retained:", out)


if __name__ == "__main__":
    main()
