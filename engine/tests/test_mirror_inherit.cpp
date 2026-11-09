#include "../gpu/vk/mirror_inherit.h"

#include <cstdio>
#include <set>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::mirror_inherit::runs;
    using Runs = std::vector<std::pair<uint64_t, uint64_t>>;
    const unsigned shift = 12;
    const uint64_t page = 4096, region = 4;   // (regions of 4 pages here)
    const uint64_t base = 0x7fd1000000;
    std::set<uint64_t> written;               // (pages the CPU wrote since the retired mirror's sync)
    std::set<uint64_t> dirty_regions;
    const auto w = [&](uint64_t p) { return written.count(p) != 0; };
    const auto clean = [&](uint64_t r) { return dirty_regions.count(r) == 0; };
    const auto mark = [&](uint64_t addr) { written.insert(addr >> shift), dirty_regions.insert((addr >> shift) / region); };

    Runs out;
    runs(base, base + 16 * page, shift, region, w, clean, out);
    CHECK("nothing written: the whole range, one run", out.size() == 1 && out[0] == std::make_pair(base, base + 16 * page));

    mark(base + 5 * page);   // (the scene's vertices: a page in the second region)
    out.clear();
    runs(base, base + 16 * page, shift, region, w, clean, out);
    CHECK("a written page is left out", out.size() == 2 && out[0] == std::make_pair(base, base + 5 * page) &&
                                            out[1] == std::make_pair(base + 6 * page, base + 16 * page));

    out.clear();
    runs(base + 100, base + 5 * page + 10, shift, region, w, clean, out);   // (a retired pool's part that is not page-aligned)
    CHECK("ends clamped to the overlap", out.size() == 1 && out[0] == std::make_pair(base + 100, base + 5 * page));

    out.clear();
    runs(base + 5 * page + 1, base + 5 * page + 20, shift, region, w, clean, out);
    CHECK("inside a written page: nothing", out.empty());

    mark(base + 0 * page), mark(base + 15 * page);
    out.clear();
    runs(base, base + 16 * page, shift, region, w, clean, out);
    CHECK("first and last pages written", out.size() == 2 && out[0] == std::make_pair(base + page, base + 5 * page) &&
                                              out[1] == std::make_pair(base + 6 * page, base + 15 * page));

    written.clear(), dirty_regions.clear();
    dirty_regions.insert((base >> shift) / region + 1);   // (a region stamped, no page of it: the pages are asked one by one)
    out.clear();
    runs(base, base + 16 * page, shift, region, w, clean, out);
    CHECK("a dirty region with clean pages is kept", out.size() == 1 && out[0] == std::make_pair(base, base + 16 * page));

    out.clear();
    runs(base, base, shift, region, w, clean, out);
    CHECK("an empty range: nothing", out.empty());
    std::puts("mirror inherit: all checks passed");
}
