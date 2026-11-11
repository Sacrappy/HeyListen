"""(TICK-061, C3) RX_VK_SHADOW_FAST: take_capture's texture-shadow block by shadow_take_fast leaves the same shadow.

gpu/vk/vk_memory.cpp's shadow (the page map, the regions' counts, shadow_n, the spans) and both forms of the block are cut out
of the source and compiled with stubs: the old block (the text in take_capture after "else if (!tex_merged.empty())") and
shadow_take_fast (taken when it says so, else the old block, as take_capture does). The same random sequence of captures
(sorted page lists, some not sorted, copies that failed, pages in and out of the texture spans, spans changing, ranges dropped
from the shadow between captures, the cap made small so that its sweep and its stop are reached) runs through each, and the
final shadow (every page's bytes and seq, every region's count, shadow_n) must be the same; it is compared after every capture.

python -I -B tests/test_shadow_fast.py --cxx <compiler>
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
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    decl = source[source.index("struct ShadowPage {"):source.index("bool in_tex_merged(uint64_t guest) {")]
    if "constexpr size_t kShadowMax = 65536;" not in decl:
        raise SystemExit("the shadow's cap is not as this check knows it")
    decl = decl.replace("constexpr size_t kShadowMax = 65536;", "constexpr size_t kShadowMax = 7000;")   # (the cap reached)
    helpers = "\n".join(function(source, m) for m in ("bool in_tex_merged(uint64_t guest) {", "void shadow_erase(uint32_t page) {",
                                                       "void shadow_drop_range(uint64_t lo, uint64_t hi) {"))
    fast = source[source.index("std::vector<std::unique_ptr<uint8_t[]>> shadow_slab;"):]
    fast = fast[:fast.index("bool shadow_take_fast(")] + function(fast, "bool shadow_take_fast(")
    take = function(source, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    marker = "} else if (!tex_merged.empty()) {"
    if "shadow_take_fast(c->pages.data(), c->pages.size(), c->copied.data(), c->slots.data(), c->copied.size()," not in take:
        raise SystemExit("take_capture does not call shadow_take_fast")
    at = take.index(marker) + len(marker) - 1
    old_body = function(take[at:], "{")
    fixture = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
struct Cap { std::vector<uint32_t> pages, copied, slots; uint8_t* map = nullptr; };
''' + decl + helpers + "\n" + fast + r'''
void old_block(const Cap* c) ''' + old_body + r'''
static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint64_t nx() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t state_hash() {
    std::vector<uint32_t> keys;
    for (const auto& [k, v] : shadow) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) { h = (h ^ x) * 1099511628211ull; };
    for (uint32_t k : keys) {
        const ShadowPage& s = shadow.at(k);
        mix(k), mix(s.seq);
        for (int i = 0; i < 4096; i += 64) mix(s.b[i]);
    }
    std::vector<std::pair<uint32_t, uint32_t>> regions(shadow_regions.begin(), shadow_regions.end());
    std::sort(regions.begin(), regions.end());
    for (auto [r, n] : regions) mix(r), mix(n);
    mix(shadow_n.load());
    // (the counts agree with the entries: each region's count is its entries, shadow_n all of them)
    std::map<uint32_t, uint32_t> want;
    for (uint32_t k : keys) want[k >> 9]++;
    if (want.size() != shadow_regions.size() || shadow_n.load() != keys.size()) { std::fprintf(stderr, "FAIL: counts\n"); std::exit(1); }
    for (auto [r, n] : want) if (shadow_regions.at(r) != n) { std::fprintf(stderr, "FAIL: region count\n"); std::exit(1); }
    return h;
}
int main(int argc, char** argv) {
    const bool fast_on = argc > 1;
    std::vector<uint8_t> map;
    int fast_taken = 0;
    for (int round = 0; round < 700; round++) {
        if (round % 50 == 0) {   // (the spans: sorted, disjoint)
            tex_merged.clear();
            uint64_t at = 0;
            for (int k = 0; k < 12; k++) {
                at += (nx() % 400) << 12;
                const uint64_t len = (1 + nx() % 600) << 12;
                tex_merged.push_back({at, at + len});
                at += len;
            }
        }
        if (round % 13 == 5) {   // (a write the capture did not take: memory_drop_shadow)
            const uint64_t lo = (nx() % 12000) << 12;
            shadow_drop_range(lo, lo + ((nx() % 2000) << 12));
        }
        Cap c;
        uint32_t p = (uint32_t)(nx() % 50);
        const int want = (int)(nx() % 1800);
        while ((int)c.pages.size() < want && p < 12000) c.pages.push_back(p), p += 1 + (nx() % 3 == 0 ? (uint32_t)(nx() % 30) : 0);
        if (round % 9 == 4) std::reverse(c.pages.begin() + c.pages.size() / 2, c.pages.end());   // (not sorted: the old block)
        map.resize(c.pages.size() * 4096 + 1);
        for (size_t i = 0; i < map.size(); i += 64) map[i] = (uint8_t)nx();
        c.map = map.data();
        for (size_t i = 0; i < c.pages.size(); i++)
            if (nx() % 40) c.copied.push_back(c.pages[i]), c.slots.push_back((uint32_t)i);   // (some copies failed)
        shadow_seq++;
        if (!c.pages.empty()) {
            if (fast_on && !tex_merged.empty() &&
                shadow_take_fast(c.pages.data(), c.pages.size(), c.copied.data(), c.slots.data(), c.copied.size(), c.map))
                fast_taken++;
            else if (!tex_merged.empty())
                old_block(&c);
        }
        std::printf("%016llx\n", (unsigned long long)state_hash());
    }
    std::fprintf(stderr, "%d of 700 by the fast path\n", fast_taken);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="shadow-fast-") as tmp:
        cpp = os.path.join(tmp, "t.cpp")
        exe = os.path.join(tmp, "t.exe")
        Path(cpp).write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-o", exe, cpp], check=True)
        a = subprocess.run([exe], capture_output=True, text=True)
        b = subprocess.run([exe, "fast"], capture_output=True, text=True)
        if a.returncode or b.returncode:
            raise SystemExit("FAIL: " + a.stderr + b.stderr)
        la, lb = a.stdout.split(), b.stdout.split()
        for i, (x, y) in enumerate(zip(la, lb)):
            if x != y:
                raise SystemExit("FAIL: the shadow differs after capture %d" % i)
        if len(la) != len(lb):
            raise SystemExit("FAIL: run lengths")
        print("PASS: the fast shadow block leaves the same shadow after every capture (%s)" % b.stderr.strip())


if __name__ == "__main__":
    main()
